#!/usr/bin/env bash
# Build the paper evaluator and run the stress/edge-case harness.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target hft_paper_eval hft_tests hft_eval hft_feed -j \
    "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

${PYTHON:-python3} tools/run_stress.py --build-dir "$BUILD_DIR" "$@"

