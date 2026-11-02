# Changelog

## 1.0.1

- Rolling snapshot retention, constant-time latest/statistics access, and observable rejects.
- Correct modified quantities and explicit execution-parent namespaces.
- Strict CSV/gzip/binary parsing and safe, checksum-verified archive downloads.
- Binance split socket routing; Bybit UUID, snapshot, timestamp and sequence handling.
- Correct pairs PnL accounting, final liquidation, and explicit Sharpe annualization.
- Portable build defaults, conditional package dependencies, and runnable shared/header consumers.
- Expanded consumer/platform, SDK, download integrity, and accounting regression checks.
- Duplicate/stale lifecycle updates are rejected; tracker deletion remains efficient under churn.
- Hawkes convergence, objective accounting, regularization and return-observation timing corrected.
- Pairs calibration can be frozen before a chronological evaluation segment with `--fit-points`.
- Incremental builds refresh Python adapter sources and bound websocket open/close handshakes.
- Added release-validation script, research mapping and explicit deployment acceptance scope.
- Truncated forward-return horizons are invalidated; late fills and duplicate cancels cannot extend a paper lifecycle.
- Native health configuration defaults to loopback; the container runs as a non-root UID with an explicit zlib runtime dependency.

## 1.0.0

- C++20 LOB signal engine, SIMD statistics, filters, backtest, and Disruptor.
- Python package, CLI, public-data download, and paper-faithful diagnostics.
- Continuous-time sum-of-exponentials Hawkes MLE with fixed decay grid.
- Production `hft_feed` runtime with config, structured logs, Prometheus
  metrics, health endpoint, replay, and stdin adapter.
- Binance USDT-M and Bybit linear perpetual Python adapters.
- Pairs-trading backtest, stress harness, cache-counter script, Dockerfile,
  pkg-config, and commercial/support documentation.
