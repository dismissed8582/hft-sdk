#include "hft/Backtest.hpp"
#include "hft/Metrics.hpp"
#include "hft/TickData.hpp"
#include "hft/Disruptor.hpp"
#include "hft/OBIFilter.hpp"
#include "hft/Paper.hpp"
#include "hft/SPSCQueue.hpp"
#include "hft/ScoringEngine.hpp"
#include "hft/SignalPipeline.hpp"
#include "hft/ingest/Binance.hpp"
#include "hft/ingest/Replay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                         \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #condition << '\n';      \
            ++failures;                                                                          \
        }                                                                                        \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                  \
    do {                                                                                         \
        const double check_actual = (actual);                                                    \
        const double check_expected = (expected);                                                \
        if (std::abs(check_actual - check_expected) > (tolerance)) {                             \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << "  " #actual "="              \
                      << check_actual << " expected " << check_expected << '\n';                 \
            ++failures;                                                                          \
        }                                                                                        \
    } while (false)

void test_histogram_exposition() {
    hft::metrics::Registry metrics;
    auto& histogram = metrics.histogram("latency", {10, 100});
    for (double value : {1.0, 10.0, 50.0, 200.0}) histogram.observe(value);
    const auto text = metrics.render();
    CHECK(text.find("latency_bucket{le=\"10\"} 2\n") != std::string::npos);
    CHECK(text.find("latency_bucket{le=\"100\"} 3\n") != std::string::npos);
    CHECK(text.find("latency_bucket{le=\"+Inf\"} 4\n") != std::string::npos);
    CHECK(text.find("latency_count 4\n") != std::string::npos);
    CHECK(text.find("latency_sum 261\n") != std::string::npos);
}

void test_stable_tick_sort() {
    std::vector<hft::MarketEvent> events(4);
    for (std::size_t i = 0; i < events.size(); ++i) events[i].order_id = i;
    events[0].timestamp_ns = 2;
    events[1].timestamp_ns = 1;
    events[2].timestamp_ns = 1;
    events[3].timestamp_ns = 0;
    hft::data::sort_ticks_by_timestamp(events);
    CHECK(events[0].order_id == 3);
    CHECK(events[1].order_id == 1);
    CHECK(events[2].order_id == 2);
    CHECK(events[3].order_id == 0);
    hft::data::sort_ticks_by_timestamp(events);
    CHECK(events[1].order_id == 1);
    events.clear();
    hft::data::sort_ticks_by_timestamp(events);
    CHECK(events.empty());
}

void test_types() {
    static_assert(sizeof(hft::MarketEvent) % hft::kCacheLine == 0);
    static_assert(alignof(hft::MarketEvent) == hft::kCacheLine);
    CHECK(hft::event_type_name(hft::EventType::Trade) == "TRADE");
    CHECK(hft::side_name(hft::Side::Ask) == "ASK");
}

void test_obi_math() {
    hft::obi::DirectionalCounts counts{};
    counts.bid = 3;
    counts.ask = 1;
    const hft::obi::OBIValue imbalance = hft::obi::order_book_imbalance(counts);
    CHECK(imbalance.valid);
    CHECK_NEAR(imbalance.value, -0.5, 1e-12);

    counts.buy = 3;
    counts.sell = 1;
    const hft::obi::OBIValue trade = hft::obi::trade_order_book_imbalance(counts);
    CHECK(trade.valid);
    CHECK_NEAR(trade.value, 0.5, 1e-12);

    const hft::obi::DirectionalCounts empty{};
    CHECK(!hft::obi::order_book_imbalance(empty).valid);
    CHECK(!hft::obi::trade_order_book_imbalance(empty).valid);
    CHECK_NEAR(hft::obi::order_book_imbalance(empty).value, 0.0, 1e-12);
}

void test_weighted_obi() {
    using namespace hft;
    obi::WeightedDirectionalCounts counts{};
    counts.bid = 2.0;
    counts.ask = 1.0;
    counts.buy = 3.0;
    counts.sell = 1.0;
    CHECK_NEAR(obi::order_book_imbalance(counts).value, -1.0 / 3.0, 1e-12);
    CHECK_NEAR(obi::trade_order_book_imbalance(counts).value, 0.5, 1e-12);
    CHECK(obi::order_book_imbalance(obi::WeightedDirectionalCounts{}).valid == false);

    obi::BucketedRollingCounts<> window(1'000'000'000);
    const std::uint8_t all = 0x0F;
    window.push(100, Side::Bid, false, all, 2.0);
    window.push(200, Side::Ask, false, all, 1.0);
    const auto& weights = window.weights(obi::FilterScheme::Unfiltered);
    CHECK_NEAR(weights.bid, 2.0, 1e-12);
    CHECK_NEAR(weights.ask, 1.0, 1e-12);
    CHECK_NEAR(obi::order_book_imbalance(weights).value, -1.0 / 3.0, 1e-12);
    window.advance(2'000'000'000);
    CHECK_NEAR(window.weights(obi::FilterScheme::Unfiltered).bid, 0.0, 1e-12);
}

void test_volume_pipeline() {
    using namespace hft;
    signal::SignalPipeline<> pipeline(obi::FilterThresholds{0, 3, 0}, 1'000'000'000);
    std::vector<MarketEvent> events(3);
    events[0].timestamp_ns = 0;
    events[0].type = EventType::New;
    events[0].side = Side::Bid;
    events[0].quantity = 10;
    events[0].bid_price_ticks = 100;
    events[0].ask_price_ticks = 101;
    events[1] = events[0];
    events[1].side = Side::Ask;
    events[1].order_id = 2;
    events[1].quantity = 30;
    events[2] = events[0];
    events[2].order_id = 3;
    events[2].timestamp_ns = 1'000'000'000;
    events[2].type = EventType::Trade;
    events[2].side = Side::Buy;
    events[2].quantity = 1;
    pipeline.process(events);
    CHECK(pipeline.snapshots().size() == 1);
    if (!pipeline.snapshots().empty()) {
        const auto& snapshot = pipeline.snapshots()[0];
        CHECK(snapshot.lob_volume[0][0].valid);
        CHECK_NEAR(snapshot.lob_volume[0][0].value, 0.5, 1e-12);
        CHECK_NEAR(snapshot.lob[0][0].value, 0.0, 1e-12);
    }
}

void test_backtest() {
    using namespace hft;
    std::vector<signal::SignalSnapshot> snapshots(5);
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        snapshots[index].timestamp_ns =
            static_cast<std::int64_t>(index) * 1'000'000'000;
        snapshots[index].mid = 100.0 + static_cast<double>(index);
    }
    snapshots[0].lob_volume[0][0] = obi::OBIValue{0.5, true};
    snapshots[2].lob_volume[0][0] = obi::OBIValue{-0.5, true};

    backtest::Config config{};
    config.weight = backtest::SignalWeight::Volume;
    config.horizon = 0;
    config.latency_ns = 0;
    config.taker_fee_bps = 0.0;
    const backtest::Summary summary = backtest::run(snapshots, config);
    CHECK(summary.trade_count == 2);
    if (summary.trade_count == 2) {
        CHECK_NEAR(summary.trades[0].gross_return, 0.01, 1e-12);
        CHECK_NEAR(summary.trades[1].gross_return, 1.0 - 103.0 / 102.0, 1e-12);
    }
}

void test_filters() {
    const hft::obi::FilterThresholds thresholds{100, 2, 10};
    const hft::obi::FilterState state{101, 2, 10};
    const std::uint8_t mask = hft::obi::filter_mask(state, thresholds);
    CHECK((mask & hft::obi::scheme_bit(hft::obi::FilterScheme::Unfiltered)) != 0);
    CHECK((mask & hft::obi::scheme_bit(hft::obi::FilterScheme::Lifetime)) != 0);
    CHECK((mask & hft::obi::scheme_bit(hft::obi::FilterScheme::ModificationCount)) != 0);
    CHECK((mask & hft::obi::scheme_bit(hft::obi::FilterScheme::ModificationTime)) != 0);

    CHECK(!hft::obi::passes_lifetime_filter({99, 0, 100}, thresholds));
    CHECK(!hft::obi::passes_modification_count_filter({200, 3, 100}, thresholds));
    CHECK(!hft::obi::passes_modification_time_filter({200, 0, 9}, thresholds));
}

void test_order_tracker() {
    hft::obi::OrderTracker<256> tracker;
    hft::MarketEvent event{};
    event.order_id = 42;
    event.timestamp_ns = 1'000;
    event.type = hft::EventType::New;
    event.side = hft::Side::Bid;
    auto observation = tracker.observe(event);
    CHECK(observation.found);
    CHECK(observation.filters.lifetime_ns == 0);
    CHECK(observation.filters.modifications == 0);

    event.type = hft::EventType::Modify;
    event.timestamp_ns = 200'000;
    observation = tracker.observe(event);
    CHECK(observation.filters.modifications == 1);
    CHECK(observation.filters.lifetime_ns == 199'000);

    event.timestamp_ns = 250'000;
    observation = tracker.observe(event);
    CHECK(observation.filters.modifications == 2);
    CHECK(observation.filters.last_modification_interval_ns == 50'000);

    event.type = hft::EventType::Cancel;
    event.timestamp_ns = 300'000;
    observation = tracker.observe(event);
    CHECK(observation.terminal);
    CHECK(observation.filters.lifetime_ns == 299'000);

    hft::MarketEvent trade{};
    trade.order_id = 999;
    trade.type = hft::EventType::Trade;
    trade.side = hft::Side::Buy;
    observation = tracker.observe(trade);
    CHECK(!observation.found);
}

void test_rolling_counts() {
    hft::obi::RollingCounts<64> window(1'000);
    const std::uint8_t all = hft::obi::scheme_bit(hft::obi::FilterScheme::Unfiltered) |
                             hft::obi::scheme_bit(hft::obi::FilterScheme::Lifetime) |
                             hft::obi::scheme_bit(hft::obi::FilterScheme::ModificationCount) |
                             hft::obi::scheme_bit(hft::obi::FilterScheme::ModificationTime);
    window.push(100, hft::Side::Bid, false, all);
    window.push(200, hft::Side::Bid, false, all);
    window.push(300, hft::Side::Ask, false, all);
    window.push(400, hft::Side::Buy, true, all);
    window.push(500, hft::Side::Buy, true, all);
    window.push(600, hft::Side::Sell, true, all);

    auto counts =
        window.counts(hft::obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 2);
    CHECK(counts.ask == 1);
    CHECK(counts.buy == 2);
    CHECK(counts.sell == 1);
    CHECK_NEAR(hft::obi::order_book_imbalance(counts).value, -1.0 / 3.0, 1e-12);
    CHECK_NEAR(hft::obi::trade_order_book_imbalance(counts).value, 1.0 / 3.0, 1e-12);

    window.advance(1'200);
    counts = window.counts(hft::obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 0);
    CHECK(counts.ask == 1);
    CHECK(counts.buy == 2);
    CHECK(counts.sell == 1);

    window.advance(1'500);
    counts = window.counts(hft::obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 0);
    CHECK(counts.ask == 0);
    CHECK(counts.buy == 0);
    CHECK(counts.sell == 1);

    window.advance(2'500);
    counts = window.counts(hft::obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 0);
    CHECK(counts.ask == 0);
    CHECK(counts.buy == 0);
    CHECK(counts.sell == 0);
}

void test_welford() {
    const std::vector<double> x{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};
    const std::vector<double> y{2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0};
    const hft::scoring::CorrelationScore score =
        hft::scoring::correlation_score(x, y);
    CHECK(score.valid);
    CHECK_NEAR(score.pearson, 1.0, 1e-12);
    CHECK_NEAR(score.r2, 1.0, 1e-12);
    CHECK(score.samples == x.size());

    const hft::scoring::RollingMoments moments = hft::scoring::rolling_moments(x);
    CHECK(moments.valid);
    CHECK(moments.samples == x.size());
    CHECK_NEAR(moments.mean, 4.0, 1e-12);
    CHECK_NEAR(moments.stddev, std::sqrt(28.0 / 6.0), 1e-12);

    const std::vector<double> inverse{14.0, 12.0, 10.0, 8.0, 6.0, 4.0, 2.0};
    CHECK_NEAR(hft::scoring::correlation_score(x, inverse).pearson, -1.0, 1e-12);
}

void test_regimes() {
    using D = hft::scoring::RegimeDiscretizer;
    CHECK(D::obi_regime(-1.0) == 0);
    CHECK(D::obi_regime(0.0) == 4);
    CHECK(D::obi_regime(1.0) == 8);
    CHECK(D::obi_regime(-4.0) == 0);
    CHECK(D::obi_regime(4.0) == 8);
    CHECK(D::return_regime(-1.0) == 0);
    CHECK(D::return_regime(0.0) == 2);
    CHECK(D::return_regime(1.0) == 3);
    CHECK(D::return_regime(8.0) == 3);
}

void test_ar1_residualization() {
    const std::vector<double> series{1.0, 2.0, 4.0, 8.0, 16.0};
    const std::vector<double> residuals =
        hft::scoring::ar1_residuals(std::span<const double>(series));
    CHECK(residuals.size() == series.size());
    CHECK(residuals[0] == 0.0);

    hft::scoring::SimdWelford lag_one;
    lag_one.add(std::span<const double>(series.data(), series.size() - 1),
                std::span<const double>(series.data() + 1, series.size() - 1));
    const double phi = lag_one.finalize().correlation();
    CHECK(std::isfinite(phi));
    for (std::size_t index = 1; index < series.size(); ++index) {
        CHECK_NEAR(residuals[index], series[index] - phi * series[index - 1], 1e-12);
    }

    const std::vector<double> flat(8, 3.0);
    const std::vector<double> flat_residuals =
        hft::scoring::ar1_residuals(std::span<const double>(flat));
    for (const double value : flat_residuals) {
        CHECK_NEAR(value, 0.0, 1e-12);
    }
}

void test_regime_export() {
    using namespace hft;
    std::vector<signal::SignalSnapshot> snapshots(3);
    const std::int64_t start = 1'000'000'000;
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        snapshots[index].timestamp_ns =
            start + static_cast<std::int64_t>(index) * kNanosecondsPerSecond;
        snapshots[index].mid = 100.0 + static_cast<double>(index);
    }
    snapshots[0].lob[0][0] = obi::OBIValue{0.5, true};
    snapshots[1].lob[0][0] = obi::OBIValue{0.0, true};
    snapshots[2].lob[0][0] = obi::OBIValue{-0.5, true};

    const auto tuples = signal::calculate_regimes(
        snapshots, obi::FilterScheme::Unfiltered, 0);
    CHECK(tuples.size() == 2);
    if (tuples.size() == 2) {
        CHECK(tuples[0].timestamp_ns == start);
        CHECK(tuples[1].timestamp_ns == start + kNanosecondsPerSecond);
        for (const auto& tuple : tuples) {
            CHECK(tuple.obi_regime < 9);
            CHECK(tuple.return_regime < 4);
        }
    }

    const auto missing = signal::calculate_regimes(
        snapshots, obi::FilterScheme::Lifetime, 0);
    CHECK(missing.empty());
}

void test_spsc_queue() {
    using hft::SPSCQueue;
    SPSCQueue<int, 8> queue;
    CHECK(queue.empty());
    CHECK(queue.capacity() == 7);
    for (int value = 0; value < 7; ++value) {
        CHECK(queue.push(value));
    }
    CHECK(queue.size() == 7);
    int value = -1;
    CHECK(!queue.push(7));
    for (int expected = 0; expected < 7; ++expected) {
        CHECK(queue.pop(value));
        CHECK(value == expected);
    }
    CHECK(queue.empty());
    CHECK(!queue.pop(value));

    // Force head/tail wrap-around.
    for (int expected = 0; expected < 5; ++expected) {
        CHECK(queue.push(expected));
    }
    for (int expected = 0; expected < 5; ++expected) {
        CHECK(queue.pop(value));
        CHECK(value == expected);
    }

    constexpr int count = 100'000;
    SPSCQueue<int, 1024> concurrent;
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for (int sequence = 0; sequence < count; ++sequence) {
            while (!concurrent.push(sequence)) {
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&] {
        int expected = 0;
        while (expected < count) {
            int received = 0;
            if (concurrent.pop(received)) {
                if (received != expected) {
                    failed.store(true, std::memory_order_relaxed);
                }
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();
    CHECK(!failed.load(std::memory_order_relaxed));
    CHECK(concurrent.empty());
}

void test_bucketed_counts() {
    using namespace hft;
    obi::BucketedRollingCounts<> window(2'000'000, 1'000'000);
    const std::uint8_t all = 0x0F;
    window.push(100, Side::Bid, false, all);
    window.push(1'500'000, Side::Ask, false, all);
    auto counts = window.counts(obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 1);
    CHECK(counts.ask == 1);

    window.advance(4'500'000);
    counts = window.counts(obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 0);
    CHECK(counts.ask == 1);

    window.advance(6'500'000);
    counts = window.counts(obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 0);
    CHECK(counts.ask == 0);

    window.push(6'000'000, Side::Bid, false, all);
    window.push(7'000'000, Side::Bid, false, all);
    counts = window.counts(obi::FilterScheme::Unfiltered);
    CHECK(counts.bid == 2);

    // Out-of-order event older than the horizon is rejected, not inserted.
    const std::size_t dropped_before = window.dropped();
    window.push(0, Side::Ask, false, all);
    CHECK(window.dropped() == dropped_before + 1);
}

void test_binance_trades_parser() {
    const auto path = std::filesystem::temp_directory_path() / "hft_binance_trades.csv";
    {
        std::ofstream output(path);
        output << "trade_id,price,qty,quoteQty,time,isBuyerMaker,isBestMatch\n";
        output << "1,100.50,2.0,201.0,1700000000000,false,true\n";
        output << "2,100.75,1.5,151.1,1700000001000,true,true\n";
    }
    std::vector<hft::ingest::TradeRecord> trades;
    const auto count = hft::ingest::read_binance_trades_csv(
        path.string(), hft::ingest::BinanceTradeFormat::SpotTrades,
        [&](const hft::ingest::TradeRecord& trade) { trades.push_back(trade); });
    CHECK(count == 2);
    CHECK(trades.size() == 2);
    CHECK(trades[0].timestamp_ns == 1'700'000'000'000LL * 1'000'000);
    CHECK_NEAR(trades[0].price, 100.50, 1e-12);
    CHECK(trades[0].aggressor == hft::Side::Buy);
    CHECK(trades[1].aggressor == hft::Side::Sell);
    std::filesystem::remove(path);
}

void test_binance_book_depth_parser() {
    const auto path = std::filesystem::temp_directory_path() / "hft_binance_book_depth.csv";
    {
        std::ofstream output(path);
        output << "timestamp,percentage,depth,notional\n";
        output << "2026-10-20 00:00:01,-5.00,10053.985,808006890.8795\n";
        output << "2026-10-20 00:00:01,0.00,512.5,42000000.0\n";
        output << "2026-10-20 00:00:01,5.00,9000.25,740000000.5\n";
    }
    std::vector<hft::ingest::DepthSnapshot> snapshots;
    const auto count = hft::ingest::read_binance_book_depth_csv(
        path.string(),
        [&](const hft::ingest::DepthSnapshot& snapshot) { snapshots.push_back(snapshot); });
    CHECK(count == 1);
    CHECK(snapshots.size() == 1);
    CHECK(snapshots[0].timestamp_ns > 0);
    CHECK(snapshots[0].has(-5));
    CHECK(snapshots[0].has(0));
    CHECK(snapshots[0].has(5));
    CHECK_NEAR(snapshots[0].base_depth[0], 10053.985, 1e-6);
    CHECK_NEAR(snapshots[0].base_depth[10], 9000.25, 1e-6);
    std::filesystem::remove(path);
}



void test_dependency_barrier() {
    using namespace hft;
    using Ring = disruptor::RingBuffer<MarketEvent, 64>;
    using Consumer = disruptor::ConsumerBarrier<MarketEvent, 64>;
    Ring ring;
    Consumer upstream(ring);
    Consumer downstream(ring);
    disruptor::ConsumerGroup<MarketEvent, 64> group;
    group.add(upstream);
    group.add(downstream);
    disruptor::GroupProducerBarrier<MarketEvent, 64> producer(ring, group);
    disruptor::DependencyBarrier<MarketEvent, 64> dependency;
    dependency.add(upstream);
    std::atomic<bool> upstream_finished{false};
    constexpr std::int64_t total = 16;
    for (std::int64_t sequence = 0; sequence < total; ++sequence) {
        auto ticket = producer.claim();
        ticket.entry->order_id = static_cast<std::uint64_t>(sequence + 1);
        producer.publish(ticket);
        upstream.wait_for(sequence);
        CHECK(upstream.get(sequence).order_id ==
              static_cast<std::uint64_t>(sequence + 1));
        upstream_finished.store(true, std::memory_order_release);
        upstream.advance_to(sequence);
        dependency.wait_for(sequence);
        CHECK(upstream_finished.load(std::memory_order_acquire));
        downstream.wait_for(sequence);
        CHECK(downstream.get(sequence).order_id ==
              static_cast<std::uint64_t>(sequence + 1));
        downstream.advance_to(sequence);
        upstream_finished.store(false, std::memory_order_release);
    }
    CHECK(dependency.sequence() == total - 1);
}

void test_consumer_group() {
    using namespace hft;
    using Ring = disruptor::RingBuffer<MarketEvent, 64>;
    using Consumer = disruptor::ConsumerBarrier<MarketEvent, 64>;
    Ring ring;
    Consumer first(ring);
    Consumer second(ring);
    disruptor::ConsumerGroup<MarketEvent, 64> group;
    group.add(first);
    group.add(second);
    disruptor::GroupProducerBarrier<MarketEvent, 64> producer(ring, group);
    constexpr std::int64_t total = 32;
    for (std::int64_t sequence = 0; sequence < total; ++sequence) {
        auto ticket = producer.claim();
        ticket.entry->order_id = static_cast<std::uint64_t>(sequence + 1);
        producer.publish(ticket);
        first.wait_for(sequence);
        second.wait_for(sequence);
        CHECK(first.get(sequence).order_id == static_cast<std::uint64_t>(sequence + 1));
        CHECK(second.get(sequence).order_id == static_cast<std::uint64_t>(sequence + 1));
        first.advance_to(sequence);
        second.advance_to(sequence);
    }
    CHECK(group.size() == 2);
    CHECK(group.sequence() == total - 1);
    CHECK(first.sequence() == total - 1);
    CHECK(second.sequence() == total - 1);
}

void test_disruptor_mpsc() {
    using namespace hft;
    constexpr std::size_t capacity = 1024;
    constexpr int per_producer = 20'000;
    constexpr int producers = 2;
    constexpr int total = per_producer * producers;

    disruptor::RingBuffer<MarketEvent, capacity> ring;
    disruptor::ConsumerBarrier<MarketEvent, capacity> consumer(ring);
    disruptor::MultiProducerBarrier<MarketEvent, capacity> producer(ring, consumer);

    auto worker = [&]() {
        for (int index = 0; index < per_producer; ++index) {
            auto ticket = producer.claim();
            ticket.entry->order_id = static_cast<std::uint64_t>(ticket.sequence);
            ticket.entry->timestamp_ns = ticket.sequence;
            producer.publish(ticket);
        }
    };
    std::thread first(worker);
    std::thread second(worker);

    for (std::int64_t sequence = 0; sequence < total; ++sequence) {
        consumer.wait_for(sequence);
        CHECK(consumer.get(sequence).order_id ==
              static_cast<std::uint64_t>(sequence));
        consumer.advance_to(sequence);
    }
    first.join();
    second.join();
    CHECK(producer.next_sequence() == total);
}

void test_replay_runner() {
    hft::ingest::ReplayRunner runner([](const hft::MarketEvent&) {});
    runner.start();
    for (int index = 0; index < 10'000; ++index) {
        hft::MarketEvent event{};
        event.order_id = static_cast<std::uint64_t>(index);
        event.timestamp_ns = index;
        event.type = hft::EventType::New;
        event.side = index % 2 == 0 ? hft::Side::Bid : hft::Side::Ask;
        runner.submit(event);
    }
    const auto stats = runner.finish();
    CHECK(stats.events == 10'000);
    CHECK(stats.p99_ns >= 0.0);
}

void test_disruptor() {
    using namespace hft;
    disruptor::RingBuffer<MarketEvent, 64> ring;
    CHECK(reinterpret_cast<std::uintptr_t>(ring.data()) % alignof(MarketEvent) == 0);
    disruptor::ConsumerBarrier<MarketEvent, 64> consumer(ring);
    disruptor::ProducerBarrier<MarketEvent, 64> producer(ring, consumer);

    constexpr std::uint64_t total = 100'000;
    for (std::uint64_t sequence = 0; sequence < total; ++sequence) {
        auto ticket = producer.claim();
        ticket.entry->order_id = sequence + 1;
        ticket.entry->timestamp_ns = static_cast<std::int64_t>(sequence);
        producer.publish(ticket);
        const std::int64_t available =
            consumer.wait_for(static_cast<std::int64_t>(sequence));
        CHECK(available >= static_cast<std::int64_t>(sequence));
        CHECK(consumer.get(static_cast<std::int64_t>(sequence)).order_id == sequence + 1);
        consumer.advance_to(static_cast<std::int64_t>(sequence));
    }
    CHECK(consumer.sequence() == static_cast<std::int64_t>(total - 1));
    CHECK(producer.next_sequence() == static_cast<std::int64_t>(total));
}


hft::MarketEvent paper_event(std::uint64_t order_id, std::int64_t timestamp_ns,
                             hft::EventType type, hft::Side side,
                             std::int64_t price, std::int64_t quantity = 1) {
    hft::MarketEvent event{};
    event.order_id = order_id;
    event.timestamp_ns = timestamp_ns;
    event.type = type;
    event.side = side;
    event.price_ticks = price;
    event.quantity = quantity;
    event.bid_price_ticks = 100;
    event.ask_price_ticks = 101;
    return event;
}

void test_paper_filters() {
    using namespace hft;
    constexpr std::int64_t ms = kNanosecondsPerMillisecond;
    std::vector<MarketEvent> events;
    // Long-lived, one modification: passes every filter.
    events.push_back(paper_event(1, 0, EventType::New, Side::Bid, 100));
    events.push_back(paper_event(1, 50 * ms, EventType::Modify, Side::Bid, 100));
    events.push_back(paper_event(1, 150 * ms, EventType::Cancel, Side::Bid, 100));
    // Four fast modifications: fails modification count and modification time.
    events.push_back(paper_event(2, 0, EventType::New, Side::Bid, 100));
    for (int index = 1; index <= 4; ++index) {
        events.push_back(paper_event(2, index * 10 * ms, EventType::Modify,
                                     Side::Bid, 100));
    }
    events.push_back(paper_event(2, 200 * ms, EventType::Cancel, Side::Bid, 100));
    // Two modifications separated by 60 ms: passes every filter.
    events.push_back(paper_event(3, 0, EventType::New, Side::Ask, 101));
    events.push_back(paper_event(3, 60 * ms, EventType::Modify, Side::Ask, 101));
    events.push_back(paper_event(3, 120 * ms, EventType::Modify, Side::Ask, 101));
    events.push_back(paper_event(3, 130 * ms, EventType::Cancel, Side::Ask, 101));
    // Short-lived: fails lifetime only.
    events.push_back(paper_event(4, 0, EventType::New, Side::Bid, 100));
    events.push_back(paper_event(4, 10 * ms, EventType::Cancel, Side::Bid, 100));
    // Four modifications but 60 ms apart: fails modification count only.
    events.push_back(paper_event(5, 0, EventType::New, Side::Ask, 101));
    for (int index = 1; index <= 4; ++index) {
        events.push_back(paper_event(5, index * 60 * ms, EventType::Modify,
                                     Side::Ask, 101));
    }
    events.push_back(paper_event(5, 300 * ms, EventType::Cancel, Side::Ask, 101));
    // Trade-only ID: should not receive a lifecycle-derived mask.
    events.push_back(paper_event(99, 10 * ms, EventType::Trade, Side::Buy, 100));

    const obi::FilterThresholds thresholds{100 * ms, 3, 50 * ms};
    const paper::FilterMaskTable masks = paper::build_filter_masks(events, thresholds);
    const auto bit = [](obi::FilterScheme scheme) { return obi::scheme_bit(scheme); };
    const std::uint8_t all = bit(obi::FilterScheme::Unfiltered) |
                             bit(obi::FilterScheme::Lifetime) |
                             bit(obi::FilterScheme::ModificationCount) |
                             bit(obi::FilterScheme::ModificationTime);
    CHECK(masks.get(1) == all);
    CHECK(masks.get(2) == (bit(obi::FilterScheme::Unfiltered) |
                           bit(obi::FilterScheme::Lifetime)));
    CHECK(masks.get(3) == all);
    CHECK(masks.get(4) == (bit(obi::FilterScheme::Unfiltered) |
                           bit(obi::FilterScheme::ModificationCount) |
                           bit(obi::FilterScheme::ModificationTime)));
    CHECK(masks.get(5) == (bit(obi::FilterScheme::Unfiltered) |
                           bit(obi::FilterScheme::Lifetime) |
                           bit(obi::FilterScheme::ModificationTime)));
    CHECK(!masks.contains(99));
    CHECK(masks.get(99) == paper::kAllSchemes);
}


void test_paper_full_execution() {
    using namespace hft;
    constexpr std::int64_t ms = kNanosecondsPerMillisecond;
    std::vector<MarketEvent> events;
    events.push_back(paper_event(6, 0, EventType::New, Side::Bid, 100, 10));
    MarketEvent fill = paper_event(9006, 200 * ms, EventType::Trade, Side::Sell, 100, 10);
    fill.parent_order_id = 6;
    events.push_back(fill);
    events.push_back(paper_event(6, 1000 * ms, EventType::Cancel, Side::Bid, 100, 10));

    // Exit is the full fill at 200 ms, not the later cancel at 1000 ms.
    const obi::FilterThresholds thresholds{500 * ms, 3, 0};
    const paper::FilterMaskTable masks = paper::build_filter_masks(events, thresholds);
    CHECK((masks.get(6) & obi::scheme_bit(obi::FilterScheme::Lifetime)) == 0);
    CHECK((masks.get(6) & obi::scheme_bit(obi::FilterScheme::ModificationCount)) != 0);

    auto tracker = std::make_unique<obi::OrderTracker<(std::size_t{1} << 12)>>();
    const auto new_observation = tracker->observe(events[0]);
    CHECK(new_observation.found);
    CHECK(tracker->active() == 1);
    const auto trade_observation = tracker->observe(events[1]);
    CHECK(trade_observation.found);
    CHECK(tracker->active() == 0);
}

void test_paper_evaluation() {
    using namespace hft;
    constexpr std::int64_t ms = kNanosecondsPerMillisecond;
    std::vector<MarketEvent> events;
    events.push_back(paper_event(10, 0, EventType::New, Side::Bid, 100));
    events.push_back(paper_event(11, 0, EventType::New, Side::Ask, 101));
    events.push_back(paper_event(12, 200 * ms, EventType::New, Side::Bid, 100));
    events.push_back(paper_event(13, 300 * ms, EventType::New, Side::Bid, 100));
    events.push_back(paper_event(14, 400 * ms, EventType::New, Side::Ask, 101));
    events.push_back(paper_event(10, 1100 * ms, EventType::Trade, Side::Buy, 100));
    events.push_back(paper_event(11, 1500 * ms, EventType::Trade, Side::Sell, 105));
    hft::data::sort_ticks_by_timestamp(events);

    paper::PaperConfig config{};
    config.obi_window_ns = 5'000 * ms;
    config.sample_ns = 500 * ms;
    config.anchor_ns = 1'000 * ms;
    config.paper_filters = false;
    const std::vector<paper::PaperTick> ticks =
        paper::evaluate(events, obi::FilterThresholds{}, config);
    CHECK(!ticks.empty());

    bool found = false;
    for (const paper::PaperTick& tick : ticks) {
        if (tick.timestamp_ns == 1'000 * ms &&
            tick.scheme == static_cast<std::uint8_t>(obi::FilterScheme::Unfiltered)) {
            found = true;
            CHECK(tick.lob_valid);
            CHECK_NEAR(tick.lob_obi, -0.2, 1e-12);
            CHECK(tick.forward_valid[0] == 1);
            CHECK_NEAR(tick.forward_returns[0], 0.05, 1e-12);
            CHECK(tick.return_regime[0] == 2);
        }
    }
    CHECK(found);

    const std::vector<paper::PaperTick> empty =
        paper::evaluate(std::span<const MarketEvent>{}, obi::FilterThresholds{}, config);
    CHECK(empty.empty());
}

}

int main() {
    test_histogram_exposition();
    test_stable_tick_sort();
    test_types();
    test_obi_math();
    test_weighted_obi();
    test_volume_pipeline();
    test_backtest();
    test_filters();
    test_order_tracker();
    test_rolling_counts();
    test_welford();
    test_regimes();
    test_ar1_residualization();
    test_regime_export();
    test_disruptor();
    test_disruptor_mpsc();
    test_consumer_group();
    test_dependency_barrier();
    test_bucketed_counts();
    test_spsc_queue();
    test_binance_trades_parser();
    test_binance_book_depth_parser();
    test_replay_runner();
    test_paper_filters();
    test_paper_full_execution();
    test_paper_evaluation();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}

