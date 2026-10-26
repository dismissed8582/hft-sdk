"""Black-box runtime checks; no live exchange or third-party Python packages."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request

FEED = Path(os.environ.get("HFT_FEED_EXECUTABLE", "build/hft_feed")).resolve()


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class FeedRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.port = free_port()
        self.process = subprocess.Popen(
            [str(FEED), "--stdin", "--http-port", str(self.port), "--stall-ms", "1000"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            text=True,
        )
        self.wait_health(lambda data: data["status"] == "starting")

    def tearDown(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()

    def request(self, path: str) -> str:
        with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/{path}", timeout=3) as response:
            return response.read().decode()

    def wait_health(self, predicate, timeout: float = 5) -> dict:
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail(f"feed exited with {self.process.returncode}")
            try:
                last = json.loads(self.request("health"))
                if predicate(last):
                    return last
            except OSError:
                pass
            time.sleep(0.02)
        self.fail(f"health condition not reached; last response: {last}")

    def send(self, text: str) -> None:
        self.process.stdin.write(text)
        self.process.stdin.flush()

    def test_short_batch_health_and_gap(self) -> None:
        self.send("1000000000,NEW,BID,1,100,1,100,101,,1\n")
        self.wait_health(lambda h: h["events"] == 1 and h["status"] == "live")
        self.assertIn("hft_feed_state 1\n", self.request("metrics"))
        self.wait_health(lambda h: h["status"] == "stalled")
        self.assertIn("hft_feed_state 3\n", self.request("metrics"))
        self.send("1250000000,NEW,ASK,2,101,1,100,101,,3\n")
        health = self.wait_health(lambda h: h["events"] == 2 and h["status"] == "degraded")
        self.assertEqual(health["gaps"], 1)
        self.assertIn("hft_feed_state 2\n", self.request("metrics"))
        self.send("1500000000,NEW,BID,3,100,1,100,101,,4\n")
        self.wait_health(lambda h: h["events"] == 3 and h["status"] == "degraded")

    def test_invalid_optional_field_is_rejected(self) -> None:
        self.send("1000000000,NEW,BID,1,100,1,100,101,bad-parent,1\n")
        self.wait_health(lambda h: h["status"] == "degraded")
        self.assertIn("hft_parse_errors_total 1\n", self.request("metrics"))
        self.assertEqual(json.loads(self.request("health"))["events"], 0)

    def test_partial_line_and_idle_client_do_not_block_shutdown(self) -> None:
        self.send("1000000000,NEW")
        with socket.create_connection(("127.0.0.1", self.port)) as client:
            client.sendall(b"GET /health HTTP/1.1\r\n")
            time.sleep(0.1)
            self.process.send_signal(signal.SIGTERM)
            self.assertEqual(self.process.wait(timeout=2), 0)

    def test_fragmented_http_request(self) -> None:
        with socket.create_connection(("127.0.0.1", self.port)) as client:
            client.settimeout(3)
            client.sendall(b"GET /health HTTP/1.1\r\n")
            time.sleep(0.05)
            client.sendall(b"Host: localhost\r\n\r\n")
            response = b""
            while chunk := client.recv(4096):
                response += chunk
        self.assertIn(b"HTTP/1.1 200", response)
        self.assertIn(b'"status":"starting"', response)

    def test_idle_http_client_expires(self) -> None:
        with socket.create_connection(("127.0.0.1", self.port)) as client:
            client.settimeout(3)
            self.assertEqual(client.recv(1), b"")
        self.assertEqual(json.loads(self.request("health"))["status"], "starting")

    def test_replay_pacing_is_interruptible(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            ticks = Path(directory) / "ticks.csv"
            ticks.write_text(
                "timestamp_ns,event_type,side,order_id,price_ticks,quantity,bid_price_ticks,ask_price_ticks\n"
                "0,NEW,BID,1,100,1,100,101\n"
                "60000000000,NEW,ASK,2,101,1,100,101\n")
            replay = subprocess.Popen([str(FEED), "--replay", str(ticks), "--replay-speed", "1",
                                       "--http-port", str(free_port())],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(0.3)
                self.assertIsNone(replay.poll())
                replay.terminate()
                self.assertEqual(replay.wait(timeout=2), 0)
            finally:
                if replay.poll() is None:
                    replay.kill()
                    replay.wait()

    def test_invalid_options_fail(self) -> None:
        for args in (("--http-port", "65536"), ("--sample-ms", "0"),
                     ("--stall-ms", "-1"), ("--replay-speed", "nan"), ("--max-mods", "-1")):
            with self.subTest(args=args):
                result = subprocess.run([str(FEED), "--stdin", *args], input="", text=True,
                                        capture_output=True, timeout=3)
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--feed", type=Path)
    args, remaining = parser.parse_known_args()
    if args.feed:
        FEED = args.feed.resolve()
    unittest.main(argv=[__file__, *remaining])
