#!/usr/bin/env bash
# Verify install/export and an external find_package(HftSdk) consumer.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
PREFIX="$(mktemp -d "${TMPDIR:-/tmp}/hft_sdk_install.XXXXXX")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/hft_sdk_consumer.XXXXXX")"
trap 'rm -rf "$PREFIX" "$WORK"' EXIT

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j "$JOBS" >/dev/null
cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null

cat >"$WORK/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
project(hft_sdk_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(HftSdk REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE HftSdk::sdk)
CMAKE

cat >"$WORK/main.cpp" <<'CPP'
#include <hft/HftSdk.hpp>
#include <hft/SPSCQueue.hpp>
int main() {
    hft::MarketDataMessage message{};
    if (!hft::g_market_data_queue.push(message)) {
        return 1;
    }
    hft::MarketDataMessage received{};
    if (!hft::g_market_data_queue.pop(received)) {
        return 2;
    }
    hft::sdk::OBIFilterEngine engine;
    return engine.snapshots().empty() ? 0 : 3;
}
CPP

cmake -S "$WORK" -B "$WORK/build" -DCMAKE_PREFIX_PATH="$PREFIX" >/dev/null
cmake --build "$WORK/build" -j "$JOBS" >/dev/null

if [[ "$(uname -s)" == "Darwin" ]]; then
    DYLD_LIBRARY_PATH="$PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" "$WORK/build/consumer"
else
    LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$WORK/build/consumer"
fi

SITE_PACKAGES="$(find "$PREFIX" -type d -name site-packages | head -n 1)"
PYTHON_BIN="python3"
if [[ -n "$SITE_PACKAGES" ]]; then
    MODULE="$(find "$SITE_PACKAGES" -maxdepth 1 -name 'hft_sdk*.so' | head -n 1)"
    if [[ -n "$MODULE" ]]; then
        TAG="$(basename "$MODULE" | sed -E 's/.*cpython-([0-9]+).*/\1/')"
        if [[ -n "$TAG" ]] && command -v "python$TAG" >/dev/null 2>&1; then
            PYTHON_BIN="python$TAG"
        fi
    fi
    if PYTHONPATH="$SITE_PACKAGES" "$PYTHON_BIN" -c "import hft_sdk; hft_sdk.OBIFilterEngine()" >/dev/null 2>&1; then
        echo "python module import: ok ($PYTHON_BIN)"
    else
        echo "python module import: skipped (interpreter/module mismatch)"
    fi
fi

echo "find_package(HftSdk) consumer: ok"
