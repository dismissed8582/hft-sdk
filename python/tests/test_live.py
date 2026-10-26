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
        with patch("hft_sdk.live.websockets.connect", side_effect=[Socket(x) for x in sessions]), \
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


if __name__ == "__main__":
    unittest.main()

