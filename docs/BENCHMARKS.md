# Benchmark methodology

All performance claims must be reproducible:

- Build type: Release.
- CPU model and OS recorded with each run.
- Warm-up run discarded before measured runs.
- Median and p99 reported, not only the mean.
- Throughput and per-event latency reported separately.
- Queue comparison uses one producer and one consumer thread.
- Microbenchmarks are compiler- and host-dependent.

Commands:

```bash
./tools/run_lowlatency_bench.sh
./tools/run_stress.sh
python3 benchmarks/plot_results.py
```

Do not compare numbers across different CPUs, compilers, or build flags without
recording those differences.
