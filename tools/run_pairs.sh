#!/usr/bin/env bash
# Cointegration pairs-trading backtest (paper 2, Section 4).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

rm -rf benchmarks/results/pairs benchmarks/plots/pairs
mkdir -p benchmarks/results/pairs benchmarks/plots/pairs
${PYTHON:-python3} benchmarks/pairs_trading.py \
    --output-dir benchmarks/results/pairs \
    --plots-dir benchmarks/plots/pairs \
    --report benchmarks/results/pairs/PAIRS_REPORT.md
