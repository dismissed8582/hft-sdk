#include "hft/HftSdk.hpp"

#include <utility>

namespace hft::sdk {

OBIFilterEngine::OBIFilterEngine(EngineConfig config) : config_(config) {
    reset();
}

OBIFilterEngine::~OBIFilterEngine() = default;

OBIFilterEngine::OBIFilterEngine(OBIFilterEngine&&) noexcept = default;
OBIFilterEngine& OBIFilterEngine::operator=(OBIFilterEngine&&) noexcept = default;

void OBIFilterEngine::reset() {
    pipeline_ = std::make_unique<signal::SignalPipeline<>>(
        config_.thresholds, config_.sample_interval_ns, config_.max_snapshots);
}

void OBIFilterEngine::process(std::span<const MarketEvent> events) {
    pipeline_->process(events);
}

void OBIFilterEngine::process(const std::vector<MarketEvent>& events) {
    process(std::span<const MarketEvent>(events.data(), events.size()));
}

const std::vector<signal::SignalSnapshot>& OBIFilterEngine::snapshots() const {
    return pipeline_->snapshots();
}

std::optional<signal::SignalSnapshot> OBIFilterEngine::latest_snapshot() const {
    const auto* latest = pipeline_->latest_snapshot();
    return latest ? std::optional<signal::SignalSnapshot>(*latest) : std::nullopt;
}

EngineStats OBIFilterEngine::stats() const {
    return {pipeline_->processed_events(), pipeline_->processed_trades(), pipeline_->snapshot_count(), pipeline_->total_snapshots(),
            pipeline_->evicted_snapshots(), pipeline_->out_of_order_events(),
            pipeline_->invalid_events(), pipeline_->tracker_dropped(), pipeline_->skipped_samples()};
}

std::vector<signal::SignalMetrics> OBIFilterEngine::metrics() const {
    return signal::calculate_metrics(pipeline_->snapshots());
}

std::vector<signal::RegimeTuple> OBIFilterEngine::regimes(
    obi::FilterScheme scheme, std::size_t horizon, bool volume) const {
    return signal::calculate_regimes(pipeline_->snapshots(), scheme, horizon, volume);
}

backtest::Summary OBIFilterEngine::backtest(const backtest::Config& config) const {
    return hft::backtest::run(pipeline_->snapshots(), config);
}

const EngineConfig& OBIFilterEngine::config() const noexcept {
    return config_;
}

} // namespace hft::sdk

