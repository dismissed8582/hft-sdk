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
