import unittest
import numpy as np
from unittest.mock import patch
from benchmarks.pairs_trading import next_position, backtest, synthetic_series


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


class AccountingTests(unittest.TestCase):
    def test_entry_and_exit_costs_reconcile(self):
        x = np.array([100., 101., 102., 103.])
        y = 2 * x + np.array([0., 1., -1., 0.])
        for positions in ([1, 1, 0], [1, 1, 1], [0, 0, 0]):
            with self.subTest(positions=positions), patch(
                    "benchmarks.pairs_trading.next_position", side_effect=positions):
                trades, result = backtest(x, y, 2, 2, .5, 10)
                self.assertAlmostEqual(trades['pnl'].sum(), result['equity'][-1])
                self.assertIsNone(result['summary']['sharpe_annualized'])
                self.assertEqual(result['summary']['pnl_units'], 'spread_price_units')

    def test_zero_gross_profit_loses_both_fees(self):
        x = np.array([100., 101., 102., 103.])
        y = 2 * x
        with patch("benchmarks.pairs_trading.next_position", side_effect=[1, 1, 0]):
            trades, result = backtest(x, y, 2, 2, .5, 10)
        expected_cost = .001 * (4 * x[0] + 4 * x[2])
        self.assertAlmostEqual(trades.iloc[0]['pnl'], -expected_cost)
        self.assertAlmostEqual(result['equity'][-1], -expected_cost)

    def test_default_synthetic_prices_are_valid(self):
        x, y = synthetic_series(5000, 7)
        self.assertTrue(np.all(x > 0) and np.all(y > 0))
        _, result = backtest(x, y, 60, 2, .5, 1)
        self.assertTrue(np.isfinite(result["equity"]).all())
        with self.assertRaises(ValueError):
            synthetic_series(0, 7)

    def test_input_validation(self):
        for x, y in (([], []), ([1, 2], [1]), ([1, np.nan], [2, 3]), ([0, 1], [1, 2])):
            with self.subTest(x=x, y=y), self.assertRaises(ValueError):
                backtest(np.array(x), np.array(y), 2, 2, .5, 1)
