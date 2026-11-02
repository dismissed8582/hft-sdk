import unittest
import numpy as np
from benchmarks.pairs_trading import backtest, next_position


class PositionRuleTests(unittest.TestCase):
    def test_boundaries_and_overshoots(self):
        cases = [
            (0, -2, 1), (0, 2, -1), (0, 0, 0),
            (1, -0.6, 1), (1, -0.5, 0), (1, 3, 0),
            (-1, 0.6, -1), (-1, 0.5, 0), (-1, -3, 0),
            (1, float("nan"), 1), (-1, float("inf"), -1),
        ]
        for previous, z, expected in cases:
            with self.subTest(previous=previous, z=z):
                self.assertEqual(next_position(previous, z, 2, 0.5), expected)

    def test_reversal_requires_another_observation(self):
        closed = next_position(1, 3, 2, 0.5)
        self.assertEqual(closed, 0)
        self.assertEqual(next_position(closed, 3, 2, 0.5), -1)

    def test_backtest_validates_edge_inputs_and_annualization(self):
        x = np.linspace(100.0, 110.0, 20)
        y = 2.0 * x + np.sin(np.arange(20))
        _, result = backtest(x, y, 5, 1.5, 0.5, 1.0)
        self.assertTrue(np.isnan(result["summary"]["sharpe_annualized"]))
        self.assertEqual(result["summary"]["periods_per_year"], None)
        with self.assertRaises(ValueError):
            backtest(np.array([1.0, -1.0]), np.array([2.0, 3.0]), 2, 2, 0.5, 1)
        with self.assertRaises(ValueError):
            backtest(x, y, 5, 1.0, 1.5, 1)
        with self.assertRaises(ValueError):
            backtest(x, y, 5, 2, 0.5, 1, 0)
