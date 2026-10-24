#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
RESULTS="benchmarks/results"
PLOTS="benchmarks/plots"

mkdir -p "$RESULTS" "$PLOTS"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure

python3 tools/generate_synthetic_ticks.py

"$BUILD_DIR/hft_eval" \
    --ticks data/sample_ticks.csv \
    --scores "$RESULTS/signal_scores.csv" \
    --regimes "$RESULTS/layer3_regimes.csv" \
    --pnl-summary "$RESULTS/pnl_summary.json" \
    --pnl-trades "$RESULTS/pnl_trades.csv"

HFT_LATENCY_CSV="$RESULTS/latency_samples.csv" "$BUILD_DIR/hft_bench"

python3 benchmarks/plot_results.py

if [[ "${RUN_PAPER:-1}" == "1" ]]; then
    ./tools/run_paper_pipeline.sh
fi
if [[ "${RUN_PAIRS:-1}" == "1" ]]; then
    ./tools/run_pairs.sh
fi
if [[ "${RUN_STRESS:-0}" == "1" ]]; then
    ./tools/run_stress.sh
fi
if [[ "${RUN_LOWLATENCY:-0}" == "1" ]]; then
    ./tools/run_lowlatency_bench.sh
fi
