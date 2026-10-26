# Data adapters

## Built-in Python adapters

- `hft_sdk.live.BinancePerpFeed` — Binance USDT-M futures.
- `hft_sdk.live.BybitPerpFeed` — Bybit linear perpetuals.

Both normalize depth/trade messages into the same event arrays used by the C++
engine, reconnect with exponential backoff, and record depth sequence gaps.

```python
from hft_sdk.live import BinancePerpFeed, LiveConfig

feed = BinancePerpFeed(LiveConfig(symbol="BTCUSDT", scheme="MTF", horizon=10))
feed.start()
print(feed.health())
feed.stop()
```

## Native adapters

Implement the `hft::ingest` feed contract and write normalized events to the
C++ engine or to `hft_feed --stdin`. The line protocol is documented in
[`DEPLOYMENT.md`](DEPLOYMENT.md).

## Production checklist for a new venue

- websocket reconnect and backoff
- snapshot + diff synchronization
- sequence-gap detection and recovery
- timestamp normalization to nanoseconds
- symbol and contract-roll mapping
- rate-limit handling
- exchange maintenance/outage handling
- fixture-based parser tests
- data-license review

Public exchange data remains subject to each exchange's terms. The SDK stores
and normalizes data; it does not grant redistribution rights.


### Binance synchronization boundary

The USDT-M adapter waits for a diff satisfying `U <= lastUpdateId <= u`, drops
older buffered diffs, and requires each subsequent `pu` to equal the previous
`u`. Missing continuity or a snapshot-buffer overflow triggers reconnection and
a fresh REST snapshot. Every snapshot resets signal-engine history and synthetic
order tracking; counters remain cumulative. Buffered trades before the bridge
are discarded rather than mixed with a later book epoch. The adapter reports
`syncing` until the bridge is established.

Protocol reference: [Binance local order book procedure](https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams/How-to-manage-a-local-order-book-correctly).
Offline fixtures validate recovery decisions; they do not establish live venue
availability or certify unattended operation.
