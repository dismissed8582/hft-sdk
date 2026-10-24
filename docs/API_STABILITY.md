# API stability

- Project version: see `include/hft/Version.hpp`.
- C++ SDK follows semantic versioning.
- Header-only `HftSdk::core` and compiled `HftSdk::sdk` share the same version.
- The Python module and wheel version match the C++ project version.
- Deprecations are documented in `CHANGELOG.md` for at least one minor release.
- ABI stability is guaranteed within a major version for the compiled SDK.
- The wire format of `hft_feed --stdin` and `/health` is versioned by the
  project version; additive fields are allowed in minor releases.

Before a release:

1. `cmake --build ... && ctest`
2. Python wheel build
3. live parser tests
4. stress/soak run
5. changelog and version bump
