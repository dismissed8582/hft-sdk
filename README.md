# HFT LOB SDK

A C++20 low-latency limit-order-book signal engine, benchmark suite, and Python research toolkit.

The project ingests market-data events, maintains rolling order-book imbalance signals, evaluates them against forward returns, exports regime tuples for Python-side Hawkes analysis, and runs a deterministic execution-cost backtest. It is engineering and research infrastructure, not a live trading system or investment advice.

## Highlights

- Cache-line-aligned lock-free ring buffers and an SPSC queue.
- Count-based and volume-weighted order-book imbalance (`OBI`, `OBI(V)`).
- Structural filters: lifetime, modification count, and modification time.
- 1s / 10s / 30s / 50s rolling horizons.
- SIMD Welford statistics with AR(1) residualized regime correlations.
- 13x13 Hawkes kernel and 4x9 masked cross-excitation in Python.
- Deterministic PnL with configurable maker/taker fees and execution latency.
- C++ shared library with `find_package(HftSdk)` support.
- pybind11 bindings for NumPy/Pandas workflows.
- Reproducible Google Benchmark suite, dark-mode charts, and generated reports.

## Architecture

```text
market data
    |
    v
ingest adapters (Binance, Hyperliquid, crypto-lob-stream, LOBSTER, CSV/Parquet)
    |
    v
SPSC queue / ring buffer
    |
    v
order tracker -> filter masks -> rolling count & volume windows
    |
    v
signal snapshots -> metrics / regime tuples
    |
    +--> C++ PnL backtest
    |
    +--> Python Layer 3: Hawkes kernel + report
```

## Requirements

- CMake 3.24+
- C++20 compiler (Clang or GCC)
- Python 3.9+ for plotting and bindings
- `numpy`, `pandas`, `matplotlib`, `seaborn`, `scipy` (see `benchmarks/requirements.txt`)

`xsimd`, Google Benchmark, and pybind11 are fetched automatically when not installed.

## Quick Start

```bash
python3 -m pip install -r benchmarks/requirements.txt

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./tools/run_pipeline.sh
```

The pipeline builds, runs tests, generates synthetic data, evaluates signals and PnL, runs the benchmark suite, and regenerates charts and `BENCHMARK_REPORT.md`.

## Outputs

```text
BENCHMARK_REPORT.md
benchmarks/results/
  latest_bench.json          Google Benchmark JSON
  signal_scores.csv          count + volume OBI metrics
  layer3_regimes.csv         time-synchronized regime tuples
  pnl_summary.json           return, Sharpe, drawdown, fees
  pnl_trades.csv             trade-level PnL
  hawkes_kernel.csv          13x13 integrated kernel
  hawkes_cross_excitation.csv
  hawkes_summary.json
benchmarks/plots/
  queue_vs_ringbuffer.png
  latency_distribution.png
  signal_quality_heatmap.png
  hawkes_kernel.png
  hawkes_cross_excitation.png
```

## Real Data

The engine needs event-level L2 data. A Hugging Face dataset with monthly Binance depth is available:

```bash
curl -L -o data/btc_depth.parquet \
  "https://huggingface.co/datasets/Goooddy/crypto-lob-stream/resolve/main/depth/binance/BTCUSDT/2026-08.parquet"

python3 tools/parquet_to_csv.py --table depth \
  --input data/btc_depth.parquet \
  --out data/btc_depth.csv \
  --asset BTCUSDT --exchange binance \
  --limit 5000000

./tools/run_real_data.sh data/btc_depth.csv BTCUSDT binance 1000 3000000
```

Real-data charts and report:

```text
benchmarks/plots/real/
benchmarks/results/REAL_DATA_REPORT.md
```

The AHR999 dataset (`kshift/ahr999-dataset`) is a daily valuation series, not order-book data, so it is not suitable for the OBI pipeline.

## C++ SDK

Build and install:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHFT_BUILD_SHARED_SDK=ON \
  -DHFT_BUILD_PYTHON=ON
cmake --build build -j
cmake --install build --prefix /opt/hft
```

Consume from another CMake project:

```cmake
find_package(HftSdk REQUIRED)
target_link_libraries(your_target PRIVATE HftSdk::sdk)
```

Header-only usage:

```cmake
find_package(HftSdk REQUIRED)
target_link_libraries(your_target PRIVATE HftSdk::core)
```

Example:

```cpp
#include <hft/HftSdk.hpp>

hft::sdk::OBIFilterEngine engine;
engine.process(events);

auto snapshots = engine.snapshots();
auto metrics = engine.metrics();
auto regimes = engine.regimes(hft::obi::FilterScheme::Lifetime, 2, true);

hft::backtest::Config config;
config.weight = hft::backtest::SignalWeight::Volume;
config.taker_fee_bps = 2.0;
config.latency_ns = 100'000;
auto pnl = engine.backtest(config);
```

Standalone SPSC queue:

```cpp
#include <hft/SPSCQueue.hpp>

hft::SPSCQueue<hft::MarketDataMessage, 65536> queue;
hft::MarketDataMessage message{};
queue.push(message);

hft::MarketDataMessage received{};
queue.pop(received);

// Or the SDK-provided global instance:
hft::g_market_data_queue.push(message);
```

## Python Bindings

The module is built for the Python interpreter selected by CMake:

```bash
PYTHONPATH=build python3 - <<'PY'
import hft_sdk

engine = hft_sdk.OBIFilterEngine()
engine.process_arrays(order_id, timestamp_ns, price_ticks, quantity,
                      bid_price_ticks, ask_price_ticks, type_codes, side_codes)

print(engine.metrics())
print(engine.backtest(hft_sdk.BacktestConfig()))
PY
```

For pandas:

```python
import hft_sdk
import pandas as pd

frame = pd.read_csv("data/sample_ticks.csv")
engine = hft_sdk.OBIFilterEngine()
engine.process_frame(frame)
print(pd.DataFrame(engine.metrics_table()).head())
```

## Benchmark Snapshot

Latest generated run:

| Metric | Result |
| --- | --- |
| RingBuffer vs mutex queue at 1M events | ~2.8x lower per-event latency |
| Cache-warm RingBuffer p50 / p99 | ~1.3 ns / ~3.2 ns |
| SIMD Welford vs scalar | ~2x |
| Best synthetic count OBI | Pearson 0.52081, R2 0.27124 |
| Best synthetic volume OBI | Pearson 0.54889, R2 0.30128 |
| Default synthetic PnL | -4.35% after 2 bps/side |
| Real BTC L2 volume OBI | R2 ~0.0003 |

Numbers are host- and dataset-dependent. See [`BENCHMARK_REPORT.md`](BENCHMARK_REPORT.md) and [`benchmarks/results/REAL_DATA_REPORT.md`](benchmarks/results/REAL_DATA_REPORT.md).

## Repository Layout

```text
include/hft/        C++ headers: queue, filters, scoring, pipeline, SDK, backtest
src/                benchmark, evaluator, replay, SDK, SPSC explicit instantiation
tests/              unit, concurrency, volume, and backtest tests
tools/              data ingest, pipeline, real-data runner, acceptance/consumer tests
benchmarks/         plotting, generated results, charts, requirements
python/             pybind11 bindings
cmake/              package config template
.github/workflows/  CI benchmark pipeline
```

## CI

`.github/workflows/benchmark.yml` runs on pushes to `main`:

- Ubuntu runner, Clang, CMake
- Builds the engine, tests, SDK, and Python module
- Runs the benchmark/reporting pipeline
- Verifies `find_package(HftSdk)` from an external consumer
- Uploads benchmark artifacts

## Limitations

- The current OBI implementation summarizes event flow; it is not a full resting-depth reconstruction.
- Real-data alpha is not demonstrated yet. The crypto depth slice produces near-zero R2.
- The mutex-queue benchmark is a single-thread hand-off comparison, not a contention benchmark.
- The PnL simulator uses mid-price fills and parameterized fees; it is not a production execution model.
- Hawkes estimation is a discrete-time least-squares fit, not continuous-time maximum likelihood.

## License

Proprietary. All rights reserved. Contact the repository owner for commercial licensing terms.
