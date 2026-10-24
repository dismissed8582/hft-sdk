# Stress & Edge-Case Report

Generated: 2026-10-24 17:20:21

| Case | Return | Time (s) | Peak RSS (MB) | Output rows | Anchors | Status |
| --- | --- | --- | --- | --- | --- | --- |
| paper_eval/empty | 1 | 0.006 | 0.6 | 0 | 0 | error |
| paper_eval/single_event | 0 | 0.006 | 0.6 | 4 | 0 | ok |
| paper_eval/duplicate_id | 0 | 0.006 | 0.6 | 4 | 0 | ok |
| paper_eval/out_of_order | 0 | 0.007 | 0.5 | 4 | 0 | ok |
| paper_eval/no_trades | 0 | 0.007 | 0.5 | 4 | 0 | ok |
| paper_eval/timestamp_gap | 0 | 0.006 | 0.5 | 8 | 4 | ok |
| paper_eval/max_order_id | 0 | 0.006 | 0.6 | 4 | 0 | ok |
| paper_eval/60s/paper_filters | 0 | 0.128 | 61.3 | 960 | 12 | ok |
| paper_eval/60s/streaming_filters | 0 | 0.119 | 88.1 | 960 | 12 | ok |
| paper_eval/300s/paper_filters | 0 | 0.604 | 287.8 | 4800 | 76 | ok |
| paper_eval/300s/streaming_filters | 0 | 0.827 | 310.9 | 4800 | 76 | ok |
| paper_eval/900s/paper_filters | 0 | 2.237 | 855.8 | 14400 | 236 | ok |
| paper_eval/900s/streaming_filters | 0 | 1.834 | 852.1 | 14400 | 236 | ok |
| paper_analysis | 0 | 10.965 | 329.2 | 0 | 0 | ok |
| paper_analysis/single_event | 0 | 2.932 | 223.9 | 0 | 0 | ok |
| paper_analysis/no_trades | 0 | 2.350 | 223.5 | 0 | 0 | ok |
| threaded_bench | 0 | 0.061 | 19.5 | 25 | 0 | ok |

## Notes

- `empty` (header-only) inputs are expected to fail with a clean error.
- `single_event`, `duplicate_id`, `out_of_order`, `no_trades`,
  `timestamp_gap` and `max_order_id` are expected to complete without
  crashing.
- The machine-readable results are in `stress_report.json`.

