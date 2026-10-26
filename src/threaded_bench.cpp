#include "hft/Disruptor.hpp"
#include "hft/Types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::size_t events{1'000'000};
    int runs{15};
    int warmup{1};
    std::string out{"benchmarks/results/threaded_bench.json"};
    bool ring{true};
    bool queue{true};
};

[[nodiscard]] std::size_t parse_size(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer for ") + name + ": " +
                                 value);
    }
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: hft_threaded_bench [options]\n"
                << "  --events N   events per run (default 1000000)\n"
                << "  --warmup N   warmup runs (default 1)\n"
                << "  --runs N     measured runs (default 15)\n"
                << "  --out FILE   JSON output path\n"
                << "  --ring       run the Disruptor ring\n"
                << "  --queue      run the mutex queue\n";
            std::exit(0);
        }
        if (argument == "--ring") {
            options.ring = true;
            continue;
        }
        if (argument == "--queue") {
            options.queue = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--events") {
            options.events = parse_size(value, "events");
        } else if (argument == "--runs") {
            options.runs = static_cast<int>(parse_size(value, "runs"));
        } else if (argument == "--warmup") {
            options.warmup = static_cast<int>(parse_size(value, "warmup"));
        } else if (argument == "--out") {
            options.out = value;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (options.events == 0 || options.runs <= 0) {
        throw std::runtime_error("events and runs must be positive");
    }
    return options;
}

[[nodiscard]] hft::MarketEvent make_event(std::size_t sequence) {
    hft::MarketEvent event{};
    event.order_id = sequence + 1;
    event.timestamp_ns = static_cast<std::int64_t>(sequence);
    event.price_ticks = 100;
    event.bid_price_ticks = 100;
    event.ask_price_ticks = 101;
    event.quantity = 1;
    return event;
}

[[nodiscard]] double queue_run(std::size_t events) {
    std::queue<hft::MarketEvent> queue;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> producer_done{false};
    std::uint64_t sink = 0;

    const auto start = Clock::now();
    std::thread producer([&]() {
        for (std::size_t sequence = 0; sequence < events; ++sequence) {
            hft::MarketEvent event = make_event(sequence);
            {
                const std::lock_guard<std::mutex> lock(mutex);
                queue.push(event);
            }
            condition.notify_one();
        }
        producer_done.store(true, std::memory_order_release);
        condition.notify_all();
    });
    std::thread consumer([&]() {
        while (true) {
            hft::MarketEvent event{};
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [&]() {
                    return !queue.empty() ||
                           producer_done.load(std::memory_order_acquire);
                });
                if (queue.empty()) {
                    if (producer_done.load(std::memory_order_acquire)) {
                        break;
                    }
                    continue;
                }
                event = queue.front();
                queue.pop();
            }
            sink += event.order_id;
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });
    producer.join();
    consumer.join();
    const auto finish = Clock::now();
    if (consumed.load() != events) {
        throw std::runtime_error("queue consumer did not drain all events");
    }
    asm volatile("" : : "r"(sink) : "memory");
    return std::chrono::duration<double, std::nano>(finish - start).count();
}

[[nodiscard]] double ring_run(std::size_t events) {
    constexpr std::size_t capacity = std::size_t{1} << 16;
    using Ring = hft::disruptor::RingBuffer<hft::MarketEvent, capacity>;
    using Consumer = hft::disruptor::ConsumerBarrier<
        hft::MarketEvent, capacity, hft::disruptor::BusySpinWaitStrategy>;
    using Producer = hft::disruptor::ProducerBarrier<
        hft::MarketEvent, capacity, hft::disruptor::BusySpinWaitStrategy>;

    Ring ring;
    Consumer consumer(ring);
    Producer producer(ring, consumer);
    std::uint64_t sink = 0;

    const auto start = Clock::now();
    std::thread producer_thread([&]() {
        for (std::size_t sequence = 0; sequence < events; ++sequence) {
            auto ticket = producer.claim();
            *ticket.entry = make_event(sequence);
            producer.publish(ticket);
        }
    });
    std::thread consumer_thread([&]() {
        for (std::size_t sequence = 0; sequence < events; ++sequence) {
            const auto available = consumer.wait_for(
                static_cast<std::int64_t>(sequence));
            sink += consumer.get(available).order_id;
            consumer.advance_to(available);
        }
    });
    producer_thread.join();
    consumer_thread.join();
    const auto finish = Clock::now();
    asm volatile("" : : "r"(sink) : "memory");
    return std::chrono::duration<double, std::nano>(finish - start).count();
}

struct Summary {
    double total_ns_mean{0.0};
    double total_ns_median{0.0};
    double total_ns_std{0.0};
    double per_event_ns_mean{0.0};
    double per_event_ns_median{0.0};
    double per_event_ns_p50{0.0};
    double per_event_ns_p99{0.0};
    double events_per_second{0.0};
    std::vector<double> samples;
};

[[nodiscard]] Summary summarize(const std::vector<double>& totals,
                                std::size_t events) {
    Summary summary{};
    summary.samples = totals;
    if (totals.empty()) {
        return summary;
    }
    const double mean = std::accumulate(totals.begin(), totals.end(), 0.0) /
                        static_cast<double>(totals.size());
    double variance = 0.0;
    for (const double value : totals) {
        const double delta = value - mean;
        variance += delta * delta;
    }
    if (totals.size() > 1) {
        variance /= static_cast<double>(totals.size() - 1);
    }
    summary.total_ns_mean = mean;
    std::vector<double> sorted_totals = totals;
    std::sort(sorted_totals.begin(), sorted_totals.end());
    const double middle = sorted_totals[sorted_totals.size() / 2];
    summary.total_ns_median = middle;
    summary.total_ns_std = std::sqrt(variance);
    summary.per_event_ns_mean = mean / static_cast<double>(events);
    summary.per_event_ns_median = middle / static_cast<double>(events);

    std::vector<double> per_event;
    per_event.reserve(totals.size());
    for (const double value : totals) {
        per_event.push_back(value / static_cast<double>(events));
    }
    std::sort(per_event.begin(), per_event.end());
    const auto percentile = [&](double quantile) {
        if (per_event.empty()) {
            return 0.0;
        }
        const double position = quantile * static_cast<double>(per_event.size() - 1);
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, per_event.size() - 1);
        const double fraction = position - static_cast<double>(lower);
        return per_event[lower] * (1.0 - fraction) +
               per_event[upper] * fraction;
    };
    summary.per_event_ns_p50 = percentile(0.50);
    summary.per_event_ns_p99 = percentile(0.99);
    summary.events_per_second = 1e9 / summary.per_event_ns_mean;
    return summary;
}

void write_json(const Options& options, const Summary& queue,
                const Summary& ring) {
    const std::filesystem::path path(options.out);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("unable to write " + options.out);
    }
    output << std::setprecision(12);
    output << "{\n";
    output << "  \"events\": " << options.events << ",\n";
    output << "  \"runs\": " << options.runs << ",\n";
    const auto emit = [&](const char* name, const Summary& summary, bool comma) {
        output << "  \"" << name << "\": {\n";
        output << "    \"total_ns_mean\": " << summary.total_ns_mean << ",\n";
        output << "    \"total_ns_median\": " << summary.total_ns_median << ",\n";
        output << "    \"total_ns_std\": " << summary.total_ns_std << ",\n";
        output << "    \"per_event_ns_mean\": " << summary.per_event_ns_mean << ",\n";
        output << "    \"per_event_ns_median\": " << summary.per_event_ns_median << ",\n";
        output << "    \"per_event_ns_p50\": " << summary.per_event_ns_p50 << ",\n";
        output << "    \"per_event_ns_p99\": " << summary.per_event_ns_p99 << ",\n";
        output << "    \"events_per_second\": " << summary.events_per_second << ",\n";
        output << "    \"samples_ns\": [";
        for (std::size_t index = 0; index < summary.samples.size(); ++index) {
            if (index != 0) {
                output << ", ";
            }
            output << summary.samples[index];
        }
        output << "]\n";
        output << "  }" << (comma ? "," : "") << "\n";
    };
    bool emitted = false;
    if (options.queue) {
        emit("queue_mutex", queue, options.ring);
        emitted = true;
    }
    if (options.ring) {
        emit("ring_buffer", ring, false);
        emitted = true;
    }
    (void)emitted;
    output << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::vector<double> queue_totals;
        std::vector<double> ring_totals;
        if (options.queue) {
            for (int run = 0; run < options.warmup; ++run) {
                (void)queue_run(options.events);
            }
            queue_totals.reserve(static_cast<std::size_t>(options.runs));
            for (int run = 0; run < options.runs; ++run) {
                queue_totals.push_back(queue_run(options.events));
            }
        }
        if (options.ring) {
            for (int run = 0; run < options.warmup; ++run) {
                (void)ring_run(options.events);
            }
            ring_totals.reserve(static_cast<std::size_t>(options.runs));
            for (int run = 0; run < options.runs; ++run) {
                ring_totals.push_back(ring_run(options.events));
            }
        }
        const Summary queue = summarize(queue_totals, options.events);
        const Summary ring = summarize(ring_totals, options.events);
        write_json(options, queue, ring);
        if (options.queue) {
            std::cout << "queue events=" << options.events
                      << " per_event_ns_median=" << queue.per_event_ns_median
                      << " p99_ns=" << queue.per_event_ns_p99
                      << " eps=" << queue.events_per_second << '\n';
        }
        if (options.ring) {
            std::cout << "ring  events=" << options.events
                      << " per_event_ns_median=" << ring.per_event_ns_median
                      << " p99_ns=" << ring.per_event_ns_p99
                      << " eps=" << ring.events_per_second << '\n';
        }
        if (options.queue && options.ring &&
            queue.per_event_ns_mean > 0.0 && ring.per_event_ns_mean > 0.0) {
            std::cout << "speedup="
                      << queue.per_event_ns_median / ring.per_event_ns_median
                      << "x\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

