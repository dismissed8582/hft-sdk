#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
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
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '+' || text[index] == '-') {
        negative = text[index] == '-';
        ++index;
    }

    std::uint64_t mantissa = 0;
    int digits = 0;
    int fractional = 0;
    bool seen_digit = false;
    bool seen_point = false;

    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '.') {
            if (seen_point) {
                return false;
            }
            seen_point = true;
            continue;
        }
        if (character < '0' || character > '9') {
            break;
        }
        seen_digit = true;
        if (digits < 18) {
            mantissa = mantissa * 10 + static_cast<std::uint64_t>(character - '0');
            ++digits;
            if (seen_point) {
                ++fractional;
            }
        } else if (seen_point) {
            ++fractional;
        }
    }
    if (!seen_digit) {
        return false;
    }

    int exponent = 0;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        bool exponent_negative = false;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            exponent_negative = text[index] == '-';
            ++index;
        }
        if (index >= text.size()) {
            return false;
        }
        int magnitude = 0;
        for (; index < text.size(); ++index) {
            const char character = text[index];
            if (character < '0' || character > '9') {
                return false;
            }
            magnitude = magnitude * 10 + (character - '0');
            if (magnitude > 308) {
                return false;
            }
        }
        exponent = exponent_negative ? -magnitude : magnitude;
    } else if (index != text.size()) {
        return false;
    }

    static constexpr double kPow10[] = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
        1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
        1e20, 1e21, 1e22};
    const int power = exponent - fractional;
    double result = 0.0;
    if (power >= 0 && power <= 22) {
        result = static_cast<double>(mantissa) * kPow10[power];
    } else if (power < 0 && power >= -22) {
        result = static_cast<double>(mantissa) / kPow10[-power];
    } else {
        result = static_cast<double>(mantissa) * std::pow(10.0, power);
    }
    value = negative ? -result : result;
    return std::isfinite(value);
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
    if (text.size() < 19) {
        return false;
    }
    if (text[10] != ' ' && text[10] != 'T' && text[10] != 't') {
        return false;
    }
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
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
        minute > 59 || second > 60) {
        return false;
    }
    std::int64_t nanoseconds = 0;
    if (text.size() > 19 && text[19] == '.') {
        std::int64_t scale = 100'000'000;
        for (std::size_t index = 20; index < text.size(); ++index) {
            const char character = text[index];
            if (character < '0' || character > '9') {
                return false;
            }
            nanoseconds += static_cast<std::int64_t>(character - '0') * scale;
            scale /= 10;
            if (scale == 0) {
                break;
            }
        }
    }
    const std::int64_t days = days_from_civil(year, month, day);
    timestamp_ns = ((days * 24 + static_cast<std::int64_t>(hour)) * 60 +
                    static_cast<std::int64_t>(minute)) *
                       60 * 1'000'000'000 +
                   static_cast<std::int64_t>(second) * 1'000'000'000 +
                   nanoseconds;
    return true;
}

// Supports .gz transparently when the build links zlib. The callback receives
// a string_view valid only for the duration of the call.
template <typename Callback>
inline std::uint64_t for_each_line(const std::string& path, Callback&& callback) {
    std::uint64_t lines = 0;
    const bool gzip = path.size() > 3 &&
                      path.compare(path.size() - 3, 3, ".gz") == 0;
#if defined(HFT_WITH_ZLIB)
    if (gzip) {
        gzFile file = gzopen(path.c_str(), "rb");
        if (file == nullptr) {
            throw std::runtime_error("unable to open gzip file: " + path);
        }
        std::string buffer(1u << 20, '\0');
        while (gzgets(file, buffer.data(), static_cast<int>(buffer.size())) !=
               nullptr) {
            std::string_view line(buffer.data(), std::strlen(buffer.data()));
            if (!line.empty() && line.back() == '\n') {
                line.remove_suffix(1);
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            callback(line);
            ++lines;
        }
        gzclose(file);
        return lines;
    }
#else
    if (gzip) {
        throw std::runtime_error(
            "gzip input requires zlib; reconfigure with HFT_WITH_ZLIB=ON: " +
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
        gzFile file = gzopen(path.c_str(), "rb");
        if (file == nullptr) {
            throw std::runtime_error("unable to open gzip file: " + path);
        }
        std::array<std::uint8_t, RecordSize> record{};
        while (true) {
            std::size_t filled = 0;
            while (filled < record.size()) {
                const int read = gzread(
                    file, record.data() + filled,
                    static_cast<unsigned>(record.size() - filled));
                if (read <= 0) {
                    break;
                }
                filled += static_cast<std::size_t>(read);
            }
            if (filled == 0) {
                break;
            }
            if (filled != record.size()) {
                gzclose(file);
                throw std::runtime_error("truncated binary record: " + path);
            }
            callback(record);
            ++records;
        }
        gzclose(file);
        return records;
    }
#else
    if (gzip) {
        throw std::runtime_error(
            "gzip input requires zlib; reconfigure with HFT_WITH_ZLIB=ON: " +
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
    return records;
}

} // namespace hft::ingest
