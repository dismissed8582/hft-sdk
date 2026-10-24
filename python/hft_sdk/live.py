"""Live crypto-perpetual adapters for Binance USDT-M and Bybit linear.

The adapters normalize websocket depth/trade messages into the SDK's
``MarketEvent`` arrays, run the C++ engine in a background asyncio thread, and
expose health metrics. They reconnect with exponential backoff and track depth
sequence gaps.

``websockets`` is optional: install with ``pip install "hft-sdk[live]"``.
"""

from __future__ import annotations

import argparse
import asyncio
import heapq
import json
import threading
import time
import urllib.request
from dataclasses import dataclass
from typing import Any

import numpy as np

from ._core import EngineConfig, OBIFilterEngine

try:
    import websockets
except ImportError:  # pragma: no cover - optional dependency
    websockets = None

EVENT_NEW = 0
EVENT_MODIFY = 1
EVENT_CANCEL = 2
EVENT_TRADE = 3

SIDE_BID = 1
SIDE_ASK = 2
SIDE_BUY = 3
SIDE_SELL = 4

BINANCE_FUTURES_WS = "wss://fstream.binance.com/stream?streams={symbol}@trade/{symbol}@depth@100ms"
BINANCE_FUTURES_DEPTH = "https://fapi.binance.com/fapi/v1/depth?symbol={symbol}&limit={limit}"
BYBIT_LINEAR_WS = "wss://stream.bybit.com/v5/public/linear"


@dataclass
class LiveConfig:
    venue: str = "binance"
    symbol: str = "BTCUSDT"
    scheme: str = "MTF"
    horizon: int = 10
    price_scale: float = 100.0
    quantity_scale: float = 1e8
    flush_interval_ms: int = 50
    max_batch: int = 1024
    reconnect_initial_s: float = 0.25
    reconnect_max_s: float = 30.0
    snapshot_limit: int = 1000
    engine: EngineConfig | None = None


class LiveBook:
    """Persistent level book used to synthesize New/Modify/Cancel events."""

    def __init__(self) -> None:
        self.ids: dict[tuple[int, int], int] = {}
        self.quantities: dict[tuple[int, int], int] = {}
        self.bids: list[int] = []
        self.asks: list[int] = []
        self.next_id = 1

    def clear(self) -> None:
        self.ids.clear()
        self.quantities.clear()
        self.bids.clear()
        self.asks.clear()

    def _clean(self) -> None:
        while self.bids and (SIDE_BID, -self.bids[0]) not in self.ids:
            heapq.heappop(self.bids)
        while self.asks and (SIDE_ASK, self.asks[0]) not in self.ids:
            heapq.heappop(self.asks)

    def update(self, side: int, price_ticks: int,
               quantity: int) -> tuple[int, int, int, int] | None:
        key = (side, price_ticks)
        if quantity > 0:
            if key in self.ids:
                event_type = EVENT_MODIFY
                order_id = self.ids[key]
            else:
                event_type = EVENT_NEW
                order_id = self.next_id
                self.next_id += 1
                self.ids[key] = order_id
                heapq.heappush(self.bids if side == SIDE_BID else self.asks,
                               -price_ticks if side == SIDE_BID else price_ticks)
            self.quantities[key] = quantity
        else:
            if key not in self.ids:
                return None
            event_type = EVENT_CANCEL
            order_id = self.ids.pop(key)
            self.quantities.pop(key, None)
        self._clean()
        best_bid = -self.bids[0] if self.bids else 0
        best_ask = self.asks[0] if self.asks else 0
        return event_type, order_id, best_bid, best_ask


def _timestamp_ns(value: Any) -> int:
    integer = int(value)
    magnitude = abs(integer)
    if magnitude >= 100_000_000_000_000_000:
        return integer
    if magnitude >= 100_000_000_000_000:
        return integer * 1_000
    if magnitude >= 100_000_000_000:
        return integer * 1_000_000
    return integer * 1_000_000_000


class LiveFeed:
    """Base class for a live venue adapter."""

    def __init__(self, config: LiveConfig) -> None:
        if websockets is None:
            raise ImportError(
                "live adapters require websockets: pip install 'hft-sdk[live]'")
        self.config = config
        self.engine = OBIFilterEngine(
            config.engine if config.engine is not None else EngineConfig())
        self._book = LiveBook()
        self._arrays: dict[str, list[int]] = {
            "order_id": [], "timestamp_ns": [], "price_ticks": [],
            "quantity": [], "bid_price_ticks": [], "ask_price_ticks": [],
            "type_codes": [], "side_codes": [],
        }
        self._messages = 0
        self._trades = 0
        self._gaps = 0
        self._reconnects = 0
        self._dropped = 0
        self._last_message_ns = 0
        self._state = "starting"
        self._last_error = ""
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._last_flush = time.monotonic()
        self._last_update_id: int | None = None
        self._synced = False

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="hft-live-feed",
                                        daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._loop is not None:
            self._loop.call_soon_threadsafe(lambda: None)
        if self._thread is not None:
            self._thread.join(timeout=10.0)
            self._thread = None
        self._set_state("stopped")

    def _run(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._main())
        finally:
            self._flush(force=True)
            self._loop.close()
            self._loop = None

    async def _main(self) -> None:
        backoff = self.config.reconnect_initial_s
        while not self._stop.is_set():
            try:
                self._set_state("connecting")
                await self._stream()
                backoff = self.config.reconnect_initial_s
            except Exception as error:  # noqa: BLE001
                with self._lock:
                    self._reconnects += 1
                    self._last_error = str(error)
                self._set_state("degraded")
                if self._stop.is_set():
                    break
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2.0, self.config.reconnect_max_s)

    async def _stream(self) -> None:
        raise NotImplementedError

    # -- event helpers -----------------------------------------------------
    def _append(self, timestamp_ns: int, event_type: int, side: int,
                order_id: int, price_ticks: int, quantity: int,
                bid_price_ticks: int, ask_price_ticks: int) -> None:
        arrays = self._arrays
        arrays["timestamp_ns"].append(int(timestamp_ns))
        arrays["type_codes"].append(event_type)
        arrays["side_codes"].append(side)
        arrays["order_id"].append(int(order_id))
        arrays["price_ticks"].append(int(price_ticks))
        arrays["quantity"].append(int(quantity))
        arrays["bid_price_ticks"].append(int(bid_price_ticks))
        arrays["ask_price_ticks"].append(int(ask_price_ticks))
        self._messages += 1
        if event_type == EVENT_TRADE:
            self._trades += 1
        self._last_message_ns = int(timestamp_ns)
        if len(arrays["order_id"]) >= self.config.max_batch:
            self._flush()

    def _flush(self, force: bool = False) -> None:
        if not self._arrays["order_id"]:
            return
        now = time.monotonic()
        if not force and (now - self._last_flush) * 1000.0 < self.config.flush_interval_ms:
            return
        payload = {key: np.asarray(value, dtype=np.int64)
                   for key, value in self._arrays.items()}
        try:
            with self._lock:
                self.engine.process_arrays(**payload)
        except Exception as error:  # noqa: BLE001
            with self._lock:
                self._dropped += len(payload["order_id"])
                self._last_error = str(error)
            self._set_state("degraded")
        for value in self._arrays.values():
            value.clear()
        self._last_flush = now

    def _set_state(self, state: str) -> None:
        with self._lock:
            self._state = state

    def _record_gap(self, missing: int) -> None:
        if missing > 0:
            with self._lock:
                self._gaps += missing
            self._set_state("degraded")

    # -- health ------------------------------------------------------------
    def health(self) -> dict[str, Any]:
        with self._lock:
            now = time.time_ns()
            age = ((now - self._last_message_ns) / 1e9
                   if self._last_message_ns else -1.0)
            snapshots = len(self.engine.snapshots())
            return {
                "venue": self.config.venue,
                "symbol": self.config.symbol,
                "scheme": self.config.scheme,
                "horizon": self.config.horizon,
                "state": self._state,
                "messages": self._messages,
                "trades": self._trades,
                "gaps": self._gaps,
                "reconnects": self._reconnects,
                "dropped": self._dropped,
                "last_message_age_s": age,
                "snapshots": snapshots,
                "last_error": self._last_error,
            }

    def metrics(self) -> dict[str, Any]:
        with self._lock:
            return self.engine.metrics_table()

    def snapshot(self):
        with self._lock:
            snapshots = self.engine.snapshots()
            return snapshots[-1] if snapshots else None


class BinancePerpFeed(LiveFeed):
    def __init__(self, config: LiveConfig) -> None:
        super().__init__(config)

    async def _stream(self) -> None:
        symbol = self.config.symbol.lower()
        url = BINANCE_FUTURES_WS.format(symbol=symbol)
        async with websockets.connect(url, ping_interval=20, ping_timeout=20,
                                      max_queue=1024) as websocket:
            self._set_state("syncing")
            snapshot_task = asyncio.create_task(
                asyncio.to_thread(self._fetch_snapshot))
            buffered: list[dict[str, Any]] = []
            while not snapshot_task.done() and not self._stop.is_set():
                try:
                    raw = await asyncio.wait_for(websocket.recv(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                parsed = json.loads(raw)
                data = parsed.get("data", parsed)
                if data.get("e") in {"depthUpdate", "aggTrade", "trade"}:
                    if len(buffered) < 10_000:
                        buffered.append(data)
                    else:
                        self._dropped += 1
            snapshot = await snapshot_task
            self._apply_snapshot(snapshot, timestamp_ns=time.time_ns())
            for data in buffered:
                if data.get("e") == "depthUpdate":
                    self._handle_depth(data)
                elif data.get("e") in {"aggTrade", "trade"}:
                    self._handle_trade(data)
            self._flush(force=True)
            self._set_state("live")
            while not self._stop.is_set():
                try:
                    raw = await asyncio.wait_for(websocket.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    self._flush(force=True)
                    continue
                payload = json.loads(raw)
                data = payload.get("data", payload)
                event = data.get("e")
                if event == "depthUpdate":
                    self._handle_depth(data)
                elif event in {"aggTrade", "trade"}:
                    self._handle_trade(data)
                self._flush()

    def _fetch_snapshot(self) -> dict[str, Any]:
        url = BINANCE_FUTURES_DEPTH.format(
            symbol=self.config.symbol.upper(), limit=self.config.snapshot_limit)
        request = urllib.request.Request(url, headers={"User-Agent": "hft-sdk/1.0"})
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read())

    def _apply_snapshot(self, snapshot: dict[str, Any], timestamp_ns: int) -> None:
        self._book.clear()
        self._last_update_id = int(snapshot.get("lastUpdateId", 0))
        self._synced = False
        for side, key in ((SIDE_BID, "bids"), (SIDE_ASK, "asks")):
            for price, quantity in snapshot.get(key, []):
                self._apply_level(timestamp_ns, side, float(price), float(quantity))

    def _handle_depth(self, data: dict[str, Any]) -> None:
        first = int(data.get("U", 0))
        last = int(data.get("u", 0))
        if self._last_update_id is not None:
            if last <= self._last_update_id:
                return
            if not self._synced:
                # Binance aggregates update IDs in the diff stream, so after the
                # snapshot overlap check we do not treat skipped IDs as loss.
                if first > self._last_update_id + 1:
                    self._record_gap(1)
                self._synced = True
        self._last_update_id = last
        timestamp_ns = _timestamp_ns(data.get("E", data.get("T", 0)))
        for side, key in ((SIDE_BID, "b"), (SIDE_ASK, "a")):
            for price, quantity in data.get(key, []):
                self._apply_level(timestamp_ns, side, float(price), float(quantity))

    def _apply_level(self, timestamp_ns: int, side: int, price: float,
                     quantity: float) -> None:
        price_ticks = int(round(price * self.config.price_scale))
        quantity_ticks = int(round(quantity * self.config.quantity_scale))
        update = self._book.update(side, price_ticks, quantity_ticks)
        if update is None:
            return
        event_type, order_id, best_bid, best_ask = update
        self._append(timestamp_ns, event_type, side, order_id, price_ticks,
                     quantity_ticks, best_bid, best_ask)

    def _handle_trade(self, data: dict[str, Any]) -> None:
        timestamp_ns = _timestamp_ns(data.get("T", data.get("E", 0)))
        price_ticks = int(round(float(data["p"]) * self.config.price_scale))
        quantity_ticks = int(round(float(data["q"]) * self.config.quantity_scale))
        side = SIDE_SELL if bool(data.get("m", False)) else SIDE_BUY
        best_bid = -self._book.bids[0] if self._book.bids else 0
        best_ask = self._book.asks[0] if self._book.asks else 0
        self._append(timestamp_ns, EVENT_TRADE, side,
                     int(data.get("a", data.get("t", 0))),
                     price_ticks, quantity_ticks, best_bid, best_ask)


class BybitPerpFeed(LiveFeed):
    def __init__(self, config: LiveConfig) -> None:
        super().__init__(config)

    async def _stream(self) -> None:
        async with websockets.connect(BYBIT_LINEAR_WS, ping_interval=20,
                                      ping_timeout=20, max_queue=1024) as websocket:
            await websocket.send(json.dumps({
                "op": "subscribe",
                "args": [f"publicTrade.{self.config.symbol}",
                         f"orderbook.1.{self.config.symbol}"],
            }))
            self._set_state("live")
            while not self._stop.is_set():
                try:
                    raw = await asyncio.wait_for(websocket.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    self._flush(force=True)
                    continue
                payload = json.loads(raw)
                topic = payload.get("topic", "")
                if topic.startswith("publicTrade."):
                    self._handle_trade(payload.get("data", []))
                elif topic.startswith("orderbook.1."):
                    self._handle_orderbook(payload)
                self._flush()

    def _handle_orderbook(self, payload: dict[str, Any]) -> None:
        data = payload.get("data", {})
        timestamp_ns = _timestamp_ns(payload.get("ts", data.get("ts", 0)))
        sequence = int(data.get("u", data.get("seq", 0)))
        if payload.get("type") == "snapshot":
            self._book.clear()
            self._last_update_id = sequence
        elif self._last_update_id is not None and sequence > self._last_update_id + 1:
            self._record_gap(sequence - self._last_update_id - 1)
        if sequence:
            self._last_update_id = sequence
        for side, key in ((SIDE_BID, "b"), (SIDE_ASK, "a")):
            for price, quantity in data.get(key, []):
                price_ticks = int(round(float(price) * self.config.price_scale))
                quantity_ticks = int(round(float(quantity) *
                                           self.config.quantity_scale))
                update = self._book.update(side, price_ticks, quantity_ticks)
                if update is None:
                    continue
                event_type, order_id, best_bid, best_ask = update
                self._append(timestamp_ns, event_type, side, order_id,
                             price_ticks, quantity_ticks, best_bid, best_ask)

    def _handle_trade(self, data: list[dict[str, Any]]) -> None:
        for trade in data:
            timestamp_ns = _timestamp_ns(trade.get("T", 0))
            price_ticks = int(round(float(trade["p"]) * self.config.price_scale))
            quantity_ticks = int(round(float(trade["v"]) *
                                       self.config.quantity_scale))
            side = SIDE_BUY if trade.get("S") == "Buy" else SIDE_SELL
            best_bid = -self._book.bids[0] if self._book.bids else 0
            best_ask = self._book.asks[0] if self._book.asks else 0
            self._append(timestamp_ns, EVENT_TRADE, side, int(trade.get("i", 0)),
                         price_ticks, quantity_ticks, best_bid, best_ask)


def create_feed(config: LiveConfig) -> LiveFeed:
    if config.venue == "binance":
        return BinancePerpFeed(config)
    if config.venue == "bybit":
        return BybitPerpFeed(config)
    raise ValueError(f"unsupported venue: {config.venue}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="hft-sdk live",
        description="Stream crypto-perpetual depth/trades into the HFT engine.")
    parser.add_argument("--venue", choices=["binance", "bybit"], default="binance")
    parser.add_argument("--symbol", default="BTCUSDT")
    parser.add_argument("--scheme", default="MTF")
    parser.add_argument("--horizon", type=int, default=10)
    parser.add_argument("--price-scale", type=float, default=100.0)
    parser.add_argument("--qty-scale", type=float, default=1e8)
    parser.add_argument("--flush-ms", type=int, default=50)
    parser.add_argument("--health-interval", type=float, default=5.0)
    args = parser.parse_args(argv)

    config = LiveConfig(
        venue=args.venue, symbol=args.symbol, scheme=args.scheme,
        horizon=args.horizon, price_scale=args.price_scale,
        quantity_scale=args.qty_scale, flush_interval_ms=args.flush_ms)
    try:
        feed = create_feed(config)
    except ImportError as error:
        print(error)
        return 2
    feed.start()
    try:
        while True:
            print(json.dumps(feed.health()), flush=True)
            time.sleep(args.health_interval)
    except KeyboardInterrupt:
        pass
    finally:
        feed.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
