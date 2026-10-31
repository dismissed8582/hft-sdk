import csv
import subprocess
import tempfile
import unittest
from pathlib import Path


class PaperEvalTimingTests(unittest.TestCase):
    def test_future_event_is_not_visible_in_earlier_sample(self) -> None:
        """A 1.00s event must not appear in the 0.25/0.50/0.75s OBI samples."""
        binary = Path("build/hft_paper_eval")
        self.assertTrue(binary.exists(), f"missing test binary: {binary}")

        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            ticks = directory / "ticks.csv"
            output = directory / "paper_ticks.csv"
            ticks.write_text(
                "timestamp_ns,event_type,side,order_id,price_ticks,quantity\n"
                "0,NEW,BID,1,100,1\n"
                "1000000000,NEW,ASK,2,101,1\n"
            )

            subprocess.run(
                [
                    str(binary),
                    "--ticks", str(ticks),
                    "--out", str(output),
                    "--sample-ms", "250",
                    "--anchor-ms", "15000",
                    "--obi-window-ms", "10000",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            with output.open(newline="") as handle:
                rows = list(csv.DictReader(handle))

        uf = {
            int(row["timestamp_ns"]): row
            for row in rows
            if row["scheme"] == "UF"
        }

        # Before the 1.00s ask event arrives, the rolling book contains only
        # the bid at t=0: (ask - bid) / (ask + bid) = -1.
        for timestamp in (250_000_000, 500_000_000, 750_000_000):
            self.assertIn(timestamp, uf)
            self.assertEqual(uf[timestamp]["lob_valid"], "1")
            self.assertAlmostEqual(float(uf[timestamp]["lob_obi"]), -1.0)

        # At exactly 1.00s the endpoint is inclusive, so the ask event is now
        # visible and the book-event counts are balanced.
        self.assertIn(1_000_000_000, uf)
        self.assertAlmostEqual(float(uf[1_000_000_000]["lob_obi"]), 0.0)


if __name__ == "__main__":
    unittest.main()
