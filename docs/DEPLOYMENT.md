# Deployment

## `hft_feed`

`hft_feed` is the long-running runtime. It accepts either:

- `--replay FILE` — replay a normalized tick CSV.
- `--stdin` — read newline-delimited normalized events from a parent process.

It exposes:

- `GET /health` — JSON state, counters, last-event age.
- `GET /metrics` — Prometheus text format.

### stdin protocol

```text
timestamp_ns,event_type,side,order_id,price_ticks,quantity,bid_price_ticks,ask_price_ticks[,parent_order_id[,sequence]]
```

- `event_type`: `NEW|MODIFY|CANCEL|TRADE` or `0|1|2|3`
- `side`: `BID|ASK|BUY|SELL` or `1|2|3|4`
- `sequence` is optional; when present, gaps are counted.

### Configuration

`config/hft_feed.conf`:

```ini
feed.stdin=true
feed.sample_ms=250
feed.lifetime_ms=100
feed.max_modifications=3
feed.modification_time_ms=50
feed.stall_ms=5000
http.address=0.0.0.0
http.port=9100
replay.speed=0
log.level=info
```

Environment overrides:

```bash
HFT_HTTP_PORT=9200 hft_feed --config config/hft_feed.conf --stdin
```

### Docker

```bash
docker build -t hft-sdk:1.0.1 .
docker run --rm -i -p 127.0.0.1:9100:9100 hft-sdk:1.0.1 --config /etc/hft/hft_feed.conf --stdin
```

### Health semantics

- `starting` — no event processed yet.
- `live` — event processed within `feed.stall_ms`.
- `degraded` — a sequence gap, malformed line, or pipeline data loss was recorded;
  latched until process restart. The native stdin runtime does not reconnect.
- `stalled` — no processed event within `feed.stall_ms`; takes precedence over
  degradation after the first event. Fresh input returns to `live` or latched
  `degraded`. Health and the state gauge refresh at least every 50 ms while idle.
- `stopped` — clean shutdown (the HTTP server closes during shutdown).


Stdin batches flush after each available read, including batches below 4096
records. A partial line waits for newline or EOF. Lines above 1 MiB are rejected.
SIGINT/SIGTERM interrupts idle input and replay pacing. HTTP clients have a
2-second total request/response deadline and cannot hold shutdown open.
`/health` returns HTTP 200 for a successfully served diagnostic response; inspect
its JSON `status` for feed readiness. It is not a readiness HTTP status endpoint.

The Docker command keeps stdin open with `-i`. Pipe a producer of normalized
records into it. EOF and replay completion stop the runtime and its endpoints.
Published metrics are restricted to the host loopback interface in this example.

Duplicate/out-of-order sequence numbers on stdin are discarded and counted in
`dropped`; they cannot count the same event twice. Invalid event values and late
timestamps are also exposed as drops. Valid event counters exclude these rows.
Historical snapshot evictions are normal bounded retention and have a separate
`hft_snapshot_evictions_total` counter; they do not degrade feed readiness.

Use `--max-snapshots N` or `feed.max_snapshots=N` to set retention; zero is an
explicit unbounded-history opt-in. Default sampling remains 250 ms.
