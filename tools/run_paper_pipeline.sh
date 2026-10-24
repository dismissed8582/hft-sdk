#!/usr/bin/env bash
# Paper-faithful diagnostics: ex-post filters, realized trade returns,
# 9x3 regime scoring, and a sum-of-exponentials Hawkes fit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
DURATION="${PAPER_DURATION:-900}"
SEED="${PAPER_SEED:-11}"
RESULTS="benchmarks/results/paper"
PLOTS="benchmarks/plots/paper"
WORK="build/paper"
mkdir -p "$RESULTS" "$PLOTS" "$WORK"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target hft_paper_eval -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

DATA="${PAPER_INPUT:-}"
if [[ -z "$DATA" ]]; then
    DATA="$WORK/synthetic_${DURATION}s_seed${SEED}.csv"
    if [[ ! -f "$DATA" ]]; then
        ${PYTHON:-python3} tools/generate_synthetic_ticks.py \
            --duration "$DURATION" --seed "$SEED" --output "$DATA"
    fi
fi

"$BUILD_DIR/hft_paper_eval" \
    --ticks "$DATA" \
    --out "$RESULTS/paper_ticks.csv"

${PYTHON:-python3} benchmarks/paper_analysis.py \
    --ticks-csv "$RESULTS/paper_ticks.csv" \
    --output-dir "$RESULTS" \
    --plots-dir "$PLOTS" \
    --report "$RESULTS/PAPER_REPORT.md" \
    --dataset "$DATA"

echo "paper report: $RESULTS/PAPER_REPORT.md"
