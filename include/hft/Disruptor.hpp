#pragma once

#include "hft/Types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace hft::disruptor {

namespace detail {

[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

[[gnu::always_inline]] inline std::int64_t now_ns() noexcept {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock::now().time_since_epoch())
        .count();
}

} // namespace detail

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------
// One sequence per cache line. get()/set() are the only operations on the
// hot path. increment() is retained for diagnostics and is deliberately
// acq_rel because it is not used to publish payload data.
class alignas(kCacheLine) Sequence {
public:
    constexpr Sequence() noexcept = default;
    explicit constexpr Sequence(std::int64_t value) noexcept : value_(value) {}

    Sequence(const Sequence&) = delete;
    Sequence& operator=(const Sequence&) = delete;

    [[nodiscard]] std::int64_t get() const noexcept {
        return value_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::int64_t relaxed() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

    void set(std::int64_t value) noexcept {
        value_.store(value, std::memory_order_release);
    }

    void set_relaxed(std::int64_t value) noexcept {
        value_.store(value, std::memory_order_relaxed);
    }

    // Monotonic high-water update. Required because a producer that batches
    // claims may publish ticket N+1 before ticket N. Never move the cursor
    // backwards: doing so lets the consumer observe overwritten slots.
    void set_max(std::int64_t value) noexcept {
        std::int64_t current = value_.load(std::memory_order_relaxed);
        while (current < value &&
               !value_.compare_exchange_weak(current, value,
                                             std::memory_order_release,
                                             std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] std::int64_t increment(std::int64_t delta = 1) noexcept {
        return value_.fetch_add(delta, std::memory_order_acq_rel) + delta;
    }

private:
    alignas(kCacheLine) std::atomic<std::int64_t> value_{-1};
};

// ---------------------------------------------------------------------------
// Wait strategies
// ---------------------------------------------------------------------------
// Busy spin: lowest latency, burns 100% of a core. Pin the consumer.
struct BusySpinWaitStrategy {
    template <typename Predicate>
    [[gnu::always_inline]] void wait(Predicate&& predicate) const noexcept {
        while (!predicate()) {
            detail::cpu_relax();
        }
    }
};

// Hybrid: spin for SpinCount iterations (fast path), then yield the core.
// This keeps sub-microsecond wakeups while avoiding a permanently pegged CPU
// when the feed is idle. In a pinned production setup prefer BusySpin.
template <std::size_t SpinCount = 4096>
struct SpinYieldWaitStrategy {
    template <typename Predicate>
    [[gnu::always_inline]] void wait(Predicate&& predicate) const noexcept {
        for (;;) {
            for (std::size_t spin = 0; spin < SpinCount; ++spin) {
                if (predicate()) {
                    return;
                }
                detail::cpu_relax();
            }
            if (predicate()) {
                return;
            }
            std::this_thread::yield();
        }
    }
};

// ---------------------------------------------------------------------------
// Claim strategies
// ---------------------------------------------------------------------------
// Single producer: plain non-atomic counter, no locked instruction. Safe only
// when exactly one thread calls ProducerBarrier::claim(). This is the fastest
// option and is what the benchmark exercises.
struct SingleProducerClaim {
    [[nodiscard]] std::int64_t peek() const noexcept { return next_; }

    [[nodiscard]] bool try_claim(std::int64_t consumer_sequence,
                                 std::size_t capacity,
                                 std::int64_t& out) noexcept {
        if (next_ - consumer_sequence >= static_cast<std::int64_t>(capacity)) {
            return false;
        }
        out = next_++;
        return true;
    }

    void reset(std::int64_t sequence) noexcept { next_ = sequence; }

private:
    std::int64_t next_{0};
};

// Multi producer: one atomic fetch/CAS per event. The capacity check and the
// increment must be a single CAS, otherwise two producers can both observe a
// free slot and overrun the consumer.
class alignas(kCacheLine) MultiProducerClaim {
public:
    [[nodiscard]] std::int64_t peek() const noexcept {
        return next_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool try_claim(std::int64_t consumer_sequence,
                                 std::size_t capacity,
                                 std::int64_t& out) noexcept {
        std::int64_t current = next_.load(std::memory_order_relaxed);
        for (;;) {
            if (current - consumer_sequence >= static_cast<std::int64_t>(capacity)) {
                return false;
            }
            if (next_.compare_exchange_weak(current, current + 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                out = current;
                return true;
            }
        }
    }

    void reset(std::int64_t sequence) noexcept {
        next_.store(sequence, std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> next_{0};
};

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------
template <typename Entry, std::size_t Capacity>
class alignas(kCacheLine) RingBuffer {
    static_assert(Capacity >= 2);
    static_assert((Capacity & (Capacity - 1)) == 0);
    static_assert(std::is_default_constructible_v<Entry>);
    // Each entry must occupy a whole destructive-interference line. With
    // kCacheLine == 128 on Apple silicon this prevents the producer writing
    // slot N from invalidating the consumer's line for slot N-1.
    static_assert(sizeof(Entry) % kCacheLine == 0,
                  "Entry must be padded to kCacheLine; see MarketEvent");
    static_assert(alignof(Entry) >= kCacheLine);

public:
    using value_type = Entry;

    RingBuffer()
        : entries_(std::make_unique<Entry[]>(Capacity)),
          published_(std::make_unique<std::atomic<std::int64_t>[]>(Capacity)) {
        for (std::size_t index = 0; index < Capacity; ++index) {
            published_[index].store(std::numeric_limits<std::int64_t>::min(),
                                    std::memory_order_relaxed);
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    [[nodiscard]] static constexpr std::size_t mask() noexcept {
        return Capacity - 1;
    }

    [[nodiscard]] Entry* data() noexcept { return entries_.get(); }
    [[nodiscard]] const Entry* data() const noexcept { return entries_.get(); }

    [[nodiscard]] Entry& at(std::int64_t sequence) noexcept {
        return entries_[static_cast<std::size_t>(sequence) & mask()];
    }

    [[nodiscard]] const Entry& at(std::int64_t sequence) const noexcept {
        return entries_[static_cast<std::size_t>(sequence) & mask()];
    }

    [[nodiscard]] Entry& operator[](std::int64_t sequence) noexcept {
        return at(sequence);
    }

    [[nodiscard]] const Entry& operator[](std::int64_t sequence) const noexcept {
        return at(sequence);
    }

    // The slot flag stores the full sequence, not a boolean. A boolean is
    // vulnerable to ABA after wrap-around: a consumer could mistake the flag
    // left behind by sequence N-Capacity for the new sequence N.
    [[nodiscard]] bool is_published(std::int64_t sequence) const noexcept {
        return published_[static_cast<std::size_t>(sequence) & mask()].load(
                   std::memory_order_acquire) == sequence;
    }

    void publish(std::int64_t sequence) noexcept {
        published_[static_cast<std::size_t>(sequence) & mask()].store(
            sequence, std::memory_order_release);
        // High-water mark for diagnostics only. The consumer never relies on
        // it for visibility; it waits on the per-slot sequence above.
        cursor_.set_max(sequence);
    }

    // Single-producer fast path: tickets are published in claim order, so the
    // high-water mark is already monotonic and no CAS is required.
    void publish_single(std::int64_t sequence) noexcept {
        published_[static_cast<std::size_t>(sequence) & mask()].store(
            sequence, std::memory_order_release);
        cursor_.set(sequence);
    }

    [[nodiscard]] Sequence& cursor() noexcept { return cursor_; }
    [[nodiscard]] const Sequence& cursor() const noexcept { return cursor_; }

    [[nodiscard]] std::int64_t published_sequence() const noexcept {
        return cursor_.get();
    }

    void warm_cache() noexcept {
        volatile const std::uint8_t* base =
            reinterpret_cast<volatile const std::uint8_t*>(entries_.get());
        for (std::size_t index = 0; index < Capacity; ++index) {
            const std::size_t offset = index * sizeof(Entry);
            __builtin_prefetch(entries_.get() + index, 1, 3);
            (void)base[offset];
        }
        __builtin_prefetch(std::addressof(cursor_), 1, 3);
    }

    [[gnu::always_inline]] void warm() noexcept { warm_cache(); }

private:
    alignas(kCacheLine) std::unique_ptr<Entry[]> entries_;
    alignas(kCacheLine) std::unique_ptr<std::atomic<std::int64_t>[]> published_;
    alignas(kCacheLine) Sequence cursor_{};
};

// ---------------------------------------------------------------------------
// Consumer barrier
// ---------------------------------------------------------------------------
template <typename Entry, std::size_t Capacity,
          typename WaitStrategy = SpinYieldWaitStrategy<>>
class alignas(kCacheLine) ConsumerBarrier {
public:
    using RingBufferType = RingBuffer<Entry, Capacity>;

    explicit ConsumerBarrier(RingBufferType& ring) noexcept : ring_(ring) {}

    ConsumerBarrier(const ConsumerBarrier&) = delete;
    ConsumerBarrier& operator=(const ConsumerBarrier&) = delete;

    [[nodiscard]] std::int64_t sequence() const noexcept {
        return sequence_.get();
    }

    [[nodiscard]] std::int64_t last_processed() const noexcept {
        return sequence_.relaxed();
    }

    [[nodiscard]] bool is_available(std::int64_t sequence) const noexcept {
        return ring_.is_published(sequence);
    }

    [[gnu::always_inline]] std::int64_t wait_for(std::int64_t sequence) noexcept {
        if (!ring_.is_published(sequence)) {
            wait_strategy_.wait(
                [this, sequence]() noexcept { return ring_.is_published(sequence); });
        }
        return sequence;
    }

    [[nodiscard]] const Entry& get(std::int64_t sequence) const noexcept {
        return ring_.at(sequence);
    }

    [[nodiscard]] Entry& get(std::int64_t sequence) noexcept {
        return ring_.at(sequence);
    }

    void advance_to(std::int64_t sequence) noexcept {
        // Release: the producer's capacity check acquires this value, ensuring
        // the consumer has finished reading before the slot is reused.
        sequence_.set(sequence);
    }

    [[nodiscard]] RingBufferType& ring() noexcept { return ring_; }

private:
    RingBufferType& ring_;
    alignas(kCacheLine) Sequence sequence_{-1};
    [[no_unique_address]] WaitStrategy wait_strategy_{};
};

// ---------------------------------------------------------------------------
// Producer barrier
// ---------------------------------------------------------------------------
template <typename Entry, std::size_t Capacity,
          typename WaitStrategy = SpinYieldWaitStrategy<>,
          typename ClaimStrategy = SingleProducerClaim>
class alignas(kCacheLine) ProducerBarrier {
public:
    using RingBufferType = RingBuffer<Entry, Capacity>;
    using ConsumerBarrierType = ConsumerBarrier<Entry, Capacity, WaitStrategy>;

    struct Ticket {
        std::int64_t sequence;
        Entry* entry;
    };

    ProducerBarrier(RingBufferType& ring, ConsumerBarrierType& consumer) noexcept
        : ring_(ring), consumer_(consumer) {}

    ProducerBarrier(const ProducerBarrier&) = delete;
    ProducerBarrier& operator=(const ProducerBarrier&) = delete;

    [[gnu::always_inline]] Ticket claim() noexcept {
        std::int64_t sequence = 0;
        while (!claim_strategy_.try_claim(consumer_.sequence(), Capacity, sequence)) {
            wait_strategy_.wait([this]() noexcept {
                return static_cast<std::int64_t>(Capacity) >
                       claim_strategy_.peek() - consumer_.sequence();
            });
        }
        return Ticket{sequence, std::addressof(ring_.at(sequence))};
    }

    [[gnu::always_inline]] void publish(Ticket ticket) noexcept {
        if constexpr (std::is_same_v<ClaimStrategy, SingleProducerClaim>) {
            ring_.publish_single(ticket.sequence);
        } else {
            ring_.publish(ticket.sequence);
        }
    }

    [[nodiscard]] std::int64_t next_sequence() const noexcept {
        return claim_strategy_.peek();
    }

private:
    RingBufferType& ring_;
    ConsumerBarrierType& consumer_;
    alignas(kCacheLine) ClaimStrategy claim_strategy_{};
    [[no_unique_address]] WaitStrategy wait_strategy_{};
};

// Convenience alias for the multi-producer case. The consumer is still
// single-threaded here; a multi-consumer gating sequence is intentionally out
// of scope for this SDK because the signal path is a single consumer.
template <typename Entry, std::size_t Capacity,
          typename WaitStrategy = SpinYieldWaitStrategy<>>
using MultiProducerBarrier =
    ProducerBarrier<Entry, Capacity, WaitStrategy, MultiProducerClaim>;

} // namespace hft::disruptor
