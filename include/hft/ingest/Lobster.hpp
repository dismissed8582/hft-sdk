#pragma once

#include "hft/Types.hpp"
#include "hft/ingest/Csv.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hft::ingest {

// LOBSTER message files are the optional traditional-market bridge.
//
// Columns (no header):
//   Time,Type,Order ID,Size,Price,Direction
//
// Type mapping (LOBSTER documentation):
//   1 submission bid     2 submission ask
//   3 cancellation bid   4 cancellation ask
//   5 deletion bid       6 deletion ask
//   7 execution bid      8 execution ask
//
// Time is seconds after midnight with nanosecond precision. Pass the trading
// day's UTC epoch in base_timestamp_ns.
struct LobsterOptions {
    std::int64_t base_timestamp_ns{0};
    double price_scale{10'000.0};    // $0.0001 ticks
    double quantity_scale{1.0};      // shares
};

[[nodiscard]] inline bool decode_lobster_type(std::int64_t type, EventType& event,
                                              Side& side) noexcept {
    switch (type) {
    case 1: event = EventType::New; side = Side::Bid; return true;
    case 2: event = EventType::New; side = Side::Ask; return true;
    case 3: event = EventType::Cancel; side = Side::Bid; return true;
    case 4: event = EventType::Cancel; side = Side::Ask; return true;
    case 5: event = EventType::Cancel; side = Side::Bid; return true;
    case 6: event = EventType::Cancel; side = Side::Ask; return true;
    case 7: event = EventType::Trade; side = Side::Sell; return true;
    case 8: event = EventType::Trade; side = Side::Buy; return true;
    default: return false;
    }
}

template <typename Sink>
inline std::uint64_t read_lobster_messages(const std::string& path,
                                           const LobsterOptions& options,
                                           Sink&& sink) {
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
        if (!decode_lobster_type(type, event_type, side)) {
            return;
        }
        if (event_type == EventType::New) {
            side = direction > 0 ? Side::Bid : Side::Ask;
        }

        MarketEvent event{};
        event.order_id = order_id;
        event.timestamp_ns = options.base_timestamp_ns +
                             static_cast<std::int64_t>(
                                 std::llround(seconds * 1'000'000'000.0));
        event.price_ticks =
            static_cast<std::int64_t>(std::llround(price * options.price_scale));
        event.quantity = static_cast<std::int64_t>(
            std::llround(size * options.quantity_scale));
        event.type = event_type;
        event.side = side;
        sink(event);
        ++emitted;
    });
    return emitted;
}

} // namespace hft::ingest
