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
import hashlib
import math
from contextlib import asynccontextmanager
from decimal import Decimal, InvalidOperation
import json
import threading
import time
import urllib.request
from dataclasses import dataclass, replace
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

BINANCE_FUTURES_WS = "wss://fstream.binance.com/public/stream?streams={symbol}@depth@100ms"
BINANCE_FUTURES_TRADES_WS = "wss://fstream.binance.com/market/stream?streams={symbol}@aggTrade"
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
    stall_ms: int = 5000

    def __post_init__(self):
        self.symbol = self.symbol.upper()
        self.scheme = self.scheme.upper()
        if self.venue not in {"binance", "bybit"} or not self.symbol.isalnum():
            raise ValueError("unsupported venue or invalid symbol")
        if self.scheme not in {"UF", "LF", "MF", "MTF"} or self.horizon not in {1, 10, 30, 50}:
            raise ValueError("invalid scheme or horizon")
        if any(not math.isfinite(v) or v <= 0 for v in
               (self.price_scale, self.quantity_scale, self.reconnect_initial_s, self.reconnect_max_s)):
            raise ValueError("scales and reconnect intervals must be finite and positive")
        if (self.flush_interval_ms <= 0 or self.max_batch <= 0 or self.stall_ms <= 0 or
                not 1 <= self.snapshot_limit <= 1000 or self.reconnect_max_s < self.reconnect_initial_s):
            raise ValueError("invalid batching, snapshot, or reconnect configuration")


class FeedSyncError(RuntimeError):
    """The depth book must be replaced before more events can be processed."""


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
        self.next_id = 1

    def _clean(self) -> None:
        while self.bids and (SIDE_BID, -self.bids[0]) not in self.ids:
            heapq.heappop(self.bids)
        while self.asks and (SIDE_ASK, self.asks[0]) not in self.ids:
            heapq.heappop(self.asks)

    def update(self, side: int, price_ticks: int,
               quantity: int) -> tuple[int, int, int, int] | None:
        if side not in {SIDE_BID, SIDE_ASK} or price_ticks <= 0 or quantity < 0:
            raise ValueError("invalid book level")
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
        # Lazy-deletion heaps must not grow forever when non-best levels churn.
        if len(self.bids) + len(self.asks) > 2 * len(self.ids) + 1024:
            self.bids = [-price for side, price in self.ids if side == SIDE_BID]
            self.asks = [price for side, price in self.ids if side == SIDE_ASK]
            heapq.heapify(self.bids)
            heapq.heapify(self.asks)
        best_bid = -self.bids[0] if self.bids else 0
        best_ask = self.asks[0] if self.asks else 0
        return event_type, order_id, best_bid, best_ask


def _timestamp_ns(value: Any) -> int:
    """Both supported exchange protocols specify milliseconds explicitly."""
    result = int(value) * 1_000_000
    if result < 0 or result > np.iinfo(np.int64).max:
        raise ValueError("exchange timestamp is out of range")
    return result


def _scaled_int(value: Any, scale: float) -> int:
    try:
        number = Decimal(str(value)) * Decimal(str(scale))
        if not number.is_finite() or number < 0:
            raise ValueError("price/size must be finite and nonnegative")
        result = int(number.to_integral_value())
        if result > np.iinfo(np.int64).max:
            raise ValueError("scaled price/size is out of range")
        return result
    except InvalidOperation as error:
        raise ValueError("invalid price/size") from error


def _trade_id(value: Any) -> int:
    text = str(value)
    if text.isdecimal() and int(text) <= np.iinfo(np.int64).max:
        return int(text)
    # The SDK has integer IDs. UUID/string IDs become deterministic 63-bit
    # surrogates, never parent-order links; raw recording must retain originals.
    return int.from_bytes(hashlib.blake2b(text.encode(), digest_size=8).digest(), "big") & ((1 << 63) - 1)


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
        self._last_receipt = 0.0
        self._state = "starting"
        self._last_error = ""
        self._processing_error = False
        self._lock = threading.RLock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._last_flush = time.monotonic()
        self._last_update_id: int | None = None
        self._synced = False
        self._pending_snapshot: dict[str, Any] | None = None
        self._main_task: asyncio.Task | None = None

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="hft-live-feed",
                                        daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._loop is not None:
            try:
                self._loop.call_soon_threadsafe(self._cancel_main)
            except RuntimeError:
                pass  # The worker may have just closed its loop.
        if self._thread is not None:
            self._thread.join(timeout=10.0)
            if self._thread.is_alive():
                raise RuntimeError("live feed did not stop within 10 seconds")
            self._thread = None
        self._set_state("stopped")

    def _cancel_main(self) -> None:
        if self._main_task is not None:
            self._main_task.cancel()

    def _run(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._main_task = self._loop.create_task(self._main())
            self._loop.run_until_complete(self._main_task)
        except asyncio.CancelledError:
            pass
        finally:
            self._flush(force=True)
            self._loop.close()
            self._loop = None
            self._main_task = None

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

    def _reset_epoch(self) -> None:
        self._set_state("syncing")
        with self._lock:
            previous = self.engine.stats()
            self._dropped += (len(self._arrays["order_id"]) + previous.invalid_events +
                              previous.out_of_order_events + previous.tracker_dropped)
            for values in self._arrays.values():
                values.clear()
            self.engine = OBIFilterEngine(self.config.engine if self.config.engine is not None else EngineConfig())
            self._processing_error = False
            self._last_receipt = 0.0
        self._book.clear()
        self._synced = False
        self._last_update_id = None

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
        self._last_receipt = time.monotonic()
        if len(arrays["order_id"]) >= self.config.max_batch:
            self._flush(force=True)

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
                self._processing_error = True
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
            age = time.monotonic() - self._last_receipt if self._last_receipt else -1.0
            stats = self.engine.stats()
            state = self._state
            if state == "live" and age > self.config.stall_ms / 1000:
                state = "stalled"
            engine_drops = stats.invalid_events + stats.out_of_order_events + stats.tracker_dropped
            if engine_drops and state == "live":
                state = "degraded"
            snapshots = stats.snapshots
            return {
                "venue": self.config.venue,
                "symbol": self.config.symbol,
                "scheme": self.config.scheme,
                "horizon": self.config.horizon,
                "state": state,
                "messages": self._messages,
                "trades": self._trades,
                "gaps": self._gaps,
                "reconnects": self._reconnects,
                "dropped": self._dropped + engine_drops,
                "evicted_snapshots": stats.evicted_snapshots,
                "last_message_age_s": age,
                "snapshots": snapshots,
                "last_error": self._last_error,
            }

    def metrics(self) -> dict[str, Any]:
        with self._lock:
            table = self.engine.metrics_table()
            selected = (np.asarray(table["scheme"], dtype=str) == self.config.scheme) & (
                np.asarray(table["horizon_s"]) == self.config.horizon)
            return {name: np.asarray(values)[selected] for name, values in table.items()}

    def snapshot(self):
        with self._lock:
            return self.engine.latest_snapshot()


class BinancePerpFeed(LiveFeed):
    def __init__(self, config: LiveConfig) -> None:
        super().__init__(replace(config, venue="binance"))

    @asynccontextmanager
    async def _connection(self):
        symbol = self.config.symbol.lower()
        queue: asyncio.Queue = asyncio.Queue(maxsize=10000)

        async def pump(socket):
            try:
                while True:
                    await queue.put(await socket.recv())
            except Exception as error:
                await queue.put(error)

        class MergedSocket:
            async def recv(self):
                item = await queue.get()
                if isinstance(item, Exception):
                    raise item
                return item

        async with websockets.connect(BINANCE_FUTURES_WS.format(symbol=symbol),
                                      ping_interval=20, ping_timeout=20, max_queue=1024) as depth, \
                   websockets.connect(BINANCE_FUTURES_TRADES_WS.format(symbol=symbol),
                                      ping_interval=20, ping_timeout=20, max_queue=1024) as trades:
            tasks = [asyncio.create_task(pump(depth)), asyncio.create_task(pump(trades))]
            try:
                yield MergedSocket()
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)

    async def _stream(self) -> None:
        async with self._connection() as websocket:
            self._set_state("syncing")
            snapshot_task = asyncio.create_task(
                asyncio.to_thread(self._fetch_snapshot))
            buffered: list[dict[str, Any]] = []
            try:
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
                            self._dropped += len(buffered) + 1
                            self._record_gap(1)
                            raise FeedSyncError("Binance snapshot buffer overflow")
                snapshot = await snapshot_task
            finally:
                if not snapshot_task.done():
                    snapshot_task.cancel()
                await asyncio.gather(snapshot_task, return_exceptions=True)
            self._apply_snapshot(snapshot, timestamp_ns=time.time_ns())
            for data in buffered:
                if data.get("e") == "depthUpdate":
                    self._handle_depth(data)
                elif data.get("e") in {"aggTrade", "trade"}:
                    if self._synced:
                        self._handle_trade(data)
                    else:
                        self._dropped += 1
            self._flush(force=True)
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
                    if self._synced:
                        self._handle_trade(data)
                    else:
                        self._dropped += 1
                self._flush()

    def _fetch_snapshot(self) -> dict[str, Any]:
        url = BINANCE_FUTURES_DEPTH.format(
            symbol=self.config.symbol.upper(), limit=self.config.snapshot_limit)
        request = urllib.request.Request(url, headers={"User-Agent": "hft-sdk/1.0"})
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read())

    def _apply_snapshot(self, snapshot: dict[str, Any], timestamp_ns: int) -> None:
        self._reset_epoch()
        self._last_update_id = int(snapshot["lastUpdateId"])
        self._synced = False
        self._pending_snapshot = snapshot
        self._set_state("syncing")
        # Emit snapshot levels at the first bridging diff's exchange timestamp,
        # rather than wall time (which can put buffered events in the past).

    def _sync_error(self, message: str) -> None:
        self._record_gap(1)
        self._synced = False
        self._pending_snapshot = None
        self._dropped += len(self._arrays["order_id"])
        for values in self._arrays.values():
            values.clear()
        raise FeedSyncError(message)

    def _handle_depth(self, data: dict[str, Any]) -> None:
        first, last = int(data["U"]), int(data["u"])
        previous = self._last_update_id
        if previous is None:
            self._sync_error("Binance depth received without a snapshot")
        timestamp_ns = _timestamp_ns(data.get("E", data.get("T", 0)))
        if not self._synced:
            if last < previous:
                return  # Buffered update predates the snapshot.
            if not first <= previous <= last:
                self._sync_error("Binance diff does not bridge the snapshot")
            snapshot = self._pending_snapshot
            if snapshot is None:
                self._sync_error("Binance snapshot is unavailable")
            for side, key in ((SIDE_BID, "bids"), (SIDE_ASK, "asks")):
                for price, quantity in snapshot.get(key, []):
                    self._apply_level(timestamp_ns, side, price, quantity)
            self._pending_snapshot = None
            self._synced = True
        else:
            if last <= previous:
                return  # Duplicate or stale message; never rewind the book.
            if "pu" not in data or int(data["pu"]) != previous:
                self._sync_error("Binance depth sequence discontinuity")
        self._last_update_id = last
        for side, key in ((SIDE_BID, "b"), (SIDE_ASK, "a")):
            for price, quantity in data.get(key, []):
                self._apply_level(timestamp_ns, side, price, quantity)
        self._last_receipt = time.monotonic()
        self._set_state("degraded" if self._processing_error else "live")

    def _apply_level(self, timestamp_ns: int, side: int, price: float,
                     quantity: float) -> None:
        price_ticks = _scaled_int(price, self.config.price_scale)
        quantity_ticks = _scaled_int(quantity, self.config.quantity_scale)
        update = self._book.update(side, price_ticks, quantity_ticks)
        if update is None:
            return
        event_type, order_id, best_bid, best_ask = update
        self._append(timestamp_ns, event_type, side, order_id, price_ticks,
                     quantity_ticks, best_bid, best_ask)

    def _handle_trade(self, data: dict[str, Any]) -> None:
        timestamp_ns = _timestamp_ns(data.get("T", data.get("E", 0)))
        price_ticks = _scaled_int(data["p"], self.config.price_scale)
        quantity_ticks = _scaled_int(data["q"], self.config.quantity_scale)
        side = SIDE_SELL if bool(data.get("m", False)) else SIDE_BUY
        best_bid = -self._book.bids[0] if self._book.bids else 0
        best_ask = self._book.asks[0] if self._book.asks else 0
        self._append(timestamp_ns, EVENT_TRADE, side,
                     _trade_id(data.get("a", data.get("t", ""))),
                     price_ticks, quantity_ticks, best_bid, best_ask)


class BybitPerpFeed(LiveFeed):
    def __init__(self, config: LiveConfig) -> None:
        super().__init__(replace(config, venue="bybit"))

    async def _stream(self) -> None:
        async with websockets.connect(BYBIT_LINEAR_WS, ping_interval=20,
                                      ping_timeout=20, max_queue=1024) as websocket:
            await websocket.send(json.dumps({
                "op": "subscribe",
                "args": [f"publicTrade.{self.config.symbol}",
                         f"orderbook.50.{self.config.symbol}"],
            }))
            self._reset_epoch()
            self._set_state("syncing")
            while not self._stop.is_set():
                try:
                    raw = await asyncio.wait_for(websocket.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    self._flush(force=True)
                    continue
                payload = json.loads(raw)
                if payload.get("success") is False:
                    raise FeedSyncError("Bybit subscription rejected: " + str(payload.get("ret_msg", "")))
                topic = payload.get("topic", "")
                if topic.startswith("publicTrade.") and self._synced:
                    self._handle_trade(payload.get("data", []))
                elif topic.startswith("orderbook.50."):
                    self._handle_orderbook(payload)
                self._flush()

    def _handle_orderbook(self, payload: dict[str, Any]) -> None:
        data = payload.get("data", {})
        timestamp_ns = _timestamp_ns(payload.get("ts", data.get("ts", 0)))
        sequence = int(data.get("u", data.get("seq", 0)))
        if payload.get("type") == "snapshot":
            self._reset_epoch()
            self._synced = True
        elif not self._synced or sequence == 1:
            self._record_gap(1)
            raise FeedSyncError("Bybit delta requires a fresh snapshot")
        elif self._last_update_id is not None and sequence <= self._last_update_id:
            return
        # Bybit does not specify contiguous update IDs; jumps alone are not
        # evidence of missing messages. New snapshots replace the whole epoch.
        self._last_update_id = sequence
        for side, key in ((SIDE_BID, "b"), (SIDE_ASK, "a")):
            for price, quantity in data.get(key, []):
                price_ticks = _scaled_int(price, self.config.price_scale)
                quantity_ticks = _scaled_int(quantity, self.config.quantity_scale)
                update = self._book.update(side, price_ticks, quantity_ticks)
                if update is None:
                    continue
                event_type, order_id, best_bid, best_ask = update
                self._append(timestamp_ns, event_type, side, order_id,
                             price_ticks, quantity_ticks, best_bid, best_ask)

        self._last_receipt = time.monotonic()
        self._set_state("degraded" if self._processing_error else "live")

    def _handle_trade(self, data: list[dict[str, Any]]) -> None:
        for trade in data:
            timestamp_ns = _timestamp_ns(trade.get("T", 0))
            price_ticks = _scaled_int(trade["p"], self.config.price_scale)
            quantity_ticks = _scaled_int(trade["v"], self.config.quantity_scale)
            side = SIDE_BUY if trade.get("S") == "Buy" else SIDE_SELL
            best_bid = -self._book.bids[0] if self._book.bids else 0
            best_ask = self._book.asks[0] if self._book.asks else 0
            self._append(timestamp_ns, EVENT_TRADE, side, _trade_id(trade.get("i", "")),
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

