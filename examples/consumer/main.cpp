#include <hft/SignalPipeline.hpp>
#ifndef HFT_CONSUMER_HEADER_ONLY
#include <hft/HftSdk.hpp>
#include <hft/SPSCQueue.hpp>
#endif
#include <cmath>
#include <iostream>
#include <vector>

int main() {
#ifdef HFT_CONSUMER_HEADER_ONLY
    hft::signal::SignalPipeline<> engine(hft::obi::FilterThresholds{}, 250'000'000, 3);
#else
    hft::sdk::EngineConfig config;
    config.max_snapshots = 3;
    hft::sdk::OBIFilterEngine engine(config);
    hft::MarketDataMessage sent{};
    sent.symbol_id = 42;
    hft::MarketDataMessage received{};
    if (!hft::g_market_data_queue.push(sent) || !hft::g_market_data_queue.pop(received) ||
        received.symbol_id != sent.symbol_id) return 1;
#endif
    std::vector<hft::MarketEvent> events(7);
    for (std::size_t i = 0; i < events.size(); ++i) {
        auto& event = events[i];
        event.order_id = i + 1;
        event.timestamp_ns = static_cast<std::int64_t>(i) * 250'000'000;
        event.type = hft::EventType::New;
        event.side = hft::Side::Bid;
        event.price_ticks = 100;
        event.quantity = 2;
        event.bid_price_ticks = 100;
        event.ask_price_ticks = 102;
    }
    engine.process(events);
    const auto& history = engine.snapshots();
    if (history.size() != 3 || history.back().timestamp_ns != events.back().timestamp_ns ||
        history.front().timestamp_ns != 1'000'000'000 || history.back().mid != 101) return 2;
    const auto value = history.back().lob[0][0];
    if (!value.valid || std::abs(value.value + 1) > 1e-12) return 3;
#ifndef HFT_CONSUMER_HEADER_ONLY
    const auto stats = engine.stats();
    if (stats.total_snapshots != 6 || stats.evicted_snapshots != 3) return 4;
    engine.reset();
    if (engine.stats().total_snapshots != 0 || engine.latest_snapshot()) return 5;
#endif
    std::cout << "consumer: processing, imbalance, rolling retention, reset: ok\n";
}
