# Low-Latency LOB Engine: Benchmark & Signal-Quality Report

*Generated 2026-10-24 15:18 UTC*

## Abstract

This report evaluates the low-latency transport, count- and volume-weighted OBI signal layers, and deterministic backtest of the HFT LOB engine on `data/sample_ticks.csv`. All numbers are reproducible from the commands in the final section.

## Executive Summary

- At **1,000,000 events**, the lock-free RingBuffer is **2.06x faster per event** than `std::queue` guarded by `std::mutex`.
- Cache-warm RingBuffer hand-off latency is **2.6 ns p50** and **7.8 ns p99**.
- SIMD Welford is **2.03x** faster than the scalar accumulator on the benchmark workload.
- Strongest in-sample count OBI correlation: **LF** at **30s** (Pearson 0.52081, R2 0.27124).
- Strongest in-sample volume OBI correlation: **LF** at **30s** (Pearson 0.54889, R2 0.30128).
- Deterministic backtest: **119** trades, total return **-4.353%**, max drawdown **4.353%**.
- Layer-3 Hawkes branching ratio (spectral radius): **0.9900**; 4x9 OBI-to-return cross-excitation norm: **0.6696**.

## 1. Lock-Free Transport vs. Mutex Queue

![Latency scaling: mutex queue vs lock-free RingBuffer](benchmarks/plots/queue_vs_ringbuffer.png)

| Events | std::queue + mutex (ns/event) | RingBuffer (ns/event) | Speedup |
| --- | --- | --- | --- |
| 10 | 29.96 | 8.03 | 3.73x |
| 100 | 27.61 | 12.47 | 2.21x |
| 1,000 | 28.75 | 10.90 | 2.64x |
| 10,000 | 23.75 | 11.81 | 2.01x |
| 100,000 | 20.58 | 10.26 | 2.01x |
| 1,000,000 | 18.87 | 9.18 | 2.06x |

The RingBuffer is preallocated and reuses a fixed-capacity ring; the queue baseline performs a locked push and a locked pop for every event. Both paths execute the hand-off on one thread, so this isolates synchronization and allocation overhead rather than multi-core contention.

## 2. Cache-Cold vs. Cache-Warm Hand-Off

![RingBuffer latency: cache cold vs cache warm](benchmarks/plots/latency_distribution.png)

| Path | p50 (ns) | p90 (ns) | p99 (ns) | Samples |
| --- | --- | --- | --- | --- |
| Cache cold | 11.7 | 18.2 | 25.9 | 313 |
| Cache warm | 2.6 | 3.9 | 7.8 | 313 |

Cache-warm measurements run an explicit warm-up pass and prefetch the ring before sampling. Latency is amortised over batches of 64 hand-offs to stay above the monotonic-clock granularity.

## 3. Signal Quality

![Signal-quality heatmap](benchmarks/plots/signal_quality_heatmap.png)

| Filter | Count mean abs Pearson | Count mean R2 | Volume mean abs Pearson | Volume mean R2 | Trade count mean abs Pearson | Trade volume mean abs Pearson |
| --- | --- | --- | --- | --- | --- | --- |
| UF | 0.06939 | 0.00834 | 0.06553 | 0.00798 | 0.46794 | 0.47493 |
| LF | 0.37798 | 0.17317 | 0.38970 | 0.18926 | 0.46491 | 0.46040 |
| MF | 0.37065 | 0.15265 | 0.38425 | 0.16359 | 0.48933 | 0.49861 |
| MTF | 0.38175 | 0.16346 | 0.39070 | 0.17009 | 0.48891 | 0.49799 |

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

![Trade-level PnL](benchmarks/plots/pnl_trades.png)

The top panel is the compounded equity curve, the middle panel shows
each trade's net return after fees, and the bottom panel is the
running drawdown for the same 119-trade run.

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
- `benchmarks/results/paper/` - paper-faithful ex-post diagnostics
- `benchmarks/results/pairs/` - cointegration pairs-trading backtest
- `benchmarks/results/lowlatency/` - threaded transport and microbenchmarks
- `benchmarks/results/stress/` - stress/edge-case report
- `benchmarks/plots/*.png` - publication charts

### Benchmark Environment

| Field | Value |
| --- | --- |
| Host | MacBook-Air.fritz.box |
| Logical CPUs | 10 |
| Nominal MHz | 24 |
| Google Benchmark | v1.9.1 |
| Benchmark timestamp | 2026-10-24T17:04:02+02:00 |

## Scope and Limitations

- Signal-quality numbers are computed on the deterministic synthetic LOB stream supplied for pipeline validation; they are not a production alpha forecast.
- This report uses the causal streaming filters. The paper-faithful ex-post
  filters, realized trade returns, 9x3 regime scores and sum-of-exponentials
  Hawkes fit are reported separately in `benchmarks/results/paper/PAPER_REPORT.md`.
- The queue comparison is a single-thread hand-off benchmark. It measures mutex and allocation overhead, not multi-core contention.
- Latency figures are host- and compiler-dependent. Re-run the pipeline on the target deployment hardware before making capacity decisions.
- The Hawkes kernel is a discrete-time least-squares estimate, not a continuous-time maximum-likelihood fit. Validate it out of sample.
