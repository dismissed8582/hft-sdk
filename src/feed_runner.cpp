#include "hft/Config.hpp"
#include "hft/HttpServer.hpp"
#include "hft/Logging.hpp"
#include "hft/Metrics.hpp"
#include "hft/SignalPipeline.hpp"
#include "hft/TickData.hpp"
#include "hft/Version.hpp"
#include "hft/ingest/Csv.hpp"
#include "hft/ingest/Feed.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cmath>
#include <limits>
#include <poll.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hft;

namespace {

std::atomic<bool> g_running{true};

void handle_signal(int) {
    g_running.store(false, std::memory_order_release);
}

struct Options {
    std::string config;
    std::string replay;
    bool stdin_mode{false};
    std::string http_address{"127.0.0.1"};
    std::uint16_t http_port{9100};
    std::int64_t sample_ms{250};
    std::int64_t lifetime_ms{100};
    std::uint32_t max_modifications{3};
    std::int64_t mod_time_ms{50};
    std::int64_t stall_ms{5'000};
    double replay_speed{0.0};
    std::string log_level{"info"};
};

struct CliOverrides {
    std::optional<std::string> config;
    std::optional<std::string> replay;
    std::optional<bool> stdin_mode;
    std::optional<std::string> http_address;
    std::optional<std::int64_t> http_port;
    std::optional<std::int64_t> sample_ms;
    std::optional<std::int64_t> lifetime_ms;
    std::optional<std::int64_t> max_modifications;
    std::optional<std::int64_t> mod_time_ms;
    std::optional<std::int64_t> stall_ms;
    std::optional<double> replay_speed;
    std::optional<std::string> log_level;
};

[[nodiscard]] std::int64_t parse_i64(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer for ") + name + ": " +
                                 value);
    }
}

[[nodiscard]] double parse_double(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid number for ") + name + ": " +
                                 value);
    }
}

[[nodiscard]] CliOverrides parse_cli(int argc, char** argv) {
    CliOverrides overrides{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: hft_feed [options]\n"
                << "  --config FILE          key=value config file\n"
                << "  --replay FILE          replay a tick CSV\n"
                << "  --stdin                read normalized CSV lines from stdin\n"
                << "  --http-address ADDR    metrics/health bind address\n"
                << "  --http-port N          metrics/health port (default 9100)\n"
                << "  --sample-ms N          snapshot interval (default 250)\n"
                << "  --lifetime-ms N        lifetime filter threshold\n"
                << "  --max-mods N           modification-count threshold\n"
                << "  --mod-time-ms N        modification-time threshold\n"
                << "  --stall-ms N           health stall threshold\n"
                << "  --replay-speed X       0 = as fast as possible\n"
                << "  --log-level LEVEL      trace|debug|info|warn|error|off\n"
                << "\n"
                << "stdin line protocol:\n"
                << "  timestamp_ns,event_type,side,order_id,price_ticks,quantity,\n"
                << "  bid_price_ticks,ask_price_ticks[,parent_order_id[,sequence]]\n";
            std::exit(0);
        }
        if (argument == "--stdin") {
            overrides.stdin_mode = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--config") {
            overrides.config = value;
        } else if (argument == "--replay") {
            overrides.replay = value;
        } else if (argument == "--http-address") {
            overrides.http_address = value;
        } else if (argument == "--http-port") {
            overrides.http_port = parse_i64(value, "http-port");
        } else if (argument == "--sample-ms") {
            overrides.sample_ms = parse_i64(value, "sample-ms");
        } else if (argument == "--lifetime-ms") {
            overrides.lifetime_ms = parse_i64(value, "lifetime-ms");
        } else if (argument == "--max-mods") {
            overrides.max_modifications = parse_i64(value, "max-mods");
        } else if (argument == "--mod-time-ms") {
            overrides.mod_time_ms = parse_i64(value, "mod-time-ms");
        } else if (argument == "--stall-ms") {
            overrides.stall_ms = parse_i64(value, "stall-ms");
        } else if (argument == "--replay-speed") {
            overrides.replay_speed = parse_double(value, "replay-speed");
        } else if (argument == "--log-level") {
            overrides.log_level = value;
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    return overrides;
}

[[nodiscard]] Options merge_options(const CliOverrides& cli) {
    Options options{};
    config::Config config_file;
    if (cli.config) {
        config_file.load(*cli.config);
        options.config = *cli.config;
    }
    options.replay = config_file.get_string("feed.replay", options.replay);
    options.stdin_mode = config_file.get_bool("feed.stdin", options.stdin_mode);
    options.http_address =
        config_file.get_string("http.address", options.http_address);
    const auto port = cli.http_port ? *cli.http_port
        : config_file.get_int("http.port", options.http_port);
    if (port < 0 || port > 65535) throw std::runtime_error("http.port must be 0..65535");
    options.http_port = static_cast<std::uint16_t>(port);
    options.sample_ms = config_file.get_int("feed.sample_ms", options.sample_ms);
    options.lifetime_ms =
        config_file.get_int("feed.lifetime_ms", options.lifetime_ms);
    const auto max_mods = cli.max_modifications ? *cli.max_modifications
        : config_file.get_int("feed.max_modifications", options.max_modifications);
    if (max_mods < 0 || max_mods > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("feed.max_modifications is out of range");
    }
    options.max_modifications = static_cast<std::uint32_t>(max_mods);
    options.mod_time_ms =
        config_file.get_int("feed.modification_time_ms", options.mod_time_ms);
    options.stall_ms = config_file.get_int("feed.stall_ms", options.stall_ms);
    options.replay_speed =
        config_file.get_double("replay.speed", options.replay_speed);
    options.log_level = config_file.get_string("log.level", options.log_level);
    if (cli.replay) options.replay = *cli.replay;
    if (cli.stdin_mode) options.stdin_mode = *cli.stdin_mode;
    if (cli.http_address) options.http_address = *cli.http_address;
    if (cli.sample_ms) options.sample_ms = *cli.sample_ms;
    if (cli.lifetime_ms) options.lifetime_ms = *cli.lifetime_ms;
    if (cli.mod_time_ms) options.mod_time_ms = *cli.mod_time_ms;
    if (cli.stall_ms) options.stall_ms = *cli.stall_ms;
    if (cli.replay_speed) options.replay_speed = *cli.replay_speed;
    if (cli.log_level) options.log_level = *cli.log_level;
    constexpr auto max_ms = std::numeric_limits<std::int64_t>::max() /
                            hft::kNanosecondsPerMillisecond;
    if (options.sample_ms <= 0 || options.sample_ms > max_ms ||
        options.stall_ms <= 0 || options.stall_ms > max_ms ||
        options.lifetime_ms < 0 || options.lifetime_ms > max_ms ||
        options.mod_time_ms < 0 || options.mod_time_ms > max_ms ||
        !std::isfinite(options.replay_speed) || options.replay_speed < 0) {
        throw std::runtime_error("invalid interval, filter threshold, or replay speed");
    }
    return options;
}

[[nodiscard]] bool parse_event_type(std::string_view text, hft::EventType& type) {
    std::string value;
    value.reserve(text.size());
    for (const char character : text) {
        value.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(character))));
    }
    if (value == "NEW" || value == "ADD" || value == "0") {
        type = hft::EventType::New;
    } else if (value == "MODIFY" || value == "UPDATE" || value == "1") {
        type = hft::EventType::Modify;
    } else if (value == "CANCEL" || value == "DELETE" || value == "2") {
        type = hft::EventType::Cancel;
    } else if (value == "TRADE" || value == "FILL" || value == "3") {
        type = hft::EventType::Trade;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_side(std::string_view text, hft::Side& side) {
    std::string value;
    value.reserve(text.size());
    for (const char character : text) {
        value.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(character))));
    }
    if (value == "BID" || value == "B" || value == "1") {
        side = hft::Side::Bid;
    } else if (value == "ASK" || value == "A" || value == "2") {
        side = hft::Side::Ask;
    } else if (value == "BUY" || value == "3") {
        side = hft::Side::Buy;
    } else if (value == "SELL" || value == "4") {
        side = hft::Side::Sell;
    } else if (value == "NONE" || value == "0") {
        side = hft::Side::None;
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Runtime {
    hft::metrics::Registry metrics;
    hft::ingest::FeedStats stats;
    hft::ingest::GapDetector gaps;
    std::atomic<int> state{static_cast<int>(hft::ingest::FeedState::Starting)};
    std::int64_t started_ns{steady_now_ns()};
    bool degraded{false};
    std::int64_t stall_ns{5'000'000'000};
};

[[nodiscard]] std::string health_json(const Runtime& runtime) {
    const std::int64_t now = steady_now_ns();
    const std::int64_t last = runtime.stats.last_update_ns.load(
        std::memory_order_relaxed);
    const double age = last > 0 ? static_cast<double>(now - last) / 1e9 : -1.0;
    std::ostringstream output;
    output << "{\"status\":\""
           << hft::ingest::feed_state_name(
                  static_cast<hft::ingest::FeedState>(runtime.state.load()))
           << "\",\"version\":\"" << hft::version_string << "\",\"uptime_s\":"
           << static_cast<double>(now - runtime.started_ns) / 1e9
           << ",\"events\":" << runtime.stats.messages.load()
           << ",\"trades\":" << runtime.stats.trades.load()
           << ",\"gaps\":" << runtime.stats.gaps.load()
           << ",\"reconnects\":" << runtime.stats.reconnects.load()
           << ",\"dropped\":" << runtime.stats.dropped.load()
           << ",\"last_event_age_s\":" << age << "}\n";
    return output.str();
}

void update_state(Runtime& runtime) {
    const std::int64_t now = steady_now_ns();
    const std::int64_t last = runtime.stats.last_update_ns.load(
        std::memory_order_relaxed);
    const auto state = last != 0 && now - last > runtime.stall_ns
        ? hft::ingest::FeedState::Stalled
        : runtime.degraded ? hft::ingest::FeedState::Degraded
        : last == 0 ? hft::ingest::FeedState::Starting : hft::ingest::FeedState::Live;
    runtime.state.store(static_cast<int>(state), std::memory_order_relaxed);
}

class FeedProcessor {
public:
    FeedProcessor(const Options& options, Runtime& runtime)
        : runtime_(runtime),
          pipeline_(std::make_unique<hft::signal::SignalPipeline<>>(
              hft::obi::FilterThresholds{
                  options.lifetime_ms * hft::kNanosecondsPerMillisecond,
                  options.max_modifications,
                  options.mod_time_ms * hft::kNanosecondsPerMillisecond},
              options.sample_ms * hft::kNanosecondsPerMillisecond)) {
        events_total_ = &runtime_.metrics.counter(
            "hft_events_total", "Normalized market events processed");
        trades_total_ = &runtime_.metrics.counter(
            "hft_trades_total", "Trade events processed");
        snapshots_total_ = &runtime_.metrics.counter(
            "hft_snapshots_total", "Signal snapshots captured");
        parse_errors_ = &runtime_.metrics.counter(
            "hft_parse_errors_total", "Malformed input lines");
        last_timestamp_ = &runtime_.metrics.gauge(
            "hft_last_event_timestamp_ns", "Last event timestamp in nanoseconds");
        state_gauge_ = &runtime_.metrics.gauge(
            "hft_feed_state", "0=starting,1=live,2=degraded,3=stalled,4=stopped");
        process_latency_ = &runtime_.metrics.histogram(
            "hft_batch_latency_ns", {1e3, 1e4, 1e5, 1e6, 1e7},
            "Batch processing latency in nanoseconds");
    }

    void process(std::span<const hft::MarketEvent> events) {
        if (events.empty()) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        const std::size_t before = pipeline_->snapshots().size();
        pipeline_->process(events);
        const std::size_t after = pipeline_->snapshots().size();
        const auto finish = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::nano>(
            finish - start).count();
        process_latency_->observe(elapsed);
        events_total_->increment(events.size());
        snapshots_total_->increment(after - before);
        std::uint64_t trades = 0;
        for (const hft::MarketEvent& event : events) {
            if (hft::is_trade(event.type)) {
                ++trades;
            }
        }
        trades_total_->increment(trades);
        last_timestamp_->set(events.back().timestamp_ns);
        runtime_.stats.messages.fetch_add(events.size(), std::memory_order_relaxed);
        runtime_.stats.trades.fetch_add(trades, std::memory_order_relaxed);
        runtime_.stats.last_update_ns.store(steady_now_ns(),
                                            std::memory_order_relaxed);
        const auto dropped = pipeline_->out_of_order_events() + pipeline_->tracker_dropped();
        runtime_.stats.dropped.store(dropped, std::memory_order_relaxed);
        if (dropped > 0 || pipeline_->dropped_snapshots() > 0 ||
            pipeline_->skipped_samples() > 0) runtime_.degraded = true;
        refresh_health();
    }

    void set_state(hft::ingest::FeedState state) {
        runtime_.state.store(static_cast<int>(state), std::memory_order_relaxed);
        state_gauge_->set(static_cast<int>(state));
    }

    void refresh_health() {
        update_state(runtime_);
        state_gauge_->set(runtime_.state.load(std::memory_order_relaxed));
    }

    void parse_error() {
        parse_errors_->increment();
        runtime_.degraded = true;
        refresh_health();
    }
    [[nodiscard]] Runtime& runtime() { return runtime_; }

private:
    Runtime& runtime_;
    std::unique_ptr<hft::signal::SignalPipeline<>> pipeline_;
    hft::metrics::Counter* events_total_{};
    hft::metrics::Counter* trades_total_{};
    hft::metrics::Counter* snapshots_total_{};
    hft::metrics::Counter* parse_errors_{};
    hft::metrics::Gauge* last_timestamp_{};
    hft::metrics::Gauge* state_gauge_{};
    hft::metrics::Histogram* process_latency_{};
};

void run_replay(const Options& options, FeedProcessor& processor) {
    logging::info("feed", "loading replay " + options.replay);
    std::vector<hft::MarketEvent> events = hft::data::load_ticks_csv(options.replay);
    hft::data::sort_ticks_by_timestamp(events);
    if (events.empty()) {
        throw std::runtime_error("replay file contains no events");
    }
    constexpr std::size_t batch_size = 4096;
    const std::int64_t first_timestamp = events.front().timestamp_ns;
    const auto wall_start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < events.size() && g_running.load();) {
        const std::size_t end = std::min(index + batch_size, events.size());
        if (options.replay_speed > 0.0) {
            const std::int64_t target_ns = static_cast<std::int64_t>(
                static_cast<double>(events[end - 1].timestamp_ns - first_timestamp) /
                options.replay_speed);
            const auto target = wall_start + std::chrono::nanoseconds(target_ns);
            while (g_running.load() && std::chrono::steady_clock::now() < target) {
                processor.refresh_health();
                std::this_thread::sleep_until(std::min(target,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
            }
            if (!g_running.load()) break;
        }
        processor.process(std::span<const hft::MarketEvent>(events.data() + index,
                                                            end - index));
        index = end;
    }
}

void run_stdin([[maybe_unused]] const Options& options, FeedProcessor& processor) {
    logging::info("feed", "reading normalized CSV lines from stdin");
    std::string line;
    std::array<std::string_view, 16> fields{};
    std::vector<hft::MarketEvent> batch;
    batch.reserve(4096);
    const auto process_line = [&]() {
        if (line.empty() || line.front() == '#') {
            return;
        }
        const std::size_t count = hft::ingest::split_fields(line, fields);
        if (count < 8) {
            processor.parse_error();
            return;
        }
        hft::MarketEvent event{};
        std::int64_t sequence = 0;
        bool has_sequence = false;
        const bool ok =
            hft::ingest::parse_integer(fields[0], event.timestamp_ns) &&
            parse_event_type(fields[1], event.type) &&
            parse_side(fields[2], event.side) &&
            hft::ingest::parse_integer(fields[3], event.order_id) &&
            hft::ingest::parse_integer(fields[4], event.price_ticks) &&
            hft::ingest::parse_integer(fields[5], event.quantity) &&
            hft::ingest::parse_integer(fields[6], event.bid_price_ticks) &&
            hft::ingest::parse_integer(fields[7], event.ask_price_ticks);
        if (!ok) {
            processor.parse_error();
            return;
        }
        if (count >= 9 && !fields[8].empty()) {
            if (!hft::ingest::parse_integer(fields[8], event.parent_order_id)) {
                processor.parse_error();
                return;
            }
        }
        if (count >= 10 && !fields[9].empty()) {
            if (!hft::ingest::parse_integer(fields[9], sequence) || sequence < 0) {
                processor.parse_error();
                return;
            }
            has_sequence = true;
        }
        if (has_sequence) {
            const auto gap = processor.runtime().gaps.check("stdin", sequence);
            if (gap.missing > 0) {
                processor.runtime().stats.gaps.fetch_add(gap.missing,
                                                          std::memory_order_relaxed);
                processor.runtime().degraded = true;
                processor.refresh_health();
                logging::warn("feed", "sequence gap: " +
                                          std::to_string(gap.missing) + " missing");
            }
        }
        batch.push_back(event);
        if (batch.size() >= 4096) {
            processor.process(batch);
            batch.clear();
        }
    };
    // Read chunks rather than getline: poll must also cover partial lines and
    // idle pipes, so signals and health transitions never wait for a newline.
    constexpr std::size_t max_line_bytes = 1 << 20;
    bool discarding = false;
    char input[8192];
    while (g_running.load()) {
        processor.refresh_health();
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 50);
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("stdin poll failed");
        }
        if (ready == 0) continue;
        if (descriptor.revents & POLLNVAL) throw std::runtime_error("invalid stdin");
        const ssize_t received = ::read(STDIN_FILENO, input, sizeof(input));
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            throw std::runtime_error("stdin read failed");
        }
        if (received == 0) {
            if (!line.empty() && !discarding) process_line();
            break;
        }
        for (ssize_t index = 0; index < received; ++index) {
            if (input[index] == '\n') {
                if (!discarding) process_line();
                line.clear();
                discarding = false;
            } else if (!discarding) {
                line.push_back(input[index]);
                if (line.size() > max_line_bytes) {
                    processor.parse_error();
                    line.clear();
                    discarding = true;
                }
            }
        }
        // Publish a short batch even if the producer keeps stdin open.
        if (!batch.empty()) {
            processor.process(batch);
            batch.clear();
        }
    }
    if (!batch.empty()) {
        processor.process(batch);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
#ifdef SIGPIPE
        std::signal(SIGPIPE, SIG_IGN);
#endif

        const CliOverrides cli = parse_cli(argc, argv);
        const Options options = merge_options(cli);
        logging::Logger::instance().set_level(
            logging::level_from_string(options.log_level));

        Runtime runtime{};
        runtime.stall_ns = options.stall_ms * hft::kNanosecondsPerMillisecond;
        FeedProcessor processor(options, runtime);
        processor.set_state(hft::ingest::FeedState::Starting);

        http::Server server;
        const bool http_started = server.start(
            options.http_address, options.http_port,
            [&runtime](const http::Request& request) {
                if (request.path == "/metrics") {
                    return http::Response{200, "text/plain; version=0.0.4",
                                          runtime.metrics.render()};
                }
                if (request.path == "/health" || request.path == "/") {
                    return http::Response{200, "application/json",
                                          health_json(runtime)};
                }
                return http::Response{404, "text/plain", "not found\n"};
            });
        if (!http_started) {
            throw std::runtime_error("unable to bind metrics/health endpoint");
        }
        logging::info("feed", "hft_feed " + std::string(hft::version_string) +
                                  " listening on " + options.http_address + ":" +
                                  std::to_string(options.http_port));

        if (!options.replay.empty()) {
            run_replay(options, processor);
        } else if (options.stdin_mode) {
            run_stdin(options, processor);
        } else {
            throw std::runtime_error("choose --replay FILE or --stdin");
        }

        processor.set_state(hft::ingest::FeedState::Stopped);
        server.stop();
        logging::info("feed", "stopped: events=" +
                                  std::to_string(runtime.stats.messages.load()) +
                                  " trades=" +
                                  std::to_string(runtime.stats.trades.load()) +
                                  " gaps=" +
                                  std::to_string(runtime.stats.gaps.load()) +
                                  " dropped=" +
                                  std::to_string(runtime.stats.dropped.load()));
        return 0;
    } catch (const std::exception& error) {
        logging::error("feed", error.what());
        return 1;
    }
}

