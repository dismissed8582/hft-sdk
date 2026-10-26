import unittest
import numpy as np
import hft_sdk


class SDKTests(unittest.TestCase):
    def arrays(self):
        count = 9
        return dict(order_id=np.arange(1, count + 1, dtype=np.int64),
                    timestamp_ns=np.arange(count, dtype=np.int64) * 250_000_000,
                    price_ticks=np.full(count, 100), quantity=np.ones(count, dtype=np.int64),
                    bid_price_ticks=np.full(count, 100), ask_price_ticks=np.full(count, 102),
                    type_codes=np.zeros(count, dtype=np.int64), side_codes=np.ones(count, dtype=np.int64))

    def test_rolling_history_and_stats(self):
        config = hft_sdk.EngineConfig()
        config.max_snapshots = 3
        engine = hft_sdk.OBIFilterEngine(config)
        engine.process_arrays(**self.arrays())
        self.assertEqual(engine.stats().snapshots, 3)
        self.assertEqual(engine.stats().total_snapshots, 8)
        self.assertEqual(engine.stats().evicted_snapshots, 5)
        self.assertEqual(engine.latest_snapshot().timestamp_ns, 2_000_000_000)
        self.assertEqual([s.timestamp_ns for s in engine.snapshots()],
                         [1_500_000_000, 1_750_000_000, 2_000_000_000])
        engine.reset()
        self.assertIsNone(engine.latest_snapshot())
        self.assertEqual(engine.stats().total_snapshots, 0)

    def test_invalid_configuration(self):
        config = hft_sdk.EngineConfig()
        config.sample_interval_ns = 0
        with self.assertRaises(ValueError):
            hft_sdk.OBIFilterEngine(config)

    def test_rejected_event_is_observable(self):
        arrays = self.arrays()
        arrays['timestamp_ns'][0] = -1
        engine = hft_sdk.OBIFilterEngine()
        engine.process_arrays(**arrays)
        self.assertEqual(engine.stats().invalid_events, 1)

    def test_dataframe_parent_links_are_preserved(self):
        try:
            import pandas as pd
        except ImportError:
            self.skipTest('pandas extra not installed')
        config = hft_sdk.EngineConfig()
        config.thresholds.max_modifications = 0
        frame = pd.DataFrame(dict(order_id=[1, 1, 99, 2], timestamp_ns=[0, 50_000_000, 200_000_000, 250_000_000],
                                  price_ticks=[100]*4, quantity=[10,20,10,1],
                                  bid_price_ticks=[100]*4, ask_price_ticks=[101]*4,
                                  type=[0,1,3,0], side=[1,1,4,2], parent_order_id=[0,0,1,0]))
        engine = hft_sdk.OBIFilterEngine(config)
        engine.process_frame(frame)
        self.assertFalse(engine.latest_snapshot().trade(hft_sdk.FilterScheme.ModificationCount, 0).valid)
        self.assertTrue(engine.latest_snapshot().trade(hft_sdk.FilterScheme.Unfiltered, 0).valid)
