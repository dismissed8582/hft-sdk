import unittest

import numpy as np
import pandas as pd

from benchmarks.paper_analysis import OBI_REGIMES, _ct_hawkes_mle, _ct_transition_events


class HawkesTests(unittest.TestCase):
    def test_return_is_timestamped_when_it_is_observable(self):
        frame = pd.DataFrame({"timestamp_ns": [0, 250_000_000],
                              "obi_regime": [1, 2], "return_regime_1s": [0, 2]})
        self.assertEqual(_ct_transition_events(frame, "obi_regime"),
                         [(0.0, 1), (1.0, OBI_REGIMES),
                          (0.25, 2), (1.25, OBI_REGIMES + 2)])

    def test_fit_does_not_claim_convergence_after_one_update(self):
        rng = np.random.default_rng(19)
        events = list(zip(np.cumsum(rng.exponential(0.2, 120)),
                          rng.integers(0, 2, 120)))
        one = _ct_hawkes_mle(events, 2, [1.0], 1e-6, max_iterations=1)
        fit = _ct_hawkes_mle(events, 2, [1.0], 1e-6)
        self.assertFalse(one["success"])
        self.assertGreater(fit["iterations"], 1)
        self.assertLessEqual(fit["objective"], one["objective"] + 1e-8)
        self.assertTrue(np.isfinite(fit["kernel"]).all())
        self.assertTrue((fit["kernel"] >= 0).all())

    def test_invalid_fit_configuration_is_rejected(self):
        for half_lives, ridge in (([0.0], 0), ([float("nan")], 0), ([1], -1)):
            with self.subTest(half_lives=half_lives), self.assertRaises(ValueError):
                _ct_hawkes_mle([], 2, half_lives, ridge)
