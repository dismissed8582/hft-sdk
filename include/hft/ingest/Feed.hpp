#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hft::ingest {

enum class FeedState : std::uint8_t {
    Starting = 0,
    Live = 1,
    Degraded = 2,
    Stalled = 3,
    Stopped = 4,
};

[[nodiscard]] inline std::string_view feed_state_name(FeedState state) noexcept {
    switch (state) {
    case FeedState::Starting: return "starting";
    case FeedState::Live: return "live";
    case FeedState::Degraded: return "degraded";
    case FeedState::Stalled: return "stalled";
    case FeedState::Stopped: return "stopped";
    }
    return "unknown";
}

struct FeedStats {
    std::atomic<std::uint64_t> messages{0};
    std::atomic<std::uint64_t> trades{0};
    std::atomic<std::uint64_t> gaps{0};
    std::atomic<std::uint64_t> reconnects{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::int64_t> last_update_ns{0};
    std::atomic<std::int64_t> connected_since_ns{0};
};

// Per-stream sequence tracking. A gap is reported when the next sequence is
// larger than expected; out-of-order/duplicate messages are counted separately.
class GapDetector {
public:
    struct Result {
        std::uint64_t missing{0};
        bool duplicate{false};
        bool first{false};
    };

    Result check(const std::string& stream, std::uint64_t sequence) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = expected_.find(stream);
        if (iterator == expected_.end()) {
            expected_.emplace(stream, sequence + 1);
            return Result{0, false, true};
        }
        const std::uint64_t expected = iterator->second;
        if (sequence < expected) {
            return Result{0, true, false};
        }
        const std::uint64_t missing = sequence - expected;
        iterator->second = sequence + 1;
        return Result{missing, false, false};
    }

    void reset() {
        const std::lock_guard<std::mutex> lock(mutex_);
        expected_.clear();
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> expected_;
};

class ReconnectPolicy {
public:
    ReconnectPolicy(std::chrono::milliseconds initial = std::chrono::milliseconds(250),
                    std::chrono::milliseconds maximum = std::chrono::milliseconds(30'000),
                    double multiplier = 2.0)
        : initial_(initial), maximum_(maximum), multiplier_(multiplier),
          current_(initial) {}

    [[nodiscard]] std::chrono::milliseconds next() {
        const auto value = current_;
        const auto next_value = std::chrono::milliseconds(
            static_cast<std::int64_t>(current_.count() * multiplier_));
        current_ = std::min(next_value, maximum_);
        return value;
    }

    void reset() noexcept { current_ = initial_; }

private:
    std::chrono::milliseconds initial_;
    std::chrono::milliseconds maximum_;
    double multiplier_;
    std::chrono::milliseconds current_;
};

[[nodiscard]] inline std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace hft::ingest
