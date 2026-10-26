#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
DATA="${SOAK_DATA:-build/paper/synthetic_1800s_seed11.csv}"
OUT="benchmarks/results/soak"
ITERATIONS="${SOAK_ITERATIONS:-5}"
mkdir -p "$OUT"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target hft_feed -j \
    "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

if [[ ! -f "$DATA" ]]; then
    python3 tools/generate_synthetic_ticks.py --duration 1800 --seed 11 --output "$DATA"
fi

TIME_CMD=()
if command -v /usr/bin/time >/dev/null 2>&1; then
    if [[ "$(uname -s)" == "Darwin" ]]; then
        TIME_CMD=(/usr/bin/time -l)
    else
        TIME_CMD=(/usr/bin/time -v)
    fi
fi

{
    echo "Repeated replay test; does not establish continuous live-feed soak coverage."
    echo "soak generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "data=$DATA iterations=$ITERATIONS"
    for iteration in $(seq 1 "$ITERATIONS"); do
        echo "--- iteration $iteration ---"
        "${TIME_CMD[@]}" "$BUILD_DIR/hft_feed" \
            --replay "$DATA" --http-port 0 --replay-speed 1000 \
            2>&1 | tail -20
    done
} | tee "$OUT/SOAK_REPORT.txt"

