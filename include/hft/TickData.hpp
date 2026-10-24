#pragma once

#include "hft/Types.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hft::data {

namespace detail {

[[nodiscard]] constexpr bool is_ascii_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == '\v';
}

[[nodiscard]] constexpr char ascii_lower(char character) noexcept {
    return (character >= 'A' && character <= 'Z')
               ? static_cast<char>(character - 'A' + 'a')
               : character;
}

[[nodiscard]] constexpr bool iequals(std::string_view left,
                                     std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

inline std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

inline std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

inline std::size_t split_fields(std::string_view line,
                                std::array<std::string_view, 32>& fields) noexcept {
    std::size_t count = 0;
    std::size_t start = 0;
    while (start <= line.size() && count < fields.size()) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            fields[count++] = trim(line.substr(start));
            break;
        }
        fields[count++] = trim(line.substr(start, comma - start));
        start = comma + 1;
    }
    return count;
}

template <typename Integer>
inline bool parse_integer(std::string_view text, Integer& value) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

inline bool parse_event_type(std::string_view text, EventType& type) noexcept {
    text = trim(text);
    if (iequals(text, "new") || iequals(text, "add") || iequals(text, "submit")) {
        type = EventType::New;
    } else if (iequals(text, "modify") || iequals(text, "update") ||
               iequals(text, "replace")) {
        type = EventType::Modify;
    } else if (iequals(text, "cancel") || iequals(text, "delete") ||
               iequals(text, "remove")) {
        type = EventType::Cancel;
    } else if (iequals(text, "trade") || iequals(text, "fill") ||
               iequals(text, "execution")) {
        type = EventType::Trade;
    } else {
        return false;
    }
    return true;
}

inline bool parse_side(std::string_view text, Side& side) noexcept {
    text = trim(text);
    if (iequals(text, "bid") || iequals(text, "b")) {
        side = Side::Bid;
    } else if (iequals(text, "ask") || iequals(text, "offer") || iequals(text, "a")) {
        side = Side::Ask;
    } else if (iequals(text, "buy") || iequals(text, "bought")) {
        side = Side::Buy;
    } else if (iequals(text, "sell") || iequals(text, "sold")) {
        side = Side::Sell;
    } else {
        return false;
    }
    return true;
}

struct ColumnMap {
    std::size_t timestamp{0};
    std::size_t event_type{1};
    std::size_t side{2};
    std::size_t order_id{3};
    std::size_t price{4};
    std::size_t quantity{5};
    std::size_t modifications{6};
    std::size_t bid{7};
    std::size_t ask{8};
    std::size_t parent_order_id{9};
    bool has_modifications{true};
    bool has_bid{true};
    bool has_ask{true};
    bool has_parent_order{false};
};

inline ColumnMap map_header(const std::array<std::string_view, 32>& fields,
                            std::size_t count) {
    ColumnMap map{};
    map.has_modifications = false;
    map.has_bid = false;
    map.has_ask = false;
    map.has_parent_order = false;
    bool has_quantity = false;
    bool has_price = false;
    bool has_timestamp = false;
    bool has_event = false;
    bool has_side = false;
    bool has_order = false;

    for (std::size_t index = 0; index < count; ++index) {
        const std::string name = lowercase(fields[index]);
        if (name == "timestamp_ns" || name == "timestamp" || name == "ts_ns" ||
            name == "time_ns" || name == "time") {
            map.timestamp = index;
            has_timestamp = true;
        } else if (name == "event_type" || name == "event" || name == "type") {
            map.event_type = index;
            has_event = true;
        } else if (name == "side" || name == "aggressor_side") {
            map.side = index;
            has_side = true;
        } else if (name == "order_id" || name == "orderid" || name == "id") {
            map.order_id = index;
            has_order = true;
        } else if (name == "price_ticks" || name == "price" || name == "px") {
            map.price = index;
            has_price = true;
        } else if (name == "quantity" || name == "qty" || name == "size") {
            map.quantity = index;
            has_quantity = true;
        } else if (name == "parent_order_id" || name == "parent_orderid" ||
                   name == "parent_order" || name == "parent_id" ||
                   name == "parent") {
            map.parent_order_id = index;
            map.has_parent_order = true;
        } else if (name == "modifications" || name == "modification_count" ||
                   name == "mod_count" || name == "mods") {
            map.modifications = index;
            map.has_modifications = true;
        } else if (name == "bid_price_ticks" || name == "bid_price" || name == "bid" ||
                   name == "best_bid") {
            map.bid = index;
            map.has_bid = true;
        } else if (name == "ask_price_ticks" || name == "ask_price" || name == "ask" ||
                   name == "offer" || name == "best_ask") {
            map.ask = index;
            map.has_ask = true;
        }
    }

    if (!has_timestamp || !has_event || !has_side || !has_order || !has_price ||
        !has_quantity) {
        throw std::runtime_error("CSV header is missing required columns");
    }
    return map;
}

inline bool is_header(std::string_view line) {
    std::array<std::string_view, 32> fields{};
    const std::size_t count = split_fields(line, fields);
    if (count == 0) {
        return false;
    }
    std::int64_t probe = 0;
    return !parse_integer(fields[0], probe);
}

}

inline std::vector<MarketEvent> load_ticks_csv(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("unable to open tick file: " + path);
    }

    std::vector<MarketEvent> events;
    input.seekg(0, std::ios::end);
    const std::streamoff file_size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (file_size > 0) {
        events.reserve(static_cast<std::size_t>(file_size / 64));
    }

    std::string line;
    std::array<std::string_view, 32> fields{};
    detail::ColumnMap map{};
    bool have_map = false;
    bool first_line = true;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t count = detail::split_fields(line, fields);
        if (!have_map) {
            if (detail::is_header(line)) {
                map = detail::map_header(fields, count);
                have_map = true;
                continue;
            }
            map = detail::ColumnMap{};
            have_map = true;
        }

        const std::size_t required = std::max(
            {map.timestamp, map.event_type, map.side, map.order_id, map.price,
             map.quantity});
        if (count <= required) {
            throw std::runtime_error("short tick row: " + line);
        }

        MarketEvent event{};
        if (!detail::parse_integer(fields[map.timestamp], event.timestamp_ns) ||
            !detail::parse_integer(fields[map.order_id], event.order_id) ||
            !detail::parse_integer(fields[map.price], event.price_ticks) ||
            !detail::parse_integer(fields[map.quantity], event.quantity) ||
            !detail::parse_event_type(fields[map.event_type], event.type) ||
            !detail::parse_side(fields[map.side], event.side)) {
            throw std::runtime_error("malformed tick row: " + line);
        }
        if (map.has_modifications && map.modifications < count &&
            !fields[map.modifications].empty()) {
            detail::parse_integer(fields[map.modifications], event.modifications);
        }
        if (map.has_bid && map.bid < count) {
            detail::parse_integer(fields[map.bid], event.bid_price_ticks);
        }
        if (map.has_ask && map.ask < count) {
            detail::parse_integer(fields[map.ask], event.ask_price_ticks);
        }
        if (map.has_parent_order && map.parent_order_id < count &&
            !fields[map.parent_order_id].empty()) {
            event.parent_order_id = 0;
            detail::parse_integer(fields[map.parent_order_id],
                                  event.parent_order_id);
        }
        events.push_back(event);
    }
    return events;
}

}
