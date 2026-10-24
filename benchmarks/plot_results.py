#!/usr/bin/env python3
"""Render benchmark, signal-quality, and delegated Hawkes outputs."""

from __future__ import annotations

import argparse
import json
import re
from datetime import datetime, timezone
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

try:
    from scipy.optimize import lsq_linear
except ImportError:  # pragma: no cover - numpy fallback
    lsq_linear = None

SCHEME_ORDER = ["UF", "LF", "MF", "MTF"]
HORIZON_ORDER = [1, 10, 30, 50]
OBI_REGIME_COUNT = 9
RETURN_REGIME_COUNT = 4
HAWKES_STATE_COUNT = OBI_REGIME_COUNT + RETURN_REGIME_COUNT
HAWKES_LABELS = [f"OBI_{index}" for index in range(OBI_REGIME_COUNT)] + [
    f"RET_{index}" for index in range(RETURN_REGIME_COUNT)
]


DARK_BACKGROUND = "#0d1117"
DARK_AXES = "#161b22"
DARK_GRID = "#30363d"
DARK_TEXT = "#e6edf3"
ACCENT_COLD = "#f0883e"
ACCENT_WARM = "#58a6ff"
ACCENT_QUEUE = "#f85149"
ACCENT_RING = "#3fb950"


def apply_dark_theme() -> None:
    sns.set_theme(style="darkgrid", context="talk")
    plt.rcParams.update(
        {
            "figure.facecolor": DARK_BACKGROUND,
            "axes.facecolor": DARK_AXES,
            "axes.edgecolor": DARK_GRID,
            "axes.labelcolor": DARK_TEXT,
            "axes.titlecolor": DARK_TEXT,
            "text.color": DARK_TEXT,
            "xtick.color": DARK_TEXT,
            "ytick.color": DARK_TEXT,
            "grid.color": DARK_GRID,
            "legend.facecolor": DARK_AXES,
            "legend.edgecolor": DARK_GRID,
            "savefig.facecolor": DARK_BACKGROUND,
        }
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--benchmark-json",
        type=Path,
        default=Path("benchmarks/results/latest_bench.json"),
    )
    parser.add_argument(
        "--latency-csv", type=Path, default=Path("benchmarks/results/latency_samples.csv")
    )
    parser.add_argument(
        "--scores-csv", type=Path, default=Path("benchmarks/results/signal_scores.csv")
    )
    parser.add_argument(
        "--pnl-summary", type=Path, default=Path("benchmarks/results/pnl_summary.json")
    )
    parser.add_argument(
        "--regimes-csv", type=Path, default=Path("benchmarks/results/layer3_regimes.csv")
    )
    parser.add_argument("--hawkes-lags", type=int, default=16)
    parser.add_argument(
        "--hawkes-kernel-csv",
        type=Path,
        default=Path("benchmarks/results/hawkes_kernel.csv"),
    )
    parser.add_argument(
        "--hawkes-cross-csv",
        type=Path,
        default=Path("benchmarks/results/hawkes_cross_excitation.csv"),
    )
    parser.add_argument(
        "--hawkes-summary-json",
        type=Path,
        default=Path("benchmarks/results/hawkes_summary.json"),
    )
    parser.add_argument("--plots-dir", type=Path, default=Path("benchmarks/plots"))
    parser.add_argument("--report", type=Path, default=Path("BENCHMARK_REPORT.md"))
    parser.add_argument("--dataset", default="data/sample_ticks.csv")
    parser.add_argument("--repro-command", default="")
    parser.add_argument(
        "--cache-counters",
        type=Path,
        default=Path("benchmarks/results/cache_counters.txt"),
    )
    return parser.parse_args()


def load_benchmarks(path: Path) -> list[dict]:
    if not path.exists():
        print(f"warning: missing benchmark JSON at {path}")
        return []
    with path.open(encoding="utf-8") as source:
        payload = json.load(source)
    return payload.get("benchmarks", [])


def find_benchmark(records: list[dict], prefix: str) -> dict | None:
    for record in records:
        if record.get("name", "").startswith(prefix):
            return record
    return None


def to_float(value, fallback: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def percentile(series: pd.Series, value: float) -> float:
    if series.empty:
        return float("nan")
    return float(np.percentile(series.to_numpy(), value))


def load_frames(args: argparse.Namespace):
    latency = (
        pd.read_csv(args.latency_csv)
        if args.latency_csv.exists()
        else pd.DataFrame(columns=["pipeline", "latency_ns"])
    )
    scores = (
        pd.read_csv(args.scores_csv)
        if args.scores_csv.exists()
        else pd.DataFrame()
    )
    regimes = (
        pd.read_csv(args.regimes_csv)
        if args.regimes_csv.exists()
        else pd.DataFrame(columns=["timestamp_ns", "obi_regime", "return_regime"])
    )
    if not scores.empty:
        scores["scheme"] = pd.Categorical(
            scores["scheme"], categories=SCHEME_ORDER, ordered=True
        )
        scores["horizon_s"] = pd.Categorical(
            scores["horizon_s"], categories=HORIZON_ORDER, ordered=True
        )
        scores = scores.sort_values(["scheme", "horizon_s"])
    return latency, scores, regimes


def _nonnegative_coefficients(design: np.ndarray, target: np.ndarray) -> np.ndarray:
    coefficients = np.zeros(design.shape[1], dtype=float)
    if not np.any(target > 0.0):
        return coefficients
    active = np.zeros(design.shape[1], dtype=bool)
    active[0] = True
    active[1:] = np.ptp(design[:, 1:], axis=0) > 0.0
    if not np.any(active):
        return coefficients
    reduced = design[:, active]
    if lsq_linear is not None:
        fitted = lsq_linear(
            reduced,
            target,
            bounds=(0.0, np.inf),
            lsmr_tol="auto",
            max_iter=2000,
        ).x
    else:  # pragma: no cover - numpy fallback
        gram = reduced.T @ reduced + 1e-8 * np.eye(reduced.shape[1])
        fitted = np.linalg.solve(gram, reduced.T @ target)
        fitted = np.maximum(fitted, 0.0)
    coefficients[active] = fitted
    return coefficients


def estimate_hawkes(regimes: pd.DataFrame, lags: int = 16) -> dict | None:
    """Fit a discrete-time exponential-memory Hawkes model.

    Each observation contributes one OBI event and one forward-return event,
    giving a 13-state marked point process (9 OBI regimes + 4 return regimes).
    The lagged kernel is estimated by nonnegative least squares. The reported
    13x13 matrix is the integrated kernel, i.e. the sum of the lag coefficients.
    """
    required = {"timestamp_ns", "obi_regime", "return_regime"}
    if regimes.empty or not required <= set(regimes.columns) or lags < 1:
        return None

    obi = regimes["obi_regime"].to_numpy(dtype=np.int64)
    returns = regimes["return_regime"].to_numpy(dtype=np.int64)
    valid = (
        (obi >= 0)
        & (obi < OBI_REGIME_COUNT)
        & (returns >= 0)
        & (returns < RETURN_REGIME_COUNT)
    )
    obi = obi[valid]
    returns = returns[valid]
    samples = int(obi.size)
    if samples <= lags + 2:
        print(
            f"warning: {samples} regime tuples are insufficient for {lags} Hawkes lags"
        )
        return None

    events = np.zeros((samples, HAWKES_STATE_COUNT), dtype=float)
    index = np.arange(samples)
    events[index, obi] = 1.0
    events[index, OBI_REGIME_COUNT + returns] = 1.0

    rows = samples - lags
    design = np.empty((rows, 1 + HAWKES_STATE_COUNT * lags), dtype=float)
    design[:, 0] = 1.0
    for lag in range(1, lags + 1):
        start = 1 + (lag - 1) * HAWKES_STATE_COUNT
        design[:, start : start + HAWKES_STATE_COUNT] = events[
            lags - lag : samples - lag
        ]
    targets = events[lags:]

    coefficients = np.zeros((1 + HAWKES_STATE_COUNT * lags, HAWKES_STATE_COUNT))
    for target in range(HAWKES_STATE_COUNT):
        coefficients[:, target] = _nonnegative_coefficients(design, targets[:, target])

    kernels = np.zeros((lags, HAWKES_STATE_COUNT, HAWKES_STATE_COUNT))
    for target in range(HAWKES_STATE_COUNT):
        kernels[:, :, target] = coefficients[1:, target].reshape(
            lags, HAWKES_STATE_COUNT
        )

    branching = kernels.sum(axis=0)  # source x target
    kernel = branching.T  # target x source
    eigenvalues = np.linalg.eigvals(branching)
    raw_ratio = float(np.max(np.abs(eigenvalues))) if eigenvalues.size else 0.0
    if not np.isfinite(raw_ratio):
        raw_ratio = 0.0
    stabilized = False
    branching_ratio = raw_ratio
    if raw_ratio >= 0.99:
        kernel = kernel * (0.99 / raw_ratio)
        branching_ratio = 0.99
        stabilized = True
    masked = kernel[OBI_REGIME_COUNT:, :OBI_REGIME_COUNT]  # return x obi
    row_norms = np.linalg.norm(masked, axis=1)

    return {
        "kernel": kernel,
        "masked": masked,
        "branching_ratio": branching_ratio,
        "raw_branching_ratio": raw_ratio,
        "stabilized": stabilized,
        "masked_frobenius_norm": float(np.linalg.norm(masked)),
        "masked_row_norms": row_norms,
        "samples": samples,
        "lags": int(lags),
    }


def benchmark_range(record: dict) -> int | None:
    for key in ("arg", "range", "arg0"):
        value = record.get(key)
        if value is not None:
            try:
                return int(value)
            except (TypeError, ValueError):
                pass
    match = re.search(r"/(\d+)(?:/|$)", record.get("name", ""))
    return int(match.group(1)) if match else None


def benchmark_ns_per_event(record: dict) -> float:
    items = to_float(record.get("items_per_second"), 0.0)
    if items > 0.0:
        return 1e9 / items
    count = benchmark_range(record)
    real_time = to_float(record.get("real_time"), 0.0)
    return real_time / count if count else real_time


def scaling_series(records: list[dict], prefix: str) -> list[tuple[int, float]]:
    rows = []
    for record in records:
        if not record.get("name", "").startswith(prefix):
            continue
        count = benchmark_range(record)
        if count:
            rows.append((count, benchmark_ns_per_event(record)))
    return sorted(rows)


def plot_latency(
    args: argparse.Namespace, latency: pd.DataFrame, benchmarks: list[dict]
) -> None:
    apply_dark_theme()
    fig, ax = plt.subplots(figsize=(12, 7), constrained_layout=True)
    categories = ["p50", "p90", "p99"]
    values = []
    for name in ("ring_buffer_raw", "ring_buffer_warmed"):
        series = latency.loc[latency["pipeline"] == name, "latency_ns"]
        values.append(
            [percentile(series, 50), percentile(series, 90), percentile(series, 99)]
        )
    if any(np.all(np.isnan(row)) for row in values):
        fallback = []
        for prefix in ("Disruptor/Throughput/CacheCold", "Disruptor/Throughput/CacheWarmed"):
            record = find_benchmark(benchmarks, prefix)
            fallback.append(benchmark_ns_per_event(record) if record else float("nan"))
        categories = ["mean"]
        values = [[fallback[0]], [fallback[1]]]

    x = np.arange(len(categories))
    width = 0.36
    bars_cold = ax.bar(
        x - width / 2,
        values[0],
        width,
        label="Cache cold",
        color=ACCENT_COLD,
        edgecolor=DARK_BACKGROUND,
    )
    bars_warm = ax.bar(
        x + width / 2,
        values[1],
        width,
        label="Cache warm",
        color=ACCENT_WARM,
        edgecolor=DARK_BACKGROUND,
    )
    for bars, series in ((bars_cold, values[0]), (bars_warm, values[1])):
        ax.bar_label(
            bars,
            labels=[f"{value:.1f}" if np.isfinite(value) else "n/a" for value in series],
            padding=4,
            color=DARK_TEXT,
            fontsize=11,
        )
    ax.set_xticks(x, categories)
    ax.set_ylabel("Latency (ns per event)")
    ax.set_title("RingBuffer hand-off latency: cache cold vs cache warm")
    ax.legend(frameon=True)
    path = args.plots_dir / "latency_distribution.png"
    fig.savefig(path, dpi=220, bbox_inches="tight", facecolor=DARK_BACKGROUND)
    plt.close(fig)


def plot_queue_vs_ringbuffer(
    args: argparse.Namespace, benchmarks: list[dict]
) -> None:
    apply_dark_theme()
    fig, ax = plt.subplots(figsize=(13, 7), constrained_layout=True)
    queue = scaling_series(benchmarks, "Queue/Mutex")
    ring = scaling_series(benchmarks, "RingBuffer/Scaled")
    if queue:
        ax.plot(
            [row[0] for row in queue],
            [row[1] for row in queue],
            marker="o",
            markersize=7,
            linewidth=2.4,
            color=ACCENT_QUEUE,
            label="std::queue + std::mutex",
        )
    if ring:
        ax.plot(
            [row[0] for row in ring],
            [row[1] for row in ring],
            marker="s",
            markersize=6,
            linewidth=2.4,
            color=ACCENT_RING,
            label="Lock-free RingBuffer",
        )
    if not queue and not ring:
        ax.text(
            0.5,
            0.5,
            "Scaling benchmark data unavailable",
            ha="center",
            va="center",
            transform=ax.transAxes,
            color=DARK_TEXT,
            fontsize=18,
        )
    else:
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_ylim(bottom=2.5)
        ax.set_xlabel("Events per benchmark iteration")
        ax.set_ylabel("Latency (ns per event, log scale)")
        ax.set_title("Latency scaling: mutex queue vs lock-free RingBuffer")
        ax.legend(frameon=True)
        if queue and ring:
            queue_max = queue[-1][1]
            ring_max = ring[-1][1]
            speedup = queue_max / ring_max if ring_max > 0 else float("nan")
            if np.isfinite(speedup):
                ax.text(
                    0.98,
                    0.04,
                    f"At 1,000,000 events: {speedup:.2f}x lower latency",
                    transform=ax.transAxes,
                    color=DARK_TEXT,
                    fontsize=13,
                    ha="right",
                    va="bottom",
                    bbox={
                        "boxstyle": "round,pad=0.3",
                        "facecolor": DARK_AXES,
                        "edgecolor": DARK_GRID,
                        "alpha": 0.92,
                    },
                )
    path = args.plots_dir / "queue_vs_ringbuffer.png"
    fig.savefig(path, dpi=220, bbox_inches="tight", facecolor=DARK_BACKGROUND)
    plt.close(fig)


def plot_signal_quality(args: argparse.Namespace, scores: pd.DataFrame) -> None:
    apply_dark_theme()
    schemes = ["UF", "LF", "MTF"]
    metrics = [
        ("pearson", "Count OBI: Pearson", "RdBu_r", True),
        ("r2", "Count OBI: R-squared", "rocket", False),
        ("volume_pearson", "Volume OBI: Pearson", "RdBu_r", True),
        ("volume_r2", "Volume OBI: R-squared", "rocket", False),
    ]
    fig, axes = plt.subplots(2, 2, figsize=(17, 11), constrained_layout=True)
    for ax, (column, title, cmap, diverging) in zip(axes.ravel(), metrics):
        if scores.empty or column not in scores:
            ax.set_axis_off()
            ax.set_title(f"{title} (no data)")
            continue
        pivot = scores.pivot_table(
            index="scheme",
            columns="horizon_s",
            values=column,
            aggfunc="mean",
            observed=True,
        ).reindex(index=schemes, columns=HORIZON_ORDER)
        limit = max(float(np.nanmax(np.abs(pivot.to_numpy()))), 1e-9)
        sns.heatmap(
            pivot,
            annot=True,
            fmt=".3f",
            cmap=cmap,
            vmin=-limit if diverging else 0.0,
            vmax=limit,
            center=0.0 if diverging else None,
            linewidths=0.6,
            linecolor=DARK_BACKGROUND,
            cbar_kws={"shrink": 0.85},
            ax=ax,
        )
        ax.set_title(title)
        ax.set_xlabel("Forward return horizon (s)")
        ax.set_ylabel("Structural filter")
        ax.tick_params(colors=DARK_TEXT)
    fig.suptitle(
        "Signal quality: count vs volume-weighted OBI (UF/LF/MTF)",
        fontsize=20,
        fontweight="bold",
        color=DARK_TEXT,
    )
    path = args.plots_dir / "signal_quality_heatmap.png"
    fig.savefig(path, dpi=220, bbox_inches="tight", facecolor=DARK_BACKGROUND)
    plt.close(fig)


def plot_hawkes_kernel(args: argparse.Namespace, kernel: np.ndarray) -> None:
    apply_dark_theme()
    fig, ax = plt.subplots(figsize=(13, 11), constrained_layout=True)
    sns.heatmap(
        kernel,
        xticklabels=HAWKES_LABELS,
        yticklabels=HAWKES_LABELS,
        cmap="rocket",
        linewidths=0.5,
        linecolor="white",
        cbar_kws={"label": "Integrated kernel coefficient", "shrink": 0.8},
        ax=ax,
    )
    ax.set_xlabel("Source state (excitation)")
    ax.set_ylabel("Target state")
    ax.set_title("13x13 Hawkes kernel matrix")
    ax.tick_params(axis="x", rotation=45)
    path = args.plots_dir / "hawkes_kernel.png"
    fig.savefig(path, dpi=220, bbox_inches="tight", facecolor=DARK_BACKGROUND)
    plt.close(fig)


def plot_hawkes_cross_excitation(args: argparse.Namespace, masked: np.ndarray) -> None:
    apply_dark_theme()
    fig, ax = plt.subplots(figsize=(11, 5), constrained_layout=True)
    sns.heatmap(
        masked,
        xticklabels=HAWKES_LABELS[:OBI_REGIME_COUNT],
        yticklabels=HAWKES_LABELS[OBI_REGIME_COUNT:],
        annot=True,
        fmt=".4f",
        cmap="rocket",
        linewidths=0.5,
        linecolor="white",
        cbar_kws={"label": "Kernel coefficient", "shrink": 0.85},
        ax=ax,
    )
    ax.set_xlabel("OBI regime (source)")
    ax.set_ylabel("Return regime (target)")
    ax.set_title("4x9 masked OBI-to-return cross-excitation")
    path = args.plots_dir / "hawkes_cross_excitation.png"
    fig.savefig(path, dpi=220, bbox_inches="tight", facecolor=DARK_BACKGROUND)
    plt.close(fig)


def write_hawkes_outputs(args: argparse.Namespace, hawkes: dict) -> None:
    kernel = pd.DataFrame(
        hawkes["kernel"], index=HAWKES_LABELS, columns=HAWKES_LABELS
    )
    kernel.index.name = "target"
    kernel.columns.name = "source"
    kernel.to_csv(args.hawkes_kernel_csv)

    masked = pd.DataFrame(
        hawkes["masked"],
        index=HAWKES_LABELS[OBI_REGIME_COUNT:],
        columns=HAWKES_LABELS[:OBI_REGIME_COUNT],
    )
    masked.index.name = "return_regime"
    masked.columns.name = "obi_regime"
    masked.to_csv(args.hawkes_cross_csv)

    summary = {
        "source": str(args.regimes_csv),
        "samples": hawkes["samples"],
        "lags": hawkes["lags"],
        "branching_ratio": hawkes["branching_ratio"],
        "raw_branching_ratio": hawkes["raw_branching_ratio"],
        "stabilized": hawkes["stabilized"],
        "masked_frobenius_norm": hawkes["masked_frobenius_norm"],
        "masked_row_norms": {
            f"RET_{index}": float(value)
            for index, value in enumerate(hawkes["masked_row_norms"])
        },
    }
    args.hawkes_summary_json.write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )


def load_benchmark_context(path: Path) -> dict:
    if not path.exists():
        return {}
    with path.open(encoding="utf-8") as source:
        payload = json.load(source)
    return payload.get("context", {})


def load_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def fmt(value: float, digits: int = 2) -> str:
    return f"{value:,.{digits}f}" if np.isfinite(value) else "n/a"


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    lines = ["| " + " | ".join(headers) + " |"]
    lines.append("| " + " | ".join(["---"] * len(headers)) + " |")
    for row in rows:
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def latency_stats(latency: pd.DataFrame, name: str) -> dict[str, float]:
    values = latency.loc[latency["pipeline"] == name, "latency_ns"]
    return {
        "count": float(values.size),
        "p50": percentile(values, 50),
        "p90": percentile(values, 90),
        "p99": percentile(values, 99),
        "p999": percentile(values, 99.9),
    }


def build_report(
    args: argparse.Namespace,
    latency: pd.DataFrame,
    scores: pd.DataFrame,
    hawkes: dict | None,
    pnl: dict | None,
    benchmarks: list[dict],
    context: dict,
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    plot_prefix = args.plots_dir.as_posix()
    raw = latency_stats(latency, "ring_buffer_raw")
    warmed = latency_stats(latency, "ring_buffer_warmed")
    simd = latency_stats(latency, "obi_simd")
    scalar = latency_stats(latency, "obi_scalar")

    queue = scaling_series(benchmarks, "Queue/Mutex")
    ring = scaling_series(benchmarks, "RingBuffer/Scaled")
    ring_by_count = dict(ring)
    scaling_rows = []
    for count, queue_ns in queue:
        ring_ns = ring_by_count.get(count, float("nan"))
        ratio = queue_ns / ring_ns if np.isfinite(ring_ns) and ring_ns > 0 else float("nan")
        scaling_rows.append(
            [
                f"{count:,}",
                fmt(queue_ns),
                fmt(ring_ns),
                f"{ratio:.2f}x" if np.isfinite(ratio) else "n/a",
            ]
        )
    peak_speedup = float("nan")
    if queue and ring:
        peak_speedup = queue[-1][1] / ring[-1][1] if ring[-1][1] > 0 else float("nan")

    simd_bench = find_benchmark(benchmarks, "OBI/SIMD/Welford")
    scalar_bench = find_benchmark(benchmarks, "OBI/Scalar/Welford")
    simd_time = to_float(simd_bench.get("real_time")) if simd_bench else float("nan")
    scalar_time = (
        to_float(scalar_bench.get("real_time")) if scalar_bench else float("nan")
    )
    simd_speedup = (
        scalar_time / simd_time
        if np.isfinite(simd_time) and np.isfinite(scalar_time) and simd_time > 0.0
        else float("nan")
    )

    signal_rows = []
    if not scores.empty:
        for scheme in SCHEME_ORDER:
            subset = scores.loc[scores["scheme"] == scheme]
            if subset.empty:
                continue
            def mean_abs(column: str) -> float:
                return (
                    float(subset[column].abs().mean())
                    if column in subset
                    else float("nan")
                )

            def mean_value(column: str) -> float:
                return float(subset[column].mean()) if column in subset else float("nan")

            signal_rows.append(
                [
                    scheme,
                    f"{mean_abs('pearson'):.5f}",
                    f"{mean_value('r2'):.5f}",
                    f"{mean_abs('volume_pearson'):.5f}",
                    f"{mean_value('volume_r2'):.5f}",
                    f"{mean_abs('trade_pearson'):.5f}",
                    f"{mean_abs('volume_trade_pearson'):.5f}",
                ]
            )

    best_row = None
    best_volume_row = None
    if not scores.empty:
        ranked = scores.assign(
            objective=scores["pearson"].abs().fillna(0.0)
        ).sort_values("objective", ascending=False)
        if not ranked.empty:
            best_row = ranked.iloc[0]
        if "volume_pearson" in scores:
            ranked_volume = scores.assign(
                objective=scores["volume_pearson"].abs().fillna(0.0)
            ).sort_values("objective", ascending=False)
            if not ranked_volume.empty:
                best_volume_row = ranked_volume.iloc[0]

    hawkes_ratio = (
        hawkes["branching_ratio"] if hawkes is not None else float("nan")
    )
    hawkes_masked_norm = (
        hawkes["masked_frobenius_norm"] if hawkes is not None else float("nan")
    )

    environment_rows = []
    for key, label in (
        ("host_name", "Host"),
        ("cpu_info", "CPU"),
        ("num_cpus", "Logical CPUs"),
        ("mhz_per_cpu", "Nominal MHz"),
        ("library_version", "Google Benchmark"),
        ("date", "Benchmark timestamp"),
    ):
        if key in context:
            environment_rows.append([label, str(context[key])])

    summary = [
        "## Abstract",
        "",
        f"This report evaluates the low-latency transport, count- and volume-weighted OBI "
        f"signal layers, and deterministic backtest of the HFT LOB engine on `{args.dataset}`. "
        f"All numbers are reproducible from the commands in the final section.",
        "",
        "## Executive Summary",
        "",
    ]
    if scaling_rows:
        summary.append(
            f"- At **1,000,000 events**, the lock-free RingBuffer is "
            f"**{peak_speedup:.2f}x faster per event** than `std::queue` guarded by "
            f"`std::mutex`."
        )
    if np.isfinite(warmed["p50"]):
        summary.append(
            f"- Cache-warm RingBuffer hand-off latency is **{warmed['p50']:.1f} ns p50** "
            f"and **{warmed['p99']:.1f} ns p99**."
        )
    if np.isfinite(simd_speedup):
        summary.append(
            f"- SIMD Welford is **{simd_speedup:.2f}x** faster than the scalar accumulator "
            f"on the benchmark workload."
        )
    if best_row is not None:
        summary.append(
            f"- Strongest in-sample count OBI correlation: **{best_row['scheme']}** at "
            f"**{int(best_row['horizon_s'])}s** (Pearson {best_row['pearson']:.5f}, "
            f"R2 {best_row['r2']:.5f})."
        )
    if best_volume_row is not None:
        summary.append(
            f"- Strongest in-sample volume OBI correlation: **{best_volume_row['scheme']}** "
            f"at **{int(best_volume_row['horizon_s'])}s** "
            f"(Pearson {best_volume_row['volume_pearson']:.5f}, "
            f"R2 {best_volume_row['volume_r2']:.5f})."
        )
    if pnl is not None:
        summary.append(
            f"- Deterministic backtest: **{pnl.get('trade_count', 0)}** trades, "
            f"total return **{100.0 * to_float(pnl.get('total_return')):.3f}%**, "
            f"max drawdown **{100.0 * to_float(pnl.get('max_drawdown')):.3f}%**."
        )
    if hawkes is not None:
        summary.append(
            f"- Layer-3 Hawkes branching ratio (spectral radius): **{hawkes_ratio:.4f}**; "
            f"4x9 OBI-to-return cross-excitation norm: **{hawkes_masked_norm:.4f}**."
        )

    lines = [
        "# Low-Latency LOB Engine: Benchmark & Signal-Quality Report",
        "",
        f"*Generated {now}*",
        "",
    ]
    lines.extend(summary)
    lines.extend(
        [
            "",
            "## 1. Lock-Free Transport vs. Mutex Queue",
            "",
            f"![Latency scaling: mutex queue vs lock-free RingBuffer]"
            f"({plot_prefix}/queue_vs_ringbuffer.png)",
            "",
        ]
    )
    if scaling_rows:
        lines.append(
            markdown_table(
                ["Events", "std::queue + mutex (ns/event)", "RingBuffer (ns/event)", "Speedup"],
                scaling_rows,
            )
        )
        lines.append(
            "\nThe RingBuffer is preallocated and reuses a fixed-capacity ring; the queue "
            "baseline performs a locked push and a locked pop for every event. Both paths "
            "execute the hand-off on one thread, so this isolates synchronization and "
            "allocation overhead rather than multi-core contention."
        )
    else:
        lines.append(
            "Scaling records were not found. Run `./tools/run_pipeline.sh` to regenerate "
            "`benchmarks/results/latest_bench.json`."
        )

    lines.extend(
        [
            "",
            "## 2. Cache-Cold vs. Cache-Warm Hand-Off",
            "",
            f"![RingBuffer latency: cache cold vs cache warm]"
            f"({plot_prefix}/latency_distribution.png)",
            "",
            markdown_table(
                ["Path", "p50 (ns)", "p90 (ns)", "p99 (ns)", "Samples"],
                [
                    [
                        "Cache cold",
                        fmt(raw["p50"], 1),
                        fmt(raw["p90"], 1),
                        fmt(raw["p99"], 1),
                        f"{int(raw['count']):,}",
                    ],
                    [
                        "Cache warm",
                        fmt(warmed["p50"], 1),
                        fmt(warmed["p90"], 1),
                        fmt(warmed["p99"], 1),
                        f"{int(warmed['count']):,}",
                    ],
                ],
            ),
            "",
            "Cache-warm measurements run an explicit warm-up pass and prefetch the ring before "
            "sampling. Latency is amortised over batches of 64 hand-offs to stay above the "
            "monotonic-clock granularity.",
            "",
            "## 3. Signal Quality",
            "",
            f"![Signal-quality heatmap]({plot_prefix}/signal_quality_heatmap.png)",
            "",
            markdown_table(
                [
                    "Filter",
                    "Count mean abs Pearson",
                    "Count mean R2",
                    "Volume mean abs Pearson",
                    "Volume mean R2",
                    "Trade count mean abs Pearson",
                    "Trade volume mean abs Pearson",
                ],
                signal_rows,
            ),
            "",
            "Mean absolute Pearson is used for ranking so that the OBI sign convention does not "
            "obscure magnitude. R2 is the square of the contemporaneous Pearson correlation. "
            "Volume-weighted OBI uses the same event stream with quantities instead of flat "
            "counts. Regime cross-correlations are computed on AR(1) residuals.",
            "",
            "## 4. Layer 3: Hawkes Cross-Excitation",
            "",
        ]
    )
    hawkes_table: list[list[str]] = []
    if hawkes is not None:
        for index, label in enumerate(HAWKES_LABELS[OBI_REGIME_COUNT:]):
            hawkes_table.append(
                [label]
                + [f"{value:.4f}" for value in hawkes["masked"][index]]
                + [f"{hawkes['masked_row_norms'][index]:.4f}"]
            )
        lines.extend(
            [
                f"- Regime tuples: **{hawkes['samples']:,}**; lags: **{hawkes['lags']}**.",
                f"- Branching ratio: **{hawkes['branching_ratio']:.4f}** "
                f"(raw **{hawkes['raw_branching_ratio']:.4f}**)."
                + (
                    " The raw estimate was rescaled to the stability boundary."
                    if hawkes["stabilized"]
                    else ""
                ),
                f"- 4x9 masked OBI-to-return norm: **{hawkes['masked_frobenius_norm']:.4f}**.",
                "",
                markdown_table(
                    ["Return target"]
                    + HAWKES_LABELS[:OBI_REGIME_COUNT]
                    + ["L2 norm"],
                    hawkes_table,
                ),
                "",
                "The 13x13 kernel is fitted in `benchmarks/plot_results.py` from the "
                "time-synchronized regime tuples exported by `hft_eval`. It is a discrete-time "
                "exponential-memory Hawkes model with nonnegative lag coefficients; the matrix "
                "shown is the integrated kernel.",
                "",
                f"![13x13 Hawkes kernel]({plot_prefix}/hawkes_kernel.png)",
                "",
                f"![4x9 masked cross-excitation]({plot_prefix}/hawkes_cross_excitation.png)",
            ]
        )
    else:
        lines.append(
            "Regime tuples were unavailable or too small; the Python Hawkes layer was skipped."
        )

    lines.extend(["", "## 5. Deterministic Backtest (PnL)", ""])
    if pnl is not None:
        config = pnl.get("config", {})
        lines.extend(
            [
                f"- Signal: **{config.get('signal', 'volume')}** OBI, "
                f"scheme **{config.get('scheme', 'UF')}**, "
                f"horizon **{config.get('horizon_s', 1)}s**.",
                f"- Execution: **{config.get('liquidity', 'taker')}**, "
                f"fee **{to_float(config.get('fee_bps')):.2f} bps per side**, "
                f"latency **{to_float(config.get('latency_us')):.1f}us**, "
                f"threshold **{to_float(config.get('threshold')):.4f}**.",
                "",
                markdown_table(
                    ["Metric", "Value"],
                    [
                        ["Trades", f"{int(to_float(pnl.get('trade_count'))):,}"],
                        ["Total return", f"{100.0 * to_float(pnl.get('total_return')):.4f}%"],
                        ["Mean trade return", f"{1e4 * to_float(pnl.get('mean_trade_return')):.3f} bps"],
                        ["Median trade return", f"{1e4 * to_float(pnl.get('median_trade_return')):.3f} bps"],
                        ["Std trade return", f"{1e4 * to_float(pnl.get('std_trade_return')):.3f} bps"],
                        ["Win rate", f"{100.0 * to_float(pnl.get('win_rate')):.2f}%"],
                        ["Profit factor", f"{to_float(pnl.get('profit_factor')):.3f}"],
                        ["Sharpe (per trade)", f"{to_float(pnl.get('sharpe_trade')):.3f}"],
                        ["Sharpe (daily, annualized)", f"{to_float(pnl.get('sharpe_daily_annualized')):.3f}"],
                        ["Max drawdown", f"{100.0 * to_float(pnl.get('max_drawdown')):.4f}%"],
                        ["Total fees", f"{1e4 * to_float(pnl.get('total_fees')):.3f} bps"],
                        ["Average holding", f"{to_float(pnl.get('average_holding_ns')) / 1e6:.3f} ms"],
                    ],
                ),
                "",
                f"Trade-level PnL is stored in `{pnl.get('trades_file', 'benchmarks/results/pnl_trades.csv')}`; "
                "the raw net-return vector is also embedded in `pnl_summary.json`.",
                "",
                "The simulator fills at the first snapshot on or after signal time plus the "
                "configured latency, holds for the selected horizon, and applies the fee to both "
                "legs. Trades are non-overlapping and use mid-price fills.",
            ]
        )
    else:
        lines.append("PnL summary unavailable; run `hft_eval` to generate it.")

    lines.extend(
        [
            "",
            "## Methodology & Reproducibility",
            "",
            "### Pipeline",
            "",
            "1. The CSV loader normalizes timestamps, event types, sides, quantities, and quotes.",
            "2. A preallocated open-addressed tracker computes lifetime, modification-count, and "
            "modification-time state per order.",
            "3. Four rolling event-count windows (1s, 10s, 30s, 50s) maintain UF/LF/MF/MTF "
            "aggregates in O(1).",
            "4. Pearson/R2 use a SIMD Welford accumulator. Regime scores discretize OBI into 9 "
            "buckets and returns into 4 buckets, then remove AR(1) persistence.",
            "5. C++ exports `(timestamp_ns, obi_regime, return_regime)` tuples; Python fits the "
            "13x13 Hawkes kernel and 4x9 mask.",
            "6. Volume-weighted OBI substitutes event and trade quantities for flat counts. The "
            "deterministic backtest fills at mid after the configured latency and applies "
            "maker/taker fees.",
            "",
            "### Exact Commands",
            "",
            "```bash",
            "python3 -m pip install -r benchmarks/requirements.txt",
            "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release",
            "cmake --build build -j",
            "ctest --test-dir build --output-on-failure",
            "cmake --install build --prefix /opt/hft",
            "./tools/run_pipeline.sh",
            "",
            "# Regenerate charts and this report from existing artifacts",
            "python3 benchmarks/plot_results.py",
            "```",
            "",
            "`run_pipeline.sh` performs the build, unit tests, synthetic-data generation, signal "
            "evaluation, Google Benchmark run, and chart/report generation. The benchmark "
            "executable writes strictly to `benchmarks/results/latest_bench.json`; latency "
            "samples, scores, regime tuples, and Hawkes artifacts land in "
            "`benchmarks/results/`.",
            "",
            *(
                [f"Real-data replay: `{args.repro_command}`", ""]
                if args.repro_command
                else []
            ),
            "### SDK Consumption",
            "",
            "```cmake",
            "find_package(HftSdk REQUIRED)",
            "target_link_libraries(your_target PRIVATE HftSdk::sdk)",
            "```",
            "",
            "```cpp",
            "#include <hft/HftSdk.hpp>",
            "hft::sdk::OBIFilterEngine engine;",
            "engine.process(events);",
            "auto metrics = engine.metrics();",
            "```",
            "",
            "```python",
            "import hft_sdk",
            "engine = hft_sdk.OBIFilterEngine()",
            "engine.process_arrays(order_id, timestamp_ns, price_ticks, quantity,",
            "                      bid_price_ticks, ask_price_ticks, type_codes, side_codes)",
            "```",
            "",
            "Build the compiled SDK with `-DHFT_BUILD_SHARED_SDK=ON` and bindings with "
            "`-DHFT_BUILD_PYTHON=ON` (both are on by default).",
            "",
            "### Artifacts",
            "",
            "- `benchmarks/results/latest_bench.json` - Google Benchmark JSON",
            "- `benchmarks/results/latency_samples.csv` - raw latency samples",
            "- `benchmarks/results/signal_scores.csv` - filter/horizon scores",
            "- `benchmarks/results/layer3_regimes.csv` - Layer-3 tuple export",
            "- `benchmarks/results/pnl_summary.json` - backtest summary",
            "- `benchmarks/results/pnl_trades.csv` - trade-level PnL",
            f"- `{plot_prefix}/*.png` - publication charts",
            "",
        ]
    )
    if environment_rows:
        lines.extend(
            [
                "### Benchmark Environment",
                "",
                markdown_table(["Field", "Value"], environment_rows),
                "",
            ]
        )
    lines.extend(
        [
            "## Scope and Limitations",
            "",
            "- Signal-quality numbers are computed on the deterministic synthetic LOB stream "
            "supplied for pipeline validation; they are not a production alpha forecast.",
            "- The queue comparison is a single-thread hand-off benchmark. It measures mutex and "
            "allocation overhead, not multi-core contention.",
            "- Latency figures are host- and compiler-dependent. Re-run the pipeline on the target "
            "deployment hardware before making capacity decisions.",
            "- The Hawkes kernel is a discrete-time least-squares estimate, not a continuous-time "
            "maximum-likelihood fit. Validate it out of sample.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    args = parse_args()
    args.plots_dir.mkdir(parents=True, exist_ok=True)
    benchmarks = load_benchmarks(args.benchmark_json)
    context = load_benchmark_context(args.benchmark_json)
    latency, scores, regimes = load_frames(args)
    hawkes = estimate_hawkes(regimes, args.hawkes_lags)
    pnl = load_json(args.pnl_summary)

    plot_latency(args, latency, benchmarks)
    plot_queue_vs_ringbuffer(args, benchmarks)
    plot_signal_quality(args, scores)
    if hawkes is not None:
        plot_hawkes_kernel(args, hawkes["kernel"])
        plot_hawkes_cross_excitation(args, hawkes["masked"])
        write_hawkes_outputs(args, hawkes)

    report = build_report(args, latency, scores, hawkes, pnl, benchmarks, context)
    args.report.write_text(report, encoding="utf-8")
    print(f"wrote {args.report}")
    written = [
        "latency_distribution.png",
        "queue_vs_ringbuffer.png",
        "signal_quality_heatmap.png",
    ]
    if hawkes is not None:
        written.extend(["hawkes_kernel.png", "hawkes_cross_excitation.png"])
        written.extend(
            [
                str(args.hawkes_kernel_csv),
                str(args.hawkes_cross_csv),
                str(args.hawkes_summary_json),
            ]
        )
    for name in written:
        path = args.plots_dir / name if not name.endswith((".csv", ".json")) else Path(name)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
