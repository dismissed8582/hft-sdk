#pragma once

#include "hft/Types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace hft::obi {

struct DirectionalCounts {
    std::uint64_t bid{0};
    std::uint64_t ask{0};
    std::uint64_t buy{0};
    std::uint64_t sell{0};

    [[nodiscard]] constexpr std::uint64_t lob_total() const noexcept {
        return bid > std::numeric_limits<std::uint64_t>::max() - ask
            ? std::numeric_limits<std::uint64_t>::max() : bid + ask;
    }

    [[nodiscard]] constexpr std::uint64_t trade_total() const noexcept {
        return buy > std::numeric_limits<std::uint64_t>::max() - sell
            ? std::numeric_limits<std::uint64_t>::max() : buy + sell;
    }
};

struct WeightedDirectionalCounts {
    double bid{0.0};
    double ask{0.0};
    double buy{0.0};
    double sell{0.0};

    [[nodiscard]] constexpr double lob_total() const noexcept { return bid + ask; }
    [[nodiscard]] constexpr double trade_total() const noexcept { return buy + sell; }
};

struct OBIValue {
    double value{0.0};
    bool valid{false};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid;
    }
};

[[nodiscard, gnu::always_inline]] constexpr OBIValue order_book_imbalance(DirectionalCounts counts) noexcept {
    const double denominator = static_cast<double>(counts.bid) + static_cast<double>(counts.ask);
    const double sign = static_cast<double>(counts.ask) - static_cast<double>(counts.bid);
    const double inverse = denominator == 0 ? 0.0 : 1.0 / static_cast<double>(denominator);
    return OBIValue{sign * inverse, denominator != 0};
}

[[nodiscard, gnu::always_inline]] constexpr OBIValue trade_order_book_imbalance(DirectionalCounts counts) noexcept {
    const double denominator = static_cast<double>(counts.buy) + static_cast<double>(counts.sell);
    const double sign = static_cast<double>(counts.buy) - static_cast<double>(counts.sell);
    const double inverse = denominator == 0 ? 0.0 : 1.0 / static_cast<double>(denominator);
    return OBIValue{sign * inverse, denominator != 0};
}

[[nodiscard, gnu::always_inline]] constexpr OBIValue order_book_imbalance(
    WeightedDirectionalCounts counts) noexcept {
    const double denominator = counts.lob_total();
    return OBIValue{
        denominator > 0.0 ? (counts.ask - counts.bid) / denominator : 0.0,
        denominator > 0.0};
}

[[nodiscard, gnu::always_inline]] constexpr OBIValue trade_order_book_imbalance(
    WeightedDirectionalCounts counts) noexcept {
    const double denominator = counts.trade_total();
    return OBIValue{
        denominator > 0.0 ? (counts.buy - counts.sell) / denominator : 0.0,
        denominator > 0.0};
}

[[nodiscard]] constexpr std::uint64_t delta_n(DirectionalCounts counts, Side side,
                                                bool trade_event) noexcept {
    if (trade_event) {
        return side == Side::Buy ? counts.buy : counts.sell;
    }
    return side == Side::Bid ? counts.bid : counts.ask;
}

struct FilterThresholds {
    std::int64_t lifetime_ns{0};
    std::uint32_t max_modifications{kNoModificationLimit};
    std::int64_t min_modification_interval_ns{0};
};

struct FilterState {
    std::int64_t lifetime_ns{0};
    std::uint32_t modifications{0};
    std::int64_t last_modification_interval_ns{std::numeric_limits<std::int64_t>::max()};
};

enum class FilterScheme : std::uint8_t {
    Unfiltered = 0,
    Lifetime = 1,
    ModificationCount = 2,
    ModificationTime = 3,
    Count = 4,
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(FilterScheme::Count)>
    kFilterSchemeNames{"UF", "LF", "MF", "MTF"};

[[nodiscard]] constexpr std::uint8_t scheme_bit(FilterScheme scheme) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(scheme));
}

[[nodiscard]] constexpr bool passes_lifetime_filter(FilterState state,
                                                    FilterThresholds thresholds) noexcept {
    return state.lifetime_ns >= thresholds.lifetime_ns;
}

[[nodiscard]] constexpr bool passes_modification_count_filter(
    FilterState state, FilterThresholds thresholds) noexcept {
    return state.modifications <= thresholds.max_modifications;
}

[[nodiscard]] constexpr bool passes_modification_time_filter(
    FilterState state, FilterThresholds thresholds) noexcept {
    return state.last_modification_interval_ns >= thresholds.min_modification_interval_ns;
}

[[nodiscard, gnu::always_inline]] constexpr std::uint8_t filter_mask(FilterState state,
                                                 FilterThresholds thresholds) noexcept {
    std::uint8_t mask = scheme_bit(FilterScheme::Unfiltered);
    if (passes_lifetime_filter(state, thresholds)) {
        mask |= scheme_bit(FilterScheme::Lifetime);
    }
    if (passes_modification_count_filter(state, thresholds)) {
        mask |= scheme_bit(FilterScheme::ModificationCount);
    }
    if (passes_modification_time_filter(state, thresholds)) {
        mask |= scheme_bit(FilterScheme::ModificationTime);
    }
    return mask;
}

template <std::size_t Capacity = (std::size_t{1} << 19)>
class alignas(kCacheLine) OrderTracker {
    static_assert(Capacity >= 64);
    static_assert((Capacity & (Capacity - 1)) == 0);

public:
    static constexpr std::size_t kCapacity = Capacity;

    struct alignas(32) Record {
        std::uint64_t order_id{0};
        std::int64_t first_seen_ns{0};
        std::int64_t last_modification_ns{0};
        std::int64_t last_modification_interval_ns{std::numeric_limits<std::int64_t>::max()};
        std::uint32_t modifications{0};
        std::int64_t remaining_quantity{0};
        Side side{Side::None};
        std::uint8_t state{0};
    };

    struct Observation {
        FilterState filters;
        bool terminal{false};
        bool found{false};
    };

    [[nodiscard]] Observation observe(const MarketEvent& event) noexcept {
        if (event.type == EventType::Trade && event.parent_order_id == 0) {
            return Observation{FilterState{std::numeric_limits<std::int64_t>::max(), 0,
                std::numeric_limits<std::int64_t>::max()}, false, false};
        }
        const std::uint64_t lookup_id =
            event.type == EventType::Trade && event.parent_order_id != 0
                ? event.parent_order_id
                : event.order_id;
        Record* record = locate(lookup_id);
        if (record == nullptr) {
            // A trade is an execution, not a new resting order. A cancel for an
            // order we never saw (or already evicted) must not allocate a
            // record either: that is how active_ used to underflow.
            if (event.type == EventType::Trade) {
                // A trade does not create a resting order. Neutral filter
                // values keep it in every trade-OBI regime, matching the
                // unfiltered execution flow.
                return Observation{FilterState{std::numeric_limits<std::int64_t>::max(),
                                               0,
                                               std::numeric_limits<std::int64_t>::max()},
                                   false, false};
            }
            if (event.type == EventType::Cancel) {
                return Observation{FilterState{}, false, false};
            }
            record = insert(event.order_id, event.timestamp_ns);
            if (record == nullptr) {
                ++dropped_;
                return Observation{};
            }
        }

        if (event.type == EventType::New) {
            record->first_seen_ns = event.timestamp_ns;
            record->last_modification_ns = std::numeric_limits<std::int64_t>::min();
            record->last_modification_interval_ns = std::numeric_limits<std::int64_t>::max();
            record->modifications = 0;
            record->remaining_quantity = event.quantity > 0 ? event.quantity : 0;
        } else if (event.type == EventType::Modify) {
            if (record->last_modification_ns != std::numeric_limits<std::int64_t>::min()) {
                record->last_modification_interval_ns =
                    event.timestamp_ns - record->last_modification_ns;
            }
            record->last_modification_ns = event.timestamp_ns;
            if (record->modifications != std::numeric_limits<std::uint32_t>::max()) {
                ++record->modifications;
            }
            // Modify quantity is the absolute remaining size after the change.
            record->remaining_quantity = event.quantity > 0 ? event.quantity : 0;
        }

        if ((event.side == Side::Bid || event.side == Side::Ask) &&
            event.type != EventType::Trade) {
            record->side = event.side;
        }

        const Observation observation{
            FilterState{event.timestamp_ns - record->first_seen_ns, record->modifications,
                        record->last_modification_interval_ns},
            event.type == EventType::Cancel, true};

        if (event.type == EventType::Cancel) {
            if (record->state == 1) {
                record->state = 2;
                if (active_ != 0) {
                    --active_;
                }
            }
        } else if (event.type == EventType::Trade && event.quantity > 0 &&
                   record->remaining_quantity > 0) {
            record->remaining_quantity -= event.quantity;
            if (record->remaining_quantity <= 0) {
                record->state = 2;
                if (active_ != 0) {
                    --active_;
                }
            }
        }
        return observation;
    }

    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_;
    }

    [[nodiscard]] std::size_t active() const noexcept {
        return active_;
    }

private:
    [[nodiscard]] static std::uint64_t hash(std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    [[nodiscard]] Record* locate(std::uint64_t order_id) noexcept {
        const std::size_t start = static_cast<std::size_t>(hash(order_id)) & (kCapacity - 1);
        for (std::size_t probe = 0; probe < kCapacity; ++probe) {
            const std::size_t index = (start + probe) & (kCapacity - 1);
            Record& record = records_[index];
            if (record.state == 0) {
                return nullptr;
            }
            if (record.state == 1 && record.order_id == order_id) {
                return std::addressof(record);
            }
        }
        return nullptr;
    }

    Record* insert(std::uint64_t order_id, std::int64_t timestamp_ns) noexcept {
        const std::size_t start = static_cast<std::size_t>(hash(order_id)) & (kCapacity - 1);
        for (std::size_t probe = 0; probe < kCapacity; ++probe) {
            const std::size_t index = (start + probe) & (kCapacity - 1);
            Record& record = records_[index];
            if (record.state != 1) {
                record.order_id = order_id;
                record.first_seen_ns = timestamp_ns;
                record.last_modification_ns = std::numeric_limits<std::int64_t>::min();
                record.last_modification_interval_ns =
                    std::numeric_limits<std::int64_t>::max();
                record.modifications = 0;
                record.remaining_quantity = 0;
                record.side = Side::None;
                record.state = 1;
                ++active_;
                return std::addressof(record);
            }
        }
        return nullptr;
    }

    alignas(kCacheLine) std::array<Record, kCapacity> records_{};
    std::size_t active_{0};
    std::size_t dropped_{0};
};

template <std::size_t Capacity>
class RollingCounts {
    static_assert(Capacity >= 64);
    static_assert((Capacity & (Capacity - 1)) == 0);

public:
    explicit RollingCounts(std::int64_t horizon_ns) noexcept : horizon_ns_(horizon_ns) {}

    void clear() noexcept {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
        counts_.fill(DirectionalCounts{});
    }

    [[gnu::always_inline]] void advance(std::int64_t now_ns) noexcept {
        const std::int64_t boundary = now_ns - horizon_ns_;
        while (size_ != 0 && samples_[head_].timestamp_ns <= boundary) {
            pop_front();
        }
    }

    [[gnu::always_inline]] void push(std::int64_t timestamp_ns, Side side, bool trade_event,
                                     std::uint8_t pass_mask) noexcept {
        advance(timestamp_ns);
        if (size_ == Capacity) {
            pop_front();
            ++dropped_;
        }
        samples_[tail_] = Sample{timestamp_ns, side, static_cast<std::uint8_t>(trade_event),
                                 pass_mask, {}};
        tail_ = (tail_ + 1) & (Capacity - 1);
        ++size_;

        if ((trade_event && (side == Side::Buy || side == Side::Sell)) ||
            (!trade_event && (side == Side::Bid || side == Side::Ask))) {
            add_masked<0>(side, trade_event, pass_mask);
            add_masked<1>(side, trade_event, pass_mask);
            add_masked<2>(side, trade_event, pass_mask);
            add_masked<3>(side, trade_event, pass_mask);
        }
    }

    [[nodiscard]] std::uint32_t count(FilterScheme scheme, Side side,
                                      bool trade_event) const noexcept {
        return delta_n(counts_[static_cast<std::size_t>(scheme)], side, trade_event);
    }

    [[nodiscard]] const DirectionalCounts& counts(FilterScheme scheme) const noexcept {
        return counts_[static_cast<std::size_t>(scheme)];
    }

    [[nodiscard]] std::int64_t horizon_ns() const noexcept {
        return horizon_ns_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_;
    }

private:
    struct alignas(16) Sample {
        std::int64_t timestamp_ns;
        Side side;
        std::uint8_t trade_event;
        std::uint8_t pass_mask;
        std::uint8_t padding[5];
    };
    static_assert(sizeof(Sample) == 16);

    [[gnu::always_inline]] static void add(DirectionalCounts& counts, Side side,
                                           bool trade_event, std::uint32_t quantity) noexcept {
        if (trade_event) {
            if (side == Side::Buy) {
                counts.buy += quantity;
            } else if (side == Side::Sell) {
                counts.sell += quantity;
            }
        } else if (side == Side::Bid) {
            counts.bid += quantity;
        } else if (side == Side::Ask) {
            counts.ask += quantity;
        }
    }

    template <std::size_t Scheme>
    [[gnu::always_inline]] void add_masked(Side side, bool trade_event,
                                           std::uint8_t pass_mask) noexcept {
        if ((pass_mask & (1u << Scheme)) != 0) {
            add(counts_[Scheme], side, trade_event, 1u);
        }
    }

    template <std::size_t Scheme>
    [[gnu::always_inline]] void remove_masked(Side side, bool trade_event,
                                              std::uint8_t pass_mask) noexcept {
        if ((pass_mask & (1u << Scheme)) == 0) {
            return;
        }
        if (trade_event) {
            if (side == Side::Buy) {
                --counts_[Scheme].buy;
            } else {
                --counts_[Scheme].sell;
            }
        } else if (side == Side::Bid) {
            --counts_[Scheme].bid;
        } else {
            --counts_[Scheme].ask;
        }
    }

    [[gnu::always_inline]] void pop_front() noexcept {
        const Sample& sample = samples_[head_];
        const bool trade_event = sample.trade_event != 0;
        if ((trade_event && (sample.side == Side::Buy || sample.side == Side::Sell)) ||
            (!trade_event && (sample.side == Side::Bid || sample.side == Side::Ask))) {
            remove_masked<0>(sample.side, trade_event, sample.pass_mask);
            remove_masked<1>(sample.side, trade_event, sample.pass_mask);
            remove_masked<2>(sample.side, trade_event, sample.pass_mask);
            remove_masked<3>(sample.side, trade_event, sample.pass_mask);
        }
        head_ = (head_ + 1) & (Capacity - 1);
        --size_;
    }

    alignas(kCacheLine) std::array<Sample, Capacity> samples_{};
    alignas(kCacheLine) std::array<DirectionalCounts,
                                   static_cast<std::size_t>(FilterScheme::Count)>
        counts_{};
    std::int64_t horizon_ns_{0};
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
    std::size_t dropped_{0};
};

// ---------------------------------------------------------------------------
// Bucketed rolling counts
// ---------------------------------------------------------------------------
// A per-event ring is O(events in horizon). At Hyperliquid peak rates a 50 s
// window exceeds the old 2^18 sample ring and silently drops live samples.
// This implementation buckets events by time (1 ms by default): memory is a
// function of the horizon, not of the event rate, and push() is O(1) amortised.
// The only cost is sub-millisecond quantisation of the window boundary.
template <std::size_t SchemeCount = static_cast<std::size_t>(FilterScheme::Count)>
class BucketedRollingCounts {
    static_assert(SchemeCount == 4);

public:
    static constexpr std::int64_t kDefaultBucketNs = 1'000'000;

    explicit BucketedRollingCounts(std::int64_t horizon_ns,
                                   std::int64_t bucket_ns = kDefaultBucketNs)
        : horizon_ns_(horizon_ns), bucket_ns_(bucket_ns) {
        if (horizon_ns <= 0 || bucket_ns <= 0 ||
            horizon_ns / bucket_ns > std::numeric_limits<std::int64_t>::max() - 2) {
            throw std::invalid_argument("invalid rolling window configuration");
        }
        const std::int64_t needed = horizon_ns_ / bucket_ns_ + 2;
        const auto max_capacity = std::numeric_limits<std::size_t>::max() / sizeof(Bucket);
        if (static_cast<std::uint64_t>(needed) > max_capacity) {
            throw std::length_error("rolling window is too large");
        }
        std::size_t capacity = 2;
        while (capacity < static_cast<std::size_t>(needed)) {
            if (capacity > max_capacity / 2) throw std::length_error("rolling window is too large");
            capacity <<= 1;
        }
        capacity_ = capacity;
        mask_ = capacity_ - 1;
        horizon_buckets_ = horizon_ns_ / bucket_ns_ + 1;
        buckets_ = std::make_unique<Bucket[]>(capacity_);
        clear();
    }

    void clear() noexcept {
        for (std::size_t index = 0; index < capacity_; ++index) {
            buckets_[index].counts.fill(DirectionalCounts{});
            buckets_[index].weights.fill(WeightedDirectionalCounts{});
        }
        counts_.fill(DirectionalCounts{});
        weights_.fill(WeightedDirectionalCounts{});
        min_bucket_ = 0;
        max_bucket_ = -1;
        dropped_ = 0;
    }

    [[gnu::always_inline]] void advance(std::int64_t now_ns) noexcept {
        const std::int64_t now_bucket = floor_div(now_ns, bucket_ns_);
        evict_before(now_bucket - horizon_buckets_);
    }

    [[gnu::always_inline]] void push(std::int64_t timestamp_ns, Side side,
                                     bool trade_event,
                                     std::uint8_t pass_mask,
                                     double quantity = 0.0) noexcept {
        const std::int64_t bucket_index = floor_div(timestamp_ns, bucket_ns_);
        advance(timestamp_ns);

        if (bucket_index < min_bucket_) {
            // Out-of-order event older than the horizon. It cannot be
            // represented without rewinding the aggregate.
            ++dropped_;
            return;
        }

        if (max_bucket_ < min_bucket_) {
            // The aggregate was fully drained by advance(). Do not loop over
            // every empty bucket between the old horizon and now.
            counts_.fill(DirectionalCounts{});
            weights_.fill(WeightedDirectionalCounts{});
            min_bucket_ = bucket_index;
            max_bucket_ = bucket_index;
        } else if (bucket_index > max_bucket_) {
            const std::int64_t gap = bucket_index - max_bucket_;
            if (gap >= static_cast<std::int64_t>(capacity_)) {
                // A multi-day timestamp jump must not loop over millions of
                // empty buckets (that is a latency spike). Reset instead.
                reset_to(bucket_index);
            } else {
                for (std::int64_t index = max_bucket_ + 1; index <= bucket_index;
                     ++index) {
                    Bucket& bucket = buckets_[static_cast<std::size_t>(index) & mask_];
                    subtract(counts_, bucket.counts);
                    subtract(weights_, bucket.weights);
                    bucket.counts.fill(DirectionalCounts{});
                    bucket.weights.fill(WeightedDirectionalCounts{});
                }
                max_bucket_ = bucket_index;
            }
        }

        evict_before(bucket_index - horizon_buckets_);

        Bucket& bucket = buckets_[static_cast<std::size_t>(bucket_index) & mask_];
        add_masked(bucket, side, trade_event, pass_mask);
        add_weighted_masked(bucket, side, trade_event, pass_mask, quantity);
        add_masked_global(side, trade_event, pass_mask);
        add_weighted_global(side, trade_event, pass_mask, quantity);
    }

    [[nodiscard]] std::uint32_t count(FilterScheme scheme, Side side,
                                      bool trade_event) const noexcept {
        return delta_n(counts_[static_cast<std::size_t>(scheme)], side, trade_event);
    }

    [[nodiscard]] const DirectionalCounts& counts(FilterScheme scheme) const noexcept {
        return counts_[static_cast<std::size_t>(scheme)];
    }

    [[nodiscard]] const WeightedDirectionalCounts& weights(
        FilterScheme scheme) const noexcept {
        return weights_[static_cast<std::size_t>(scheme)];
    }

    [[nodiscard]] std::int64_t horizon_ns() const noexcept { return horizon_ns_; }
    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return capacity_; }

private:
    struct alignas(64) Bucket {
        std::array<DirectionalCounts, SchemeCount> counts{};
        std::array<WeightedDirectionalCounts, SchemeCount> weights{};
    };

    static_assert(sizeof(Bucket) == SchemeCount *
        (sizeof(DirectionalCounts) + sizeof(WeightedDirectionalCounts)));

    [[nodiscard]] static std::int64_t floor_div(std::int64_t value,
                                                std::int64_t divisor) noexcept {
        const std::int64_t quotient = value / divisor;
        const std::int64_t remainder = value % divisor;
        return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1
                                                                      : quotient;
    }

    void reset_to(std::int64_t bucket_index) noexcept {
        counts_.fill(DirectionalCounts{});
        weights_.fill(WeightedDirectionalCounts{});
        for (std::size_t index = 0; index < capacity_; ++index) {
            buckets_[index].counts.fill(DirectionalCounts{});
            buckets_[index].weights.fill(WeightedDirectionalCounts{});
        }
        min_bucket_ = bucket_index;
        max_bucket_ = bucket_index;
    }

    void evict_before(std::int64_t keep_from) noexcept {
        while (min_bucket_ < keep_from && min_bucket_ <= max_bucket_) {
            Bucket& bucket = buckets_[static_cast<std::size_t>(min_bucket_) & mask_];
            subtract(counts_, bucket.counts);
            subtract(weights_, bucket.weights);
            bucket.counts.fill(DirectionalCounts{});
            bucket.weights.fill(WeightedDirectionalCounts{});
            ++min_bucket_;
        }
        if (min_bucket_ > max_bucket_) {
            if (min_bucket_ < keep_from) {
                min_bucket_ = keep_from;
            }
            max_bucket_ = min_bucket_ - 1;
        }
    }

    static void subtract(std::array<DirectionalCounts, SchemeCount>& target,
                         const std::array<DirectionalCounts, SchemeCount>& value) noexcept {
        for (std::size_t scheme = 0; scheme < SchemeCount; ++scheme) {
            target[scheme].bid -= value[scheme].bid;
            target[scheme].ask -= value[scheme].ask;
            target[scheme].buy -= value[scheme].buy;
            target[scheme].sell -= value[scheme].sell;
        }
    }

    static void subtract(std::array<WeightedDirectionalCounts, SchemeCount>& target,
                         const std::array<WeightedDirectionalCounts, SchemeCount>& value) noexcept {
        for (std::size_t scheme = 0; scheme < SchemeCount; ++scheme) {
            target[scheme].bid -= value[scheme].bid;
            target[scheme].ask -= value[scheme].ask;
            target[scheme].buy -= value[scheme].buy;
            target[scheme].sell -= value[scheme].sell;
        }
    }

    static void add_one(DirectionalCounts& counts, Side side,
                        bool trade_event) noexcept {
        if (trade_event) {
            if (side == Side::Buy) {
                ++counts.buy;
            } else if (side == Side::Sell) {
                ++counts.sell;
            }
        } else if (side == Side::Bid) {
            ++counts.bid;
        } else if (side == Side::Ask) {
            ++counts.ask;
        }
    }

    static void add_weighted_one(WeightedDirectionalCounts& counts, Side side,
                                 bool trade_event, double quantity) noexcept {
        const double weight = quantity < 0.0 ? -quantity : quantity;
        if (trade_event) {
            if (side == Side::Buy) {
                counts.buy += weight;
            } else if (side == Side::Sell) {
                counts.sell += weight;
            }
        } else if (side == Side::Bid) {
            counts.bid += weight;
        } else if (side == Side::Ask) {
            counts.ask += weight;
        }
    }

    static void add_weighted_masked(Bucket& bucket, Side side, bool trade_event,
                                    std::uint8_t pass_mask, double quantity) noexcept {
        if ((trade_event && (side == Side::Buy || side == Side::Sell)) ||
            (!trade_event && (side == Side::Bid || side == Side::Ask))) {
            for (std::size_t scheme = 0; scheme < SchemeCount; ++scheme) {
                if ((pass_mask & (1u << scheme)) != 0) {
                    add_weighted_one(bucket.weights[scheme], side, trade_event, quantity);
                }
            }
        }
    }

    void add_weighted_global(Side side, bool trade_event,
                             std::uint8_t pass_mask, double quantity) noexcept {
        if ((trade_event && (side == Side::Buy || side == Side::Sell)) ||
            (!trade_event && (side == Side::Bid || side == Side::Ask))) {
            for (std::size_t scheme = 0; scheme < SchemeCount; ++scheme) {
                if ((pass_mask & (1u << scheme)) != 0) {
                    add_weighted_one(weights_[scheme], side, trade_event, quantity);
                }
            }
        }
    }

    static void add_masked(Bucket& bucket, Side side, bool trade_event,
                           std::uint8_t pass_mask) noexcept {
        if ((trade_event && (side == Side::Buy || side == Side::Sell)) ||
            (!trade_event && (side == Side::Bid || side == Side::Ask))) {
            for (std::size_t scheme = 0; scheme < SchemeCount; ++scheme) {
                if ((pass_mask & (1u << scheme)) != 0) {
                    add_one(bucket.counts[scheme], side, trade_event);
                }
            }
        }
    }

    void add_masked_global(Side side, bool trade_event,
                           std::uint8_t pass_mask) noexcept {
        if ((trade_event && (side == Side::Buy || side == Side::Sell)) ||
            (!trade_event && (side == Side::Bid || side == Side::Ask))) {
            for (std::size_t scheme = 0; scheme < SchemeCount; ++scheme) {
                if ((pass_mask & (1u << scheme)) != 0) {
                    add_one(counts_[scheme], side, trade_event);
                }
            }
        }
    }

    std::array<DirectionalCounts, SchemeCount> counts_{};
    std::array<WeightedDirectionalCounts, SchemeCount> weights_{};
    std::unique_ptr<Bucket[]> buckets_;
    std::size_t capacity_{0};
    std::size_t mask_{0};
    std::int64_t horizon_ns_{0};
    std::int64_t bucket_ns_{0};
    std::int64_t horizon_buckets_{0};
    std::int64_t min_bucket_{0};
    std::int64_t max_bucket_{-1};
    std::size_t dropped_{0};
};

}

