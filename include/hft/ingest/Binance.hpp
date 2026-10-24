#pragma once

#include "hft/Types.hpp"
#include "hft/ingest/Csv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hft::ingest {

// ---------------------------------------------------------------------------
// Binance trade archives (data.binance.vision)
// ---------------------------------------------------------------------------
// Spot trades:    trade_id,price,qty,quoteQty,time,isBuyerMaker,isBestMatch
// Spot aggTrades: agg_id,price,quantity,first_id,last_id,time,isBuyerMaker,best
// USD-M futures:  trade_id,price,qty,quoteQty,time,isBuyerMaker
// Timestamps are ms for futures/aggTrades, and microseconds for spot from
// 2025-01-01 onwards. normalize_timestamp_ns handles all three.
enum class BinanceTradeFormat : std::uint8_t {
    SpotTrades,
    FuturesTrades,
    AggTrades,
};

struct TradeRecord {
    std::uint64_t trade_id{0};
    std::int64_t timestamp_ns{0};
    double price{0.0};
    double quantity{0.0};
    Side aggressor{Side::None};
    bool buyer_maker{false};
};

[[nodiscard]] inline std::int64_t normalize_timestamp_ns(std::int64_t value) noexcept {
    if (value > 100'000'000'000'000'000LL) { // ns since epoch
        return value;
    }
    if (value > 100'000'000'000'000LL) { // microseconds
        return value * 1'000;
    }
    if (value > 100'000'000'000LL) { // milliseconds
        return value * 1'000'000;
    }
    return value * 1'000'000'000; // seconds
}

[[nodiscard]] inline MarketEvent to_market_event(const TradeRecord& trade,
                                                 double price_scale = 1e8,
                                                 double quantity_scale = 1e8) noexcept {
    MarketEvent event{};
    event.order_id = trade.trade_id;
    event.timestamp_ns = trade.timestamp_ns;
    event.price_ticks = static_cast<std::int64_t>(
        std::llround(trade.price * price_scale));
    event.quantity = static_cast<std::int64_t>(
        std::llround(trade.quantity * quantity_scale));
    event.type = EventType::Trade;
    event.side = trade.aggressor;
    return event;
}

namespace detail {

struct TradeColumns {
    std::size_t id{0};
    std::size_t price{1};
    std::size_t quantity{2};
    std::size_t time{4};
    std::size_t buyer_maker{5};
    bool has_header{false};
};

[[nodiscard]] inline TradeColumns trade_columns(
    BinanceTradeFormat format, const std::array<std::string_view, 16>& header,
    std::size_t count) noexcept {
    TradeColumns columns{};
    switch (format) {
    case BinanceTradeFormat::SpotTrades:
    case BinanceTradeFormat::FuturesTrades:
        columns = TradeColumns{0, 1, 2, 4, 5, true};
        break;
    case BinanceTradeFormat::AggTrades:
        columns = TradeColumns{0, 1, 2, 5, 6, true};
        break;
    }
    if (count < 6) {
        return columns;
    }
    // Detect a header row by asking whether the time field is numeric.
    std::int64_t probe = 0;
    if (parse_integer(header[columns.time], probe)) {
        return columns;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::string_view name = header[index];
        if (name == "trade Id" || name == "trade_id" || name == "id" ||
            name == "agg_trade_id" || name == "a") {
            columns.id = index;
        } else if (name == "price" || name == "p") {
            columns.price = index;
        } else if (name == "qty" || name == "quantity" || name == "q") {
            columns.quantity = index;
        } else if (name == "time" || name == "transact_time" || name == "T") {
            columns.time = index;
        } else if (name == "isBuyerMaker" || name == "is_buyer_maker" ||
                   name == "m") {
            columns.buyer_maker = index;
        }
    }
    return columns;
}

} // namespace detail

template <typename Sink>
inline std::uint64_t read_binance_trades_csv(const std::string& path,
                                             BinanceTradeFormat format,
                                             Sink&& sink) {
    std::uint64_t emitted = 0;
    std::array<std::string_view, 16> fields{};
    detail::TradeColumns columns{};
    bool have_columns = false;

    for_each_line(path, [&](std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const std::size_t count = split_fields(line, fields);
        if (!have_columns) {
            columns = detail::trade_columns(format, fields, count);
            have_columns = true;
            std::int64_t probe = 0;
            if (!parse_integer(fields[columns.time], probe)) {
                return; // header row
            }
        }
        const std::size_t required = std::max(
            {columns.id, columns.price, columns.quantity, columns.time,
             columns.buyer_maker});
        if (count <= required) {
            throw std::runtime_error("short Binance trade row: " +
                                     std::string(line));
        }

        TradeRecord trade{};
        std::int64_t raw_time = 0;
        bool buyer_maker = false;
        if (!parse_integer(fields[columns.id], trade.trade_id) ||
            !parse_double(fields[columns.price], trade.price) ||
            !parse_double(fields[columns.quantity], trade.quantity) ||
            !parse_integer(fields[columns.time], raw_time) ||
            !parse_bool(fields[columns.buyer_maker], buyer_maker)) {
            throw std::runtime_error("malformed Binance trade row: " +
                                     std::string(line));
        }
        trade.timestamp_ns = normalize_timestamp_ns(raw_time);
        trade.buyer_maker = buyer_maker;
        // If the buyer is the maker, the aggressor is the seller.
        trade.aggressor = trade.buyer_maker ? Side::Sell : Side::Buy;
        sink(trade);
        ++emitted;
    });
    return emitted;
}

// ---------------------------------------------------------------------------
// Binance bookDepth archives
// ---------------------------------------------------------------------------
// data/futures/um/daily/bookDepth/... has four columns:
//   timestamp,percentage,depth,notional
// with percentage in [-5, 5] and one row per percentage level per snapshot.
// This is aggregate depth, not an order-by-order L2/L4 feed. It is ideal for
// validating OBI levels, not for stressing per-order lifecycle code.
struct DepthSnapshot {
    static constexpr std::size_t kLevels = 11; // -5..+5 percent

    std::int64_t timestamp_ns{0};
    std::array<double, kLevels> percentage{};
    std::array<double, kLevels> base_depth{};
    std::array<double, kLevels> notional{};
    std::uint16_t present{0};

    [[nodiscard]] bool has(int percent) const noexcept {
        const int index = percent + 5;
        return index >= 0 && index < static_cast<int>(kLevels) &&
               (present & (1u << index)) != 0;
    }

    [[nodiscard]] double obi(int percent = 1) const noexcept {
        const int bid_index = 5 - percent;
        const int ask_index = 5 + percent;
        if (bid_index < 0 || ask_index >= static_cast<int>(kLevels) ||
            !has(-percent) || !has(percent)) {
            return 0.0;
        }
        const double bid = notional[static_cast<std::size_t>(bid_index)];
        const double ask = notional[static_cast<std::size_t>(ask_index)];
        const double total = bid + ask;
        return total > 0.0 ? (bid - ask) / total : 0.0;
    }

    [[nodiscard]] double mid_proxy(int percent = 1) const noexcept {
        const int bid_index = 5 - percent;
        const int ask_index = 5 + percent;
        if (bid_index < 0 || ask_index >= static_cast<int>(kLevels)) {
            return 0.0;
        }
        const double bid_depth = base_depth[static_cast<std::size_t>(bid_index)];
        const double ask_depth = base_depth[static_cast<std::size_t>(ask_index)];
        const double bid_notional = notional[static_cast<std::size_t>(bid_index)];
        const double ask_notional = notional[static_cast<std::size_t>(ask_index)];
        if (bid_depth <= 0.0 || ask_depth <= 0.0) {
            return 0.0;
        }
        return 0.5 * (bid_notional / bid_depth + ask_notional / ask_depth);
    }
};

template <typename Sink>
inline std::uint64_t read_binance_book_depth_csv(const std::string& path,
                                                 Sink&& sink) {
    std::uint64_t emitted = 0;
    std::array<std::string_view, 16> fields{};
    DepthSnapshot snapshot{};
    std::int64_t snapshot_time = 0;
    bool have_snapshot = false;
    bool header_skipped = false;

    const auto flush = [&]() {
        if (!have_snapshot) {
            return;
        }
        if (snapshot.present != 0) {
            sink(snapshot);
            ++emitted;
        }
        snapshot = DepthSnapshot{};
        have_snapshot = false;
    };

    for_each_line(path, [&](std::string_view line) {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const std::size_t count = split_fields(line, fields);
        if (count < 4) {
            throw std::runtime_error("short Binance bookDepth row: " +
                                     std::string(line));
        }
        if (!header_skipped) {
            header_skipped = true;
            double probe = 0.0;
            if (!parse_double(fields[1], probe)) {
                return; // header: timestamp,percentage,depth,notional
            }
        }

        std::int64_t timestamp_ns = 0;
        double percent_value = 0.0;
        double depth = 0.0;
        double notional = 0.0;
        if (!parse_utc_ns(fields[0], timestamp_ns) ||
            !parse_double(fields[1], percent_value) ||
            !parse_double(fields[2], depth) ||
            !parse_double(fields[3], notional)) {
            throw std::runtime_error("malformed Binance bookDepth row: " +
                                     std::string(line));
        }
        const std::int64_t percent =
            static_cast<std::int64_t>(std::llround(percent_value));
        if (percent < -5 || percent > 5) {
            throw std::runtime_error("bookDepth percentage out of range");
        }
        if (!have_snapshot || timestamp_ns != snapshot_time) {
            flush();
            snapshot = DepthSnapshot{};
            snapshot.timestamp_ns = timestamp_ns;
            snapshot_time = timestamp_ns;
            have_snapshot = true;
        }
        const std::size_t index = static_cast<std::size_t>(percent + 5);
        snapshot.percentage[index] = static_cast<double>(percent);
        snapshot.base_depth[index] = depth;
        snapshot.notional[index] = notional;
        snapshot.present |= static_cast<std::uint16_t>(1u << index);
    });
    flush();
    return emitted;
}

// Convert one aggregate snapshot into two synthetic book events so the same
// RingBuffer and signal path can be exercised. The generated order ids are
// unique per snapshot; lifetime filters are meaningless for aggregate depth.
inline void emit_depth_snapshot_events(const DepthSnapshot& snapshot,
                                       std::uint64_t& synthetic_oid,
                                       double price_scale,
                                       double quantity_scale,
                                       auto&& sink) {
    if (!snapshot.has(-1) || !snapshot.has(1)) {
        return;
    }
    const double mid = snapshot.mid_proxy(1);
    if (mid <= 0.0) {
        return;
    }
    MarketEvent bid{};
    bid.order_id = ++synthetic_oid;
    bid.timestamp_ns = snapshot.timestamp_ns;
    bid.price_ticks = static_cast<std::int64_t>(std::llround(mid * 0.99 * price_scale));
    bid.quantity = static_cast<std::int64_t>(
        std::llround(snapshot.notional[4] * quantity_scale));
    bid.bid_price_ticks = static_cast<std::int64_t>(
        std::llround(mid * 0.99 * price_scale));
    bid.ask_price_ticks = static_cast<std::int64_t>(
        std::llround(mid * 1.01 * price_scale));
    bid.type = EventType::New;
    bid.side = Side::Bid;
    sink(bid);

    MarketEvent ask = bid;
    ask.order_id = ++synthetic_oid;
    ask.price_ticks = static_cast<std::int64_t>(std::llround(mid * 1.01 * price_scale));
    ask.quantity = static_cast<std::int64_t>(
        std::llround(snapshot.notional[6] * quantity_scale));
    ask.side = Side::Ask;
    sink(ask);
}

} // namespace hft::ingest
