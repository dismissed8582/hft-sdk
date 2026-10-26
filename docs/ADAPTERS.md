# Data adapters

## Built-in Python adapters

- `hft_sdk.live.BinancePerpFeed` — Binance USDT-M futures.
- `hft_sdk.live.BybitPerpFeed` — Bybit linear perpetuals.

Both normalize depth/trade messages into the same event arrays used by the C++
engine and reconnect with exponential backoff. Binance verifies `pu` continuity;
Bybit uses snapshots and monotonically increasing update IDs, which are not
assumed contiguous. The Bybit adapter subscribes to depth 50 (snapshot/delta).

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

### Live data contracts

- Binance depth uses `/public`; aggregate trades use a separate `/market` socket.
  Both sockets have a bounded queue and share one processing owner. Independent
  channels do not provide a cross-channel causal ordering guarantee; SDK counters
  expose late/rejected events rather than hiding them.
- Bybit snapshots and reconnects reset the signal epoch and replace old synthetic
  orders. UUID trade IDs become deterministic 63-bit integer surrogates; retain
  the original string IDs in raw recordings if exact identity is required.
- Exchange timestamps are milliseconds and are explicitly converted to ns.
  `last_message_age_s` uses monotonic receipt time, independent of exchange clocks.
- Prices/sizes are scaled using decimal arithmetic and checked for int64 range.
  `max_batch` is a hard flush boundary. Price-level bookkeeping has bounded lazy
  heap cleanup so churn does not retain an unlimited number of deleted levels.

Bybit protocol references: [order book](https://bybit-exchange.github.io/docs/v5/websocket/public/orderbook)
and [trades](https://bybit-exchange.github.io/docs/v5/websocket/public/trade).
Binance routing: [migration notice](https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-market-streams/Important-WebSocket-Change-Notice).

### LOBSTER message files

The C++ reader uses standard message codes 1–5 and the Direction column for side;
cross trades/halts (6/7) are skipped. Prices arrive in dollar × 10,000 units and
are converted to the requested ticks per dollar. Partial cancellations become
MODIFY with an absolute remaining size. Replay must include original submissions
or seed `LobsterOptions.initial_remaining`; an unknown partial cancellation fails
explicitly. Visible executions link to the resting parent; hidden trades do not.
The message-only reader does not reconstruct quotes from a separate order-book
file. Reference: [LOBSTER format](https://lobsterdata.com/info/DataStructure.php).
