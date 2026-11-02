# Release 1.0.1 readiness

This release provides a reusable C++20/Python market-data and research SDK.
Replay and deterministic fixtures define acceptance; live-market deployments
also require the site-specific checks below. See [validation](VALIDATION.md)
for reproducible commands and [research mapping](REPRODUCTION.md).

## Corrections included

- Newest snapshots retained after bounded storage fills; cumulative health and
  O(1) latest/statistics calls continue advancing. Reset clears all counters.
- Modified absolute quantities, explicit execution-parent namespaces, invalid
  inputs and duplicate active NEWs handled; tracker deletions remove tombstones
  so long-running churn does not turn every lookup into a full-table scan.
- Strict numeric/date, gzip/binary and LOBSTER parsing; checksummed atomic
  downloads, safe expected-member extraction, portable exports/optional zlib.
- Bybit UUID trade IDs and snapshot epochs, Binance split sockets, exact decimal
  scaling, bounded queues and monotonic stall detection.
- Pairs entry/exit fees, terminal liquidation, units and annualization fixed;
  chronological calibration mode available. Full-sample mode stays labeled.
- Paper sampling fix retained; Hawkes convergence can no longer succeed on an
  infinity comparison after one update. Forward return labels become observable
  at the window end. Convergence is included with every generated fit result.

## Supported use and ownership

Local acceptance on Linux, GCC 13.3 and Python 3.12:

| Check | Outcome |
| --- | --- |
| Portable Release build, native/shared SDK and Python module | Passed |
| Native unit/concurrency suite and nine feed runtime cases | Passed (2 CTest entries) |
| Python SDK, adapter, download and timing cases | 25 passed |
| Accounting, metadata and Hawkes regression cases | 13 passed |
| Installed shared, header-only and pkg-config consumers | Passed, each processes real fixture events |
| Wheel build, separate environment import and CLI | Passed |
| Quick stress/edge harness | 15 cases, no unexpected failures |
| ASan/UBSan native/runtime suite | Passed with local leak detection disabled |

LeakSanitizer cannot inspect `/proc` under this environment's tracing setup;
the first default run failed for that reason. ASan/UBSan were rerun with
`ASAN_OPTIONS=detect_leaks=0`. CI keeps its normal leak-checking configuration.
Docker, macOS, Python 3.9 and TSan/fuzz results must be checked in the workflows;
they are not represented as locally executed checks here.

The bounded online smoke attempt received no market messages in this execution
environment (Bybit rejected the WebSocket upgrade; Binance did not complete a
data session). After bounding socket handshakes, both adapters stopped cleanly;
this does not validate a live exchange deployment.

The engine accepts normalized single-instrument events in nondecreasing time.
Feed adapters synthesize level IDs; explicit parent IDs are required to mutate
a resting order on execution. NEW/MODIFY quantity is absolute remaining size,
TRADE quantity is executed size. Unknown cancels cannot create an order.
Streaming IDs may be reused after terminal state. Ex-post replay requires
unique order IDs within a session; ambiguous reuse raises an input error.

There is one engine processing owner. Externally synchronize processing, reads
and reset. History materialization can rotate storage and invalidate references;
use the SDK's `latest_snapshot()` copy for monitoring. Bounded snapshot storage
evicts history, not new signals. Zero max_snapshots explicitly opts into
unbounded storage. Default 524,288 samples at 250ms span 36.4 hours; snapshots
are roughly 1KiB each, so reserve an appropriate memory budget or lower the
retention limit. Metrics/backtests cover retained history only.

## Deployment acceptance

- Choose symbol-specific scales from exchange metadata; verify scaled values
  against recorded messages. Public depth is aggregated, not individual orders.
- Run an observed live session for every venue/symbol/configuration, including
  intentional disconnect, resnapshot, idle/stalled state and clean shutdown.
  Replay/fixture tests do not establish live-session success.
- Measure sustained representative traffic and resource use on target hardware.
  Transport microbenchmarks are not exchange latency or production capacity.
- Keep the HTTP health/metrics endpoint on a private interface or behind an
  authenticated proxy. It is a lightweight monitoring server, not a public API.
- Rebuild consumers with matching compiler/library ABI and cache-line setting.
  Windows and native Arrow C++ integration are outside the validated profiles.
- Strategy execution, order routing, risk limits and real-market profitability
  are separate systems. No positive alpha or investment outcome is established.

Historical reports are retained with warnings instead of silently presenting
old results as release measurements. Local validation and CI outcomes must be
recorded against the final commit before release tagging.
