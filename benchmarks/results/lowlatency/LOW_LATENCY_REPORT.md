# Low-Latency Transport & Microbenchmark Report

Generated from `hft_threaded_bench` and `hft_microbench`.

## Cross-thread transport

Events per run: **1,000,000**; measured runs: **15**.

| Path | Median ns/event | p50 ns/event | p99 ns/event | Events/s |
| --- | --- | --- | --- | --- |
| std::queue + mutex | 88.704 | 88.704 | 138.236 | 10,319,034 |
| Disruptor ring | 75.227 | 75.227 | 92.563 | 13,635,948 |

Ring speedup over the mutex queue: **1.18x**.

![Cross-thread queue vs ring](benchmarks/plots/lowlatency/threaded_queue_vs_ring.png)

## Microbenchmarks

The paper-2 microbenchmarks are illustrative and host-dependent. The
comparison columns report the measured improvement of the optimised
variant over its baseline.

| Baseline | Optimised | Baseline ns | Optimised ns | Improvement |
| --- | --- | --- | --- | --- |
| RuntimeFactorial | Constexpr | 5.5816 | 0.4819 | 91.37% |
| LoopPlain | LoopUnrolled | 378.9705 | 549.2917 | -44.94% |
| NoPrefetch | WithPrefetch | 97765.9310 | 506673.2026 | -418.25% |
| UnsignedCompare | SignedCompare | 0.5329 | 0.5107 | 4.18% |
| MixedFloatDouble | FloatOnly | 8.8965 | 4.9018 | 44.90% |
| NoInline | AlwaysInline | 1.4458 | 0.6187 | 57.21% |
| BranchBaseline | BranchReduced | 749.2689 | 750.2459 | -0.13% |
| ArrayScalar | ArraySimd | 399.7819 | 559.9706 | -40.07% |
| PairsRollingScalar | PairsRollingSimd | 1100198.7480 | 412179.1397 | 62.54% |

![Microbenchmarks](benchmarks/plots/lowlatency/microbenchmarks.png)

## Hardware cache counters

```text
perf not available; skipping hardware cache counters
```

## Method notes

- The cross-thread benchmark uses one producer thread and one consumer
  thread. The queue baseline uses `std::mutex` plus a condition variable;
  the ring uses the Disruptor-style busy-spin consumer.
- Timing includes thread creation and join, so it is a conservative
  comparison rather than a pinned-core production measurement.
- The microbenchmark suite covers constexpr, loop unrolling, prefetching,
  signedness, float/double consistency, inlining, branch reduction and
  SIMD. It is not a substitute for `perf` cache counters.
- Negative results are retained: modern compilers auto-vectorise the
  scalar loops, and prefetch/SIMD can regress on Apple silicon. The
  table is a measurement of this build, not a universal ranking.

