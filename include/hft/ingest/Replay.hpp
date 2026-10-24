#pragma once

#include "hft/Disruptor.hpp"
#include "hft/Types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <numeric>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace hft::ingest {

struct ReplayStats {
    std::uint64_t events{0};
    double mean_ns{0.0};
    double p50_ns{0.0};
    double p90_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    double max_ns{0.0};
    std::uint64_t dropped{0};
};

namespace detail {

[[nodiscard]] inline std::int64_t steady_now_ns() noexcept {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] inline double percentile(const std::vector<double>& sorted,
                                       double quantile) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = quantile * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

} // namespace detail

// Threaded replay: the caller parses a dataset and calls submit(); a dedicated
// consumer thread runs the signal handler. Producer and consumer are connected
// by the cache-line padded ring buffer. Latency is measured from immediately
// before claim() to immediately after wait_for(), i.e. the full hand-off.
class ReplayRunner {
public:
    static constexpr std::size_t kCapacity = std::size_t{1} << 18;
    using Handler = std::function<void(const MarketEvent&)>;

    explicit ReplayRunner(Handler handler)
        : consumer_(ring_), producer_(ring_, consumer_), handler_(std::move(handler)) {
        for (std::int64_t index = 0; index < static_cast<std::int64_t>(kCapacity);
             ++index) {
            enqueue_ns_[static_cast<std::size_t>(index)] = 0;
        }
    }

    ReplayRunner(const ReplayRunner&) = delete;
    ReplayRunner& operator=(const ReplayRunner&) = delete;

    ~ReplayRunner() { finish(); }

    void start() {
        if (started_) {
            return;
        }
        started_ = true;
#if defined(__cpp_lib_jthread)
        thread_ = std::jthread([this](std::stop_token) { consume(); });
#else
        // Apple's libc++ in the current toolchain does not ship <stop_token>.
        // std::thread + explicit join is the portable C++20 fallback.
        thread_ = std::thread([this] { consume(); });
#endif
    }

    [[gnu::always_inline]] void submit(const MarketEvent& event) {
        if (!started_) {
            start();
        }
        auto ticket = producer_.claim();
        const std::size_t slot =
            static_cast<std::size_t>(ticket.sequence) & (kCapacity - 1);
        enqueue_ns_[slot] = detail::steady_now_ns();
        *ticket.entry = event;
        producer_.publish(ticket);
        published_hint_.store(ticket.sequence + 1, std::memory_order_release);
    }

    ReplayStats finish() {
        if (!started_) {
            return stats_;
        }
        if (finished_) {
            return stats_;
        }
        finished_ = true;
        done_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        stats_.events = latencies_.size();
        stats_.dropped = 0;
        if (latencies_.empty()) {
            return stats_;
        }
        const double sum =
            std::accumulate(latencies_.begin(), latencies_.end(), 0.0);
        stats_.mean_ns = sum / static_cast<double>(latencies_.size());
        // Copy once at the end: deque growth never reallocates/copies the
        // already-recorded samples, so the hot path has no latency spikes.
        std::vector<double> sorted(latencies_.begin(), latencies_.end());
        std::sort(sorted.begin(), sorted.end());
        stats_.p50_ns = detail::percentile(sorted, 0.50);
        stats_.p90_ns = detail::percentile(sorted, 0.90);
        stats_.p99_ns = detail::percentile(sorted, 0.99);
        stats_.p999_ns = detail::percentile(sorted, 0.999);
        stats_.max_ns = sorted.back();
        return stats_;
    }

private:
    void consume() {
        std::int64_t expected = 0;
        while (true) {
            const std::int64_t claimed =
                published_hint_.load(std::memory_order_acquire);
            if (expected < claimed) {
                consumer_.wait_for(expected);
                const std::size_t slot =
                    static_cast<std::size_t>(expected) & (kCapacity - 1);
                const std::int64_t start = enqueue_ns_[slot];
                const MarketEvent& event = consumer_.get(expected);
                handler_(event);
                if (start != 0) {
                    latencies_.push_back(
                        static_cast<double>(detail::steady_now_ns() - start));
                }
                consumer_.advance_to(expected);
                ++expected;
                continue;
            }
            if (done_.load(std::memory_order_acquire)) {
                break;
            }
            hft::disruptor::detail::cpu_relax();
        }
    }

    hft::disruptor::RingBuffer<MarketEvent, kCapacity> ring_;
    hft::disruptor::ConsumerBarrier<MarketEvent, kCapacity> consumer_;
    hft::disruptor::ProducerBarrier<MarketEvent, kCapacity> producer_;
#if defined(__cpp_lib_jthread)
    std::jthread thread_;
#else
    std::thread thread_;
#endif
    std::atomic<std::int64_t> published_hint_{0};
    std::atomic<bool> done_{false};
    Handler handler_;
    std::deque<double> latencies_;
    alignas(kCacheLine) std::int64_t enqueue_ns_[kCapacity]{};
    ReplayStats stats_{};
    bool started_{false};
    bool finished_{false};
};

// Convenience wrapper for datasets that fit in memory (e.g. a single hourly
// Hyperliquid file). Streaming readers should call ReplayRunner::submit().
template <typename Handler>
[[nodiscard]] inline ReplayStats replay_events(std::span<const MarketEvent> events,
                                               Handler&& handler) {
    ReplayRunner runner(std::forward<Handler>(handler));
    runner.start();
    for (const MarketEvent& event : events) {
        runner.submit(event);
    }
    return runner.finish();
}

} // namespace hft::ingest
