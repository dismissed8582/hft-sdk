#pragma once

#include "hft/SignalPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <vector>

namespace hft::backtest {

enum class SignalWeight : std::uint8_t {
    Count = 0,
    Volume = 1,
};

struct Config {
    obi::FilterScheme scheme{obi::FilterScheme::Unfiltered};
    std::size_t horizon{0};
    SignalWeight weight{SignalWeight::Volume};
    double threshold{0.0};
    double maker_fee_bps{2.0};
    double taker_fee_bps{2.0};
    bool maker{false};
    std::int64_t latency_ns{100'000}; // 100 microseconds
};

struct Trade {
    std::int64_t signal_ns{0};
    std::int64_t entry_ns{0};
    std::int64_t exit_ns{0};
    int side{0};
    double signal{0.0};
    double entry_mid{0.0};
    double exit_mid{0.0};
    double gross_return{0.0};
    double fee_return{0.0};
    double net_return{0.0};
};

struct Summary {
    std::size_t trade_count{0};
    double total_return{0.0};
    double mean_trade_return{0.0};
    double median_trade_return{0.0};
    double std_trade_return{0.0};
    double win_rate{0.0};
    double profit_factor{0.0};
    double sharpe_trade{0.0};
    double sharpe_daily_annualized{0.0};
    double max_drawdown{0.0};
    double total_fees{0.0};
    double average_holding_ns{0.0};
    std::vector<Trade> trades;
};

namespace detail {

constexpr std::int64_t kNanosecondsPerDay = 86'400LL * 1'000'000'000LL;

[[nodiscard]] inline std::size_t lower_time(
    const std::vector<signal::SignalSnapshot>& snapshots,
    std::size_t begin,
    std::int64_t target) noexcept {
    const auto iterator = std::lower_bound(
        snapshots.begin() + static_cast<std::ptrdiff_t>(begin), snapshots.end(),
        target, [](const signal::SignalSnapshot& snapshot, std::int64_t value) {
            return snapshot.timestamp_ns < value;
        });
    return static_cast<std::size_t>(std::distance(snapshots.begin(), iterator));
}

} // namespace detail

[[nodiscard]] inline Summary run(
    const std::vector<signal::SignalSnapshot>& snapshots,
    const Config& config = {}) {
    Summary summary{};
    if (snapshots.size() < 2 || config.horizon >= signal::kHorizonNs.size()) {
        return summary;
    }

    const std::size_t scheme = static_cast<std::size_t>(config.scheme);
    if (scheme >= static_cast<std::size_t>(obi::FilterScheme::Count)) {
        return summary;
    }

    const double fee_bps = config.maker ? config.maker_fee_bps : config.taker_fee_bps;
    const double fee_return = 2.0 * fee_bps / 10'000.0;
    const std::int64_t holding_ns = signal::kHorizonNs[config.horizon];

    std::size_t index = 0;
    while (index < snapshots.size()) {
        const signal::SignalSnapshot& source = snapshots[index];
        const obi::OBIValue& value = config.weight == SignalWeight::Volume
            ? source.lob_volume[scheme][config.horizon]
            : source.lob[scheme][config.horizon];
        if (!value.valid || std::abs(value.value) <= config.threshold) {
            ++index;
            continue;
        }

        const std::size_t entry_index = detail::lower_time(
            snapshots, index, source.timestamp_ns + config.latency_ns);
        if (entry_index >= snapshots.size()) {
            break;
        }
        const double entry_mid = snapshots[entry_index].mid;
        if (!(entry_mid > 0.0)) {
            index = entry_index + 1;
            continue;
        }

        const std::size_t exit_index = detail::lower_time(
            snapshots, entry_index,
            snapshots[entry_index].timestamp_ns + holding_ns);
        if (exit_index >= snapshots.size()) {
            break;
        }
        const double exit_mid = snapshots[exit_index].mid;
        if (!(exit_mid > 0.0)) {
            index = exit_index + 1;
            continue;
        }

        Trade trade{};
        trade.signal_ns = source.timestamp_ns;
        trade.entry_ns = snapshots[entry_index].timestamp_ns;
        trade.exit_ns = snapshots[exit_index].timestamp_ns;
        trade.side = value.value > 0.0 ? 1 : -1;
        trade.signal = value.value;
        trade.entry_mid = entry_mid;
        trade.exit_mid = exit_mid;
        trade.gross_return =
            static_cast<double>(trade.side) * (exit_mid / entry_mid - 1.0);
        trade.fee_return = fee_return;
        trade.net_return = trade.gross_return - fee_return;
        summary.trades.push_back(trade);
        index = exit_index + 1;
    }

    summary.trade_count = summary.trades.size();
    if (summary.trade_count == 0) {
        return summary;
    }

    std::vector<double> returns;
    returns.reserve(summary.trade_count);
    double sum = 0.0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    std::int64_t holding_total = 0;
    std::map<std::int64_t, double> daily_pnl;
    double equity = 1.0;
    double peak = 1.0;
    for (const Trade& trade : summary.trades) {
        returns.push_back(trade.net_return);
        sum += trade.net_return;
        if (trade.net_return > 0.0) {
            gross_profit += trade.net_return;
        } else {
            gross_loss -= trade.net_return;
        }
        holding_total += trade.exit_ns - trade.entry_ns;
        daily_pnl[trade.entry_ns / detail::kNanosecondsPerDay] += trade.net_return;
        equity *= (1.0 + trade.net_return);
        if (equity < 0.0) {
            equity = 0.0;
        }
        peak = std::max(peak, equity);
        if (peak > 0.0) {
            summary.max_drawdown =
                std::max(summary.max_drawdown, (peak - equity) / peak);
        }
    }

    summary.total_return = equity - 1.0;
    summary.mean_trade_return = sum / static_cast<double>(summary.trade_count);
    summary.total_fees = fee_return * static_cast<double>(summary.trade_count);
    summary.average_holding_ns =
        static_cast<double>(holding_total) / static_cast<double>(summary.trade_count);

    std::vector<double> sorted = returns;
    const std::size_t middle = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(middle),
                     sorted.end());
    summary.median_trade_return = sorted[middle];
    if (sorted.size() % 2 == 0 && middle > 0) {
        const auto lower = std::max_element(sorted.begin(),
                                            sorted.begin() + static_cast<std::ptrdiff_t>(middle));
        summary.median_trade_return =
            0.5 * (summary.median_trade_return + *lower);
    }

    double variance = 0.0;
    for (const double value : returns) {
        const double delta = value - summary.mean_trade_return;
        variance += delta * delta;
    }
    if (returns.size() > 1) {
        summary.std_trade_return =
            std::sqrt(variance / static_cast<double>(returns.size() - 1));
    }
    if (summary.std_trade_return > 0.0) {
        summary.sharpe_trade = summary.mean_trade_return / summary.std_trade_return;
    }

    std::size_t wins = 0;
    for (const double value : returns) {
        if (value > 0.0) {
            ++wins;
        }
    }
    summary.win_rate = static_cast<double>(wins) / static_cast<double>(returns.size());
    summary.profit_factor = gross_loss > 0.0
        ? gross_profit / gross_loss
        : (gross_profit > 0.0 ? 999.0 : 0.0);

    if (daily_pnl.size() > 1) {
        double daily_sum = 0.0;
        for (const auto& [day, value] : daily_pnl) {
            (void)day;
            daily_sum += value;
        }
        const double daily_mean = daily_sum / static_cast<double>(daily_pnl.size());
        double daily_variance = 0.0;
        for (const auto& [day, value] : daily_pnl) {
            (void)day;
            const double delta = value - daily_mean;
            daily_variance += delta * delta;
        }
        const double daily_std = std::sqrt(
            daily_variance / static_cast<double>(daily_pnl.size() - 1));
        if (daily_std > 0.0) {
            summary.sharpe_daily_annualized =
                daily_mean / daily_std * std::sqrt(365.0);
        }
    }
    return summary;
}

} // namespace hft::backtest
