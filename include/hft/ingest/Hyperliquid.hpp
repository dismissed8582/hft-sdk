#pragma once

#include "hft/Types.hpp"
#include "hft/ingest/Csv.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hft::ingest {

// ---------------------------------------------------------------------------
// Zenodo: "An Open Book: Level 4 Order Book Data from the Hyperliquid
// Exchange" (DOI 10.5281/zenodo.18184441).
//
// Three streams are supported:
//   * order statuses: 54-byte little-endian binary records, .data.gz
//   * book diffs:     gzip JSON lines, one order mutation per line
//   * trades:         gzip JSON lines
// ---------------------------------------------------------------------------

[[nodiscard]] inline double decode_hl_fixed_point(std::uint32_t encoded) noexcept {
    static constexpr double kPowers[] = {1.0,      10.0,      100.0,     1000.0,
                                         10000.0,  100000.0,  1000000.0, 10000000.0};
    const std::uint32_t decimals = encoded >> 29;
    const std::uint32_t value = encoded & 0x1FFFFFFFu;
    if (decimals > 7) {
        return 0.0;
    }
    return static_cast<double>(value) / kPowers[decimals];
}

[[nodiscard]] inline std::optional<EventType> map_hl_status(
    std::uint8_t status_id) noexcept {
    switch (status_id) {
    case 1:  // open
    case 9:  // triggered
        return EventType::New;
    case 2:  // canceled
    case 4:  // iocCancelRejected (unfilled remainder)
    case 7:  // reduceOnlyCanceled
    case 10: // scheduledCancel
    case 11: // siblingFilledCanceled
    case 12: // selfTradeCanceled
    case 13: // marginCanceled
    case 14: // vaultWithdrawalCanceled
    case 16: // liquidatedCanceled
        return EventType::Cancel;
    case 5: // filled (possibly partial)
        return EventType::Trade;
    default: // rejected statuses never reach the visible book
        return std::nullopt;
    }
}

template <typename T>
[[nodiscard]] inline T read_le(const std::uint8_t* data) noexcept {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

struct HlOrderStatus {
    std::int64_t timestamp_ns{0};
    std::uint64_t order_id{0};
    std::uint32_t user_id{0};
    std::uint8_t status_id{0};
    bool is_ask{false};
    double limit_price{0.0};
    double size{0.0};
    double original_size{0.0};
    std::uint32_t age_ms{0};
    std::uint8_t order_type_id{0};
    std::uint8_t tif_id{0};
};

// Decodes the packed 54-byte record documented in SCHEMA.md.
[[nodiscard]] inline HlOrderStatus decode_hl_order_status(
    const std::array<std::uint8_t, 54>& raw) noexcept {
    HlOrderStatus status{};
    status.timestamp_ns = static_cast<std::int64_t>(read_le<std::uint64_t>(raw.data() + 0));
    status.user_id = read_le<std::uint32_t>(raw.data() + 8);
    status.status_id = raw[13];
    status.is_ask = raw[14] != 0;
    status.limit_price = decode_hl_fixed_point(read_le<std::uint32_t>(raw.data() + 15));
    status.size = decode_hl_fixed_point(read_le<std::uint32_t>(raw.data() + 19));
    status.order_id = read_le<std::uint64_t>(raw.data() + 23);
    status.age_ms = read_le<std::uint32_t>(raw.data() + 31);
    status.order_type_id = raw[44];
    status.tif_id = raw[45];
    status.original_size = decode_hl_fixed_point(read_le<std::uint32_t>(raw.data() + 50));
    return status;
}

template <typename Sink>
inline std::uint64_t read_hl_order_status(const std::string& path, Sink&& sink) {
    return for_each_record<54>(path, [&](const std::array<std::uint8_t, 54>& raw) {
        sink(decode_hl_order_status(raw));
    });
}

// ---------------------------------------------------------------------------
// Book diffs
// ---------------------------------------------------------------------------
struct HlBookDiff {
    std::int64_t timestamp_ns{0};
    std::uint64_t order_id{0};
    Side side{Side::None};
    EventType type{EventType::New};
    double price{0.0};
    double size{0.0};
    double original_size{0.0};
    std::string_view coin{};
};

struct HlBookDiffOptions {
    std::string_view coin_filter{}; // empty = all coins
    std::int64_t base_timestamp_ns{0};
    // The JSON records carry no timestamp. Set this to the uniform spacing
    // implied by (hour_length / records_in_file), or join with order statuses.
    std::int64_t timestamp_step_ns{1'000};
};

namespace detail {

[[nodiscard]] inline std::size_t find_json_key(std::string_view json,
                                               std::string_view key) noexcept {
    return json.find(key);
}

[[nodiscard]] inline bool json_string(std::string_view json,
                                      std::string_view key,
                                      std::string_view& out) noexcept {
    const std::size_t key_position = find_json_key(json, key);
    if (key_position == std::string_view::npos) {
        return false;
    }
    std::size_t position = json.find(':', key_position + key.size());
    if (position == std::string_view::npos) {
        return false;
    }
    ++position;
    while (position < json.size() && is_space(json[position])) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return false;
    }
    ++position;
    const std::size_t end = json.find('"', position);
    if (end == std::string_view::npos) {
        return false;
    }
    out = json.substr(position, end - position);
    return true;
}

[[nodiscard]] inline bool json_uint(std::string_view json, std::string_view key,
                                    std::uint64_t& out) noexcept {
    const std::size_t key_position = find_json_key(json, key);
    if (key_position == std::string_view::npos) {
        return false;
    }
    std::size_t position = json.find(':', key_position + key.size());
    if (position == std::string_view::npos) {
        return false;
    }
    ++position;
    while (position < json.size() && is_space(json[position])) {
        ++position;
    }
    const std::size_t end = json.find_first_of(",}]", position);
    return parse_integer(json.substr(position, end - position), out);
}

[[nodiscard]] inline bool json_double(std::string_view json,
                                      std::string_view key,
                                      double& out) noexcept {
    const std::size_t key_position = find_json_key(json, key);
    if (key_position == std::string_view::npos) {
        return false;
    }
    std::size_t position = json.find(':', key_position + key.size());
    if (position == std::string_view::npos) {
        return false;
    }
    ++position;
    while (position < json.size() && is_space(json[position])) {
        ++position;
    }
    if (position < json.size() && json[position] == '"') {
        ++position;
    }
    const std::size_t end = json.find_first_of(",}\"]", position);
    return parse_double(json.substr(position, end - position), out);
}

[[nodiscard]] inline bool parse_hl_book_diff_line(
    std::string_view line, HlBookDiff& diff) noexcept {
    if (!json_uint(line, "\"oid\"", diff.order_id)) {
        return false;
    }
    if (!json_string(line, "\"coin\"", diff.coin)) {
        return false;
    }
    std::string_view side;
    if (!json_string(line, "\"side\"", side) || side.empty()) {
        return false;
    }
    diff.side = side.front() == 'A' ? Side::Ask : Side::Bid;
    if (!json_double(line, "\"px\"", diff.price)) {
        return false;
    }

    const std::size_t raw = line.find("\"raw_book_diff\"");
    if (raw == std::string_view::npos) {
        return false;
    }
    const std::string_view tail = line.substr(raw);
    if (tail.find("\"remove\"") != std::string_view::npos) {
        diff.type = EventType::Cancel;
        diff.size = 0.0;
        return true;
    }
    if (tail.find("\"new\"") != std::string_view::npos) {
        diff.type = EventType::New;
        return json_double(tail, "\"sz\"", diff.size);
    }
    if (tail.find("\"update\"") != std::string_view::npos) {
        diff.type = EventType::Modify;
        (void)json_double(tail, "\"origSz\"", diff.original_size);
        return json_double(tail, "\"newSz\"", diff.size);
    }
    return false;
}

} // namespace detail

template <typename Sink>
inline std::uint64_t read_hl_book_diffs(const std::string& path,
                                        const HlBookDiffOptions& options,
                                        Sink&& sink) {
    std::uint64_t emitted = 0;
    std::uint64_t line_index = 0;
    for_each_line(path, [&](std::string_view line) {
        const std::int64_t timestamp =
            options.base_timestamp_ns +
            static_cast<std::int64_t>(line_index) * options.timestamp_step_ns;
        ++line_index;
        if (line.empty() || line.front() == '#') {
            return;
        }
        HlBookDiff diff{};
        if (!detail::parse_hl_book_diff_line(line, diff)) {
            return; // tolerate malformed records in a research dump
        }
        if (!options.coin_filter.empty() && diff.coin != options.coin_filter) {
            return;
        }
        diff.timestamp_ns = timestamp;
        sink(diff);
        ++emitted;
    });
    return emitted;
}

// ---------------------------------------------------------------------------
// Simple L2/L4 book reconstruction
// ---------------------------------------------------------------------------
class BookState {
public:
    void clear() {
        bids_.clear();
        asks_.clear();
    }

    void set(Side side, double price, double size, double price_scale) {
        if (price <= 0.0) {
            return;
        }
        const std::int64_t key =
            static_cast<std::int64_t>(std::llround(price * price_scale));
        auto& levels = side == Side::Bid ? bids_ : asks_;
        if (size <= 0.0) {
            levels.erase(key);
        } else {
            levels[key] = size;
        }
    }

    void apply(const HlBookDiff& diff, double price_scale) {
        set(diff.side, diff.price, diff.type == EventType::Cancel ? 0.0 : diff.size,
            price_scale);
    }

    void apply(const HlOrderStatus& status, double price_scale) {
        const Side side = status.is_ask ? Side::Ask : Side::Bid;
        const double size = status.status_id == 2 ? 0.0 : status.size;
        set(side, status.limit_price, size, price_scale);
    }

    [[nodiscard]] bool best(std::int64_t& bid_ticks,
                            std::int64_t& ask_ticks) const noexcept {
        if (bids_.empty() || asks_.empty()) {
            return false;
        }
        bid_ticks = bids_.rbegin()->first;
        ask_ticks = asks_.begin()->first;
        return bid_ticks > 0 && ask_ticks > 0 && bid_ticks <= ask_ticks;
    }

    [[nodiscard]] std::size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_levels() const noexcept { return asks_.size(); }

private:
    std::map<std::int64_t, double> bids_;
    std::map<std::int64_t, double> asks_;
};

// ---------------------------------------------------------------------------
// Trades (gzip JSON lines)
// ---------------------------------------------------------------------------
struct HlTrade {
    std::int64_t timestamp_ns{0};
    std::uint64_t trade_id{0};
    Side aggressor{Side::None};
    double price{0.0};
    double quantity{0.0};
    std::string_view coin{};
};

template <typename Sink>
inline std::uint64_t read_hl_trades(const std::string& path,
                                    std::string_view coin_filter,
                                    Sink&& sink) {
    std::uint64_t emitted = 0;
    std::uint64_t trade_id = 0;
    for_each_line(path, [&](std::string_view line) {
        if (line.empty() || line.front() != '{') {
            return;
        }
        HlTrade trade{};
        std::string_view time;
        std::string_view side;
        std::string_view coin;
        if (!detail::json_string(line, "\"coin\"", coin) ||
            !detail::json_string(line, "\"side\"", side) ||
            !detail::json_string(line, "\"time\"", time) ||
            !detail::json_double(line, "\"px\"", trade.price) ||
            !detail::json_double(line, "\"sz\"", trade.quantity)) {
            return;
        }
        if (!coin_filter.empty() && coin != coin_filter) {
            return;
        }
        if (!parse_utc_ns(time, trade.timestamp_ns)) {
            return;
        }
        trade.coin = coin;
        trade.aggressor = side.empty() || side.front() == 'A' ? Side::Sell : Side::Buy;
        trade.trade_id = ++trade_id;
        sink(trade);
        ++emitted;
    });
    return emitted;
}

[[nodiscard]] inline MarketEvent to_market_event(
    const HlOrderStatus& status, const BookState& book, double price_scale,
    double quantity_scale) noexcept {
    MarketEvent event{};
    event.order_id = status.order_id;
    event.timestamp_ns = status.timestamp_ns;
    event.price_ticks =
        static_cast<std::int64_t>(std::llround(status.limit_price * price_scale));
    event.quantity =
        static_cast<std::int64_t>(std::llround(status.size * quantity_scale));
    event.type = map_hl_status(status.status_id).value_or(EventType::New);
    if (event.type == EventType::Trade) {
        // The resting order was hit on the opposite side.
        event.side = status.is_ask ? Side::Buy : Side::Sell;
    } else {
        event.side = status.is_ask ? Side::Ask : Side::Bid;
    }
    std::int64_t bid = 0;
    std::int64_t ask = 0;
    if (book.best(bid, ask)) {
        event.bid_price_ticks = bid;
        event.ask_price_ticks = ask;
    }
    return event;
}

[[nodiscard]] inline MarketEvent to_market_event(
    const HlBookDiff& diff, const BookState& book, double price_scale,
    double quantity_scale) noexcept {
    MarketEvent event{};
    event.order_id = diff.order_id;
    event.timestamp_ns = diff.timestamp_ns;
    event.price_ticks =
        static_cast<std::int64_t>(std::llround(diff.price * price_scale));
    event.quantity =
        static_cast<std::int64_t>(std::llround(diff.size * quantity_scale));
    event.type = diff.type;
    event.side = diff.side;
    std::int64_t bid = 0;
    std::int64_t ask = 0;
    if (book.best(bid, ask)) {
        event.bid_price_ticks = bid;
        event.ask_price_ticks = ask;
    }
    return event;
}

[[nodiscard]] inline MarketEvent to_market_event(
    const HlTrade& trade, double price_scale, double quantity_scale) noexcept {
    MarketEvent event{};
    event.order_id = trade.trade_id;
    event.timestamp_ns = trade.timestamp_ns;
    event.price_ticks =
        static_cast<std::int64_t>(std::llround(trade.price * price_scale));
    event.quantity =
        static_cast<std::int64_t>(std::llround(trade.quantity * quantity_scale));
    event.type = EventType::Trade;
    event.side = trade.aggressor;
    return event;
}

} // namespace hft::ingest
