#pragma once

#include "hft/Backtest.hpp"
#include "hft/Export.hpp"
#include "hft/SignalPipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hft::sdk {

struct EngineConfig {
    obi::FilterThresholds thresholds{};
    std::int64_t sample_interval_ns{kNanosecondsPerMillisecond * 250};
    std::size_t max_snapshots{std::size_t{1} << 19};
};

struct EngineStats {
    std::size_t processed_events{0};
    std::size_t processed_trades{0};
    std::size_t snapshots{0};
    std::size_t total_snapshots{0};
    std::size_t evicted_snapshots{0};
    std::size_t out_of_order_events{0};
    std::size_t invalid_events{0};
    std::size_t tracker_dropped{0};
    std::size_t skipped_samples{0};
};

// Single-owner engine. Calls and returned history references require external
// synchronization; process/reset/history access may invalidate prior references.
class HFT_SDK_API OBIFilterEngine {
public:
    explicit OBIFilterEngine(EngineConfig config = {});
    ~OBIFilterEngine();

    OBIFilterEngine(OBIFilterEngine&&) noexcept;
    OBIFilterEngine& operator=(OBIFilterEngine&&) noexcept;
    OBIFilterEngine(const OBIFilterEngine&) = delete;
    OBIFilterEngine& operator=(const OBIFilterEngine&) = delete;

    void process(std::span<const MarketEvent> events);
    void process(const std::vector<MarketEvent>& events);

    [[nodiscard]] const std::vector<signal::SignalSnapshot>& snapshots() const;
    [[nodiscard]] std::optional<signal::SignalSnapshot> latest_snapshot() const;
    [[nodiscard]] EngineStats stats() const;
    [[nodiscard]] std::vector<signal::SignalMetrics> metrics() const;
    [[nodiscard]] std::vector<signal::RegimeTuple> regimes(
        obi::FilterScheme scheme = obi::FilterScheme::Unfiltered,
        std::size_t horizon = 0,
        bool volume = true) const;
    [[nodiscard]] backtest::Summary backtest(
        const backtest::Config& config) const;

    void reset();
    [[nodiscard]] const EngineConfig& config() const noexcept;

private:
    EngineConfig config_{};
    std::unique_ptr<signal::SignalPipeline<>> pipeline_;
};

} // namespace hft::sdk

