# API stability

- Project version: see `include/hft/Version.hpp`.
- C++ SDK follows semantic versioning.
- Header-only `HftSdk::core` and compiled `HftSdk::sdk` share the same version.
- The Python module and wheel version match the C++ project version.
- Deprecations are documented in `CHANGELOG.md` for at least one minor release.
- Compiled C++ consumers must use matching headers, architecture, compiler/
  standard-library ABI, and `HFT_CACHE_LINE` settings. Rebuild consumers when
  upgrading; cross-toolchain binary compatibility is not guaranteed.
- The engine has one processing owner. Synchronize all calls externally, including
  reads; history access may reorder internal storage, and processing/reset can
  invalidate references. `latest_snapshot()` returns a safe copy.
- Bounded snapshot storage retains the newest samples. `stats()` exposes
  processed/rejected events and historical evictions; reset clears all counters.
- Supported CI profiles cover Linux GCC/Clang and macOS Clang. Windows and the
  optional native Arrow integration require separate validation.
- The wire format of `hft_feed --stdin` and `/health` is versioned by the
  project version; additive fields are allowed in minor releases.

Before a release:

1. `cmake --build ... && ctest`
2. Python wheel build
3. live parser tests
4. stress/soak run
5. changelog and version bump


Snapshot interval event counters retain their original 32-bit public layout and
saturate at UINT32_MAX; cumulative event/trade counters in `stats()` are exact
64-bit counts. Directional rolling counts use 64 bits to avoid high-rate wrap.
