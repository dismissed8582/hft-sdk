#!/usr/bin/env bash
# Cross-thread Disruptor/queue benchmark and the paper-2 microbenchmark suite.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
RESULTS="benchmarks/results/lowlatency"
PLOTS="benchmarks/plots/lowlatency"
mkdir -p "$RESULTS" "$PLOTS"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DHFT_BUILD_BENCHMARKS=ON
cmake --build "$BUILD_DIR" --target hft_threaded_bench hft_microbench -j \
    "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

"$BUILD_DIR/hft_threaded_bench" --events 1000000 --runs 15 --warmup 1 \
    --out "$RESULTS/threaded_bench.json"

"$BUILD_DIR/hft_microbench" \
    --benchmark_out="$RESULTS/microbench.json" \
    --benchmark_out_format=json

BUILD_DIR="$BUILD_DIR" "$ROOT/tools/run_cache_counters.sh" || true

${PYTHON:-python3} benchmarks/lowlatency_report.py \
    --threaded-json "$RESULTS/threaded_bench.json" \
    --microbench-json "$RESULTS/microbench.json" \
    --report "$RESULTS/LOW_LATENCY_REPORT.md" \
    --plots-dir "$PLOTS"

echo "low-latency report: $RESULTS/LOW_LATENCY_REPORT.md"
