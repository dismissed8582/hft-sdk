#include "hft/Backtest.hpp"
#include "hft/SignalPipeline.hpp"
#include "hft/TickData.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string ticks{"data/sample_ticks.csv"};
    std::string scores{"signal_scores.csv"};
    std::string regimes{"layer3_regimes.csv"};
    std::string regime_scheme{"UF"};
    std::int64_t regime_horizon_seconds{1};
    std::string regime_signal{"volume"};
    std::string pnl_summary{"benchmarks/results/pnl_summary.json"};
    std::string pnl_trades{"benchmarks/results/pnl_trades.csv"};
    std::string pnl_scheme{"UF"};
    std::int64_t pnl_horizon_seconds{1};
    std::string pnl_signal{"volume"};
    double maker_fee_bps{2.0};
    double taker_fee_bps{2.0};
    bool maker{false};
    std::int64_t latency_us{100};
    double pnl_threshold{0.0};
    std::int64_t sample_interval_ms{250};
    std::int64_t lifetime_ms{100};
    std::uint32_t max_modifications{3};
    std::int64_t min_modification_interval_ms{50};
};

void ensure_parent(const std::string& path) {
    const std::filesystem::path output(path);
    if (output.has_parent_path()) {
        std::filesystem::create_directories(output.parent_path());
    }
}

std::int64_t parse_i64(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer for ") + name + ": " + value);
    }
}

hft::obi::FilterScheme parse_scheme(const std::string& value) {
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(hft::obi::FilterScheme::Count); ++index) {
        if (hft::obi::kFilterSchemeNames[index] == value) {
            return static_cast<hft::obi::FilterScheme>(index);
        }
    }
    throw std::runtime_error("unknown regime scheme: " + value);
}

std::size_t parse_horizon(std::int64_t seconds) {
    for (std::size_t index = 0; index < hft::signal::kHorizonSeconds.size(); ++index) {
        if (hft::signal::kHorizonSeconds[index] == seconds) {
            return index;
        }
    }
    throw std::runtime_error("unsupported regime horizon: " + std::to_string(seconds));
}

bool parse_volume(const std::string& value) {
    if (value == "volume") {
        return true;
    }
    if (value == "count") {
        return false;
    }
    throw std::runtime_error("signal must be volume or count: " + value);
}

double parse_double(const std::string& value, const char* name) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid number for ") + name + ": " + value);
    }
}

Options parse_options(int argc, char** argv) {
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: hft_eval [--ticks file] [--scores file] [--regimes file]\n"
                << "                [--regime-scheme UF|LF|MF|MTF] "
                   "[--regime-horizon 1|10|30|50]\n"
                << "                [--regime-signal volume|count]\n"
                << "                [--sample-ms n] [--lifetime-ms n] [--max-mods n]\n"
                << "                [--mod-time-ms n]\n"
                << "                [--pnl-summary file] [--pnl-trades file]\n"
                << "                [--pnl-scheme UF|LF|MF|MTF] "
                   "[--pnl-horizon 1|10|30|50]\n"
                << "                [--pnl-signal volume|count] [--pnl-threshold x]\n"
                << "                [--maker-fee-bps x] [--taker-fee-bps x] "
                   "[--liquidity maker|taker]\n"
                << "                [--latency-us n]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + argument);
        }
        const std::string value = argv[++index];
        if (argument == "--ticks") {
            options.ticks = value;
        } else if (argument == "--scores") {
            options.scores = value;
        } else if (argument == "--regimes") {
            options.regimes = value;
        } else if (argument == "--regime-scheme") {
            options.regime_scheme = value;
            (void)parse_scheme(options.regime_scheme);
        } else if (argument == "--regime-horizon") {
            options.regime_horizon_seconds = parse_i64(value, "regime-horizon");
            (void)parse_horizon(options.regime_horizon_seconds);
        } else if (argument == "--regime-signal") {
            options.regime_signal = value;
            (void)parse_volume(options.regime_signal);
        } else if (argument == "--pnl-summary") {
            options.pnl_summary = value;
        } else if (argument == "--pnl-trades") {
            options.pnl_trades = value;
        } else if (argument == "--pnl-scheme") {
            options.pnl_scheme = value;
            (void)parse_scheme(options.pnl_scheme);
        } else if (argument == "--pnl-horizon") {
            options.pnl_horizon_seconds = parse_i64(value, "pnl-horizon");
            (void)parse_horizon(options.pnl_horizon_seconds);
        } else if (argument == "--pnl-signal") {
            options.pnl_signal = value;
            (void)parse_volume(options.pnl_signal);
        } else if (argument == "--pnl-threshold") {
            options.pnl_threshold = parse_double(value, "pnl-threshold");
        } else if (argument == "--maker-fee-bps") {
            options.maker_fee_bps = parse_double(value, "maker-fee-bps");
        } else if (argument == "--taker-fee-bps") {
            options.taker_fee_bps = parse_double(value, "taker-fee-bps");
        } else if (argument == "--liquidity") {
            if (value == "maker") {
                options.maker = true;
            } else if (value == "taker") {
                options.maker = false;
            } else {
                throw std::runtime_error("liquidity must be maker or taker");
            }
        } else if (argument == "--latency-us") {
            options.latency_us = parse_i64(value, "latency-us");
        } else if (argument == "--sample-ms") {
            options.sample_interval_ms = parse_i64(value, "sample-ms");
        } else if (argument == "--lifetime-ms") {
            options.lifetime_ms = parse_i64(value, "lifetime-ms");
        } else if (argument == "--max-mods") {
            const auto max_mods = parse_i64(value, "max-mods");
            if (max_mods < 0 || max_mods > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("max-mods is out of range");
            }
            options.max_modifications = static_cast<std::uint32_t>(max_mods);
        } else if (argument == "--mod-time-ms") {
            options.min_modification_interval_ms = parse_i64(value, "mod-time-ms");
        } else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    if (options.sample_interval_ms <= 0 || options.lifetime_ms < 0 ||
        options.min_modification_interval_ms < 0 || options.latency_us < 0 ||
        options.maker_fee_bps < 0.0 || options.taker_fee_bps < 0.0) {
        throw std::runtime_error("sampling, latency, and fee inputs must be non-negative");
    }
    if (options.sample_interval_ms > std::numeric_limits<std::int64_t>::max() / 1'000'000 ||
        options.lifetime_ms > std::numeric_limits<std::int64_t>::max() / 1'000'000 ||
        options.min_modification_interval_ms > std::numeric_limits<std::int64_t>::max() / 1'000'000 ||
        options.latency_us > std::numeric_limits<std::int64_t>::max() / 1000 ||
        !std::isfinite(options.maker_fee_bps) || !std::isfinite(options.taker_fee_bps) ||
        !std::isfinite(options.pnl_threshold) || options.pnl_threshold < 0) throw std::runtime_error("numeric option is out of range");
    return options;
}

void write_scores(const std::string& path,
                  const std::vector<hft::signal::SignalMetrics>& metrics) {
    ensure_parent(path);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("unable to write scores to " + path);
    }
    output << "scheme,horizon_s,samples,pearson,r2,regime_pearson,regime_r2,"
              "trade_samples,trade_pearson,trade_r2,trade_regime_pearson,"
              "trade_regime_r2,volume_samples,volume_pearson,volume_r2,"
              "volume_regime_pearson,volume_regime_r2,volume_trade_samples,"
              "volume_trade_pearson,volume_trade_r2,volume_trade_regime_pearson,"
              "volume_trade_regime_r2\n";
    output << std::setprecision(12);
    for (const auto& row : metrics) {
        output << row.scheme << ',' << row.horizon_seconds << ',' << row.samples << ','
               << row.pearson.pearson << ',' << row.pearson.r2 << ','
               << row.regime.pearson << ',' << row.regime.r2 << ','
               << row.trade_samples << ','
               << row.trade_pearson.pearson << ',' << row.trade_pearson.r2 << ','
               << row.trade_regime.pearson << ',' << row.trade_regime.r2 << ','
               << row.volume_samples << ','
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
        throw std::runtime_error("unable to write regime tuples to " + path);
    }
    output << "timestamp_ns,obi_regime,return_regime\n";
    for (const auto& tuple : tuples) {
        output << tuple.timestamp_ns << ',' << static_cast<int>(tuple.obi_regime)
               << ',' << static_cast<int>(tuple.return_regime) << '\n';
    }
}

void write_pnl(const Options& options, const hft::backtest::Summary& summary) {
    ensure_parent(options.pnl_summary);
    ensure_parent(options.pnl_trades);

    {
        std::ofstream trades(options.pnl_trades);
        if (!trades) {
            throw std::runtime_error("unable to write " + options.pnl_trades);
        }
        trades << "signal_ns,entry_ns,exit_ns,side,signal,entry_mid,exit_mid,"
                  "gross_return,fee_return,net_return\n";
        trades << std::setprecision(12);
        for (const auto& trade : summary.trades) {
            trades << trade.signal_ns << ',' << trade.entry_ns << ',' << trade.exit_ns
                   << ',' << trade.side << ',' << trade.signal << ','
                   << trade.entry_mid << ',' << trade.exit_mid << ','
                   << trade.gross_return << ',' << trade.fee_return << ','
                   << trade.net_return << '\n';
        }
    }

    std::ofstream output(options.pnl_summary);
    if (!output) {
        throw std::runtime_error("unable to write " + options.pnl_summary);
    }
    output << std::setprecision(12);
    output << "{\n";
    output << "  \"config\": {\n";
    output << "    \"scheme\": \"" << options.pnl_scheme << "\",\n";
    output << "    \"horizon_s\": " << options.pnl_horizon_seconds << ",\n";
    output << "    \"signal\": \"" << options.pnl_signal << "\",\n";
    output << "    \"liquidity\": \"" << (options.maker ? "maker" : "taker")
           << "\",\n";
    output << "    \"fee_bps\": "
           << (options.maker ? options.maker_fee_bps : options.taker_fee_bps)
           << ",\n";
    output << "    \"latency_us\": " << options.latency_us << ",\n";
    output << "    \"threshold\": " << options.pnl_threshold << "\n";
    output << "  },\n";
    output << "  \"trade_count\": " << summary.trade_count << ",\n";
    output << "  \"total_return\": " << summary.total_return << ",\n";
    output << "  \"mean_trade_return\": " << summary.mean_trade_return << ",\n";
    output << "  \"median_trade_return\": " << summary.median_trade_return << ",\n";
    output << "  \"std_trade_return\": " << summary.std_trade_return << ",\n";
    output << "  \"win_rate\": " << summary.win_rate << ",\n";
    output << "  \"profit_factor\": " << summary.profit_factor << ",\n";
    output << "  \"sharpe_trade\": " << summary.sharpe_trade << ",\n";
    output << "  \"sharpe_daily_annualized\": "
           << summary.sharpe_daily_annualized << ",\n";
    output << "  \"max_drawdown\": " << summary.max_drawdown << ",\n";
    output << "  \"total_fees\": " << summary.total_fees << ",\n";
    output << "  \"average_holding_ns\": " << summary.average_holding_ns << ",\n";
    output << "  \"trades_file\": \"" << options.pnl_trades << "\",\n";
    output << "  \"trade_pnl\": [";
    for (std::size_t index = 0; index < summary.trades.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << summary.trades[index].net_return;
    }
    output << "]\n";
    output << "}\n";
}

}

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::vector<hft::MarketEvent> events = hft::data::load_ticks_csv(options.ticks);
        if (events.empty()) {
            throw std::runtime_error("tick file contains no events");
        }
        hft::data::sort_ticks_by_timestamp(events);

        const hft::obi::FilterThresholds thresholds{
            options.lifetime_ms * hft::kNanosecondsPerMillisecond,
            options.max_modifications,
            options.min_modification_interval_ms * hft::kNanosecondsPerMillisecond};

        auto pipeline = std::make_unique<hft::signal::SignalPipeline<>>(
            thresholds, options.sample_interval_ms * hft::kNanosecondsPerMillisecond);
        const std::int64_t duration_ns =
            events.back().timestamp_ns - events.front().timestamp_ns;
        const std::int64_t sample_ns =
            options.sample_interval_ms * hft::kNanosecondsPerMillisecond;
        if (sample_ns > 0) {
            const std::size_t estimated =
                static_cast<std::size_t>(duration_ns / sample_ns) + 2;
            pipeline->reserve_snapshots(std::min<std::size_t>(estimated, 1u << 21));
        }
        pipeline->process(events);

        const auto& snapshots = pipeline->snapshots();
        if (snapshots.empty()) {
            throw std::runtime_error("no signal snapshots were generated");
        }
        const std::vector<hft::signal::SignalMetrics> metrics =
            hft::signal::calculate_metrics(snapshots);
        const hft::obi::FilterScheme scheme = parse_scheme(options.regime_scheme);
        const std::size_t horizon = parse_horizon(options.regime_horizon_seconds);
        const bool regime_volume = parse_volume(options.regime_signal);
        const std::vector<hft::signal::RegimeTuple> regimes =
            hft::signal::calculate_regimes(snapshots, scheme, horizon, regime_volume);

        hft::backtest::Config pnl_config{};
        pnl_config.scheme = parse_scheme(options.pnl_scheme);
        pnl_config.horizon = parse_horizon(options.pnl_horizon_seconds);
        pnl_config.weight = parse_volume(options.pnl_signal)
            ? hft::backtest::SignalWeight::Volume
            : hft::backtest::SignalWeight::Count;
        pnl_config.threshold = options.pnl_threshold;
        pnl_config.maker = options.maker;
        pnl_config.maker_fee_bps = options.maker_fee_bps;
        pnl_config.taker_fee_bps = options.taker_fee_bps;
        pnl_config.latency_ns = options.latency_us * 1'000;
        const hft::backtest::Summary pnl =
            hft::backtest::run(snapshots, pnl_config);

        write_scores(options.scores, metrics);
        write_regimes(options.regimes, regimes);
        write_pnl(options, pnl);

        std::cout << "events=" << events.size() << " snapshots=" << snapshots.size()
                  << " regimes=" << regimes.size()
                  << " trades=" << pnl.trade_count
                  << " window_drops=" << pipeline->dropped_events()
                  << " tracker_drops=" << pipeline->tracker_dropped() << '\n';
        std::cout << "scores=" << options.scores << " regimes=" << options.regimes
                  << " scheme=" << hft::obi::kFilterSchemeNames[
                         static_cast<std::size_t>(scheme)]
                  << " horizon_s=" << options.regime_horizon_seconds
                  << " signal=" << options.regime_signal << '\n';
        std::cout << "pnl_summary=" << options.pnl_summary
                  << " pnl_trades=" << options.pnl_trades
                  << " total_return=" << pnl.total_return
                  << " sharpe_daily=" << pnl.sharpe_daily_annualized
                  << " max_drawdown=" << pnl.max_drawdown << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hft_eval: " << error.what() << '\n';
        return 1;
    }
}

