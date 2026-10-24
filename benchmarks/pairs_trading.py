#!/usr/bin/env python3
"""Cointegration pairs-trading backtest (paper 2, Section 4)."""

from __future__ import annotations

import argparse
import json
import warnings
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

try:
    from statsmodels.tsa.stattools import adfuller
except ImportError:  # pragma: no cover
    adfuller = None

DARK_BACKGROUND = "#0d1117"
DARK_AXES = "#161b22"
DARK_GRID = "#30363d"
DARK_TEXT = "#e6edf3"
ACCENT = "#58a6ff"
ACCENT_LONG = "#3fb950"
ACCENT_SHORT = "#f85149"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", type=Path, help="CSV with two price columns")
    parser.add_argument("--x-column", default="x")
    parser.add_argument("--y-column", default="y")
    parser.add_argument("--window", type=int, default=60)
    parser.add_argument("--entry-z", type=float, default=2.0)
    parser.add_argument("--exit-z", type=float, default=0.5)
    parser.add_argument("--fee-bps", type=float, default=1.0)
    parser.add_argument("--points", type=int, default=5000)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--output-dir", type=Path, default=Path("benchmarks/results/pairs"))
    parser.add_argument("--plots-dir", type=Path, default=Path("benchmarks/plots/pairs"))
    parser.add_argument("--report", type=Path, default=Path("benchmarks/results/pairs/PAIRS_REPORT.md"))
    return parser.parse_args()


def synthetic_series(points: int, seed: int) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(seed)
    x = np.cumsum(rng.normal(0.0, 1.0, points)) + 100.0
    noise = np.zeros(points)
    for index in range(1, points):
        noise[index] = 0.92 * noise[index - 1] + rng.normal(0.0, 0.8)
    y = 2.0 * x + noise
    return x, y


def load_series(args: argparse.Namespace) -> tuple[np.ndarray, np.ndarray, str]:
    if args.csv is not None:
        frame = pd.read_csv(args.csv)
        return (frame[args.x_column].to_numpy(dtype=float),
                frame[args.y_column].to_numpy(dtype=float), str(args.csv))
    x, y = synthetic_series(args.points, args.seed)
    return x, y, f"synthetic(points={args.points}, seed={args.seed})"


def backtest(x: np.ndarray, y: np.ndarray, window: int, entry_z: float,
             exit_z: float, fee_bps: float) -> tuple[pd.DataFrame, dict]:
    design = np.column_stack([np.ones(x.size), x])
    intercept, hedge = np.linalg.lstsq(design, y, rcond=None)[0]
    spread = y - hedge * x - intercept
    spread_series = pd.Series(spread)
    mean = spread_series.rolling(window).mean()
    std = spread_series.rolling(window).std(ddof=1)
    zscore = (spread_series - mean) / std.replace(0.0, np.nan)
    zscore = zscore.to_numpy()

    fee = fee_bps / 10_000.0
    position = 0
    previous_position = 0
    equity = [0.0]
    trades = []
    entry_index = None
    entry_side = 0
    for index in range(1, x.size):
        z = zscore[index - 1]
        if np.isfinite(z):
            if position == 0:
                if z <= -entry_z:
                    position = 1
                elif z >= entry_z:
                    position = -1
            elif position > 0 and z >= -exit_z:
                position = 0
            elif position < 0 and z <= exit_z:
                position = 0
        spread_change = spread[index] - spread[index - 1]
        gross = position * spread_change
        cost = abs(position - previous_position) * fee * (abs(y[index]) + abs(hedge * x[index]))
        equity.append(equity[-1] + gross - cost)
        if position != previous_position:
            if previous_position == 0 and position != 0:
                entry_index = index
                entry_side = position
            elif position == 0 and previous_position != 0 and entry_index is not None:
                trades.append({
                    "entry_index": entry_index,
                    "exit_index": index,
                    "side": entry_side,
                    "pnl": equity[-1] - equity[entry_index],
                })
                entry_index = None
        previous_position = position

    equity_array = np.asarray(equity, dtype=float)
    peak = np.maximum.accumulate(equity_array)
    drawdown = equity_array - peak
    daily = np.diff(equity_array)
    std_daily = float(np.std(daily, ddof=1)) if daily.size > 1 else 0.0
    sharpe = float(np.mean(daily) / std_daily * np.sqrt(252)) if std_daily > 0 else 0.0
    summary = {
        "hedge_ratio": float(hedge),
        "intercept": float(intercept),
        "spread_mean": float(np.mean(spread)),
        "spread_std": float(np.std(spread, ddof=1)),
        "trades": len(trades),
        "total_return": float(equity_array[-1]),
        "max_drawdown": float(drawdown.min()) if drawdown.size else 0.0,
        "sharpe_annualized": sharpe,
        "win_rate": float(np.mean([trade["pnl"] > 0 for trade in trades])) if trades else 0.0,
        "window": window,
        "entry_z": entry_z,
        "exit_z": exit_z,
        "fee_bps": fee_bps,
    }
    if adfuller is not None and spread.size > 20:
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                summary["adf_pvalue"] = float(adfuller(spread, autolag="AIC")[1])
        except Exception:  # noqa: BLE001
            summary["adf_pvalue"] = float("nan")
    else:
        summary["adf_pvalue"] = float("nan")

    trades_frame = pd.DataFrame(trades)
    return trades_frame, {
        "summary": summary,
        "equity": equity_array,
        "zscore": zscore,
        "spread": spread,
    }


def plot_results(result: dict, args: argparse.Namespace) -> None:
    plt.rcParams.update({
        "figure.facecolor": DARK_BACKGROUND,
        "axes.facecolor": DARK_AXES,
        "axes.edgecolor": DARK_GRID,
        "axes.labelcolor": DARK_TEXT,
        "text.color": DARK_TEXT,
        "xtick.color": DARK_TEXT,
        "ytick.color": DARK_TEXT,
        "grid.color": DARK_GRID,
        "savefig.facecolor": DARK_BACKGROUND,
    })
    equity = result["equity"]
    zscore = result["zscore"]
    figure, axes = plt.subplots(2, 1, figsize=(13, 9), sharex=True,
                                gridspec_kw={"height_ratios": [1.4, 1.0]})
    axes[0].plot(equity, color=ACCENT, linewidth=2.0)
    axes[0].axhline(0.0, color=DARK_GRID, linewidth=1.0)
    axes[0].set_ylabel("Cumulative PnL")
    axes[0].set_title("Pairs trading: equity and spread z-score")
    axes[1].plot(zscore, color=ACCENT, linewidth=1.0, alpha=0.8)
    axes[1].axhline(args.entry_z, color=ACCENT_SHORT, linewidth=1.2, linestyle="--")
    axes[1].axhline(-args.entry_z, color=ACCENT_LONG, linewidth=1.2, linestyle="--")
    axes[1].axhline(args.exit_z, color=DARK_GRID, linewidth=1.0, linestyle=":")
    axes[1].axhline(-args.exit_z, color=DARK_GRID, linewidth=1.0, linestyle=":")
    axes[1].set_ylabel("Z-score")
    axes[1].set_xlabel("Bar")
    figure.tight_layout()
    args.plots_dir.mkdir(parents=True, exist_ok=True)
    figure.savefig(args.plots_dir / "pairs_equity.png", dpi=200)
    plt.close(figure)


def main() -> int:
    args = parse_args()
    x, y, source = load_series(args)
    trades, result = backtest(x, y, args.window, args.entry_z, args.exit_z, args.fee_bps)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "pairs_summary.json").write_text(
        json.dumps({"source": source, **result["summary"]}, indent=2) + "\n")
    trades.to_csv(args.output_dir / "pairs_trades.csv", index=False)
    plot_results(result, args)
    summary = result["summary"]
    lines = [
        "# Cointegration Pairs-Trading Backtest",
        "",
        f"Source: `{source}`",
        "",
        "## Strategy",
        "",
        "- OLS hedge ratio of `y` on `x`.",
        "- Rolling spread z-score with the configured window.",
        "- Enter long spread at `z <= -entry`, short at `z >= entry`.",
        "- Exit when `|z| <= exit`.",
        "- Per-side transaction cost in bps of gross leg value.",
        "",
        "## Results",
        "",
        "| Metric | Value |",
        "| --- | --- |",
        f"| ADF p-value | {summary['adf_pvalue']:.6f} |",
        f"| Hedge ratio | {summary['hedge_ratio']:.6f} |",
        f"| Trades | {summary['trades']} |",
        f"| Total PnL | {summary['total_return']:.6f} |",
        f"| Max drawdown | {summary['max_drawdown']:.6f} |",
        f"| Sharpe (annualized) | {summary['sharpe_annualized']:.3f} |",
        f"| Win rate | {summary['win_rate']:.2%} |",
        "",
        "![Pairs trading equity and z-score](benchmarks/plots/pairs/pairs_equity.png)",
        "",
        "## Limitations",
        "",
        "- This is a research backtest, not a production execution model.",
        "- Synthetic data is used when no CSV is supplied; it validates the",
        "  implementation rather than demonstrating market alpha.",
        "- The paper's latency optimisation is covered separately by the",
        "  microbenchmark suite; this script focuses on the strategy math.",
        "",
    ]
    args.report.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
