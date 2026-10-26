#!/usr/bin/env bash
# Customer-facing acceptance test. No prior knowledge of the internals required.
#
#   ./tools/customer_acceptance.sh
#   ./tools/customer_acceptance.sh --online
#
# --online additionally downloads one public Binance day and one public
# Hugging Face Parquet shard to prove the real-data path end-to-end.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-$ROOT/build-acceptance}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
ONLINE=0
for argument in "$@"; do
    case "$argument" in
        --online) ONLINE=1 ;;
        --help|-h)
            echo "usage: $0 [--online]"
            exit 0
            ;;
        *)
            echo "unknown option: $argument" >&2
            exit 2
            ;;
    esac
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/hft_accept.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

pass() { printf '[PASS] %s\n' "$1"; }
fail() { printf '[FAIL] %s\n' "$1" >&2; exit 1; }

echo "== 1. Release build =="
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j "$JOBS" >/dev/null
pass "release build"

echo "== 2. Unit and concurrency tests =="
ctest --test-dir "$BUILD_DIR" --output-on-failure
pass "ctest suite"

echo "== 3. Deterministic signal evaluation =="
python3 tools/generate_synthetic_ticks.py --duration 120 --seed 11 --output "$WORK/ticks.csv" >/dev/null
"$BUILD_DIR/hft_eval" \
    --ticks "$WORK/ticks.csv" \
    --scores "$WORK/scores.csv" \
    --regimes "$WORK/regimes.csv" \
    --pnl-summary "$WORK/pnl.json" \
    --pnl-trades "$WORK/pnl_trades.csv" >"$WORK/eval.txt"
python3 - "$WORK/scores.csv" "$WORK/regimes.csv" "$WORK/pnl.json" <<'PY'
import csv
import json
import sys

scores, regimes, pnl_path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(scores, newline="") as handle:
    rows = list(csv.DictReader(handle))
schemes = {row["scheme"] for row in rows}
horizons = {int(row["horizon_s"]) for row in rows}
assert schemes == {"UF", "LF", "MF", "MTF"}, schemes
assert horizons == {1, 10, 30, 50}, horizons
assert "volume_pearson" in rows[0] and "volume_r2" in rows[0]
with open(regimes, newline="") as handle:
    regime_rows = list(csv.DictReader(handle))
assert regime_rows, "no Layer-3 regime tuples"
assert set(regime_rows[0]) == {"timestamp_ns", "obi_regime", "return_regime"}
assert all(0 <= int(row["obi_regime"]) < 9 for row in regime_rows)
assert all(0 <= int(row["return_regime"]) < 4 for row in regime_rows)
with open(pnl_path, encoding="utf-8") as handle:
    pnl = json.load(handle)
for key in ("trade_count", "total_return", "sharpe_daily_annualized", "max_drawdown"):
    assert key in pnl, key
PY
cat "$WORK/eval.txt"
pass "16 signal regimes, volume metrics, Layer-3 tuples, and PnL produced"

echo "== 4. Disruptor throughput smoke test =="
HFT_BENCHMARK_OUT="$WORK/latest_bench.json" \
HFT_LATENCY_CSV="$WORK/latency_samples.csv" \
"$BUILD_DIR/hft_bench" \
    --benchmark_filter='Disruptor/Throughput/CacheCold' \
    --benchmark_min_time=0.1s >/dev/null 2>"$WORK/bench.err"
python3 - "$WORK/latest_bench.json" <<'PY'
import json
import math
import os
import sys

with open(sys.argv[1]) as handle:
    payload = json.load(handle)
benchmarks = payload["benchmarks"]
assert benchmarks, "no benchmark records"
throughput = benchmarks[0]["items_per_second"]
minimum = float(os.environ.get("HFT_MIN_THROUGHPUT", "0"))
assert math.isfinite(throughput) and throughput > minimum, throughput
print(f"ring throughput = {throughput / 1e6:.1f}M events/s")
PY
pass "ring benchmark"

echo "== 5. Binance trade replay =="
cat >"$WORK/trades.csv" <<'EOF'
trade_id,price,qty,quoteQty,time,isBuyerMaker,isBestMatch
1,100.50,2.0,201.0,1700000000000,false,true
2,100.75,1.5,151.1,1700000001000,true,true
EOF
"$BUILD_DIR/hft_replay" binance-trades --file "$WORK/trades.csv" --format spot \
    >"$WORK/binance.txt"
grep -q 'events=2' "$WORK/binance.txt" || fail "Binance replay count"
cat "$WORK/binance.txt"
pass "Binance trade parser"

echo "== 6. Hyperliquid book-diff replay =="
cat >"$WORK/book.jsonl" <<'EOF'
{"user":"0x1","oid":273024455731,"coin":"ETH","side":"A","px":"2964.0","raw_book_diff":{"new":{"sz":"0.2274"}}}
{"user":"0x2","oid":273024450830,"coin":"ETH","side":"B","px":"2962.3","raw_book_diff":"remove"}
{"user":"0x3","oid":273024455174,"coin":"ETH","side":"B","px":"2962.5","raw_book_diff":{"update":{"origSz":"5.0632","newSz":"4.7287"}}}
EOF
"$BUILD_DIR/hft_replay" hl-book --file "$WORK/book.jsonl" --coin ETH \
    --step-ns 1000000 >"$WORK/hl_book.txt"
grep -q 'events=3' "$WORK/hl_book.txt" || fail "Hyperliquid book replay count"
cat "$WORK/hl_book.txt"
pass "Hyperliquid book-diff parser"

echo "== 7. Hyperliquid binary order-status replay =="
python3 - "$WORK/status.data" <<'PY'
import struct
import sys

def encode(value, decimals):
    return (decimals << 29) | int(round(value * (10 ** decimals)))

def record(ts, status, is_ask, price, size, oid, age_ms):
    data = bytearray(54)
    struct.pack_into("<Q", data, 0, ts)
    struct.pack_into("<I", data, 8, 7)
    data[13] = status
    data[14] = is_ask
    struct.pack_into("<I", data, 15, encode(price, 2))
    struct.pack_into("<I", data, 19, encode(size, 4))
    struct.pack_into("<Q", data, 23, oid)
    struct.pack_into("<I", data, 31, age_ms)
    struct.pack_into("<I", data, 50, encode(size, 4))
    return bytes(data)

with open(sys.argv[1], "wb") as output:
    output.write(
        record(1_000_000_000, 1, 0, 100.0, 1.5, 1, 0)
        + record(2_000_000_000, 1, 1, 101.0, 2.0, 2, 0)
        + record(3_000_000_000, 5, 0, 100.0, 1.5, 1, 2000)
        + record(4_000_000_000, 2, 1, 101.0, 2.0, 2, 4000)
    )
PY
"$BUILD_DIR/hft_replay" hl-orders --file "$WORK/status.data" \
    >"$WORK/hl_orders.txt"
grep -q 'events=4' "$WORK/hl_orders.txt" || fail "Hyperliquid order replay count"
cat "$WORK/hl_orders.txt"
pass "Hyperliquid order-status parser"

if [[ "$ONLINE" == "1" ]]; then
    echo "== 8. Public Binance bookDepth download =="
    python3 tools/download_binance.py \
        --market futures --kind bookDepth --symbol BTCUSDT \
        --start 2023-01-01 --end 2023-01-01 --out "$WORK/binance" \
        >/dev/null
    "$BUILD_DIR/hft_replay" binance-depth \
        --file "$WORK/binance/BTCUSDT-bookDepth-2023-01-01.csv" \
        --price-scale 100 --limit 10000 >"$WORK/binance_depth.txt"
    cat "$WORK/binance_depth.txt"
    pass "public Binance bookDepth download and replay"

    echo "== 9. Public Hugging Face Parquet download =="
    mkdir -p "$WORK/cryptolob"
    curl -fsSL \
        "https://huggingface.co/datasets/Goooddy/crypto-lob-stream/resolve/main/snapshots/binance/BTCUSDT/2026-06.parquet" \
        -o "$WORK/cryptolob/snapshots.parquet"
    python3 tools/parquet_to_csv.py \
        --table snapshots \
        --input "$WORK/cryptolob/snapshots.parquet" \
        --asset BTCUSDT --exchange binance \
        --out "$WORK/cryptolob/snapshots.csv" --limit 5000 \
        >/dev/null
    "$BUILD_DIR/hft_replay" crypto-depth \
        --file "$WORK/cryptolob/snapshots.csv" \
        --asset BTCUSDT --exchange binance --price-scale 100 \
        --limit 5000 >"$WORK/cryptolob.txt"
    grep -q 'depth_rows=5000' "$WORK/cryptolob.txt" || fail "Parquet conversion count"
    cat "$WORK/cryptolob.txt"
    pass "public Parquet download, conversion, and replay"
fi

echo
echo "ACCEPTANCE TEST PASSED"
echo "build: $BUILD_DIR"

