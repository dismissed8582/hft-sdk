from __future__ import annotations

import unittest

from hft_sdk.live import (
    BinancePerpFeed,
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
    def test_depth_gap_and_events(self) -> None:
        feed = BinancePerpFeed(LiveConfig())
        feed._last_update_id = 100
        feed._handle_depth({
            "E": 1000,
            "U": 102,
            "u": 105,
            "b": [["100.0", "2.0"]],
            "a": [["101.0", "3.0"]],
        })
        health = feed.health()
        self.assertEqual(health["gaps"], 1)
        self.assertEqual(len(feed._arrays["order_id"]), 2)
        self.assertEqual(feed._arrays["type_codes"], [EVENT_NEW, EVENT_NEW])
        self.assertEqual(feed._arrays["bid_price_ticks"][0], 10000)
        self.assertEqual(feed._arrays["quantity"][0], 200000000)

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
