#pragma once

#include "hft/Export.hpp"
#include "hft/Types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hft {

struct MarketDataMessage {
    std::uint64_t timestamp{0};
    std::uint32_t symbol_id{0};
    std::uint32_t price{0};
    std::uint32_t quantity{0};
    char side{'\0'};
};

// Bounded single-producer/single-consumer queue.
//
// One slot is intentionally left empty to distinguish full from empty, so the
// usable capacity is Capacity - 1. Capacity must be a power of two.
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity >= 2, "capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "capacity must be a power of two");
    static_assert(std::is_copy_assignable_v<T>,
                  "T must be copy-assignable");

public:
    SPSCQueue() = default;
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    [[nodiscard]] bool push(const T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t current_head =
            head_.load(std::memory_order_relaxed);
        const std::size_t next_head = (current_head + 1) & (Capacity - 1);

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& item) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t current_tail =
            tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[current_tail];
        tail_.store((current_tail + 1) & (Capacity - 1),
                    std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t current_head =
            head_.load(std::memory_order_acquire);
        const std::size_t current_tail =
            tail_.load(std::memory_order_acquire);

        return current_head >= current_tail
            ? current_head - current_tail
            : Capacity + current_head - current_tail;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) T buffer_[Capacity]{};
};

extern HFT_SDK_API SPSCQueue<MarketDataMessage, 65536> g_market_data_queue;

} // namespace hft
