# Build and validation

Use CMake 3.24+, a C++20 compiler, Python 3.9+ with development headers,
and zlib development headers (optional but recommended for gzip inputs).
`xsimd`, Google Benchmark and pybind11 are fetched if not found locally.

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r benchmarks/requirements.txt
python -m pip install scikit-build-core pybind11
JOBS=2 ./tools/validate_release.sh
```

The script builds portable native executables and Python bindings, runs native
and runtime tests, SDK/adapter fixtures, accounting and research regressions,
and external installed CMake/shared/header-only/pkg-config consumers. An import
or consumer failure is a test failure. Set BUILD_DIR to a relative alternative
build directory and PYTHON to the desired interpreter when needed.

Additional release checks:

```bash
python -m pip wheel . --no-deps --wheel-dir dist
python -m venv /tmp/hft-wheel-test
/tmp/hft-wheel-test/bin/pip install dist/hft_sdk-*.whl
cd /tmp
/tmp/hft-wheel-test/bin/python -c 'import hft_sdk; print(hft_sdk.__version__); hft_sdk.OBIFilterEngine()'
```

Run wheel imports outside the source tree. The base package requires NumPy;
`[pandas]` adds DataFrame helpers, `[data]` Parquet readers, `[analysis]` research
dependencies and `[live]` websockets. The supported adapter API consumes public
Binance/Bybit data only; offline protocol fixtures do not establish a successful
live exchange session.

```bash
./tools/run_stress.sh --quick
SOAK_ITERATIONS=2 ./tools/run_soak.sh
docker build -t hft-sdk:1.0.1 .
python3 tests/smoke_container.py hft-sdk:1.0.1
```

`sanitizers.yml` runs ASan/UBSan, TSan and bounded parser fuzzing on GitHub.
`benchmark.yml` covers portable installed consumers on Linux GCC/Clang and macOS,
Python 3.9/3.12 wheels and container startup, stdin, health and shutdown.
Consult the workflow outcome for the exact release revision; workflow presence
does not mean it passed. Benchmark pipelines overwrite generated artifacts;
commit only runs with complete revision/data/hardware provenance.
