#include "hft/OBIFilter.hpp"
#include "hft/SignalPipeline.hpp"
#include "hft/TickData.hpp"
#include "hft/ingest/Binance.hpp"
#include "hft/ingest/CryptoLobCsv.hpp"
#include "hft/ingest/Hyperliquid.hpp"
#include "hft/ingest/Lobster.hpp"
#include "hft/ingest/Replay.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct Options {
    std::string mode;
    std::string file;
    std::string coin;
    std::string asset;
    std::string exchange;
    std::string format{"spot"};
    double price_scale{1e8};
    double quantity_scale{1e8};
    std::int64_t base_timestamp_ns{0};
    std::int64_t timestamp_step_ns{1'000};
    std::uint64_t limit{0};
    bool metrics{false};
    std::int64_t sample_ms{1000};
    std::int64_t lifetime_ms{100};
    std::int64_t mod_time_ms{2};
    std::uint32_t max_mods{3};
    std::string scores;
    std::string regimes;
    std::string regime_scheme{"UF"};
    std::int64_t regime_horizon_seconds{1};
    std::string regime_signal{"volume"};
    std::size_t max_snapshots{std::size_t{1} << 19};
};

[[nodiscard]] hft::obi::FilterScheme parse_scheme(const std::string& value) {
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(hft::obi::FilterScheme::Count); ++index) {
        if (hft::obi::kFilterSchemeNames[index] == value) {
            return static_cast<hft::obi::FilterScheme>(index);
        }
    }
    throw std::runtime_error("unknown regime scheme: " + value);
}

[[nodiscard]] std::size_t parse_horizon(std::int64_t seconds) {
    for (std::size_t index = 0; index < hft::signal::kHorizonSeconds.size(); ++index) {
        if (hft::signal::kHorizonSeconds[index] == seconds) {
            return index;
        }
    }
    throw std::runtime_error("unsupported regime horizon: " + std::to_string(seconds));
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options{};
    if (argc < 2) {
        throw std::runtime_error(
            "usage: hft_replay <binance-trades|binance-depth|hl-book|hl-orders|"
            "hl-trades|crypto-depth|lobster|csv> --file PATH [options]");
    }
    options.mode = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--metrics") {
            options.metrics = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout
                << "modes:\n"
                << "  binance-trades --file CSV [--format spot|futures|agg]\n"
                << "  binance-depth  --file CSV\n"
                << "  hl-book        --file JSONL[.gz] [--coin BTC] "
                   "[--base-ts NS] [--step-ns NS]\n"
                << "  hl-orders      --file .data[.gz]\n"
                << "  hl-trades      --file JSONL[.gz] [--coin BTC]\n"
                << "  crypto-depth   --file CSV [--asset BTCUSDT] [--exchange binance]\n"
                << "  lobster        --file messages.csv [--base-ts NS]\n"
                << "  csv            --file normalized.csv\n"
                << "common: --price-scale N --qty-scale N --limit N\n"
                << "        --metrics [--scores out.csv] [--sample-ms N]\n"
                << "        --regimes out.csv [--regime-scheme UF|LF|MF|MTF]\n"
                << "        [--regime-horizon 1|10|30|50] "
                   "[--regime-signal volume|count] [--max-snapshots N]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--file") {
            options.file = value;
        } else if (argument == "--coin") {
            options.coin = value;
        } else if (argument == "--asset") {
            options.asset = value;
        } else if (argument == "--exchange") {
            options.exchange = value;
        } else if (argument == "--format") {
            options.format = value;
        } else if (argument == "--price-scale") {
            options.price_scale = std::stod(value);
        } else if (argument == "--qty-scale") {
            options.quantity_scale = std::stod(value);
        } else if (argument == "--base-ts") {
            options.base_timestamp_ns = std::stoll(value);
        } else if (argument == "--step-ns") {
            options.timestamp_step_ns = std::stoll(value);
        } else if (argument == "--limit") {
            options.limit = static_cast<std::uint64_t>(std::stoull(value));
        } else if (argument == "--sample-ms") {
            options.sample_ms = std::stoll(value);
        } else if (argument == "--lifetime-ms") {
            options.lifetime_ms = std::stoll(value);
        } else if (argument == "--mod-time-ms") {
            options.mod_time_ms = std::stoll(value);
        } else if (argument == "--max-mods") {
            options.max_mods = static_cast<std::uint32_t>(std::stoul(value));
        } else if (argument == "--scores") {
            options.scores = value;
        } else if (argument == "--regimes") {
            options.regimes = value;
        } else if (argument == "--regime-scheme") {
            options.regime_scheme = value;
            (void)parse_scheme(options.regime_scheme);
        } else if (argument == "--regime-horizon") {
            options.regime_horizon_seconds = std::stoll(value);
            (void)parse_horizon(options.regime_horizon_seconds);
        } else if (argument == "--regime-signal") {
            if (value != "volume" && value != "count") {
                throw std::runtime_error("regime signal must be volume or count");
            }
            options.regime_signal = value;
        } else if (argument == "--max-snapshots") {
            options.max_snapshots = static_cast<std::size_t>(std::stoull(value));
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (options.file.empty()) {
        throw std::runtime_error("--file is required");
    }
    return options;
}

class ObiHandler {
public:
    explicit ObiHandler(std::uint64_t limit) : limit_(limit) {}

    void operator()(const hft::MarketEvent& event) {
        if (limit_ != 0 && events_ >= limit_) {
            return;
        }
        const bool trade = hft::is_trade(event.type);
        window_.push(event.timestamp_ns, event.side, trade, 0x0Fu);
        const auto value =
            hft::obi::order_book_imbalance(
                window_.counts(hft::obi::FilterScheme::Unfiltered));
        if (value.valid) {
            obi_sum_ += value.value;
        }
        ++events_;
    }

    [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
    [[nodiscard]] double obi_sum() const noexcept { return obi_sum_; }

private:
    hft::obi::BucketedRollingCounts<> window_{hft::kNanosecondsPerSecond};
    std::uint64_t limit_{0};
    std::uint64_t events_{0};
    double obi_sum_{0.0};
};

void ensure_parent(const std::string& path) {
    const std::filesystem::path output(path);
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path());
    }
}

void write_scores(const std::string& path,
                  const std::vector<hft::signal::SignalMetrics>& rows) {
    ensure_parent(path);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("unable to write " + path);
    }
    output << "scheme,horizon_s,samples,pearson,r2,regime_pearson,regime_r2,"
              "trade_samples,trade_pearson,trade_r2,trade_regime_pearson,"
              "trade_regime_r2,volume_samples,volume_pearson,volume_r2,"
              "volume_regime_pearson,volume_regime_r2,volume_trade_samples,"
              "volume_trade_pearson,volume_trade_r2,volume_trade_regime_pearson,"
              "volume_trade_regime_r2\n";
    output << std::setprecision(12);
    for (const auto& row : rows) {
        output << row.scheme << ',' << row.horizon_seconds << ',' << row.samples << ','
               << row.pearson.pearson << ',' << row.pearson.r2 << ','
               << row.regime.pearson << ',' << row.regime.r2 << ','
               << row.trade_samples << ',' << row.trade_pearson.pearson << ','
               << row.trade_pearson.r2 << ',' << row.trade_regime.pearson << ','
               << row.trade_regime.r2 << ',' << row.volume_samples << ','
               << row.volume_pearson.pearson << ',' << row.volume_pearson.r2 << ','
               << row.volume_regime.pearson << ',' << row.volume_regime.r2 << ','
               << row.volume_trade_samples << ','
               << row.volume_trade_pearson.pearson << ','
               << row.volume_trade_pearson.r2 << ','
               << row.volume_trade_regime.pearson << ','
               << row.volume_trade_regime.r2 << '\n';
    }
}

void write_regimes(const std::string& path,
                   const std::vector<hft::signal::RegimeTuple>& tuples) {
    ensure_parent(path);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("unable to write " + path);
    }
    output << "timestamp_ns,obi_regime,return_regime\n";
    for (const auto& tuple : tuples) {
        output << tuple.timestamp_ns << ',' << static_cast<int>(tuple.obi_regime)
               << ',' << static_cast<int>(tuple.return_regime) << '\n';
    }
}

void print_stats(const std::string& mode, const hft::ingest::ReplayStats& stats,
                 const ObiHandler& handler) {
    std::cout << "mode=" << mode << " events=" << stats.events
              << " obi_sum=" << handler.obi_sum() << " mean_ns=" << stats.mean_ns
              << " p50_ns=" << stats.p50_ns << " p90_ns=" << stats.p90_ns
              << " p99_ns=" << stats.p99_ns << " p999_ns=" << stats.p999_ns
              << " max_ns=" << stats.max_ns << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        ObiHandler handler(options.limit);
        const hft::obi::FilterThresholds thresholds{
            options.lifetime_ms * hft::kNanosecondsPerMillisecond,
            options.max_mods,
            options.mod_time_ms * hft::kNanosecondsPerMillisecond};
        hft::signal::SignalPipeline<> pipeline(
            thresholds, options.sample_ms * hft::kNanosecondsPerMillisecond,
            options.max_snapshots);
        std::uint64_t processed = 0;
        hft::ingest::ReplayRunner runner([&](const hft::MarketEvent& event) {
            if (options.limit != 0 && processed >= options.limit) {
                return;
            }
            ++processed;
            if (options.metrics) {
                pipeline.process(std::span<const hft::MarketEvent>(&event, 1));
            } else {
                handler(event);
            }
        });
        runner.start();

        if (options.mode == "binance-trades") {
            hft::ingest::BinanceTradeFormat format =
                hft::ingest::BinanceTradeFormat::SpotTrades;
            if (options.format == "futures") {
                format = hft::ingest::BinanceTradeFormat::FuturesTrades;
            } else if (options.format == "agg") {
                format = hft::ingest::BinanceTradeFormat::AggTrades;
            }
            const auto read = hft::ingest::read_binance_trades_csv(
                options.file, format, [&](const hft::ingest::TradeRecord& trade) {
                    hft::MarketEvent event = hft::ingest::to_market_event(
                        trade, options.price_scale, options.quantity_scale);
                    // Trade files carry no quote; use the execution price as
                    // a degenerate bid/ask so downstream mid updates.
                    event.bid_price_ticks = event.price_ticks;
                    event.ask_price_ticks = event.price_ticks;
                    runner.submit(event);
                });
            std::cout << "parsed=" << read << ' ';
        } else if (options.mode == "binance-depth") {
            std::uint64_t synthetic_oid = 0;
            const auto read = hft::ingest::read_binance_book_depth_csv(
                options.file, [&](const hft::ingest::DepthSnapshot& snapshot) {
                    hft::ingest::emit_depth_snapshot_events(
                        snapshot, synthetic_oid, options.price_scale,
                        options.quantity_scale,
                        [&](const hft::MarketEvent& event) {
                            runner.submit(event);
                        });
                });
            std::cout << "snapshots=" << read << ' ';
        } else if (options.mode == "hl-book") {
            hft::ingest::BookState book;
            hft::ingest::HlBookDiffOptions diff_options{};
            diff_options.coin_filter = options.coin;
            diff_options.base_timestamp_ns = options.base_timestamp_ns;
            diff_options.timestamp_step_ns = options.timestamp_step_ns;
            const auto read = hft::ingest::read_hl_book_diffs(
                options.file, diff_options,
                [&](const hft::ingest::HlBookDiff& diff) {
                    book.apply(diff, options.price_scale);
                    runner.submit(hft::ingest::to_market_event(
                        diff, book, options.price_scale, options.quantity_scale));
                });
            std::cout << "diffs=" << read << ' ';
        } else if (options.mode == "hl-orders") {
            hft::ingest::BookState book;
            const auto read = hft::ingest::read_hl_order_status(
                options.file, [&](const hft::ingest::HlOrderStatus& status) {
                    if (!hft::ingest::map_hl_status(status.status_id).has_value()) {
                        return;
                    }
                    book.apply(status, options.price_scale);
                    runner.submit(hft::ingest::to_market_event(
                        status, book, options.price_scale,
                        options.quantity_scale));
                });
            std::cout << "order_statuses=" << read << " levels="
                      << book.bid_levels() + book.ask_levels() << ' ';
        } else if (options.mode == "hl-trades") {
            const auto read = hft::ingest::read_hl_trades(
                options.file, options.coin, [&](const hft::ingest::HlTrade& trade) {
                    hft::MarketEvent event = hft::ingest::to_market_event(
                        trade, options.price_scale, options.quantity_scale);
                    event.bid_price_ticks = event.price_ticks;
                    event.ask_price_ticks = event.price_ticks;
                    runner.submit(event);
                });
            std::cout << "trades=" << read << ' ';
        } else if (options.mode == "crypto-depth") {
            // crypto-lob-stream depth is level-based. Model each persistent
            // price level as one synthetic order: quantity -> 0 cancels it,
            // a later re-add starts a new lifetime. This lets the existing
            // Lifetime/ModificationCount/ModificationTime filters run without
            // changing the signal code.
            struct LevelInfo {
                std::int64_t first_seen_ns{0};
                std::int64_t last_modification_ns{0};
                std::uint32_t modifications{0};
            };
            hft::ingest::BookState book;
            std::unordered_map<std::uint64_t, LevelInfo> levels;
            const auto mix = [](std::uint64_t value) {
                value += 0x9e3779b97f4a7c15ULL;
                value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
                value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
                return value ^ (value >> 31);
            };
            const auto read = hft::ingest::read_crypto_lob_depth_csv(
                options.file, options.exchange, options.asset,
                [&](const hft::ingest::CryptoLobLevelRow& row) {
                    book.set(row.side, row.price, row.quantity,
                             options.price_scale);
                    const std::int64_t price_ticks = static_cast<std::int64_t>(
                        std::llround(row.price * options.price_scale));
                    std::uint64_t asset_hash = 1469598103934665603ULL;
                    for (const char character : row.asset) {
                        asset_hash ^= static_cast<std::uint64_t>(
                            static_cast<unsigned char>(character));
                        asset_hash *= 1099511628211ULL;
                    }
                    const std::uint64_t key =
                        mix(static_cast<std::uint64_t>(price_ticks) ^
                            (row.side == hft::Side::Bid ? 0x51ed270bU : 0U) ^
                            mix(asset_hash));

                    hft::MarketEvent event{};
                    event.order_id = key;
                    event.timestamp_ns = row.timestamp_ns;
                    event.price_ticks = price_ticks;
                    event.quantity = static_cast<std::int64_t>(
                        std::llround(row.quantity * options.quantity_scale));
                    event.side = row.side;
                    if (row.quantity > 0.0) {
                        auto [iterator, inserted] =
                            levels.try_emplace(key, LevelInfo{row.timestamp_ns, 0, 0});
                        if (inserted) {
                            event.type = hft::EventType::New;
                        } else {
                            event.type = hft::EventType::Modify;
                            iterator->second.last_modification_ns = row.timestamp_ns;
                            ++iterator->second.modifications;
                        }
                    } else {
                        event.type = hft::EventType::Cancel;
                        levels.erase(key);
                    }
                    std::int64_t bid_ticks = 0;
                    std::int64_t ask_ticks = 0;
                    if (book.best(bid_ticks, ask_ticks)) {
                        event.bid_price_ticks = bid_ticks;
                        event.ask_price_ticks = ask_ticks;
                    }
                    runner.submit(event);
                });
            std::cout << "depth_rows=" << read << " levels=" << levels.size()
                      << ' ';
        } else if (options.mode == "lobster") {
            hft::ingest::LobsterOptions lobster_options{};
            lobster_options.base_timestamp_ns = options.base_timestamp_ns;
            const auto read = hft::ingest::read_lobster_messages(
                options.file, lobster_options,
                [&](const hft::MarketEvent& event) { runner.submit(event); });
            std::cout << "lobster_messages=" << read << ' ';
        } else if (options.mode == "csv") {
            const std::vector<hft::MarketEvent> events =
                hft::data::load_ticks_csv(options.file);
            for (const hft::MarketEvent& event : events) {
                runner.submit(event);
            }
            std::cout << "csv_events=" << events.size() << ' ';
        } else {
            throw std::runtime_error("unknown mode: " + options.mode);
        }

        const hft::ingest::ReplayStats stats = runner.finish();
        print_stats(options.mode, stats, handler);
        if (options.metrics) {
            const auto& snapshots = pipeline.snapshots();
            const auto rows = hft::signal::calculate_metrics(snapshots);
            std::cout << "processed=" << processed
                      << " snapshots=" << snapshots.size()
                      << " skipped=" << pipeline.skipped_samples()
                      << " dropped=" << pipeline.dropped_snapshots();
            if (!options.scores.empty()) {
                write_scores(options.scores, rows);
                std::cout << " scores=" << options.scores;
            } else {
                std::cout << '\n'
                          << "scheme,horizon_s,samples,pearson,r2\n";
                for (const auto& row : rows) {
                    std::cout << row.scheme << ',' << row.horizon_seconds << ','
                              << row.samples << ',' << row.pearson.pearson << ','
                              << row.pearson.r2 << '\n';
                }
            }
            if (!options.regimes.empty()) {
                const hft::obi::FilterScheme scheme =
                    parse_scheme(options.regime_scheme);
                const std::size_t horizon =
                    parse_horizon(options.regime_horizon_seconds);
                const auto tuples = hft::signal::calculate_regimes(
                    snapshots, scheme, horizon, options.regime_signal == "volume");
                write_regimes(options.regimes, tuples);
                std::cout << " regimes=" << tuples.size()
                          << " regimes_file=" << options.regimes;
            }
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hft_replay: " << error.what() << '\n';
        return 1;
    }
}
