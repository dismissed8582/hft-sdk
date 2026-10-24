# Contributing

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Python

```bash
python3 -m pip install -r benchmarks/requirements.txt
python3 -m unittest discover -s python/tests -v
```

## Style

- C++20, no dynamic allocation on hot paths.
- Run `git diff --check`.
- Add a test for every parser, filter, or metric change.
- Keep benchmark artifacts reproducible and labeled by hardware.

## Release

1. Bump `project(VERSION ...)` and `include/hft/Version.hpp`.
2. Update `CHANGELOG.md`.
3. Run unit, stress, soak, Python, and live-parser tests.
4. Build the wheel and Docker image.
5. Tag the release.
