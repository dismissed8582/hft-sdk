import unittest
from benchmarks.pairs_trading import next_position


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
