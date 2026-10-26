#pragma once

#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(HFT_WITH_ZLIB)
#include <zlib.h>
#endif

namespace hft::ingest {

[[nodiscard]] constexpr bool is_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
}

inline std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

template <typename Integer>
[[nodiscard]] inline bool parse_integer(std::string_view text,
                                        Integer& value) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

// Allocation-free decimal parser. libc++ still does not provide floating
// point std::from_chars, and strtod allocates/copies. Binance and Hyperliquid
// only emit fixed-point decimals, so this is both faster and sufficient.
[[nodiscard]] inline bool parse_bool(std::string_view text, bool& value) noexcept {
    text = trim(text);
    if (text == "true" || text == "True" || text == "TRUE" || text == "1") {
        value = true;
        return true;
    }
    if (text == "false" || text == "False" || text == "FALSE" || text == "0") {
        value = false;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool parse_double(std::string_view text,
                                       double& value) noexcept {
    text = trim(text);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           std::isfinite(value);
}

[[nodiscard]] inline std::int64_t scaled_integer(double value, double scale) {
    const long double scaled = static_cast<long double>(value) * scale;
    if (!std::isfinite(value) || !std::isfinite(scale) || value < 0 || scale <= 0 ||
        scaled >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        throw std::out_of_range("scaled market-data value is out of range");
    }
    return std::llround(scaled);
}

template <std::size_t Capacity>
[[nodiscard]] inline std::size_t split_fields(
    std::string_view line, std::array<std::string_view, Capacity>& fields,
    char delimiter = ',') noexcept {
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= line.size() && count < Capacity) {
        const std::size_t position = line.find(delimiter, start);
        if (position == std::string_view::npos) {
            fields[count++] = trim(line.substr(start));
            break;
        }
        fields[count++] = trim(line.substr(start, position - start));
        start = position + 1;
    }
    return count;
}

// Howard Hinnant's civil calendar conversion, valid for the date ranges in
// the public datasets.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year,
                                                     unsigned month,
                                                     unsigned day) noexcept {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153u * (month > 2 ? month - 3 : month + 9) + 2u) / 5u + day - 1u;
    const unsigned day_of_era =
        year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// "YYYY-MM-DD HH:MM:SS" (UTC), optional fractional seconds.
[[nodiscard]] inline bool parse_utc_ns(std::string_view text,
                                       std::int64_t& timestamp_ns) noexcept {
    text = trim(text);
    if (text.size() < 19 || text[4] != '-' || text[7] != '-' ||
        text[13] != ':' || text[16] != ':' ||
        (text[10] != ' ' && text[10] != 'T' && text[10] != 't')) return false;

    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (!parse_integer(text.substr(0, 4), year) ||
        !parse_integer(text.substr(5, 2), month) ||
        !parse_integer(text.substr(8, 2), day) ||
        !parse_integer(text.substr(11, 2), hour) ||
        !parse_integer(text.substr(14, 2), minute) ||
        !parse_integer(text.substr(17, 2), second)) {
        return false;
    }
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 || hour > 23 ||
        minute > 59 || second > 59) return false;
    constexpr unsigned lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > lengths[month - 1] + (month == 2 && leap ? 1u : 0u)) return false;
    std::int64_t nanoseconds = 0;
    auto suffix = text.substr(19);
    if (!suffix.empty() && (suffix.back() == 'Z' || suffix.back() == 'z')) suffix.remove_suffix(1);
    if (!suffix.empty()) {
        if (suffix.front() != '.' || suffix.size() < 2 || suffix.size() > 10) return false;
        std::int64_t scale = 100'000'000;
        for (const auto character : suffix.substr(1)) {
            if (character < '0' || character > '9') return false;
            nanoseconds += (character - '0') * scale;
            scale /= 10;
        }
    }
    const auto seconds = days_from_civil(year, month, day) * 86400 +
        static_cast<std::int64_t>(hour) * 3600 + minute * 60 + second;
    constexpr std::int64_t billion = 1'000'000'000;
    if (seconds >= 0) {
        if (seconds > (std::numeric_limits<std::int64_t>::max() - nanoseconds) / billion) return false;
        timestamp_ns = seconds * billion + nanoseconds;
    } else {
        if (seconds < std::numeric_limits<std::int64_t>::min() / billion - 1) return false;
        const auto magnitude = static_cast<std::uint64_t>(-seconds) * billion - nanoseconds;
        const auto limit = std::uint64_t{1} << 63;
        if (magnitude > limit) return false;
        timestamp_ns = magnitude == limit ? std::numeric_limits<std::int64_t>::min()
                                         : -static_cast<std::int64_t>(magnitude);
    }
    return true;
}

#if defined(HFT_WITH_ZLIB)
// Use inflate directly: some gzread implementations treat a truncated footer
// as ordinary EOF. Require a verified stream end for every gzip member.
template <typename Callback>
inline void for_each_gzip_chunk(const std::string& path, Callback&& callback) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open gzip file: " + path);
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("unable to initialize gzip decoder");
    }
    struct Cleanup {
        z_stream* stream;
        ~Cleanup() { inflateEnd(stream); }
    } cleanup{&stream};
    std::array<unsigned char, 65536> compressed{}, decoded{};
    bool complete = false;
    while (input) {
        input.read(reinterpret_cast<char*>(compressed.data()), compressed.size());
        const auto count = input.gcount();
        if (count == 0) break;
        stream.next_in = compressed.data();
        stream.avail_in = static_cast<uInt>(count);
        do {
            if (complete) {
                auto* next = stream.next_in;
                const auto remaining = stream.avail_in;
                if (inflateReset2(&stream, 16 + MAX_WBITS) != Z_OK) {
                    throw std::runtime_error("gzip reset failed");
                }
                stream.next_in = next;
                stream.avail_in = remaining;
                complete = false;
            }
            stream.next_out = decoded.data();
            stream.avail_out = static_cast<uInt>(decoded.size());
            const int result = inflate(&stream, Z_NO_FLUSH);
            const auto produced = decoded.size() - stream.avail_out;
            if (result == Z_BUF_ERROR && stream.avail_in == 0 && produced == 0) break;
            if (result != Z_OK && result != Z_STREAM_END) {
                throw std::runtime_error("corrupt or truncated gzip input: " + path);
            }
            if (produced) callback(std::string_view(
                reinterpret_cast<const char*>(decoded.data()), produced));
            complete = result == Z_STREAM_END;
        } while (stream.avail_in != 0 || (stream.avail_out == 0 && !complete));
    }
    if (input.bad() || !complete) throw std::runtime_error("truncated gzip input: " + path);
}
#endif

// Supports .gz transparently when the build links zlib. The callback receives
// a string_view valid only for the duration of the call.
template <typename Callback>
inline std::uint64_t for_each_line(const std::string& path, Callback&& callback) {
    std::uint64_t lines = 0;
    const bool gzip = path.size() > 3 &&
                      path.compare(path.size() - 3, 3, ".gz") == 0;
#if defined(HFT_WITH_ZLIB)
    if (gzip) {
        std::string line;
        for_each_gzip_chunk(path, [&](std::string_view chunk) {
            for (const char value : chunk) {
                if (value == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    callback(std::string_view(line));
                    ++lines;
                    line.clear();
                } else {
                    line.push_back(value);
                }
            }
        });
        if (!line.empty()) {
            if (line.back() == '\r') line.pop_back();
            callback(std::string_view(line));
            ++lines;
        }
        return lines;
    }
#else
    if (gzip) {
        throw std::runtime_error(
            "gzip input requires zlib; install zlib development files and rebuild: " +
            path);
    }
#endif

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open file: " + path);
    }
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        callback(std::string_view(line));
        ++lines;
    }
    return lines;
}

// Reads a binary file (optionally gzip) in fixed-size records.
template <std::size_t RecordSize, typename Callback>
inline std::uint64_t for_each_record(const std::string& path,
                                     Callback&& callback) {
    static_assert(RecordSize > 0);
    std::uint64_t records = 0;
    const bool gzip = path.size() > 3 &&
                      path.compare(path.size() - 3, 3, ".gz") == 0;
#if defined(HFT_WITH_ZLIB)
    if (gzip) {
        std::array<std::uint8_t, RecordSize> record{};
        std::size_t filled = 0;
        for_each_gzip_chunk(path, [&](std::string_view chunk) {
            while (!chunk.empty()) {
                const auto count = std::min(chunk.size(), record.size() - filled);
                std::memcpy(record.data() + filled, chunk.data(), count);
                filled += count;
                chunk.remove_prefix(count);
                if (filled == record.size()) {
                    callback(record);
                    ++records;
                    filled = 0;
                }
            }
        });
        if (filled != 0) throw std::runtime_error("truncated binary record: " + path);
        return records;
    }
#else
    if (gzip) {
        throw std::runtime_error(
            "gzip input requires zlib; install zlib development files and rebuild: " +
            path);
    }
#endif
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open file: " + path);
    }
    std::array<std::uint8_t, RecordSize> record{};
    while (input.read(reinterpret_cast<char*>(record.data()),
                      static_cast<std::streamsize>(record.size()))) {
        callback(record);
        ++records;
    }
    if (input.gcount() != 0) throw std::runtime_error("truncated binary record: " + path);
    if (input.bad()) throw std::runtime_error("error reading binary file: " + path);
    return records;
}

} // namespace hft::ingest

