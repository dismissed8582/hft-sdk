#pragma once

#include "hft/Types.hpp"
#include "hft/ingest/Csv.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <limits>
#include <unordered_map>

namespace hft::ingest {

// LOBSTER message files are the optional traditional-market bridge.
//
// Columns (no header):
//   Time,Type,Order ID,Size,Price,Direction
//
// Standard message types: 1 submission, 2 partial cancellation, 3 deletion,
// 4 visible execution, 5 hidden execution, 6 cross trade, 7 trading halt.
// Direction identifies the resting side (+1 bid / -1 ask), not the event type.
// Prices are already integer dollar prices multiplied by 10,000.
// Reference: https://lobsterdata.com/info/DataStructure.php
//
// Time is seconds after midnight with nanosecond precision. Pass the trading
// day's UTC epoch in base_timestamp_ns.
struct LobsterOptions {
    std::int64_t base_timestamp_ns{0};
    double price_scale{10'000.0};    // $0.0001 ticks
    double quantity_scale{1.0};      // shares
    // Seed when replay begins after resting orders were submitted.
    std::unordered_map<std::uint64_t, double> initial_remaining;
};

[[nodiscard]] inline bool decode_lobster_type(std::int64_t type, EventType& event,
                                              Side& side, std::int64_t direction = 0) noexcept {
    if (type < 1 || type > 5 || (direction != 1 && direction != -1)) return false;
    if (type == 1) event = EventType::New;
    else if (type == 2) event = EventType::Modify;
    else if (type == 3) event = EventType::Cancel;
    else event = EventType::Trade;
    side = event == EventType::Trade ? (direction == 1 ? Side::Sell : Side::Buy)
                                    : (direction == 1 ? Side::Bid : Side::Ask);
    return true;
}

template <typename Sink>
inline std::uint64_t read_lobster_messages(const std::string& path,
                                           const LobsterOptions& options,
                                           Sink&& sink) {
    if (options.base_timestamp_ns < 0 || !std::isfinite(options.price_scale) ||
        !std::isfinite(options.quantity_scale) || options.price_scale <= 0 || options.quantity_scale <= 0) {
        throw std::invalid_argument("invalid LOBSTER scaling or base timestamp");
    }
    auto remaining = options.initial_remaining;
    std::uint64_t emitted = 0;
    std::array<std::string_view, 16> fields{};
    for_each_line(path, [&](std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const std::size_t count = split_fields(line, fields);
        if (count < 6) {
            throw std::runtime_error("short LOBSTER message row: " +
                                     std::string(line));
        }
        double seconds = 0.0;
        std::int64_t type = 0;
        std::uint64_t order_id = 0;
        double size = 0.0;
        double price = 0.0;
        std::int64_t direction = 0;
        if (!parse_double(fields[0], seconds) ||
            !parse_integer(fields[1], type) ||
            !parse_integer(fields[2], order_id) ||
            !parse_double(fields[3], size) ||
            !parse_double(fields[4], price) ||
            !parse_integer(fields[5], direction)) {
            throw std::runtime_error("malformed LOBSTER row: " +
                                     std::string(line));
        }
        EventType event_type{};
        Side side{};
        if (type == 6 || type == 7) return; // Auctions/halts are not directional order events.
        if (!decode_lobster_type(type, event_type, side, direction) || seconds < 0 || seconds >= 86400 ||
            size < 0 || price <= 0) throw std::runtime_error("invalid LOBSTER event");
        double event_size = size;
        if (type == 1) {
            remaining[order_id] = size;
        } else if (type == 2) {
            const auto found = remaining.find(order_id);
            if (found == remaining.end() || size > found->second) {
                throw std::runtime_error("partial cancellation requires seeded LOBSTER order size");
            }
            found->second -= size;
            event_size = found->second;
            if (event_size == 0) {
                event_type = EventType::Cancel;
                event_size = size;
                remaining.erase(found);
            }
        } else if (type == 3) {
            remaining.erase(order_id);
        } else if (type == 4) {
            const auto found = remaining.find(order_id);
            if (found != remaining.end()) {
                found->second = std::max(0.0, found->second - size);
                if (found->second == 0) remaining.erase(found);
            }
        }

        MarketEvent event{};
        event.order_id = order_id;
        const auto elapsed = static_cast<std::int64_t>(std::llround(seconds * 1'000'000'000.0));
        if (options.base_timestamp_ns > std::numeric_limits<std::int64_t>::max() - elapsed ||
            price * (options.price_scale / 10000.0) >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            event_size * options.quantity_scale >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("LOBSTER scaled value is out of range");
        }
        event.timestamp_ns = options.base_timestamp_ns + elapsed;
        event.price_ticks = static_cast<std::int64_t>(std::llround(price * (options.price_scale / 10000.0)));
        event.quantity = static_cast<std::int64_t>(std::llround(event_size * options.quantity_scale));
        if (type == 4) event.parent_order_id = order_id;
        event.type = event_type;
        event.side = side;
        sink(event);
        ++emitted;
    });
    return emitted;
}

} // namespace hft::ingest

