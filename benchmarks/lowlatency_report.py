#!/usr/bin/env python3
"""Report for the cross-thread transport benchmark and paper-2 microbenchmarks."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

DARK_BACKGROUND = "#0d1117"
DARK_AXES = "#161b22"
DARK_GRID = "#30363d"
DARK_TEXT = "#e6edf3"
ACCENTS = ["#58a6ff", "#3fb950", "#f0883e", "#f85149"]

PAIRS = [
    ("RuntimeFactorial", "Constexpr", "Micro/RuntimeFactorial", "Micro/Constexpr"),
    ("LoopPlain", "LoopUnrolled", "Micro/LoopPlain", "Micro/LoopUnrolled"),
    ("NoPrefetch", "WithPrefetch", "Micro/NoPrefetch", "Micro/WithPrefetch"),
    ("UnsignedCompare", "SignedCompare", "Micro/UnsignedCompare", "Micro/SignedCompare"),
    ("MixedFloatDouble", "FloatOnly", "Micro/MixedFloatDouble", "Micro/FloatOnly"),
    ("NoInline", "AlwaysInline", "Micro/NoInline", "Micro/AlwaysInline"),
    ("BranchBaseline", "BranchReduced", "Micro/BranchBaseline", "Micro/BranchReduced"),
    ("ArrayScalar", "ArraySimd", "Micro/ArrayScalar", "Micro/ArraySimd"),
    ("PairsRollingScalar", "PairsRollingSimd", "Micro/PairsRollingScalar", "Micro/PairsRollingSimd"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--threaded-json", type=Path,
                        default=Path("benchmarks/results/lowlatency/threaded_bench.json"))
    parser.add_argument("--microbench-json", type=Path,
                        default=Path("benchmarks/results/lowlatency/microbench.json"))
    parser.add_argument("--report", type=Path,
                        default=Path("benchmarks/results/lowlatency/LOW_LATENCY_REPORT.md"))
    parser.add_argument("--plots-dir", type=Path,
                        default=Path("benchmarks/plots/lowlatency"))
    parser.add_argument("--cache-counters", type=Path,
                        default=Path("benchmarks/results/cache_counters.txt"))
    return parser.parse_args()


def theme() -> None:
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


def load(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text())


def benchmark_map(payload: dict) -> dict[str, dict]:
    return {record["name"]: record for record in payload.get("benchmarks", [])}


def plot_threaded(payload: dict, path: Path) -> None:
    if not payload:
        return
    theme()
    labels = []
    means = []
    p99 = []
    for key, label in (("queue_mutex", "std::queue + mutex"), ("ring_buffer", "Disruptor ring")):
        if key in payload:
            labels.append(label)
            means.append(payload[key].get("per_event_ns_median", payload[key]["per_event_ns_mean"]))
            p99.append(payload[key]["per_event_ns_p99"])
    if not labels:
        return
    x = np.arange(len(labels))
    figure, axis = plt.subplots(figsize=(8, 5))
    axis.bar(x - 0.2, means, width=0.4, label="mean", color=ACCENTS[0])
    axis.bar(x + 0.2, p99, width=0.4, label="p99", color=ACCENTS[2])
    axis.set_xticks(x)
    axis.set_xticklabels(labels)
    axis.set_ylabel("ns per event")
    axis.set_title("Cross-thread hand-off: queue vs Disruptor ring")
    axis.legend()
    figure.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=180)
    plt.close(figure)


def plot_microbench(records: dict[str, dict], path: Path) -> None:
    if not records:
        return
    theme()
    names = []
    values = []
    for name, record in records.items():
        if not name.startswith("Micro/"):
            continue
        names.append(name.replace("Micro/", ""))
        values.append(float(record.get("cpu_time", record.get("real_time", 0.0))))
    order = np.argsort(values)
    names = [names[index] for index in order]
    values = [values[index] for index in order]
    figure, axis = plt.subplots(figsize=(10, max(5, 0.35 * len(names))))
    axis.barh(names, values, color=ACCENTS[1])
    axis.set_xscale("log")
    axis.set_xlabel("CPU time per iteration (ns, log scale)")
    axis.set_title("Paper-2 microbenchmark suite")
    figure.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=180)
    plt.close(figure)


def main() -> int:
    args = parse_args()
    threaded = load(args.threaded_json)
    micro = benchmark_map(load(args.microbench_json))
    args.plots_dir.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)

    plot_threaded(threaded, args.plots_dir / "threaded_queue_vs_ring.png")
    plot_microbench(micro, args.plots_dir / "microbenchmarks.png")

    lines = [
        "# Low-Latency Transport & Microbenchmark Report",
        "",
        "Generated from `hft_threaded_bench` and `hft_microbench`.",
        "",
    ]
    if threaded:
        queue = threaded.get("queue_mutex", {})
        ring = threaded.get("ring_buffer", {})
        queue_latency = queue.get("per_event_ns_median", queue.get("per_event_ns_mean", 0.0))
        ring_latency = ring.get("per_event_ns_median", ring.get("per_event_ns_mean", 0.0))
        speedup = (
            queue_latency / ring_latency
            if ring_latency > 0.0 else float("nan")
        )
        lines += [
            "## Cross-thread transport",
            "",
            f"Events per run: **{threaded.get('events', 0):,}**; measured runs: "
            f"**{threaded.get('runs', 0)}**.",
            "",
            "| Path | Median ns/event | p50 ns/event | p99 ns/event | Events/s |",
            "| --- | --- | --- | --- | --- |",
            f"| std::queue + mutex | {queue_latency:.3f} | "
            f"{queue.get('per_event_ns_p50', 0):.3f} | {queue.get('per_event_ns_p99', 0):.3f} | "
            f"{queue.get('events_per_second', 0):,.0f} |",
            f"| Disruptor ring | {ring_latency:.3f} | "
            f"{ring.get('per_event_ns_p50', 0):.3f} | {ring.get('per_event_ns_p99', 0):.3f} | "
            f"{ring.get('events_per_second', 0):,.0f} |",
            "",
            f"Ring speedup over the mutex queue: **{speedup:.2f}x**.",
            "",
            "![Cross-thread queue vs ring](benchmarks/plots/lowlatency/threaded_queue_vs_ring.png)",
            "",
        ]
    if micro:
        lines += [
            "## Microbenchmarks",
            "",
            "The paper-2 microbenchmarks are illustrative and host-dependent. The",
            "comparison columns report the measured improvement of the optimised",
            "variant over its baseline.",
            "",
            "| Baseline | Optimised | Baseline ns | Optimised ns | Improvement |",
            "| --- | --- | --- | --- | --- |",
        ]
        for baseline_name, optimised_name, baseline_key, optimised_key in PAIRS:
            baseline = micro.get(baseline_key)
            optimised = micro.get(optimised_key)
            if baseline is None or optimised is None:
                continue
            baseline_time = float(baseline.get("cpu_time", baseline.get("real_time", 0.0)))
            optimised_time = float(optimised.get("cpu_time", optimised.get("real_time", 0.0)))
            improvement = (baseline_time - optimised_time) / baseline_time * 100.0 \
                if baseline_time > 0.0 else float("nan")
            lines.append(
                f"| {baseline_name} | {optimised_name} | {baseline_time:.4f} | "
                f"{optimised_time:.4f} | {improvement:.2f}% |"
            )
        lines += [
            "",
            "![Microbenchmarks](benchmarks/plots/lowlatency/microbenchmarks.png)",
            "",
        ]
    if args.cache_counters.exists():
        counters = args.cache_counters.read_text().strip()
        lines += [
            "## Hardware cache counters",
            "",
            "```text",
            counters,
            "```",
            "",
        ]
    lines += [
        "## Method notes",
        "",
        "- The cross-thread benchmark uses one producer thread and one consumer",
        "  thread. The queue baseline uses `std::mutex` plus a condition variable;",
        "  the ring uses the Disruptor-style busy-spin consumer.",
        "- Timing includes thread creation and join, so it is a conservative",
        "  comparison rather than a pinned-core production measurement.",
        "- The microbenchmark suite covers constexpr, loop unrolling, prefetching,",
        "  signedness, float/double consistency, inlining, branch reduction and",
        "  SIMD. It is not a substitute for `perf` cache counters.",
        "- Negative results are retained: modern compilers auto-vectorise the",
        "  scalar loops, and prefetch/SIMD can regress on Apple silicon. The",
        "  table is a measurement of this build, not a universal ranking.",
        "",
    ]
    args.report.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
