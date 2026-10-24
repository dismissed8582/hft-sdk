#!/usr/bin/env python3
"""Stress and edge-case harness for the paper-faithful and streaming paths."""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from typing import Any

try:
    import psutil
except ImportError:  # pragma: no cover
    psutil = None


@dataclass
class RunResult:
    name: str
    command: list[str]
    returncode: int
    wall_seconds: float
    peak_rss_mb: float
    timed_out: bool = False
    stdout: str = ""
    stderr: str = ""
    output_path: str | None = None
    output_bytes: int = 0
    output_rows: int = 0
    anchors: int = 0
    notes: list[str] = field(default_factory=list)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, default=pathlib.Path("build"))
    parser.add_argument("--work-dir", type=pathlib.Path, default=pathlib.Path("build/stress"))
    parser.add_argument("--out-dir", type=pathlib.Path,
                        default=pathlib.Path("benchmarks/results/stress"))
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--quick", action="store_true")
    return parser.parse_args()


def _watch_memory(process: subprocess.Popen, state: dict[str, float]) -> None:
    if psutil is None:
        return
    try:
        handle = psutil.Process(process.pid)
        while process.poll() is None:
            try:
                state["peak"] = max(state["peak"], handle.memory_info().rss)
            except psutil.Error:
                break
            time.sleep(0.02)
    except psutil.Error:
        return


def run_command(name: str, command: list[str], *, timeout: float,
                output_path: pathlib.Path | None = None) -> RunResult:
    start = time.perf_counter()
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=os.environ.copy(),
    )
    state = {"peak": 0.0}
    watcher = threading.Thread(target=_watch_memory, args=(process, state), daemon=True)
    watcher.start()
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        stdout, stderr = process.communicate()
    wall = time.perf_counter() - start
    watcher.join(timeout=1.0)
    result = RunResult(
        name=name,
        command=command,
        returncode=process.returncode,
        wall_seconds=wall,
        peak_rss_mb=state["peak"] / (1024.0 * 1024.0),
        timed_out=timed_out,
        stdout=stdout.strip(),
        stderr=stderr.strip(),
    )
    if output_path is not None:
        result.output_path = str(output_path)
        if output_path.exists():
            result.output_bytes = output_path.stat().st_size
            with output_path.open() as handle:
                lines = handle.read().splitlines()
            result.output_rows = max(0, len(lines) - 1)
            if lines:
                header = lines[0].split(",")
                if "is_anchor" in header:
                    anchor_index = header.index("is_anchor")
                    result.anchors = sum(
                        1 for line in lines[1:]
                        if len(line.split(",")) > anchor_index and
                        line.split(",")[anchor_index] == "1"
                    )
    return result


def generate(work_dir: pathlib.Path, duration: int, seed: int) -> pathlib.Path:
    output = work_dir / f"synthetic_{duration}s_seed{seed}.csv"
    if output.exists():
        return output
    subprocess.run(
        [sys.executable, "tools/generate_synthetic_ticks.py",
         "--duration", str(duration), "--seed", str(seed),
         "--output", str(output)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return output


def write_edge_cases(work_dir: pathlib.Path) -> dict[str, pathlib.Path]:
    header = ("timestamp_ns,event_type,side,order_id,price_ticks,quantity,"
              "modifications,bid_price_ticks,ask_price_ticks\n")
    paths: dict[str, pathlib.Path] = {}
    empty = work_dir / "edge_empty.csv"
    empty.write_text(header)
    paths["empty"] = empty

    single = work_dir / "edge_single.csv"
    single.write_text(header + "0,NEW,BID,1,100,1,0,100,101\n")
    paths["single_event"] = single

    duplicate = work_dir / "edge_duplicate.csv"
    duplicate.write_text(
        header +
        "0,NEW,BID,1,100,1,0,100,101\n"
        "1000,MODIFY,BID,1,100,1,0,100,101\n"
        "2000,MODIFY,BID,1,100,1,0,100,101\n"
        "3000,CANCEL,BID,1,100,1,0,100,101\n"
    )
    paths["duplicate_id"] = duplicate

    unordered = work_dir / "edge_unordered.csv"
    unordered.write_text(
        header +
        "3000,CANCEL,ASK,2,101,1,0,100,101\n"
        "0,NEW,BID,1,100,1,0,100,101\n"
        "1000,NEW,ASK,2,101,1,0,100,101\n"
        "2000,MODIFY,BID,1,100,1,0,100,101\n"
    )
    paths["out_of_order"] = unordered

    no_trades = work_dir / "edge_no_trades.csv"
    no_trades.write_text(
        header +
        "0,NEW,BID,1,100,1,0,100,101\n"
        "1000,NEW,ASK,2,101,1,0,100,101\n"
        "2000,MODIFY,BID,1,100,1,0,100,101\n"
    )
    paths["no_trades"] = no_trades

    gap = work_dir / "edge_gap.csv"
    gap.write_text(
        header +
        "0,NEW,BID,1,100,1,0,100,101\n"
        "1000,NEW,ASK,2,101,1,0,100,101\n"
        "86400000000000,NEW,ASK,3,101,1,0,100,101\n"
        "86400001000000,NEW,BID,4,100,1,0,100,101\n"
    )
    paths["timestamp_gap"] = gap

    max_id = work_dir / "edge_max_order_id.csv"
    max_id.write_text(
        header +
        "0,NEW,BID,18446744073709551615,100,1,0,100,101\n"
        "1000,NEW,ASK,1,101,1,0,100,101\n"
        "2000,CANCEL,BID,18446744073709551615,100,1,0,100,101\n"
    )
    paths["max_order_id"] = max_id
    return paths


def summarize(result: RunResult) -> dict[str, Any]:
    return asdict(result)


def write_report(results: list[RunResult], out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "generated_unix": time.time(),
        "results": [summarize(result) for result in results],
    }
    (out_dir / "stress_report.json").write_text(json.dumps(payload, indent=2) + "\n")
    with (out_dir / "stress_summary.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["name", "returncode", "wall_seconds", "peak_rss_mb",
                         "output_rows", "anchors", "timed_out", "status"])
        for result in results:
            status = "ok"
            if result.timed_out:
                status = "timeout"
            elif result.returncode != 0:
                status = "error"
            writer.writerow([
                result.name, result.returncode, f"{result.wall_seconds:.6f}",
                f"{result.peak_rss_mb:.3f}", result.output_rows, result.anchors,
                int(result.timed_out), status,
            ])
    lines = [
        "# Stress & Edge-Case Report",
        "",
        f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        "",
        "| Case | Return | Time (s) | Peak RSS (MB) | Output rows | Anchors | Status |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for result in results:
        status = "ok"
        if result.timed_out:
            status = "timeout"
        elif result.returncode != 0:
            status = "error"
        lines.append(
            f"| {result.name} | {result.returncode} | {result.wall_seconds:.3f} | "
            f"{result.peak_rss_mb:.1f} | {result.output_rows} | {result.anchors} | "
            f"{status} |"
        )
    lines += [
        "",
        "## Notes",
        "",
        "- `empty` (header-only) inputs are expected to fail with a clean error.",
        "- `single_event`, `duplicate_id`, `out_of_order`, `no_trades`,",
        "  `timestamp_gap` and `max_order_id` are expected to complete without",
        "  crashing.",
        "- The machine-readable results are in `stress_report.json`.",
        "",
    ]
    (out_dir / "STRESS_REPORT.md").write_text("\n".join(lines) + "\n")


def main() -> int:
    args = parse_args()
    build = args.build_dir.resolve()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    paper_eval = build / "hft_paper_eval"
    if not paper_eval.exists():
        raise SystemExit(f"missing {paper_eval}; build hft_paper_eval first")

    results: list[RunResult] = []
    edge = write_edge_cases(work)
    for name, path in edge.items():
        output = args.out_dir / f"{name}_ticks.csv"
        results.append(run_command(
            f"paper_eval/{name}",
            [str(paper_eval), "--ticks", str(path), "--out", str(output)],
            timeout=min(args.timeout, 120.0),
            output_path=output,
        ))

    durations = [60, 300] if args.quick else [60, 300, 900]
    for duration in durations:
        path = generate(work, duration, 11)
        output = args.out_dir / f"paper_ticks_{duration}s.csv"
        results.append(run_command(
            f"paper_eval/{duration}s/paper_filters",
            [str(paper_eval), "--ticks", str(path), "--out", str(output)],
            timeout=args.timeout,
            output_path=output,
        ))
        streaming = args.out_dir / f"streaming_ticks_{duration}s.csv"
        results.append(run_command(
            f"paper_eval/{duration}s/streaming_filters",
            [str(paper_eval), "--ticks", str(path),
             "--out", str(streaming), "--streaming"],
            timeout=args.timeout,
            output_path=streaming,
        ))

    analysis_ticks = args.out_dir / "paper_ticks_900s.csv"
    if not analysis_ticks.exists():
        analysis_ticks = args.out_dir / "paper_ticks_300s.csv"
    if analysis_ticks.exists():
        analysis_dir = args.out_dir / "analysis"
        plots_dir = args.out_dir / "analysis_plots"
        results.append(run_command(
            "paper_analysis",
            [sys.executable, "benchmarks/paper_analysis.py",
             "--ticks-csv", str(analysis_ticks),
             "--output-dir", str(analysis_dir),
             "--plots-dir", str(plots_dir),
             "--report", str(analysis_dir / "PAPER_REPORT.md"),
             "--dataset", str(analysis_ticks)],
            timeout=args.timeout,
        ))
    for edge_name in ("single_event", "no_trades"):
        ticks = args.out_dir / f"{edge_name}_ticks.csv"
        if not ticks.exists():
            continue
        results.append(run_command(
            f"paper_analysis/{edge_name}",
            [sys.executable, "benchmarks/paper_analysis.py",
             "--ticks-csv", str(ticks),
             "--output-dir", str(args.out_dir / f"analysis_{edge_name}"),
             "--plots-dir", str(args.out_dir / f"analysis_{edge_name}_plots"),
             "--report", str(args.out_dir / f"analysis_{edge_name}" / "PAPER_REPORT.md"),
             "--dataset", str(ticks)],
            timeout=min(args.timeout, 120.0),
        ))

    threaded = build / "hft_threaded_bench"
    if threaded.exists():
        threaded_out = args.out_dir / "threaded_bench.json"
        results.append(run_command(
            "threaded_bench",
            [str(threaded), "--events", "100000", "--runs", "2",
             "--out", str(threaded_out)],
            timeout=min(args.timeout, 120.0),
            output_path=threaded_out,
        ))

    write_report(results, args.out_dir)
    failures = [result for result in results
                if result.timed_out or result.returncode != 0]
    expected_edge_failures = {"paper_eval/empty"}
    unexpected = [result for result in failures
                  if result.name not in expected_edge_failures]
    print(f"stress cases: {len(results)}")
    print(f"unexpected failures: {len(unexpected)}")
    for result in unexpected:
        print(f"  {result.name}: rc={result.returncode} {result.stderr[:200]}")
    return 1 if unexpected else 0


if __name__ == "__main__":
    raise SystemExit(main())
