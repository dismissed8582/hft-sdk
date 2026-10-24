#include "hft/Disruptor.hpp"
#include "hft/OBIFilter.hpp"
#include "hft/ScoringEngine.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#ifndef HFT_CANONICAL_BENCH_OUT
#define HFT_CANONICAL_BENCH_OUT "benchmarks/results/latest_bench.json"
#endif
#ifndef HFT_CANONICAL_LATENCY_OUT
#define HFT_CANONICAL_LATENCY_OUT "benchmarks/results/latency_samples.csv"
#endif

namespace {

constexpr std::size_t kRingCapacity = 65536;

using Clock = std::chrono::steady_clock;

template <typename Consumer, typename Producer>
void ring_events(Consumer& consumer, Producer& producer, std::size_t count) {
    for (std::size_t sequence = 0; sequence < count; ++sequence) {
        auto ticket = producer.claim();
        ticket.entry->order_id = sequence + 1;
        ticket.entry->timestamp_ns = static_cast<std::int64_t>(sequence);
        ticket.entry->bid_price_ticks = 100;
        ticket.entry->ask_price_ticks = 101;
        producer.publish(ticket);
        consumer.wait_for(ticket.sequence);
        hft::MarketEvent observed = consumer.get(ticket.sequence);
        benchmark::DoNotOptimize(observed);
        benchmark::ClobberMemory();
        consumer.advance_to(ticket.sequence);
    }
}

void BM_DisruptorCold(benchmark::State& state) {
    hft::disruptor::RingBuffer<hft::MarketEvent, kRingCapacity> ring;
    hft::disruptor::ConsumerBarrier<hft::MarketEvent, kRingCapacity> consumer(ring);
    hft::disruptor::ProducerBarrier<hft::MarketEvent, kRingCapacity> producer(ring, consumer);
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        ring_events(consumer, producer, count);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_DisruptorWarmed(benchmark::State& state) {
    hft::disruptor::RingBuffer<hft::MarketEvent, kRingCapacity> ring;
    hft::disruptor::ConsumerBarrier<hft::MarketEvent, kRingCapacity> consumer(ring);
    hft::disruptor::ProducerBarrier<hft::MarketEvent, kRingCapacity> producer(ring, consumer);
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    bool warmed = false;
    for (auto _ : state) {
        if (!warmed) {
            state.PauseTiming();
            ring.warm_cache();
            state.ResumeTiming();
            warmed = true;
        }
        ring_events(consumer, producer, count);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

hft::scoring::WelfordStats scalar_welford(const std::vector<double>& x,
                                          const std::vector<double>& y) {
    hft::scoring::WelfordStats stats{};
    for (std::size_t index = 0; index < x.size(); ++index) {
        stats.count += 1.0;
        const double dx = x[index] - stats.mean_x;
        const double dy = y[index] - stats.mean_y;
        stats.mean_x += dx / stats.count;
        stats.mean_y += dy / stats.count;
        stats.m2_x += dx * (x[index] - stats.mean_x);
        stats.m2_y += dy * (y[index] - stats.mean_y);
        stats.covariance += dx * (y[index] - stats.mean_y);
    }
    return stats;
}

void BM_OBISimdBatch(benchmark::State& state) {
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    std::vector<double> x(count);
    std::vector<double> y(count);
    for (std::size_t index = 0; index < count; ++index) {
        x[index] = std::sin(static_cast<double>(index) * 0.01);
        y[index] = std::cos(static_cast<double>(index) * 0.013);
    }

    for (auto _ : state) {
        hft::scoring::SimdWelford accumulator;
        accumulator.add_batch(x.data(), y.data(), count);
        auto stats = accumulator.finalize();
        benchmark::DoNotOptimize(stats);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_OBIScalarBatch(benchmark::State& state) {
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    std::vector<double> x(count);
    std::vector<double> y(count);
    for (std::size_t index = 0; index < count; ++index) {
        x[index] = std::sin(static_cast<double>(index) * 0.01);
        y[index] = std::cos(static_cast<double>(index) * 0.013);
    }

    for (auto _ : state) {
        auto stats = scalar_welford(x, y);
        benchmark::DoNotOptimize(stats);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_RegimeDiscretization(benchmark::State& state) {
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    std::vector<double> values(count);
    for (std::size_t index = 0; index < count; ++index) {
        values[index] = -1.0 + 2.0 * static_cast<double>(index) /
                                   static_cast<double>(count);
    }
    std::uint64_t checksum = 0;
    for (auto _ : state) {
        for (const double value : values) {
            checksum += static_cast<std::uint64_t>(
                hft::scoring::RegimeDiscretizer::obi_regime(value));
        }
        benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(checksum);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_OrderTrackerFiltering(benchmark::State& state) {
    constexpr std::size_t count = 4096;
    std::vector<hft::MarketEvent> events(count);
    for (std::size_t index = 0; index < count; ++index) {
        events[index].order_id = index + 1;
        events[index].timestamp_ns = static_cast<std::int64_t>(index) * 1'000;
        events[index].bid_price_ticks = 100;
        events[index].ask_price_ticks = 101;
        events[index].type = index % 4 == 0 ? hft::EventType::New
                                           : hft::EventType::Modify;
        events[index].side = index % 2 == 0 ? hft::Side::Bid : hft::Side::Ask;
        events[index].modifications = static_cast<std::uint32_t>(index & 7u);
    }
    const hft::obi::FilterThresholds thresholds{100, 3, 1'000};
    auto tracker = std::make_unique<hft::obi::OrderTracker<(std::size_t{1} << 16)>>();

    for (auto _ : state) {
        std::uint8_t masks = 0;
        for (const hft::MarketEvent& event : events) {
            const auto observation = tracker->observe(event);
            masks |= hft::obi::filter_mask(observation.filters, thresholds);
        }
        benchmark::DoNotOptimize(masks);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_QueueMutex(benchmark::State& state) {
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        std::queue<hft::MarketEvent> queue;
        std::mutex mutex;
        std::uint64_t checksum = 0;
        for (std::size_t sequence = 0; sequence < count; ++sequence) {
            hft::MarketEvent event{};
            event.order_id = sequence + 1;
            event.timestamp_ns = static_cast<std::int64_t>(sequence);
            event.bid_price_ticks = 100;
            event.ask_price_ticks = 101;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                queue.push(event);
            }
            hft::MarketEvent observed{};
            {
                const std::lock_guard<std::mutex> lock(mutex);
                observed = queue.front();
                queue.pop();
            }
            checksum += observed.order_id;
            benchmark::DoNotOptimize(observed);
            benchmark::ClobberMemory();
        }
        benchmark::DoNotOptimize(checksum);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_RingBufferScaling(benchmark::State& state) {
    hft::disruptor::RingBuffer<hft::MarketEvent, kRingCapacity> ring;
    hft::disruptor::ConsumerBarrier<hft::MarketEvent, kRingCapacity> consumer(ring);
    hft::disruptor::ProducerBarrier<hft::MarketEvent, kRingCapacity> producer(ring, consumer);
    const std::size_t count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        ring_events(consumer, producer, count);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void collect_ring_latencies(std::vector<double>& samples, bool warmed) {
    hft::disruptor::RingBuffer<hft::MarketEvent, kRingCapacity> ring;
    hft::disruptor::ConsumerBarrier<hft::MarketEvent, kRingCapacity> consumer(ring);
    hft::disruptor::ProducerBarrier<hft::MarketEvent, kRingCapacity> producer(ring, consumer);

    if (warmed) {
        ring.warm();
        for (std::size_t sequence = 0; sequence < (std::size_t{1} << 15); ++sequence) {
            auto ticket = producer.claim();
            ticket.entry->order_id = sequence;
            producer.publish(ticket);
            consumer.wait_for(ticket.sequence);
            consumer.advance_to(ticket.sequence);
        }
    }

    constexpr std::size_t sample_count = 20'000;
    // The macOS monotonic clock ticks at 24 MHz (41.67 ns). A single
    // hand-off is faster than one tick, so measure a batch and amortise.
    constexpr std::size_t batch = 64;
    samples.reserve(samples.size() + sample_count / batch);
    for (std::size_t base = 0; base < sample_count; base += batch) {
        const auto start = Clock::now();
        for (std::size_t offset = 0; offset < batch; ++offset) {
            const std::size_t sequence = base + offset;
            auto ticket = producer.claim();
            ticket.entry->order_id = sequence;
            ticket.entry->bid_price_ticks = 100;
            ticket.entry->ask_price_ticks = 101;
            producer.publish(ticket);
            consumer.wait_for(ticket.sequence);
            benchmark::DoNotOptimize(consumer.get(ticket.sequence));
            consumer.advance_to(ticket.sequence);
        }
        const auto finish = Clock::now();
        samples.push_back(
            std::chrono::duration<double, std::nano>(finish - start).count() /
            static_cast<double>(batch));
    }
}

void collect_obi_latencies(std::vector<double>& samples, bool simd) {
    constexpr std::size_t count = 1024;
    std::vector<double> x(count);
    std::vector<double> y(count);
    for (std::size_t index = 0; index < count; ++index) {
        x[index] = std::sin(static_cast<double>(index) * 0.017);
        y[index] = std::cos(static_cast<double>(index) * 0.011);
    }

    constexpr std::size_t sample_count = 20'000;
    samples.reserve(samples.size() + sample_count);
    for (std::size_t iteration = 0; iteration < sample_count; ++iteration) {
        const auto start = Clock::now();
        if (simd) {
            hft::scoring::SimdWelford accumulator;
            accumulator.add_batch(x.data(), y.data(), count);
            auto stats = accumulator.finalize();
            benchmark::DoNotOptimize(stats);
        } else {
            auto stats = scalar_welford(x, y);
            benchmark::DoNotOptimize(stats);
        }
        const auto finish = Clock::now();
        samples.push_back(
            std::chrono::duration<double, std::nano>(finish - start).count());
    }
}

void write_latency_samples(const char* path) {
    std::vector<double> raw;
    std::vector<double> warmed;
    std::vector<double> simd;
    std::vector<double> scalar;
    collect_ring_latencies(raw, false);
    collect_ring_latencies(warmed, true);
    collect_obi_latencies(simd, true);
    collect_obi_latencies(scalar, false);

    std::ofstream output(path);
    if (!output) {
        return;
    }
    output << "pipeline,latency_ns\n";
    output << std::fixed << std::setprecision(3);
    const auto emit = [&output](const char* name, const std::vector<double>& values) {
        for (const double value : values) {
            output << name << ',' << value << '\n';
        }
    };
    emit("ring_buffer_raw", raw);
    emit("ring_buffer_warmed", warmed);
    emit("obi_simd", simd);
    emit("obi_scalar", scalar);
}

}

BENCHMARK(BM_QueueMutex)
    ->Name("Queue/Mutex")
    ->RangeMultiplier(10)
    ->Range(10, 1'000'000)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime()
    ->MinTime(0.1);
BENCHMARK(BM_RingBufferScaling)
    ->Name("RingBuffer/Scaled")
    ->RangeMultiplier(10)
    ->Range(10, 1'000'000)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime()
    ->MinTime(0.1);
BENCHMARK(BM_DisruptorCold)
    ->Name("Disruptor/Throughput/CacheCold")
    ->Arg(65536)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime();
BENCHMARK(BM_DisruptorWarmed)
    ->Name("Disruptor/Throughput/CacheWarmed")
    ->Arg(65536)
    ->Unit(benchmark::kNanosecond)
    ->UseRealTime();
BENCHMARK(BM_OBISimdBatch)
    ->Name("OBI/SIMD/Welford")
    ->Arg(4096)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OBIScalarBatch)
    ->Name("OBI/Scalar/Welford")
    ->Arg(4096)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_RegimeDiscretization)
    ->Name("Scoring/Regime/LUT")
    ->Arg(4096)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_OrderTrackerFiltering)
    ->Name("Filter/OrderTracker")
    ->Unit(benchmark::kNanosecond);

int main(int argc, char** argv) {
    const char* benchmark_out_env = std::getenv("HFT_BENCHMARK_OUT");
    const std::filesystem::path benchmark_out =
        benchmark_out_env != nullptr ? std::filesystem::path(benchmark_out_env)
                                     : std::filesystem::path(HFT_CANONICAL_BENCH_OUT);
    std::filesystem::create_directories(benchmark_out.parent_path());
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc) + 2);
    bool skip_value = false;
    for (int index = 0; index < argc; ++index) {
        const std::string argument = argv[index];
        if (skip_value) {
            skip_value = false;
            continue;
        }
        if (argument == "--benchmark_out" || argument == "--benchmark_out_format") {
            skip_value = true;
            continue;
        }
        if (argument.rfind("--benchmark_out=", 0) == 0 ||
            argument.rfind("--benchmark_out_format=", 0) == 0) {
            continue;
        }
        arguments.push_back(argument);
    }
    // Enforce the canonical artifact path regardless of caller flags.
    arguments.push_back("--benchmark_out=" + benchmark_out.string());
    arguments.emplace_back("--benchmark_out_format=json");
    std::vector<char*> argv_storage;
    argv_storage.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv_storage.push_back(argument.data());
    }
    int normalized_argc = static_cast<int>(argv_storage.size());
    benchmark::Initialize(&normalized_argc, argv_storage.data());
    if (benchmark::ReportUnrecognizedArguments(normalized_argc, argv_storage.data())) {
        return 1;
    }
    const char* latency_env = std::getenv("HFT_LATENCY_CSV");
    const std::filesystem::path latency_out =
        latency_env != nullptr ? std::filesystem::path(latency_env)
                               : std::filesystem::path(HFT_CANONICAL_LATENCY_OUT);
    std::filesystem::create_directories(latency_out.parent_path());
    const std::string latency_path = latency_out.string();
    write_latency_samples(latency_path.c_str());
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
