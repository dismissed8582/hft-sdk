#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>

namespace hft {

// Apple silicon (M1..M4) exposes a 128-byte L1D line via sysctl even though
// libc++ reports 64 for hardware_destructive_interference_size. Using 64 there
// makes adjacent 64-byte MarketEvent slots share a line between the producer
// and consumer (false sharing). Allow an override for other microarchitectures.
#ifndef HFT_CACHE_LINE
#if defined(__aarch64__) || defined(__arm64__)
inline constexpr std::size_t kCacheLine = 128;

#else
inline constexpr std::size_t kCacheLine = 64;
#endif
#else
inline constexpr std::size_t kCacheLine = HFT_CACHE_LINE;
#endif
static_assert(kCacheLine >= 64 && (kCacheLine & (kCacheLine - 1)) == 0,
              "kCacheLine must be a power of two >= 64");
inline constexpr std::size_t CACHE_LINE_SIZE = kCacheLine;
inline constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
inline constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;

enum class EventType : std::uint8_t {
    New = 0,
    Modify = 1,
    Cancel = 2,
    Trade = 3,
};

enum class Side : std::uint8_t {
    None = 0,
    Bid = 1,
    Ask = 2,
    Buy = 3,
    Sell = 4,
};

[[nodiscard]] constexpr bool is_trade(EventType type) noexcept {
    return type == EventType::Trade;
}

[[nodiscard]] constexpr bool is_bid_side(Side side) noexcept {
    return side == Side::Bid;
}

[[nodiscard]] constexpr bool is_ask_side(Side side) noexcept {
    return side == Side::Ask;
}

[[nodiscard]] constexpr bool is_buy_side(Side side) noexcept {
    return side == Side::Buy;
}

[[nodiscard]] constexpr bool is_sell_side(Side side) noexcept {
    return side == Side::Sell;
}

[[nodiscard]] constexpr std::string_view event_type_name(EventType type) noexcept {
    switch (type) {
    case EventType::New: return "NEW";
    case EventType::Modify: return "MODIFY";
    case EventType::Cancel: return "CANCEL";
    case EventType::Trade: return "TRADE";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::string_view side_name(Side side) noexcept {
    switch (side) {
    case Side::Bid: return "BID";
    case Side::Ask: return "ASK";
    case Side::Buy: return "BUY";
    case Side::Sell: return "SELL";
    case Side::None: return "NONE";
    }
    return "NONE";
}

struct alignas(kCacheLine) MarketEvent {
    std::uint64_t order_id{0};
    std::int64_t timestamp_ns{0};
    std::int64_t price_ticks{0};
    std::int64_t quantity{0};
    std::int64_t bid_price_ticks{0};
    std::int64_t ask_price_ticks{0};
    std::uint32_t modifications{0};
    EventType type{EventType::New};
    Side side{Side::None};
    // Optional linkage from an execution to the resting parent order. Public
    // trade feeds usually leave this zero; proprietary/order-level feeds can
    // populate it to enable the paper's parent-filtered OBI(T).
    std::uint64_t parent_order_id{0};
    std::uint8_t padding[2]{};
};

static_assert(sizeof(MarketEvent) % kCacheLine == 0);
static_assert(alignof(MarketEvent) == kCacheLine);
static_assert(std::is_trivially_copyable_v<MarketEvent>);

[[nodiscard]] constexpr bool valid_quote(const MarketEvent& event) noexcept {
    return event.bid_price_ticks > 0 && event.ask_price_ticks > 0 &&
           event.bid_price_ticks <= event.ask_price_ticks;
}

[[nodiscard]] constexpr double midpoint_ticks(const MarketEvent& event) noexcept {
    return 0.5 * static_cast<double>(event.bid_price_ticks) +
           0.5 * static_cast<double>(event.ask_price_ticks);
}

[[nodiscard]] constexpr double midpoint_ticks(std::int64_t bid, std::int64_t ask) noexcept {
    return 0.5 * static_cast<double>(bid) + 0.5 * static_cast<double>(ask);
}

inline constexpr std::int64_t kNoTimestamp = std::numeric_limits<std::int64_t>::min();
inline constexpr std::uint32_t kNoModificationLimit = std::numeric_limits<std::uint32_t>::max();

}

