#!/usr/bin/env bash
# Streaming evaluation for large real L2 depth files.
#
#   ./tools/run_real_data.sh data/btc_depth.csv BTCUSDT binance 1000 3000000
#
# Arguments:
#   input CSV, asset filter, exchange filter, sample interval (ms), max snapshots
# Environment overrides: PRICE_SCALE (default 100), QTY_SCALE (default 1e8).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <depth.csv> [asset] [exchange] [sample_ms] [max_snapshots]" >&2
    exit 2
fi

FILE="$1"
ASSET="${2:-BTCUSDT}"
EXCHANGE="${3:-binance}"
SAMPLE_MS="${4:-1000}"
MAX_SNAPSHOTS="${5:-3000000}"
PRICE_SCALE="${PRICE_SCALE:-100}"
QTY_SCALE="${QTY_SCALE:-1e8}"
RESULTS="benchmarks/results"
PLOTS="benchmarks/plots/real"

mkdir -p "$RESULTS" "$PLOTS"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

./build/hft_replay crypto-depth \
    --file "$FILE" \
    --asset "$ASSET" \
    --exchange "$EXCHANGE" \
    --price-scale "$PRICE_SCALE" \
    --qty-scale "$QTY_SCALE" \
    --metrics \
    --sample-ms "$SAMPLE_MS" \
    --max-snapshots "$MAX_SNAPSHOTS" \
    --scores "$RESULTS/real_scores.csv" \
    --regimes "$RESULTS/real_regimes.csv"

python3 benchmarks/plot_results.py \
    --scores-csv "$RESULTS/real_scores.csv" \
    --regimes-csv "$RESULTS/real_regimes.csv" \
    --pnl-summary "$RESULTS/real_pnl_summary.json" \
    --dataset "$FILE" \
    --repro-command "./tools/run_real_data.sh $FILE $ASSET $EXCHANGE $SAMPLE_MS $MAX_SNAPSHOTS" \
    --hawkes-kernel-csv "$RESULTS/real_hawkes_kernel.csv" \
    --hawkes-cross-csv "$RESULTS/real_hawkes_cross_excitation.csv" \
    --hawkes-summary-json "$RESULTS/real_hawkes_summary.json" \
    --plots-dir "$PLOTS" \
    --report "$RESULTS/REAL_DATA_REPORT.md"
