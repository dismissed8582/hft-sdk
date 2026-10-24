# Low-Latency LOB Engine: Benchmark & Signal-Quality Report

*Generated 2026-10-24 11:11 UTC*

## Abstract

This report evaluates the low-latency transport, count- and volume-weighted OBI signal layers, and deterministic backtest of the HFT LOB engine on `data/sample_ticks.csv`. All numbers are reproducible from the commands in the final section.

## Executive Summary

- At **1,000,000 events**, the lock-free RingBuffer is **2.78x faster per event** than `std::queue` guarded by `std::mutex`.
- Cache-warm RingBuffer hand-off latency is **2.0 ns p50** and **3.2 ns p99**.
- SIMD Welford is **1.98x** faster than the scalar accumulator on the benchmark workload.
- Strongest in-sample count OBI correlation: **LF** at **30s** (Pearson 0.52081, R2 0.27124).
- Strongest in-sample volume OBI correlation: **LF** at **30s** (Pearson 0.54889, R2 0.30128).
- Deterministic backtest: **119** trades, total return **-4.353%**, max drawdown **4.353%**.
- Layer-3 Hawkes branching ratio (spectral radius): **0.9900**; 4x9 OBI-to-return cross-excitation norm: **0.6696**.

## 1. Lock-Free Transport vs. Mutex Queue

![Latency scaling: mutex queue vs lock-free RingBuffer](benchmarks/plots/queue_vs_ringbuffer.png)

| Events | std::queue + mutex (ns/event) | RingBuffer (ns/event) | Speedup |
| --- | --- | --- | --- |
| 10 | 14.34 | 4.06 | 3.53x |
| 100 | 10.57 | 3.60 | 2.93x |
| 1,000 | 9.77 | 3.75 | 2.61x |
| 10,000 | 10.03 | 3.64 | 2.76x |
| 100,000 | 9.91 | 3.63 | 2.73x |
| 1,000,000 | 10.13 | 3.64 | 2.78x |

The RingBuffer is preallocated and reuses a fixed-capacity ring; the queue baseline performs a locked push and a locked pop for every event. Both paths execute the hand-off on one thread, so this isolates synchronization and allocation overhead rather than multi-core contention.

## 2. Cache-Cold vs. Cache-Warm Hand-Off

![RingBuffer latency: cache cold vs cache warm](benchmarks/plots/latency_distribution.png)

| Path | p50 (ns) | p90 (ns) | p99 (ns) | Samples |
| --- | --- | --- | --- | --- |
| Cache cold | 5.9 | 7.2 | 18.2 | 313 |
| Cache warm | 2.0 | 2.0 | 3.2 | 313 |

Cache-warm measurements run an explicit warm-up pass and prefetch the ring before sampling. Latency is amortised over batches of 64 hand-offs to stay above the monotonic-clock granularity.

## 3. Signal Quality

![Signal-quality heatmap](benchmarks/plots/signal_quality_heatmap.png)

| Filter | Count mean abs Pearson | Count mean R2 | Volume mean abs Pearson | Volume mean R2 | Trade count mean abs Pearson | Trade volume mean abs Pearson |
| --- | --- | --- | --- | --- | --- | --- |
| UF | 0.06939 | 0.00834 | 0.06553 | 0.00798 | 0.46794 | 0.47493 |
| LF | 0.37798 | 0.17317 | 0.38970 | 0.18926 | 0.46491 | 0.46040 |
| MF | 0.37065 | 0.15265 | 0.38425 | 0.16359 | 0.48933 | 0.49861 |
| MTF | 0.38192 | 0.16352 | 0.38752 | 0.16700 | 0.48932 | 0.49854 |

Mean absolute Pearson is used for ranking so that the OBI sign convention does not obscure magnitude. R2 is the square of the contemporaneous Pearson correlation. Volume-weighted OBI uses the same event stream with quantities instead of flat counts. Regime cross-correlations are computed on AR(1) residuals.

## 4. Layer 3: Hawkes Cross-Excitation

- Regime tuples: **715**; lags: **16**.
- Branching ratio: **0.9900** (raw **1.1268**). The raw estimate was rescaled to the stability boundary.
- 4x9 masked OBI-to-return norm: **0.6696**.

| Return target | OBI_0 | OBI_1 | OBI_2 | OBI_3 | OBI_4 | OBI_5 | OBI_6 | OBI_7 | OBI_8 | L2 norm |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RET_0 | 0.0000 | 0.0000 | 0.0367 | 0.0000 | 0.0036 | 0.0029 | 0.2978 | 0.0000 | 0.0000 | 0.3000 |
| RET_1 | 0.0000 | 0.0000 | 0.0882 | 0.1083 | 0.0804 | 0.1347 | 0.1707 | 0.0000 | 0.0000 | 0.2707 |
| RET_2 | 0.0000 | 0.0000 | 0.2970 | 0.0539 | 0.1125 | 0.1074 | 0.4051 | 0.0000 | 0.0000 | 0.5285 |
| RET_3 | 0.0000 | 0.0000 | 0.0717 | 0.0244 | 0.0000 | 0.0037 | 0.0000 | 0.0000 | 0.0000 | 0.0759 |

The 13x13 kernel is fitted in `benchmarks/plot_results.py` from the time-synchronized regime tuples exported by `hft_eval`. It is a discrete-time exponential-memory Hawkes model with nonnegative lag coefficients; the matrix shown is the integrated kernel.

![13x13 Hawkes kernel](benchmarks/plots/hawkes_kernel.png)

![4x9 masked cross-excitation](benchmarks/plots/hawkes_cross_excitation.png)

## 5. Deterministic Backtest (PnL)

- Signal: **volume** OBI, scheme **UF**, horizon **1s**.
- Execution: **taker**, fee **2.00 bps per side**, latency **100.0us**, threshold **0.0000**.

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
| Sharpe (daily, annualized) | 0.000 |
| Max drawdown | 4.3535% |
| Total fees | 476.000 bps |
| Average holding | 1000.000 ms |

Trade-level PnL is stored in `benchmarks/results/pnl_trades.csv`; the raw net-return vector is also embedded in `pnl_summary.json`.

The simulator fills at the first snapshot on or after signal time plus the configured latency, holds for the selected horizon, and applies the fee to both legs. Trades are non-overlapping and use mid-price fills.

## Methodology & Reproducibility

### Pipeline

1. The CSV loader normalizes timestamps, event types, sides, quantities, and quotes.
2. A preallocated open-addressed tracker computes lifetime, modification-count, and modification-time state per order.
3. Four rolling event-count windows (1s, 10s, 30s, 50s) maintain UF/LF/MF/MTF aggregates in O(1).
4. Pearson/R2 use a SIMD Welford accumulator. Regime scores discretize OBI into 9 buckets and returns into 4 buckets, then remove AR(1) persistence.
5. C++ exports `(timestamp_ns, obi_regime, return_regime)` tuples; Python fits the 13x13 Hawkes kernel and 4x9 mask.
6. Volume-weighted OBI substitutes event and trade quantities for flat counts. The deterministic backtest fills at mid after the configured latency and applies maker/taker fees.

### Exact Commands

```bash
python3 -m pip install -r benchmarks/requirements.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/hft
./tools/run_pipeline.sh

# Regenerate charts and this report from existing artifacts
python3 benchmarks/plot_results.py
```

`run_pipeline.sh` performs the build, unit tests, synthetic-data generation, signal evaluation, Google Benchmark run, and chart/report generation. The benchmark executable writes strictly to `benchmarks/results/latest_bench.json`; latency samples, scores, regime tuples, and Hawkes artifacts land in `benchmarks/results/`.

### SDK Consumption

```cmake
find_package(HftSdk REQUIRED)
target_link_libraries(your_target PRIVATE HftSdk::sdk)
```

```cpp
#include <hft/HftSdk.hpp>
hft::sdk::OBIFilterEngine engine;
engine.process(events);
auto metrics = engine.metrics();
```

```python
import hft_sdk
engine = hft_sdk.OBIFilterEngine()
engine.process_arrays(order_id, timestamp_ns, price_ticks, quantity,
                      bid_price_ticks, ask_price_ticks, type_codes, side_codes)
```

Build the compiled SDK with `-DHFT_BUILD_SHARED_SDK=ON` and bindings with `-DHFT_BUILD_PYTHON=ON` (both are on by default).

### Artifacts

- `benchmarks/results/latest_bench.json` - Google Benchmark JSON
- `benchmarks/results/latency_samples.csv` - raw latency samples
- `benchmarks/results/signal_scores.csv` - filter/horizon scores
- `benchmarks/results/layer3_regimes.csv` - Layer-3 tuple export
- `benchmarks/results/pnl_summary.json` - backtest summary
- `benchmarks/results/pnl_trades.csv` - trade-level PnL
- `benchmarks/plots/*.png` - publication charts

### Benchmark Environment

| Field | Value |
| --- | --- |
| Host | MacBook-Air.fritz.box |
| Logical CPUs | 10 |
| Nominal MHz | 24 |
| Google Benchmark | v1.9.1 |
| Benchmark timestamp | 2026-10-24T13:09:42+02:00 |

## Scope and Limitations

- Signal-quality numbers are computed on the deterministic synthetic LOB stream supplied for pipeline validation; they are not a production alpha forecast.
- The queue comparison is a single-thread hand-off benchmark. It measures mutex and allocation overhead, not multi-core contention.
- Latency figures are host- and compiler-dependent. Re-run the pipeline on the target deployment hardware before making capacity decisions.
- The Hawkes kernel is a discrete-time least-squares estimate, not a continuous-time maximum-likelihood fit. Validate it out of sample.
