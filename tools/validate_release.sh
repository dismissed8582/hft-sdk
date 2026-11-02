#!/usr/bin/env bash
# Build, exercise Python and native consumers, and check research/accounting.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${BUILD_DIR:-build}"
PYTHON_BIN="${PYTHON:-python3}"
PYTHON_BIN="$(command -v "$PYTHON_BIN")"
JOBS="${JOBS:-2}"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DHFT_ARCH_NATIVE=OFF -DHFT_BUILD_TESTS=ON -DHFT_BUILD_PYTHON=ON \
    -DHFT_BUILD_FEED=ON -DHFT_BUILD_EVALUATOR=ON -DHFT_BUILD_BENCHMARKS=ON \
    -DPython_EXECUTABLE="$PYTHON_BIN" "$@"
cmake --build "$BUILD_DIR" -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure
export PYTHONPATH="$ROOT/$BUILD_DIR${PYTHONPATH:+:$PYTHONPATH}"
export HFT_TEST_BUILD_DIR="$BUILD_DIR"
export HFT_FEED_EXECUTABLE="$BUILD_DIR/hft_feed"
export OPENBLAS_NUM_THREADS=1
"$PYTHON_BIN" -m unittest discover -s python/tests -v
for pattern in test_pairs.py test_metadata.py test_paper_analysis.py; do
    "$PYTHON_BIN" -m unittest discover -s tests -p "$pattern" -v
done
BUILD_DIR="$BUILD_DIR" JOBS="$JOBS" ./tools/test_consumer.sh
git diff --check
