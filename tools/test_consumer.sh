#!/usr/bin/env bash
# Verify install/export and an external find_package(HftSdk) consumer.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
PREFIX="$(mktemp -d "${TMPDIR:-/tmp}/hft_sdk_install.XXXXXX")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/hft_sdk_consumer.XXXXXX")"
trap 'rm -rf "$PREFIX" "$WORK"' EXIT

cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "$@" >/dev/null
cmake --build "$BUILD_DIR" -j "$JOBS" >/dev/null
cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null

cp "$ROOT/examples/consumer/CMakeLists.txt" "$ROOT/examples/consumer/main.cpp" "$WORK/"
HEADER_ONLY=OFF
if test "$(sed -n 's/^HFT_BUILD_SHARED_SDK:BOOL=//p' "$BUILD_DIR/CMakeCache.txt")" != "ON"; then
    HEADER_ONLY=ON
fi
cmake -S "$WORK" -B "$WORK/build" -DCMAKE_PREFIX_PATH="$PREFIX" -DHFT_CONSUMER_HEADER_ONLY="$HEADER_ONLY" >/dev/null
cmake --build "$WORK/build" -j "$JOBS" >/dev/null

if [[ "$(uname -s)" == "Darwin" ]]; then
    DYLD_LIBRARY_PATH="$PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" "$WORK/build/consumer"
else
    LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$WORK/build/consumer"
fi

# Exercise the installed header-only target independently of the shared SDK.
cmake -S "$WORK" -B "$WORK/header-build" -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DHFT_CONSUMER_HEADER_ONLY=ON >/dev/null
cmake --build "$WORK/header-build" -j "$JOBS" >/dev/null
"$WORK/header-build/consumer"

# pkg-config must supply public header dependencies as well as linker flags.
if command -v pkg-config >/dev/null 2>&1; then
    export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # Keep the array nonempty for macOS Bash 3 under set -u.
    DEFINES=(-UHFT_CONSUMER_HEADER_ONLY)
    if [[ "$HEADER_ONLY" == "ON" ]]; then DEFINES=(-DHFT_CONSUMER_HEADER_ONLY=1); fi
    PKG_FLAGS="$(pkg-config --cflags --libs hft-sdk)"
    "${CXX:-c++}" "$WORK/main.cpp" "${DEFINES[@]}" -o "$WORK/pkg-consumer" \
        $PKG_FLAGS
    if [[ "$(uname -s)" == "Darwin" ]]; then
        DYLD_LIBRARY_PATH="$PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" "$WORK/pkg-consumer"
    else
        LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$WORK/pkg-consumer"
    fi
fi

# If this build enabled Python, an installed import failure is a test failure.
if test "$(sed -n 's/^HFT_BUILD_PYTHON:BOOL=//p' "$BUILD_DIR/CMakeCache.txt")" = "ON"; then
    SITE_PACKAGES="$(find "$PREFIX" -type d -name site-packages | head -n 1)"
    test -n "$SITE_PACKAGES"
    PYTHON_BIN="$(sed -n 's/^_Python_EXECUTABLE:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")"
    PYTHON_BIN="${PYTHON_BIN:-python3}"
    PYTHONPATH="$SITE_PACKAGES" "$PYTHON_BIN" -c "import hft_sdk; hft_sdk.OBIFilterEngine()"
    echo "installed Python import: ok ($PYTHON_BIN)"
fi

echo "find_package(HftSdk) consumer: ok"
