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
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace hft::signal {

inline constexpr std::array<std::int64_t, 4> kHorizonNs{
    kNanosecondsPerSecond,
    10 * kNanosecondsPerSecond,
    30 * kNanosecondsPerSecond,
    50 * kNanosecondsPerSecond,
};

inline constexpr std::array<int, 4> kHorizonSeconds{1, 10, 30, 50};

struct SignalSnapshot {
    std::int64_t timestamp_ns{0};
    double mid{0.0};
    std::array<std::array<obi::OBIValue, kHorizonNs.size()>,
               static_cast<std::size_t>(obi::FilterScheme::Count)>
        lob{};
    std::array<std::array<obi::OBIValue, kHorizonNs.size()>,
               static_cast<std::size_t>(obi::FilterScheme::Count)>
        trade{};
    std::array<std::array<obi::OBIValue, kHorizonNs.size()>,
               static_cast<std::size_t>(obi::FilterScheme::Count)>
        lob_volume{};
    std::array<std::array<obi::OBIValue, kHorizonNs.size()>,
               static_cast<std::size_t>(obi::FilterScheme::Count)>
        trade_volume{};
    std::uint32_t lob_events{0};
    std::uint32_t trade_events{0};
};

template <std::size_t WindowCapacity = (std::size_t{1} << 18)>
class SignalPipeline {
public:
    static constexpr std::size_t kSchemeCount =
        static_cast<std::size_t>(obi::FilterScheme::Count);

    explicit SignalPipeline(obi::FilterThresholds thresholds,
                            std::int64_t sample_interval_ns = kNanosecondsPerMillisecond * 250,
                            std::size_t max_snapshots = std::size_t{1} << 19,
                            std::size_t max_catchup_samples = 10'000)
        : thresholds_(thresholds),
          sample_interval_ns_(sample_interval_ns > 0 ? sample_interval_ns : 1),
          max_snapshots_(max_snapshots),
          max_catchup_samples_(max_catchup_samples) {
        for (std::size_t index = 0; index < kHorizonNs.size(); ++index) {
            windows_[index] = std::make_unique<obi::BucketedRollingCounts<>>(
                kHorizonNs[index]);
        }
        snapshots_.reserve(1u << 16);
    }

    void reserve_snapshots(std::size_t count) { snapshots_.reserve(count); }

    void process(std::span<const MarketEvent> events) {
        for (const MarketEvent& event : events) {
            if (!started_) {
                started_ = true;
                next_sample_ns_ = event.timestamp_ns + sample_interval_ns_;
            }
            if (event.timestamp_ns < last_event_timestamp_ns_) {
                // Rolling aggregates cannot be rewound. Sorting upstream is
                // preferred; if a late record slips through, drop it rather
                // than corrupting every subsequent window.
                ++out_of_order_events_;
                continue;
            }
            last_event_timestamp_ns_ = event.timestamp_ns;

            std::size_t caught_up = 0;
            while (next_sample_ns_ < event.timestamp_ns) {
                if (caught_up++ >= max_catchup_samples_) {
                    // A feed gap (or a pathological timestamp) must not make
                    // this event allocate an unbounded number of snapshots in
                    // one call: that is a multi-millisecond latency spike.
                    const std::int64_t remaining = event.timestamp_ns - next_sample_ns_;
                    skipped_samples_ += static_cast<std::size_t>(
                        (remaining + sample_interval_ns_ - 1) / sample_interval_ns_);
                    next_sample_ns_ = event.timestamp_ns;
                    break;
                }
                capture(next_sample_ns_);
                next_sample_ns_ += sample_interval_ns_;
            }

            apply(event);

            if (next_sample_ns_ == event.timestamp_ns) {
                capture(next_sample_ns_);
                next_sample_ns_ += sample_interval_ns_;
            }
        }
    }

    [[nodiscard]] const std::vector<SignalSnapshot>& snapshots() const noexcept {
        return snapshots_;
    }

    [[nodiscard]] std::size_t dropped_events() const noexcept {
        std::size_t dropped = 0;
        for (const auto& window : windows_) {
            dropped += window->dropped();
        }
        return dropped;
    }

    [[nodiscard]] std::size_t tracker_dropped() const noexcept {
        return tracker_->dropped();
    }

    [[nodiscard]] std::size_t out_of_order_events() const noexcept {
        return out_of_order_events_;
    }

    [[nodiscard]] std::size_t dropped_snapshots() const noexcept {
        return dropped_snapshots_;
    }

    [[nodiscard]] std::size_t skipped_samples() const noexcept {
        return skipped_samples_;
    }

private:
    [[gnu::always_inline]] void apply(const MarketEvent& event) noexcept {
        const obi::OrderTracker<>::Observation observation = tracker_->observe(event);
        if (event.type == EventType::Cancel && !observation.found) {
            return; // cannot affect a book level we never observed
        }
        const std::uint8_t mask = obi::filter_mask(observation.filters, thresholds_);
        const bool trade_event = is_trade(event.type);

        const double quantity = static_cast<double>(event.quantity);
        for (auto& window : windows_) {
            window->push(event.timestamp_ns, event.side, trade_event, mask, quantity);
        }

        if (valid_quote(event)) {
            last_bid_ticks_ = event.bid_price_ticks;
            last_ask_ticks_ = event.ask_price_ticks;
            have_quote_ = true;
        }
        if (trade_event) {
            ++interval_trade_events_;
        } else {
            ++interval_lob_events_;
        }
    }

    void capture(std::int64_t timestamp_ns) {
        if (max_snapshots_ != 0 && snapshots_.size() >= max_snapshots_) {
            ++dropped_snapshots_;
            return;
        }
        SignalSnapshot snapshot{};
        snapshot.timestamp_ns = timestamp_ns;
        snapshot.mid = have_quote_ ? midpoint_ticks(last_bid_ticks_, last_ask_ticks_) : 0.0;
        snapshot.lob_events = interval_lob_events_;
        snapshot.trade_events = interval_trade_events_;

        for (std::size_t horizon = 0; horizon < kHorizonNs.size(); ++horizon) {
            windows_[horizon]->advance(timestamp_ns);
            for (std::size_t scheme = 0; scheme < kSchemeCount; ++scheme) {
                const auto filter_scheme = static_cast<obi::FilterScheme>(scheme);
                const obi::DirectionalCounts& counts = windows_[horizon]->counts(filter_scheme);
                const obi::WeightedDirectionalCounts& weights =
                    windows_[horizon]->weights(filter_scheme);
                snapshot.lob[scheme][horizon] = obi::order_book_imbalance(counts);
                snapshot.trade[scheme][horizon] = obi::trade_order_book_imbalance(counts);
                snapshot.lob_volume[scheme][horizon] =
                    obi::order_book_imbalance(weights);
                snapshot.trade_volume[scheme][horizon] =
                    obi::trade_order_book_imbalance(weights);
            }
        }
        snapshots_.push_back(snapshot);
        interval_lob_events_ = 0;
        interval_trade_events_ = 0;
    }

    obi::FilterThresholds thresholds_{};
    std::int64_t sample_interval_ns_{0};
    std::size_t max_snapshots_{0};
    std::size_t max_catchup_samples_{0};
    std::array<std::unique_ptr<obi::BucketedRollingCounts<>>, kHorizonNs.size()> windows_{};
    std::unique_ptr<obi::OrderTracker<>> tracker_ =
        std::make_unique<obi::OrderTracker<>>();
    std::vector<SignalSnapshot> snapshots_{};
    std::int64_t next_sample_ns_{0};
    std::int64_t last_event_timestamp_ns_{0};
    std::int64_t last_bid_ticks_{0};
    std::int64_t last_ask_ticks_{0};
    std::uint32_t interval_lob_events_{0};
    std::uint32_t interval_trade_events_{0};
    std::size_t out_of_order_events_{0};
    std::size_t dropped_snapshots_{0};
    std::size_t skipped_samples_{0};
    bool started_{false};
    bool have_quote_{false};
};

struct SignalMetrics {
    std::string_view scheme{};
    int horizon_seconds{0};
    std::size_t samples{0};
    std::size_t trade_samples{0};
    std::size_t volume_samples{0};
    std::size_t volume_trade_samples{0};
    scoring::CorrelationScore pearson{};
    scoring::CorrelationScore regime{};
    scoring::CorrelationScore trade_pearson{};
    scoring::CorrelationScore trade_regime{};
    scoring::CorrelationScore volume_pearson{};
    scoring::CorrelationScore volume_regime{};
    scoring::CorrelationScore volume_trade_pearson{};
    scoring::CorrelationScore volume_trade_regime{};
};

// Layer 3 hand-off: one time-synchronized observation per snapshot. Python
// owns the multivariate Hawkes fit; C++ only emits the discretized states.
struct RegimeTuple {
    std::int64_t timestamp_ns{0};
    std::uint8_t obi_regime{0};
    std::uint8_t return_regime{0};
};

namespace detail {

inline std::size_t forward_index(const std::vector<SignalSnapshot>& snapshots, std::size_t index,
                                 std::int64_t horizon_ns) noexcept {
    const std::int64_t target = snapshots[index].timestamp_ns + horizon_ns;
    const auto begin = snapshots.begin() + static_cast<std::ptrdiff_t>(index);
    const auto it = std::lower_bound(
        begin, snapshots.end(), target,
        [](const SignalSnapshot& snapshot, std::int64_t value) {
            return snapshot.timestamp_ns < value;
        });
    if (it == snapshots.end()) {
        return snapshots.size();
    }
    return static_cast<std::size_t>(std::distance(snapshots.begin(), it));
}

inline double normalized_return(double value, double scale) noexcept {
    if (scale <= 0.0) {
        return 0.0;
    }
    return std::tanh(value / scale);
}

}

inline std::vector<SignalMetrics> calculate_metrics(
    const std::vector<SignalSnapshot>& snapshots) {
    std::vector<SignalMetrics> rows;
    const std::size_t count = snapshots.size();
    rows.reserve(static_cast<std::size_t>(obi::FilterScheme::Count) * kHorizonNs.size());
    if (count < 2) {
        return rows;
    }

    std::vector<double> forward_returns(count, 0.0);
    std::vector<std::uint8_t> have_return(count, 0);
    std::vector<double> lob;
    std::vector<double> trade;
    std::vector<double> volume_lob;
    std::vector<double> volume_trade;
    std::vector<double> returns;
    std::vector<double> trade_returns;
    std::vector<double> volume_returns;
    std::vector<double> volume_trade_returns;
    lob.reserve(count);
    trade.reserve(count);
    volume_lob.reserve(count);
    volume_trade.reserve(count);
    returns.reserve(count);
    trade_returns.reserve(count);
    volume_returns.reserve(count);
    volume_trade_returns.reserve(count);

    for (std::size_t horizon = 0; horizon < kHorizonNs.size(); ++horizon) {
        // Timestamps are sorted by the caller. One monotonic cursor computes
        // all forward indices in O(n) instead of lower_bound() per row.
        std::size_t cursor = 0;
        for (std::size_t index = 0; index < count; ++index) {
            if (cursor < index) {
                cursor = index;
            }
            const std::int64_t target =
                snapshots[index].timestamp_ns + kHorizonNs[horizon];
            while (cursor < count && snapshots[cursor].timestamp_ns < target) {
                ++cursor;
            }
            const double current_mid = snapshots[index].mid;
            if (cursor < count && current_mid > 0.0 && snapshots[cursor].mid > 0.0) {
                forward_returns[index] =
                    std::log(snapshots[cursor].mid / current_mid);
                have_return[index] = 1;
            } else {
                have_return[index] = 0;
            }
        }

        for (std::size_t scheme = 0;
             scheme < static_cast<std::size_t>(obi::FilterScheme::Count); ++scheme) {
            const auto score = [](std::span<const double> values,
                                  std::span<const double> targets,
                                  scoring::CorrelationScore& pearson_out,
                                  scoring::CorrelationScore& regime_out) {
                std::vector<double> normalized;
                normalized.reserve(targets.size());
                double mean = 0.0;
                for (const double value : targets) {
                    mean += value;
                }
                if (!targets.empty()) {
                    mean /= static_cast<double>(targets.size());
                }
                double variance = 0.0;
                for (const double value : targets) {
                    const double delta = value - mean;
                    variance += delta * delta;
                }
                if (!targets.empty()) {
                    variance /= static_cast<double>(targets.size());
                }
                const double scale = 4.0 * std::sqrt(variance);
                for (const double value : targets) {
                    normalized.push_back(detail::normalized_return(value, scale));
                }
                pearson_out = scoring::correlation_score(values, targets);
                regime_out = scoring::regime_cross_correlation(values, normalized);
            };

            lob.clear();
            trade.clear();
            volume_lob.clear();
            volume_trade.clear();
            returns.clear();
            trade_returns.clear();
            volume_returns.clear();
            volume_trade_returns.clear();
            for (std::size_t index = 0; index < count; ++index) {
                if (have_return[index] == 0) {
                    continue;
                }
                const double forward = forward_returns[index];
                const obi::OBIValue lob_value = snapshots[index].lob[scheme][horizon];
                if (lob_value.valid) {
                    lob.push_back(lob_value.value);
                    returns.push_back(forward);
                }
                const obi::OBIValue trade_value =
                    snapshots[index].trade[scheme][horizon];
                if (trade_value.valid) {
                    trade.push_back(trade_value.value);
                    trade_returns.push_back(forward);
                }
                const obi::OBIValue volume_lob_value =
                    snapshots[index].lob_volume[scheme][horizon];
                if (volume_lob_value.valid) {
                    volume_lob.push_back(volume_lob_value.value);
                    volume_returns.push_back(forward);
                }
                const obi::OBIValue volume_trade_value =
                    snapshots[index].trade_volume[scheme][horizon];
                if (volume_trade_value.valid) {
                    volume_trade.push_back(volume_trade_value.value);
                    volume_trade_returns.push_back(forward);
                }
            }

            SignalMetrics metrics{};
            metrics.scheme = obi::kFilterSchemeNames[scheme];
            metrics.horizon_seconds = kHorizonSeconds[horizon];
            metrics.samples = lob.size();
            metrics.trade_samples = trade.size();
            metrics.volume_samples = volume_lob.size();
            metrics.volume_trade_samples = volume_trade.size();
            score(lob, returns, metrics.pearson, metrics.regime);
            score(trade, trade_returns, metrics.trade_pearson, metrics.trade_regime);
            score(volume_lob, volume_returns, metrics.volume_pearson,
                  metrics.volume_regime);
            score(volume_trade, volume_trade_returns, metrics.volume_trade_pearson,
                  metrics.volume_trade_regime);
            rows.push_back(metrics);
        }
    }
    return rows;
}

// Export the discretized, time-synchronized states consumed by the Python
// Hawkes layer. Each row is (timestamp_ns, obi_regime, return_regime) where
// timestamp_ns is the prediction time and return_regime describes the forward
// log-mid return over the selected horizon.
inline std::vector<RegimeTuple> calculate_regimes(
    const std::vector<SignalSnapshot>& snapshots,
    obi::FilterScheme scheme = obi::FilterScheme::Unfiltered,
    std::size_t horizon = 0,
    bool volume = false) {
    std::vector<RegimeTuple> tuples;
    const std::size_t count = snapshots.size();
    const std::size_t scheme_index = static_cast<std::size_t>(scheme);
    if (count < 2 || horizon >= kHorizonNs.size() ||
        scheme_index >= static_cast<std::size_t>(obi::FilterScheme::Count)) {
        return tuples;
    }

    std::vector<double> forward_returns(count, 0.0);
    std::vector<std::uint8_t> have_return(count, 0);
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (cursor < index) {
            cursor = index;
        }
        const std::int64_t target =
            snapshots[index].timestamp_ns + kHorizonNs[horizon];
        while (cursor < count && snapshots[cursor].timestamp_ns < target) {
            ++cursor;
        }
        if (cursor < count && snapshots[index].mid > 0.0 &&
            snapshots[cursor].mid > 0.0) {
            forward_returns[index] =
                std::log(snapshots[cursor].mid / snapshots[index].mid);
            have_return[index] = 1;
        }
    }

    scoring::SimdWelford moments;
    for (std::size_t index = 0; index < count; ++index) {
        if (have_return[index] != 0) {
            moments.add(forward_returns[index], forward_returns[index]);
        }
    }
    const scoring::WelfordStats stats = moments.finalize();
    const double scale = 4.0 * std::sqrt(stats.variance_x(false));

    tuples.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (have_return[index] == 0) {
            continue;
        }
        const obi::OBIValue& value = volume
            ? snapshots[index].lob_volume[scheme_index][horizon]
            : snapshots[index].lob[scheme_index][horizon];
        if (!value.valid) {
            continue;
        }
        RegimeTuple tuple{};
        tuple.timestamp_ns = snapshots[index].timestamp_ns;
        tuple.obi_regime = static_cast<std::uint8_t>(
            scoring::RegimeDiscretizer::obi_regime(value.value));
        tuple.return_regime = static_cast<std::uint8_t>(
            scoring::RegimeDiscretizer::return_regime(
                detail::normalized_return(forward_returns[index], scale)));
        tuples.push_back(tuple);
    }
    return tuples;
}

}
