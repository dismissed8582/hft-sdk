#!/usr/bin/env bash
# Capture hardware cache counters when Linux perf is available. On macOS the
# command is skipped and a note is written for the report.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
OUT="${CACHE_COUNTERS_OUT:-benchmarks/results/cache_counters.txt}"
mkdir -p "$(dirname "$OUT")"

if ! command -v perf >/dev/null 2>&1; then
    echo "perf not available; skipping hardware cache counters" | tee "$OUT"
    exit 0
fi

BENCH="$BUILD_DIR/hft_microbench"
if [[ ! -x "$BENCH" ]]; then
    echo "missing $BENCH; build hft_microbench first" | tee "$OUT"
    exit 0
fi

perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses \
    "$BENCH" --benchmark_filter='Micro/ArrayScalar|Micro/PairsRollingScalar' \
    --benchmark_min_time=0.05s >/dev/null 2>"$OUT" || {
        echo "perf stat failed; see $OUT" | tee -a "$OUT"
    }
cat "$OUT"
