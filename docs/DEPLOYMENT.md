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
docker build -t hft-sdk:1.0.0 .
docker run --rm -p 9100:9100 hft-sdk:1.0.0 --config /etc/hft/hft_feed.conf --stdin
```

### Health semantics

- `starting` — no event processed yet.
- `live` — event processed within `feed.stall_ms`.
- `degraded` — a gap, reconnect, or processing error was recorded.
- `stalled` — no event within `feed.stall_ms`.
- `stopped` — clean shutdown.
