#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "libFuzzer target is Linux/Clang only; skipping on $(uname -s)"
    exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build-fuzz}"
SECONDS_TO_RUN="${FUZZ_SECONDS:-60}"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DHFT_BUILD_FUZZERS=ON \
    -DHFT_BUILD_TESTS=OFF \
    -DHFT_BUILD_BENCHMARKS=OFF \
    -DHFT_BUILD_PYTHON=OFF \
    -DHFT_BUILD_REPLAY=OFF \
    -DHFT_BUILD_FEED=OFF
cmake --build "$BUILD_DIR" --target hft_fuzz_csv -j \
    "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
"$BUILD_DIR/hft_fuzz_csv" -max_total_time="$SECONDS_TO_RUN" -print_final_stats=1
