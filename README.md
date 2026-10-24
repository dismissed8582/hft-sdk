# HFT LOB SDK

> [!WARNING]
> **Prototype status:** This repository is a research prototype. It is not production-ready, not a live trading system, and not investment advice. APIs, data adapters, benchmark results, and signal definitions may change. Do not deploy it with real capital without independent validation, risk controls, compliance review, and a production execution stack.

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
- `pip install .` plus `hft-sdk` CLI, with public-data download and caching.
- Reproducible Google Benchmark suite, dark-mode charts, and generated reports (see [Benchmarks](#benchmarks)).

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
- `pyarrow` for Parquet data (installed automatically by `pip install .`)

`xsimd`, Google Benchmark, and pybind11 are fetched automatically when not installed.

## Install as a Python package

The fastest path if you only want to use the engine:

```bash
python3 -m pip install .
hft-sdk data/sample_ticks.csv --scheme MTF --horizon 10 --signal volume
```

Or from Python:

```python
import hft_sdk

result = hft_sdk.run_csv("data/sample_ticks.csv", scheme="MTF", horizon=10)
print(result.metrics_dataframe())      # requires pandas
print(result.backtest().total_return)

# Explicit engine control:
engine = hft_sdk.OBIFilterEngine()
engine.process_arrays(**hft_sdk.load_csv("data/sample_ticks.csv"))
```

`pip install ".[pandas]"` adds pandas; pyarrow is included by default for
Parquet data. Wheels are built portable by default; pass
`--config-settings=cmake.define.HFT_ARCH_NATIVE=ON` to target the local CPU.

## Fetch public data

The package downloads public datasets directly, so you do not need to manage
CSV or Parquet files by hand. Files are cached under `~/.cache/hft-sdk`
(override with `HFT_SDK_DATA_DIR`).

```python
import hft_sdk

# Download one crypto-lob-stream shard and run it.
path = hft_sdk.fetch_crypto_lob("BTCUSDT", "2026-08", table="depth")
result = hft_sdk.run_source(path, scheme="MTF", horizon=10)

# Or fetch + run in one call.
result = hft_sdk.run_crypto_lob("BTCUSDT", "2026-08", table="depth")

# Binance public archives, checksum-verified.
paths = hft_sdk.fetch_binance("BTCUSDT", "2024-01", "2024-03",
                              market="spot", kind="aggTrades", monthly=True)
result = hft_sdk.run_sources(paths)
```

From the shell:

```bash
hft-sdk fetch --crypto-lob BTCUSDT --period 2026-08 --table depth
hft-sdk fetch --binance BTCUSDT --start 2024-01 --end 2024-03 \
  --market spot --kind aggTrades --monthly
hft-sdk https://example.com/data.parquet
hft-sdk ~/.cache/hft-sdk/crypto-lob/depth/binance/BTCUSDT/2026-08.parquet
```

Downloads cover Binance `trades`/`aggTrades`/`bookDepth` archives and
crypto-lob-stream `depth`/`trades`/`snapshots`, plus generic HTTP(S) or
`hf://` URLs. The engine normalizes tick CSVs, Binance trade/bookDepth
archives, and crypto-lob depth/trades. Use `hft_sdk.iter_tick_arrays()` to
stream normalized NumPy arrays, or `hft_sdk.to_tick_csv()` to convert a
download into the SDK's tick CSV format.

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

## Real Data (C++ streaming path)

The Python `hft-sdk` commands above download and normalize data automatically.
For large files the C++ replay tools use a separate streaming path:

```bash
python3 tools/parquet_to_csv.py --table depth \
  --input ~/.cache/hft-sdk/crypto-lob/depth/binance/BTCUSDT/2026-08.parquet \
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

## Benchmarks

Every number and chart below is generated by `./tools/run_pipeline.sh` and
checked in under [`benchmarks/`](benchmarks). The full write-ups are
[`BENCHMARK_REPORT.md`](BENCHMARK_REPORT.md) and
[`benchmarks/results/REAL_DATA_REPORT.md`](benchmarks/results/REAL_DATA_REPORT.md).

| Highlight | Result |
| --- | --- |
| RingBuffer vs mutex queue at 1M events | **2.78x** lower per-event latency |
| Cache-warm RingBuffer p50 / p99 | **2.0 ns / 3.2 ns** |
| SIMD Welford vs scalar | **1.98x** |
| Best synthetic count OBI | **LF, 30s**: Pearson 0.52081, R2 0.27124 |
| Best synthetic volume OBI | **LF, 30s**: Pearson 0.54889, R2 0.30128 |
| Synthetic backtest (2 bps/side) | 119 trades, **-4.3535%** total return |
| Hawkes branching ratio | **0.9900** |
| Real BTCUSDT volume OBI | R2 ~0.003 (not yet alpha) |

### Lock-free transport vs mutex queue

![Latency scaling: mutex queue vs lock-free RingBuffer](benchmarks/plots/queue_vs_ringbuffer.png)

| Events | `std::queue` + mutex (ns/event) | RingBuffer (ns/event) | Speedup |
| --- | --- | --- | --- |
| 10 | 14.34 | 4.06 | 3.53x |
| 100 | 10.57 | 3.60 | 2.93x |
| 1,000 | 9.77 | 3.75 | 2.61x |
| 10,000 | 10.03 | 3.64 | 2.76x |
| 100,000 | 9.91 | 3.63 | 2.73x |
| 1,000,000 | 10.13 | 3.64 | **2.78x** |

The RingBuffer is preallocated and reuses a fixed-capacity ring; the queue
baseline performs a locked push and pop per event. Both run on one thread, so
this isolates synchronization/allocation cost rather than multi-core contention.

### Cache-cold vs cache-warm hand-off

![RingBuffer latency: cache cold vs cache warm](benchmarks/plots/latency_distribution.png)

| Path | p50 (ns) | p90 (ns) | p99 (ns) | Samples |
| --- | --- | --- | --- | --- |
| Cache cold | 5.9 | 7.2 | 18.2 | 313 |
| Cache warm | 2.0 | 2.0 | 3.2 | 313 |

Cache-warm measurements run an explicit warm-up pass and prefetch the ring
before sampling. Latency is amortised over batches of 64 hand-offs.

### Signal quality (synthetic LOB stream)

![Signal-quality heatmap](benchmarks/plots/signal_quality_heatmap.png)

| Filter | Count mean abs Pearson | Count mean R2 | Volume mean abs Pearson | Volume mean R2 | Trade count ρ | Trade volume ρ |
| --- | --- | --- | --- | --- | --- | --- |
| UF | 0.06939 | 0.00834 | 0.06553 | 0.00798 | 0.46794 | 0.47493 |
| LF | **0.37798** | **0.17317** | **0.38970** | **0.18926** | 0.46491 | 0.46040 |
| MF | 0.37065 | 0.15265 | 0.38425 | 0.16359 | **0.48933** | **0.49861** |
| MTF | 0.38192 | 0.16352 | 0.38752 | 0.16700 | 0.48932 | 0.49854 |

Mean absolute Pearson is used for ranking so the OBI sign convention does not
hide magnitude. R2 is the square of the contemporaneous Pearson correlation.
Trade-count/trade-volume columns are mean absolute Pearson for the
trade-event-only signal. Regime cross-correlations are computed on AR(1)
residuals.

### Layer-3 Hawkes excitation

On the synthetic stream the fitted 13x13 kernel has a spectral radius of
**0.9900** (raw 1.1268, rescaled to the stability boundary) and the 4x9 masked
OBI-to-return cross-excitation norm is **0.6696**.

![13x13 Hawkes kernel](benchmarks/plots/hawkes_kernel.png)

![4x9 masked cross-excitation](benchmarks/plots/hawkes_cross_excitation.png)

### Deterministic backtest (PnL)

Signal: volume OBI, scheme UF, horizon 1s. Execution: taker, 2.00 bps per side,
100 us latency, threshold 0.

| Metric | Value |
| --- | --- |
| Trades | 119 |
| Total return | -4.3535% |
| Mean trade return | -3.738 bps |
| Median trade return | -2.998 bps |
| Std trade return | 5.013 bps |
| Win rate | 27.73% |
| Profit factor | 0.132 |
| Sharpe (per trade) | -0.746 |
| Max drawdown | 4.3535% |
| Total fees | 476.000 bps |
| Average holding | 1000 ms |

The simulator fills at the first snapshot on or after signal time plus the
configured latency, holds for the selected horizon, applies fees to both legs,
and uses non-overlapping mid-price fills.

### Real BTCUSDT L2 data

Real-data replay is intentionally reported as a negative result: public BTC
depth does **not** yet show usable alpha with this event-flow OBI
implementation.

| Metric | Real BTCUSDT L2 |
| --- | --- |
| Best count OBI | MF, 50s: Pearson -0.02995, R2 0.00090 |
| Best volume OBI | MF, 50s: Pearson 0.05404, R2 0.00292 |
| Hawkes branching ratio | 0.9900 |
| 4x9 cross-excitation norm | 0.0345 |
| PnL | not demonstrated |

![Real-data signal-quality heatmap](benchmarks/plots/real/signal_quality_heatmap.png)

![Real-data 4x9 masked cross-excitation](benchmarks/plots/real/hawkes_cross_excitation.png)

### Benchmark environment

| Field | Value |
| --- | --- |
| Host | MacBook-Air, Apple silicon, 10 logical CPUs |
| Google Benchmark | v1.9.1 |
| Build | Release, `-march=native` (pipeline default) |
| Benchmark timestamp | 2026-10-24 |

Latency and throughput are host- and compiler-dependent. Re-run the pipeline on
the target hardware before making capacity decisions.

## Repository Layout

```text
include/hft/        C++ headers: queue, filters, scoring, pipeline, SDK, backtest
src/                benchmark, evaluator, replay, SDK, SPSC explicit instantiation
tests/              unit, concurrency, volume, and backtest tests
tools/              data ingest, pipeline, real-data runner, acceptance/consumer tests
benchmarks/         plotting, generated results, charts, requirements
python/hft_sdk/     pip-installable package (high-level helpers + CLI)
python/bindings.cpp pybind11 bindings
pyproject.toml      scikit-build-core packaging (`pip install .`)
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

- **Research prototype:** the API, signal definitions, adapters, and results are not yet production-stable.
- The current OBI implementation summarizes event flow; it is not a full resting-depth reconstruction.
- Real-data alpha is not demonstrated yet. The crypto depth slice produces near-zero R2.
- The mutex-queue benchmark is a single-thread hand-off comparison, not a contention benchmark.
- The PnL simulator uses mid-price fills and parameterized fees; it is not a production execution model.
- Hawkes estimation is a discrete-time least-squares fit, not continuous-time maximum likelihood.

## References

1. **Order Book Filtration & Signal Extraction** — Anantha et al. (2025), arXiv:2507.22712.  
   [https://arxiv.org/abs/2507.22712](https://arxiv.org/abs/2507.22712)

   - Studies order-book-imbalance (OBI) signal optimization by removing transient/flickering quotes.
   - **Lifetime filter (`F_T`):** rejects orders whose dwell time in the book is below a threshold `T`.
   - **Modification-count filter (`F_M`):** excludes orders modified more than `M` times.
   - **Modification-time filter (`F_MTF`):** keeps only orders whose interval between successive modifications meets a minimum `M`.
   - The modification-time filter produces the strongest improvement in associative metrics: Pearson correlation `S_ρ` and regime-switch cross-correlation `S_{ρ,Λ}`.
   - OBI computed from LOB states shows only modest gains in causal Hawkes excitation norms `S_φ`, because LOB events are dominated by self-excitation (quote stuffing, replenishment).
   - OBI built only from executed trade events (`OBI^(T)`) shows large increases in Hawkes excitation under structural filtering; for example, `24.7352` on 23 January under MTF versus `9.6726` for standard OBI.
   - This repository implements the lifetime, modification-count, and modification-time filters, plus the Hawkes/regime diagnostics discussed in the paper.

2. **Low-Latency C++ Design Patterns in HFT** — Bilokon & Gunduz (2023), arXiv:2309.04259.  
   [https://arxiv.org/abs/2309.04259](https://arxiv.org/abs/2309.04259)

   - Practical optimization techniques for latency-critical C++ order-management and HFT systems.
   - Reported benchmark improvements: cache warming ~90%; `constexpr` ~90.88%; loop unrolling ~72%; lock-free programming ~63%; SIMD/AVX2 ~49%.
   - **LMAX Disruptor pattern:** replaces traditional multi-threaded queues with a preallocated ring buffer and no mutex locking.
   - The C++ Disruptor showed approximately 38–55% lower latency at high event volumes compared with a standard `std::queue` protected by condition variables.
   - Pairs-trading optimization: fixed-width buffers (no dynamic allocation), AVX2 SIMD, and inlining reduced latency by 87.33%, from 517.559 ns to 65.580 ns.
   - This repository applies the same themes: cache-friendly ring buffers, cache warming, constexpr/compile-time configuration, SIMD statistics, and a lock-free transport benchmark against a mutex-backed queue.

## License

Proprietary. All rights reserved. Contact the repository owner for commercial licensing terms.
