from __future__ import annotations

import unittest
import asyncio
import json
from unittest.mock import patch

from hft_sdk.live import (
    BinancePerpFeed,
    FeedSyncError,
    BybitPerpFeed,
    LiveBook,
    LiveConfig,
    SIDE_ASK,
    SIDE_BID,
    SIDE_BUY,
    EVENT_NEW,
    EVENT_MODIFY,
    EVENT_CANCEL,
    EVENT_TRADE,
    _scaled_int,
    _timestamp_ns,
)


class LiveBookTests(unittest.TestCase):
    def test_level_lifecycle(self) -> None:
        book = LiveBook()
        created = book.update(SIDE_BID, 100, 5)
        self.assertIsNotNone(created)
        self.assertEqual(created[0], EVENT_NEW)
        updated = book.update(SIDE_BID, 100, 7)
        self.assertIsNotNone(updated)
        self.assertEqual(updated[0], EVENT_MODIFY)
        self.assertEqual(updated[2], 100)
        removed = book.update(SIDE_BID, 100, 0)
        self.assertIsNotNone(removed)
        self.assertEqual(removed[0], EVENT_CANCEL)
        self.assertIsNone(book.update(SIDE_BID, 100, 0))


class BinanceAdapterTests(unittest.TestCase):
    def make_feed(self):
        feed = BinancePerpFeed(LiveConfig())
        feed._apply_snapshot({"lastUpdateId": 100, "bids": [["100", "2"]],
                              "asks": [["101", "3"]]}, timestamp_ns=9999999999999999999)
        return feed

    def test_snapshot_bridge_and_stale_updates(self):
        feed = self.make_feed()
        feed._handle_depth({"E": 1700000000000, "U": 90, "u": 99, "b": [], "a": []})
        self.assertEqual(feed.health()["state"], "syncing")
        self.assertEqual(len(feed._arrays["order_id"]), 0)
        feed._handle_depth({"E": 1700000000100, "U": 98, "u": 105, "b": [], "a": []})
        self.assertEqual(feed.health()["state"], "live")
        self.assertEqual(feed._arrays["type_codes"], [EVENT_NEW, EVENT_NEW])
        self.assertEqual(feed._arrays["bid_price_ticks"][0], 10000)
        self.assertEqual(feed._arrays["quantity"][0], 200000000)
        self.assertEqual(feed._arrays["timestamp_ns"], [1700000000100000000] * 2)
        feed._handle_depth({"E": 1700000000100, "U": 98, "u": 105, "b": [], "a": []})
        self.assertEqual(len(feed._arrays["order_id"]), 2)

    def test_invalid_bridge_requires_resnapshot(self):
        feed = self.make_feed()
        with self.assertRaises(FeedSyncError):
            feed._handle_depth({"E": 1700000000100, "U": 102, "u": 105, "b": [], "a": []})
        self.assertEqual(feed.health()["gaps"], 1)
        self.assertEqual(feed.health()["state"], "degraded")
        self.assertEqual(len(feed._arrays["order_id"]), 0)

    def test_continuity_and_recovery_reset_engine(self):
        feed = self.make_feed()
        original_engine = feed.engine
        feed._handle_depth({"E": 1700000000100, "U": 100, "u": 105, "b": [], "a": []})
        feed._handle_depth({"E": 1700000000200, "U": 110, "u": 115, "pu": 105,
                            "b": [["100", "4"]], "a": []})
        self.assertEqual(feed._last_update_id, 115)
        with self.assertRaises(FeedSyncError):
            feed._handle_depth({"E": 1700000000300, "U": 130, "u": 140, "pu": 129,
                                "b": [["100", "8"]], "a": []})
        self.assertEqual(feed._last_update_id, 115)
        self.assertEqual(feed._arrays["order_id"], [])
        feed._apply_snapshot({"lastUpdateId": 140, "bids": [["99", "1"]], "asks": []}, 0)
        self.assertIsNot(feed.engine, original_engine)
        feed._handle_depth({"E": 1700000000400, "U": 139, "u": 145, "b": [], "a": []})
        self.assertEqual(feed.health()["state"], "live")
        self.assertEqual(feed.health()["gaps"], 1)
        self.assertNotIn((SIDE_BID, 10000), feed._book.ids)

    def test_missing_continuity_id_requires_resnapshot(self):
        feed = self.make_feed()
        feed._handle_depth({"E": 1700000000100, "U": 100, "u": 105, "b": [], "a": []})
        with self.assertRaises(FeedSyncError):
            feed._handle_depth({"E": 1700000000200, "U": 106, "u": 110, "b": [], "a": []})

    def test_trade(self) -> None:
        feed = BinancePerpFeed(LiveConfig())
        feed._handle_trade({
            "T": 2000,
            "p": "100.5",
            "q": "0.5",
            "m": False,
            "a": 42,
        })
        self.assertEqual(len(feed._arrays["order_id"]), 1)
        self.assertEqual(feed._arrays["type_codes"][0], EVENT_TRADE)
        self.assertEqual(feed._arrays["side_codes"][0], SIDE_BUY)
        self.assertEqual(feed._arrays["order_id"][0], 42)


class BinanceRecoveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_reconnect_resnapshots_after_a_gap(self):
        feed = BinancePerpFeed(LiveConfig(reconnect_initial_s=0.001))
        sessions = [
            [{"e": "depthUpdate", "E": 1700000000100, "U": 100, "u": 105, "b": [], "a": []},
             {"e": "depthUpdate", "E": 1700000000200, "U": 120, "u": 125, "pu": 119, "b": [], "a": []}],
            [{"e": "depthUpdate", "E": 1700000000300, "U": 200, "u": 205, "b": [], "a": []}],
        ]

        class Socket:
            def __init__(self, messages):
                self.messages = iter(messages)

            async def __aenter__(self):
                return self

            async def __aexit__(self, *args):
                return False

            async def recv(self):
                await asyncio.sleep(0.01)
                message = next(self.messages, None)
                if message is None:
                    feed._stop.set()
                    return "{}"
                return json.dumps(message)

        snapshots = [
            {"lastUpdateId": 100, "bids": [["100", "1"]], "asks": []},
            {"lastUpdateId": 200, "bids": [["99", "2"]], "asks": []},
        ]
        with patch.object(feed, "_connection", side_effect=[Socket(x) for x in sessions]), \
             patch.object(feed, "_fetch_snapshot", side_effect=snapshots) as fetch:
            await asyncio.wait_for(feed._main(), timeout=5)
        self.assertEqual(fetch.call_count, 2)
        self.assertEqual(feed.health()["reconnects"], 1)
        self.assertEqual(feed.health()["gaps"], 1)
        self.assertEqual(feed._last_update_id, 205)
        self.assertNotIn((SIDE_BID, 10000), feed._book.ids)
        self.assertIn((SIDE_BID, 9900), feed._book.ids)

    async def test_stop_interrupts_reconnect_backoff(self):
        import time
        feed = BinancePerpFeed(LiveConfig(reconnect_initial_s=30))

        async def unavailable():
            raise ConnectionError("offline fixture")

        with patch.object(feed, "_stream", side_effect=unavailable):
            feed.start()
            try:
                deadline = time.monotonic() + 3
                while not feed.health()["reconnects"] and time.monotonic() < deadline:
                    await asyncio.sleep(0.01)
                self.assertEqual(feed.health()["reconnects"], 1)
                start = time.monotonic()
                feed.stop()
                self.assertLess(time.monotonic() - start, 2)
                self.assertEqual(feed.health()["state"], "stopped")
            finally:
                if feed._thread is not None:
                    feed.stop()


class BybitAdapterTests(unittest.TestCase):
    def test_orderbook_snapshot_and_delta(self) -> None:
        feed = BybitPerpFeed(LiveConfig())
        feed._handle_orderbook({
            "topic": "orderbook.1.BTCUSDT",
            "type": "snapshot",
            "ts": 1000,
            "data": {"u": 10, "b": [["100.0", "1.0"]],
                     "a": [["101.0", "1.0"]]},
        })
        self.assertEqual(len(feed._arrays["order_id"]), 2)
        feed._handle_orderbook({
            "topic": "orderbook.1.BTCUSDT",
            "type": "delta",
            "ts": 2000,
            "data": {"u": 11, "b": [["100.0", "2.0"]], "a": []},
        })
        self.assertEqual(feed._arrays["type_codes"][-1], EVENT_MODIFY)

    def test_trade(self) -> None:
        feed = BybitPerpFeed(LiveConfig())
        feed._handle_trade([{
            "T": 1000, "p": "100.0", "v": "2.0", "S": "Sell", "i": "7",
        }])
        self.assertEqual(feed._arrays["type_codes"][0], EVENT_TRADE)
        self.assertEqual(feed._arrays["side_codes"][0], 4)
        self.assertEqual(feed._arrays["order_id"][0], 7)






class AdapterEdgeTests(unittest.TestCase):
    def test_uuid_trade_and_exact_decimal_scaling(self):
        feed = BybitPerpFeed(LiveConfig())
        trade = {"T": 1700000000000, "p": "0.00000029", "v": "0.00000003",
                 "S": "Sell", "i": "20f43950-d8dd-5b31-9112-a178eb6023af"}
        feed._handle_trade([trade])
        self.assertGreaterEqual(feed._arrays['order_id'][0], 0)
        self.assertEqual(feed.health()['venue'], 'bybit')
        self.assertEqual(_scaled_int('0.00000029', 1e8), 29)
        self.assertEqual(_timestamp_ns(1000), 1_000_000_000)
        for value in ('NaN', 'Infinity', '-1', '1e100'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                _scaled_int(value, 100)

    def test_bybit_sequence_jumps_are_not_assumed_loss(self):
        feed = BybitPerpFeed(LiveConfig())
        feed._handle_orderbook({'type': 'snapshot', 'ts': 1700000000000,
                                'data': {'u': 10, 'b': [['100', '1']], 'a': []}})
        feed._handle_orderbook({'type': 'delta', 'ts': 1700000000100,
                                'data': {'u': 20, 'b': [['100', '2']], 'a': []}})
        self.assertEqual(feed.health()['gaps'], 0)
        self.assertEqual(feed._arrays['type_codes'][-1], EVENT_MODIFY)
        feed._handle_orderbook({'type': 'delta', 'ts': 1700000000050,
                                'data': {'u': 19, 'b': [['100', '9']], 'a': []}})
        self.assertEqual(feed._book.quantities[(SIDE_BID, 10000)], 200000000)
        previous = feed.engine
        feed._handle_orderbook({'type': 'snapshot', 'ts': 1700000000200,
                                'data': {'u': 1, 'b': [['99', '1']], 'a': []}})
        self.assertIsNot(feed.engine, previous)
        self.assertNotIn((SIDE_BID, 10000), feed._book.ids)

    def test_batch_limit_forces_flush(self):
        feed = BinancePerpFeed(LiveConfig(max_batch=2, flush_interval_ms=100000))
        feed._append(1_000_000_000, EVENT_NEW, SIDE_BID, 1, 100, 1, 100, 101)
        feed._append(1_250_000_000, EVENT_NEW, SIDE_ASK, 2, 101, 1, 100, 101)
        self.assertEqual(feed._arrays['order_id'], [])
        self.assertEqual(feed.engine.stats().processed_events, 2)

    def test_stale_health_uses_receipt_time(self):
        import time
        feed = BinancePerpFeed(LiveConfig(stall_ms=10))
        feed._set_state('live')
        feed._last_receipt = time.monotonic() - 1
        self.assertEqual(feed.health()['state'], 'stalled')

    def test_heap_churn_is_bounded(self):
        book = LiveBook()
        book.update(SIDE_BID, 100, 1)
        for _ in range(3000):
            book.update(SIDE_BID, 99, 1)
            book.update(SIDE_BID, 99, 0)
        self.assertLess(len(book.bids), 1100)


class BinanceConnectionTests(unittest.IsolatedAsyncioTestCase):
    async def test_public_and_market_routes_and_task_cleanup(self):
        feed = BinancePerpFeed(LiveConfig())

        class Socket:
            def __init__(self, payload):
                self.payload = payload
                self.received = False
                self.closed = False
            async def __aenter__(self):
                return self
            async def __aexit__(self, *args):
                self.closed = True
            async def recv(self):
                if not self.received:
                    self.received = True
                    return self.payload
                await asyncio.Future()

        depth, trades = Socket('depth'), Socket('trades')
        with patch('hft_sdk.live.websockets.connect', side_effect=[depth, trades]) as connect:
            async with feed._connection() as merged:
                received = {await asyncio.wait_for(merged.recv(), 1),
                            await asyncio.wait_for(merged.recv(), 1)}
                self.assertEqual(received, {'depth', 'trades'})
            self.assertIn('/public/', connect.call_args_list[0].args[0])
            self.assertIn('/market/', connect.call_args_list[1].args[0])
        self.assertTrue(depth.closed and trades.closed)

if __name__ == "__main__":
    unittest.main()
