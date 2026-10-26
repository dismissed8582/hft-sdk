#pragma once

#include "hft/OBIFilter.hpp"
#include "hft/ScoringEngine.hpp"
#include "hft/Types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft::paper {

// Horizons used by the paper-faithful diagnostic. The paper's Pearson
// appendix reports 1, 10, 30, 50, 80 and 100 seconds; its regime appendix
// reports 1, 10 and 20 seconds. Keeping one array lets the evaluator compute
// each forward return once and reuse it for all schemes.
inline constexpr std::array<std::int64_t, 7> kHorizonNs{
    kNanosecondsPerSecond,
    10 * kNanosecondsPerSecond,
    20 * kNanosecondsPerSecond,
    30 * kNanosecondsPerSecond,
    50 * kNanosecondsPerSecond,
    80 * kNanosecondsPerSecond,
    100 * kNanosecondsPerSecond,
};
inline constexpr std::array<int, 7> kHorizonSeconds{1, 10, 20, 30, 50, 80, 100};
inline constexpr std::array<std::size_t, 6> kPearsonHorizonIndices{0, 1, 3, 4, 5, 6};
inline constexpr std::array<std::size_t, 3> kRegimeHorizonIndices{0, 1, 2};

inline constexpr std::uint8_t kAllSchemes =
    static_cast<std::uint8_t>((1u << static_cast<unsigned>(obi::FilterScheme::Unfiltered)) |
                              (1u << static_cast<unsigned>(obi::FilterScheme::Lifetime)) |
                              (1u << static_cast<unsigned>(obi::FilterScheme::ModificationCount)) |
                              (1u << static_cast<unsigned>(obi::FilterScheme::ModificationTime)));

struct Lifecycle {
    std::int64_t first_seen_ns{kNoTimestamp};
    std::int64_t last_seen_ns{kNoTimestamp};
    std::int64_t cancel_ns{kNoTimestamp};
    std::int64_t previous_modification_ns{kNoTimestamp};
    std::int64_t last_modification_ns{kNoTimestamp};
    std::uint32_t modifications{0};
    std::int64_t remaining_quantity{0};
    std::int64_t fully_executed_ns{kNoTimestamp};
    bool saw_new{false};
    bool has_cancel{false};
    bool has_full_execution{false};
};

struct FilterMaskTable {
    std::unordered_map<std::uint64_t, std::uint8_t> masks;
    std::uint8_t default_mask{kAllSchemes};

    [[nodiscard]] bool contains(std::uint64_t order_id) const noexcept {
        return masks.find(order_id) != masks.end();
    }

    [[nodiscard]] std::uint8_t get(std::uint64_t order_id) const noexcept {
        const auto iterator = masks.find(order_id);
        return iterator == masks.end() ? default_mask : iterator->second;
    }
};

// Builds the ex-post order-level filter masks used by the paper. The paper
// excludes every event of an order whose *final* lifetime, modification count
// or last-two-modification interval fails the corresponding threshold. It
// retains trade executions, so trade events with no parent order record fall
// back to the all-scheme mask.
[[nodiscard]] inline FilterMaskTable build_filter_masks(
    std::span<const MarketEvent> events,
    const obi::FilterThresholds& thresholds) {
    std::unordered_map<std::uint64_t, Lifecycle> lifecycles;
    lifecycles.reserve(events.size() / 4 + 1);

    for (const MarketEvent& event : events) {
        if (event.type == EventType::Trade) {
            if (event.parent_order_id == 0) continue;
            const auto found = lifecycles.find(event.parent_order_id);
            if (found != lifecycles.end() && found->second.saw_new && event.quantity > 0) {
                auto& parent = found->second;
                parent.last_seen_ns = std::max(parent.last_seen_ns, event.timestamp_ns);
                if (!parent.has_full_execution) {
                    parent.remaining_quantity = std::max<std::int64_t>(0,
                        parent.remaining_quantity - event.quantity);
                    if (parent.remaining_quantity == 0) {
                        parent.has_full_execution = true;
                        parent.fully_executed_ns = event.timestamp_ns;
                    }
                }
            }
            continue;
        }
        Lifecycle& lifecycle = lifecycles[event.order_id];
        if (lifecycle.first_seen_ns == kNoTimestamp ||
            event.timestamp_ns < lifecycle.first_seen_ns) {
            lifecycle.first_seen_ns = event.timestamp_ns;
        }
        lifecycle.last_seen_ns = std::max(lifecycle.last_seen_ns, event.timestamp_ns);
        switch (event.type) {
        case EventType::New:
            lifecycle.saw_new = true;
            lifecycle.remaining_quantity = event.quantity > 0 ? event.quantity : 0;
            break;
        case EventType::Modify:
            lifecycle.remaining_quantity = std::max<std::int64_t>(0, event.quantity);
            lifecycle.previous_modification_ns = lifecycle.last_modification_ns;
            lifecycle.last_modification_ns = event.timestamp_ns;
            ++lifecycle.modifications;
            break;
        case EventType::Cancel:
            lifecycle.has_cancel = true;
            lifecycle.cancel_ns = event.timestamp_ns;
            break;
        case EventType::Trade: break;
        }
    }

    FilterMaskTable table{};
    table.masks.reserve(lifecycles.size());
    for (const auto& [order_id, lifecycle] : lifecycles) {
        if (!lifecycle.saw_new) {
            continue;
        }
        std::int64_t exit_ns = lifecycle.last_seen_ns;
        if (lifecycle.has_full_execution) {
            exit_ns = lifecycle.fully_executed_ns;
        } else if (lifecycle.has_cancel) {
            exit_ns = lifecycle.cancel_ns;
        }
        const std::int64_t lifetime_ns =
            std::max<std::int64_t>(0, exit_ns - lifecycle.first_seen_ns);
        const std::int64_t modification_interval_ns =
            lifecycle.modifications >= 2
                ? std::max<std::int64_t>(
                      0, lifecycle.last_modification_ns -
                             lifecycle.previous_modification_ns)
                : std::numeric_limits<std::int64_t>::max();

        std::uint8_t mask = obi::scheme_bit(obi::FilterScheme::Unfiltered);
        if (lifetime_ns >= thresholds.lifetime_ns) {
            mask |= obi::scheme_bit(obi::FilterScheme::Lifetime);
        }
        if (lifecycle.modifications <= thresholds.max_modifications) {
            mask |= obi::scheme_bit(obi::FilterScheme::ModificationCount);
        }
        if (modification_interval_ns >= thresholds.min_modification_interval_ns) {
            mask |= obi::scheme_bit(obi::FilterScheme::ModificationTime);
        }
        table.masks.emplace(order_id, mask);
    }
    return table;
}

struct PaperConfig {
    std::int64_t obi_window_ns{10 * kNanosecondsPerSecond};
    std::int64_t sample_ns{250 * kNanosecondsPerMillisecond};
    std::int64_t anchor_ns{15 * kNanosecondsPerSecond};
    std::size_t max_catchup_samples{10'000};
    bool paper_filters{true};
};

struct TradePrint {
    std::int64_t timestamp_ns{0};
    std::int64_t price{0};
};

struct PaperTick {
    std::int64_t timestamp_ns{0};
    std::uint8_t scheme{0};
    bool is_anchor{false};
    double lob_obi{0.0};
    bool lob_valid{false};
    double trade_obi{0.0};
    bool trade_valid{false};
    std::array<double, kHorizonNs.size()> forward_returns{};
    std::array<std::uint8_t, kHorizonNs.size()> forward_valid{};
    std::uint8_t obi_regime{0};
    std::uint8_t trade_obi_regime{0};
    std::array<std::uint8_t, kRegimeHorizonIndices.size()> return_regime{};
};

namespace detail {

[[nodiscard]] inline bool realized_trade_return(
    const std::vector<TradePrint>& trades,
    std::int64_t start_ns,
    std::int64_t horizon_ns,
    double& result) {
    const auto begins = std::upper_bound(
        trades.begin(), trades.end(), start_ns,
        [](std::int64_t value, const TradePrint& trade) {
            return value < trade.timestamp_ns;
        });
    const auto ends = std::upper_bound(
        trades.begin(), trades.end(), start_ns + horizon_ns,
        [](std::int64_t value, const TradePrint& trade) {
            return value < trade.timestamp_ns;
        });
    if (begins == trades.end() || begins == ends) {
        return false;
    }
    const TradePrint& first = *begins;
    const TradePrint& last = *std::prev(ends);
    if (first.price <= 0 || last.price <= 0) {
        return false;
    }
    result = static_cast<double>(last.price - first.price) /
             static_cast<double>(first.price);
    return std::isfinite(result);
}

[[nodiscard]] inline int return_regime_three(double value, double neutral) noexcept {
    if (value > neutral) {
        return 2;
    }
    if (value < -neutral) {
        return 0;
    }
    return 1;
}

} // namespace detail

// Evaluates the paper's observation design:
//   * OBI over a backward-looking 10-second window.
//   * Realized return from the first and last trade print in (t, t + horizon].
//   * Anchors every 15 seconds for the regime layer.
// It also supports the causal streaming filters for a side-by-side comparison.
[[nodiscard]] inline std::vector<PaperTick> evaluate(
    std::span<const MarketEvent> events,
    const obi::FilterThresholds& thresholds,
    const PaperConfig& config = {}) {
    if (config.obi_window_ns <= 0 || config.sample_ns <= 0 || config.anchor_ns <= 0 ||
        config.max_catchup_samples == 0) throw std::invalid_argument("invalid paper configuration");
    const auto margin = std::max({config.sample_ns, config.anchor_ns, kHorizonNs.back()});
    std::int64_t previous = 0;
    for (const auto& event : events) {
        if (event.timestamp_ns < previous || event.quantity < 0 ||
            event.timestamp_ns > std::numeric_limits<std::int64_t>::max() - margin) {
            throw std::invalid_argument("paper input must have ordered, valid timestamps and quantities");
        }
        previous = event.timestamp_ns;
    }
    std::vector<PaperTick> ticks;
    if (events.empty()) {
        return ticks;
    }

    FilterMaskTable masks{};
    std::unique_ptr<obi::OrderTracker<>> tracker;
    if (config.paper_filters) {
        masks = build_filter_masks(events, thresholds);
    } else {
        tracker = std::make_unique<obi::OrderTracker<>>();
    }

    std::vector<TradePrint> trades;
    trades.reserve(events.size() / 16 + 1);
    for (const MarketEvent& event : events) {
        if (event.type == EventType::Trade && event.price_ticks > 0) {
            trades.push_back(TradePrint{event.timestamp_ns, event.price_ticks});
        }
    }
    std::stable_sort(trades.begin(), trades.end(),
                     [](const TradePrint& left, const TradePrint& right) {
                         return left.timestamp_ns < right.timestamp_ns;
                     });

    obi::BucketedRollingCounts<> window(config.obi_window_ns);
    ticks.reserve(events.size() / 32 + 1);

    const std::int64_t first_timestamp = events.front().timestamp_ns;
    std::int64_t next_sample_ns = first_timestamp;
    std::int64_t next_anchor_ns =
        first_timestamp + std::max<std::int64_t>(1, config.anchor_ns);
    const std::int64_t sample_step = std::max<std::int64_t>(1, config.sample_ns);

    const auto capture = [&](std::int64_t timestamp_ns) {
        window.advance(timestamp_ns);
        bool anchor = timestamp_ns >= next_anchor_ns;
        if (anchor) {
            while (next_anchor_ns <= timestamp_ns) {
                next_anchor_ns += std::max<std::int64_t>(1, config.anchor_ns);
            }
        }

        std::array<double, kHorizonNs.size()> returns{};
        std::array<std::uint8_t, kHorizonNs.size()> valid{};
        for (std::size_t index = 0; index < kHorizonNs.size(); ++index) {
            double value = 0.0;
            if (detail::realized_trade_return(trades, timestamp_ns,
                                              kHorizonNs[index], value)) {
                returns[index] = value;
                valid[index] = 1;
            }
        }

        for (std::size_t scheme = 0;
             scheme < static_cast<std::size_t>(obi::FilterScheme::Count);
             ++scheme) {
            PaperTick tick{};
            tick.timestamp_ns = timestamp_ns;
            tick.scheme = static_cast<std::uint8_t>(scheme);
            tick.is_anchor = anchor;
            const obi::DirectionalCounts& counts = window.counts(
                static_cast<obi::FilterScheme>(scheme));
            const obi::OBIValue lob = obi::order_book_imbalance(counts);
            const obi::OBIValue trade = obi::trade_order_book_imbalance(counts);
            tick.lob_obi = lob.value;
            tick.lob_valid = lob.valid;
            tick.trade_obi = trade.value;
            tick.trade_valid = trade.valid;
            tick.obi_regime = static_cast<std::uint8_t>(
                scoring::RegimeDiscretizer::obi_regime(lob.value));
            tick.trade_obi_regime = static_cast<std::uint8_t>(
                scoring::RegimeDiscretizer::obi_regime(trade.value));
            tick.forward_returns = returns;
            tick.forward_valid = valid;
            ticks.push_back(tick);
        }
    };

    std::size_t index = 0;
    while (index < events.size()) {
        const std::int64_t timestamp_ns = events[index].timestamp_ns;
        while (index < events.size() &&
               events[index].timestamp_ns == timestamp_ns) {
            const MarketEvent& event = events[index];
            std::uint8_t mask = kAllSchemes;
            bool skip = false;
            if (config.paper_filters) {
                const std::uint64_t parent = event.type == EventType::Trade &&
                    event.parent_order_id != 0
                    ? event.parent_order_id
                    : event.order_id;
                if (event.type == EventType::Cancel && !masks.contains(parent)) {
                    skip = true;
                } else {
                    mask = event.type == EventType::Trade && event.parent_order_id == 0
                        ? kAllSchemes : masks.get(parent);
                }
            } else {
                const auto observation = tracker->observe(event);
                if (event.type == EventType::Cancel && !observation.found) {
                    skip = true;
                } else {
                    mask = obi::filter_mask(observation.filters, thresholds);
                }
            }
            if (!skip) {
                window.push(event.timestamp_ns, event.side,
                            is_trade(event.type), mask,
                            static_cast<double>(event.quantity));
            }
            ++index;
        }

        const std::int64_t pending_samples =
            (timestamp_ns - next_sample_ns) / sample_step + 1;
        if (pending_samples > static_cast<std::int64_t>(config.max_catchup_samples)) {
            next_sample_ns = timestamp_ns;
        }
        while (next_sample_ns <= timestamp_ns) {
            capture(next_sample_ns);
            next_sample_ns += sample_step;
        }
    }

    // Assign the paper's three return regimes using a symmetric neutral band
    // of half a standard deviation for each regime horizon.
    for (std::size_t regime_index = 0;
         regime_index < kRegimeHorizonIndices.size(); ++regime_index) {
        const std::size_t horizon_index = kRegimeHorizonIndices[regime_index];
        double sum = 0.0;
        double sum_squares = 0.0;
        std::size_t count = 0;
        for (const PaperTick& tick : ticks) {
            if (tick.forward_valid[horizon_index] != 0) {
                const double value = tick.forward_returns[horizon_index];
                sum += value;
                sum_squares += value * value;
                ++count;
            }
        }
        double neutral = 0.0;
        if (count > 1) {
            const double mean = sum / static_cast<double>(count);
            const double variance =
                std::max(0.0, sum_squares / static_cast<double>(count) - mean * mean);
            neutral = 0.5 * std::sqrt(variance);
        }
        for (PaperTick& tick : ticks) {
            if (tick.forward_valid[horizon_index] != 0) {
                tick.return_regime[regime_index] = static_cast<std::uint8_t>(
                    detail::return_regime_three(
                        tick.forward_returns[horizon_index], neutral));
            } else {
                tick.return_regime[regime_index] = 1;
            }
        }
    }

    return ticks;
}

} // namespace hft::paper

