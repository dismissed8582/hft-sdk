#!/usr/bin/env python3
"""Compare a baseline and candidate HFT SDK build on one host.

The report deliberately separates hot-path microbenchmarks from the offline
paper evaluator. HFT-style reporting uses throughput, ns/event and latency
percentiles (p50/p95/p99/p99.9), plus a p99/p50 tail-amplification ratio.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--before-bench", type=Path, required=True)
    parser.add_argument("--after-bench", type=Path, required=True)
    parser.add_argument("--before-latency", type=Path, required=True)
    parser.add_argument("--after-latency", type=Path, required=True)
    parser.add_argument("--before-binary", type=Path, required=True)
    parser.add_argument("--after-binary", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--before-paper", type=Path, required=True)
    parser.add_argument("--after-paper", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=9)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def pct_delta(before: float, after: float) -> float:
    if not math.isfinite(before) or before == 0.0:
        return float("nan")
    return 100.0 * (after - before) / before


def fmt_delta(value: float) -> str:
    return "n/a" if not math.isfinite(value) else f"{value:+.2f}%"


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    if len(ordered) == 1:
        return ordered[0]
    position = q * (len(ordered) - 1)
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def load_latency(path: Path) -> dict[str, list[float]]:
    groups: dict[str, list[float]] = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            groups.setdefault(row["pipeline"], []).append(float(row["latency_ns"]))
    return groups


def latency_stats(values: list[float]) -> dict[str, float]:
    p50 = percentile(values, 0.50)
    p95 = percentile(values, 0.95)
    p99 = percentile(values, 0.99)
    p999 = percentile(values, 0.999)
    return {
        "p50": p50,
        "p95": p95,
        "p99": p99,
        "p999": p999,
        "max": max(values),
        "mean": statistics.fmean(values),
        "stdev": statistics.pstdev(values),
        "tail_ratio": p99 / p50 if p50 > 0.0 else float("nan"),
    }


def load_benchmark(path: Path) -> tuple[dict, list[dict]]:
    payload = json.loads(path.read_text())
    return payload.get("context", {}), payload.get("benchmarks", [])


def benchmark_record(records: list[dict], prefix: str) -> dict | None:
    matches = [row for row in records if row.get("name", "").startswith(prefix)]
    if not matches:
        return None
    medians = [row for row in matches if row.get("aggregate_name") == "median"]
    if medians:
        return medians[0]
    iterations = [row for row in matches if row.get("run_type") == "iteration"]
    return iterations[0] if iterations else matches[0]


def event_count(path: Path) -> int:
    count = 0
    with path.open() as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if count == 0 and not line.split(",", 1)[0].lstrip("-").isdigit():
                count -= 1  # compensate for the header below
            count += 1
    return max(0, count)


def run_evaluator(binary: Path, dataset: Path) -> float:
    command = [
        str(binary),
        "--ticks", str(dataset),
        "--out", "/dev/null",
        "--sample-ms", "250",
        "--anchor-ms", "15000",
        "--obi-window-ms", "10000",
        "--lifetime-ms", "100",
        "--max-mods", "3",
        "--mod-time-ms", "50",
    ]
    start = time.perf_counter_ns()
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return float(time.perf_counter_ns() - start)


def evaluator_timings(before: Path, after: Path, dataset: Path,
                      repetitions: int) -> tuple[list[float], list[float]]:
    run_evaluator(before, dataset)
    run_evaluator(after, dataset)
    samples = {"before": [], "after": []}
    for index in range(repetitions):
        order = (("before", before), ("after", after)) if index % 2 == 0 else (
            ("after", after), ("before", before)
        )
        for label, binary in order:
            samples[label].append(run_evaluator(binary, dataset))
    return samples["before"], samples["after"]


def csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def paper_tick_diff(before_dir: Path, after_dir: Path) -> dict[str, float]:
    before_path = before_dir / "paper_ticks.csv"
    after_path = after_dir / "paper_ticks.csv"
    before = csv_rows(before_path)
    after = csv_rows(after_path)
    before_map = {(row["timestamp_ns"], row["scheme"]): row for row in before}
    after_map = {(row["timestamp_ns"], row["scheme"]): row for row in after}
    keys = sorted(before_map.keys() & after_map.keys())
    changed = 0
    max_abs = 0.0
    total_abs = 0.0
    compared = 0
    for key in keys:
        left = before_map[key]
        right = after_map[key]
        if left["lob_valid"] != "1" or right["lob_valid"] != "1":
            continue
        delta = abs(float(right["lob_obi"]) - float(left["lob_obi"]))
        compared += 1
        total_abs += delta
        max_abs = max(max_abs, delta)
        if delta > 1e-15:
            changed += 1
    return {
        "rows": float(compared),
        "changed": float(changed),
        "changed_pct": 100.0 * changed / compared if compared else 0.0,
        "mean_abs_delta": total_abs / compared if compared else 0.0,
        "max_abs_delta": max_abs,
    }


def keyed_csv(path: Path, keys: tuple[str, ...]) -> dict[tuple[str, ...], dict[str, str]]:
    rows = csv_rows(path)
    return {tuple(row[key] for key in keys): row for row in rows}


def safe_float(row: dict[str, str] | None, field: str) -> float:
    if not row:
        return float("nan")
    try:
        return float(row[field])
    except (KeyError, TypeError, ValueError):
        return float("nan")


def main() -> int:
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    before_context, before_bench = load_benchmark(args.before_bench)
    after_context, after_bench = load_benchmark(args.after_bench)
    before_latency = load_latency(args.before_latency)
    after_latency = load_latency(args.after_latency)

    before_times, after_times = evaluator_timings(
        args.before_binary, args.after_binary, args.dataset, args.repetitions
    )
    events = event_count(args.dataset)

    tick_diff = paper_tick_diff(args.before_paper, args.after_paper)

    before_pearson = keyed_csv(
        args.before_paper / "pearson_scores.csv", ("scheme", "horizon_s")
    )
    after_pearson = keyed_csv(
        args.after_paper / "pearson_scores.csv", ("scheme", "horizon_s")
    )
    before_regime = keyed_csv(
        args.before_paper / "regime_scores.csv",
        ("scheme", "horizon_s", "variant", "sample", "lag"),
    )
    after_regime = keyed_csv(
        args.after_paper / "regime_scores.csv",
        ("scheme", "horizon_s", "variant", "sample", "lag"),
    )
    before_hawkes = json.loads((args.before_paper / "hawkes_summary.json").read_text())
    after_hawkes = json.loads((args.after_paper / "hawkes_summary.json").read_text())

    lines = [
        "# Before/After: temporal-alignment fix",
        "",
        "Both revisions were built and measured sequentially on the same GitHub-hosted runner,",
        "using the same compiler, deterministic synthetic tick stream, thresholds and sampling grid.",
        "Hot-path figures are reported separately from the offline paper evaluator.",
        "",
        "## Environment",
        "",
        f"- Host (before): `{before_context.get('host_name', 'unknown')}`",
        f"- Host (after): `{after_context.get('host_name', 'unknown')}`",
        f"- Logical CPUs: `{after_context.get('num_cpus', 'unknown')}`",
        f"- Google Benchmark: `{after_context.get('library_version', 'unknown')}`",
        f"- Deterministic input events: **{events:,}**",
        "",
        "## HFT hot-path microbenchmarks",
        "",
        "These are the industry-relevant fast-path measures: steady-state throughput and amortized",
        "nanoseconds per event. The paper-evaluator fix is not on these execution paths, so material",
        "movement here would indicate an unintended performance regression.",
        "",
        "| Path | Before ns/event | After ns/event | Delta | Before Mmsg/s | After Mmsg/s | Delta |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    bench_specs = [
        ("Disruptor warm hand-off", "Disruptor/Throughput/CacheWarmed"),
        ("Order tracker/filter", "Filter/OrderTracker"),
        ("SIMD OBI/Welford", "OBI/SIMD/Welford"),
        ("Regime discretization", "Scoring/Regime/LUT"),
    ]
    for label, prefix in bench_specs:
        before = benchmark_record(before_bench, prefix)
        after = benchmark_record(after_bench, prefix)
        if not before or not after:
            continue
        b_rate = float(before.get("items_per_second", float("nan")))
        a_rate = float(after.get("items_per_second", float("nan")))
        b_ns = 1e9 / b_rate if b_rate > 0.0 else float("nan")
        a_ns = 1e9 / a_rate if a_rate > 0.0 else float("nan")
        lines.append(
            f"| {label} | {b_ns:.3f} | {a_ns:.3f} | {fmt_delta(pct_delta(b_ns, a_ns))} | "
            f"{b_rate / 1e6:.3f} | {a_rate / 1e6:.3f} | {fmt_delta(pct_delta(b_rate, a_rate))} |"
        )

    lines += [
        "",
        "## Tail latency",
        "",
        "Percentiles are computed from the raw latency-sample CSV emitted by the benchmark harness.",
        "`p99/p50` is a compact tail-amplification/jitter indicator; closer to 1 is tighter.",
        "",
        "| Pipeline | Rev | p50 ns | p95 ns | p99 ns | p99.9 ns | max ns | p99/p50 |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for pipeline in ("ring_buffer_warmed", "ring_buffer_raw", "obi_simd"):
        if pipeline not in before_latency or pipeline not in after_latency:
            continue
        for rev, data in (("before", before_latency[pipeline]), ("after", after_latency[pipeline])):
            stat = latency_stats(data)
            lines.append(
                f"| {pipeline} | {rev} | {stat['p50']:.3f} | {stat['p95']:.3f} | "
                f"{stat['p99']:.3f} | {stat['p999']:.3f} | {stat['max']:.3f} | "
                f"{stat['tail_ratio']:.2f}x |"
            )

    before_med = percentile(before_times, 0.50)
    after_med = percentile(after_times, 0.50)
    before_p99 = percentile(before_times, 0.99)
    after_p99 = percentile(after_times, 0.99)
    before_rate = events / (before_med / 1e9) if before_med > 0.0 else float("nan")
    after_rate = events / (after_med / 1e9) if after_med > 0.0 else float("nan")

    lines += [
        "",
        "## Paper evaluator throughput",
        "",
        "This is an offline batch evaluator, not a trading hot path. Each timing parses the same tick CSV,",
        "computes filtered rolling OBI and forward-return observations, formats the output, and writes to",
        "`/dev/null`. Runs are alternated before/after to reduce order/thermal bias.",
        "",
        "| Metric | Before | After | Delta |",
        "| --- | ---: | ---: | ---: |",
        f"| Median wall time | {before_med / 1e6:.3f} ms | {after_med / 1e6:.3f} ms | {fmt_delta(pct_delta(before_med, after_med))} |",
        f"| p99 wall time | {before_p99 / 1e6:.3f} ms | {after_p99 / 1e6:.3f} ms | {fmt_delta(pct_delta(before_p99, after_p99))} |",
        f"| Median throughput | {before_rate / 1e6:.3f} Mevents/s | {after_rate / 1e6:.3f} Mevents/s | {fmt_delta(pct_delta(before_rate, after_rate))} |",
        f"| Median amortized cost | {before_med / max(events, 1):.3f} ns/event | {after_med / max(events, 1):.3f} ns/event | {fmt_delta(pct_delta(before_med, after_med))} |",
        "",
        "## Correctness impact on paper observations",
        "",
        f"- Comparable valid LOB-OBI rows: **{int(tick_diff['rows']):,}**",
        f"- Rows changed by removing look-ahead: **{int(tick_diff['changed']):,} ({tick_diff['changed_pct']:.3f}%)**",
        f"- Mean absolute OBI delta: **{tick_diff['mean_abs_delta']:.8f}**",
        f"- Maximum absolute OBI delta: **{tick_diff['max_abs_delta']:.8f}**",
        "",
        "### 1-second paper diagnostics",
        "",
        "| Scheme | Metric | Before | After | Delta |",
        "| --- | --- | ---: | ---: | ---: |",
    ]

    for scheme in ("UF", "LF", "MF", "MTF"):
        p_key = (scheme, "1")
        b_p = safe_float(before_pearson.get(p_key), "pearson_residual")
        a_p = safe_float(after_pearson.get(p_key), "pearson_residual")
        lines.append(
            f"| {scheme} | residual Pearson | {b_p:.6f} | {a_p:.6f} | {a_p - b_p:+.6f} |"
        )
        r_key = (scheme, "1", "residual", "full", "0")
        b_rho = safe_float(before_regime.get(r_key), "S_rho")
        a_rho = safe_float(after_regime.get(r_key), "S_rho")
        b_r2 = safe_float(before_regime.get(r_key), "S_R")
        a_r2 = safe_float(after_regime.get(r_key), "S_R")
        lines.append(
            f"| {scheme} | regime S_rho | {b_rho:.6f} | {a_rho:.6f} | {a_rho - b_rho:+.6f} |"
        )
        lines.append(
            f"| {scheme} | regime S_R | {b_r2:.6f} | {a_r2:.6f} | {a_r2 - b_r2:+.6f} |"
        )

    lines += [
        "",
        "### Hawkes aligned excitation norm",
        "",
        "| Scheme/signal | Before | After | Delta |",
        "| --- | ---: | ---: | ---: |",
    ]
    for key in sorted(before_hawkes.keys() & after_hawkes.keys()):
        b = float(before_hawkes[key]["aligned_norm"])
        a = float(after_hawkes[key]["aligned_norm"])
        lines.append(f"| {key} | {b:.6f} | {a:.6f} | {a - b:+.6f} |")

    lines += [
        "",
        "## Interpretation",
        "",
        "- **Correctness:** the candidate never inserts an event with timestamp `t` into samples whose",
        "  timestamp is `< t`; events exactly on the sampling endpoint remain included.",
        "- **Performance:** use the hot-path tables to judge production latency impact. The paper evaluator",
        "  timing is an offline research-pipeline measure and should not be presented as order-to-wire latency.",
        "- **Sign convention:** this change does not alter the paper's formal `(sell/bid-side labeling)`,",
        "  OBI sign convention. That ambiguity is deliberately kept separate from the temporal-leak fix.",
        "",
    ]

    args.output.write_text("\n".join(lines))
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
