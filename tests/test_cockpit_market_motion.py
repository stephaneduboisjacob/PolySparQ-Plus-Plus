"""Read-only tests for V5 market-window movement telemetry."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class MarketMotionTests(unittest.TestCase):
    def test_empty_and_invalid_ids_do_not_query(self):
        with patch.object(cockpit, "q") as query:
            self.assertEqual(cockpit.api_v5_market_motion("")["markets"], [])
            self.assertEqual(cockpit.api_v5_market_motion("bad/id")["error"],
                             "invalid condition id")
            query.assert_not_called()

    def test_ids_are_deduplicated_and_bounded(self):
        ids = [f"condition-{i}" for i in range(17)]
        with patch.object(cockpit, "q", return_value=[]) as query:
            self.assertIn("at most 16", cockpit.api_v5_market_motion(",".join(ids))["error"])
            query.assert_not_called()
            result = cockpit.api_v5_market_motion("condition-1,condition-1,condition-2")
        self.assertEqual(result["markets"], [])
        self.assertEqual(query.call_args.args[1], (["condition-1", "condition-2"],))
        self.assertIn("ANY(%s::text[])", query.call_args.args[0])

    def test_reports_observed_in_window_change_and_tape(self):
        row = {
            "condition_id": "condition-1", "asset": "BTC", "horizon_seconds": 300,
            "first_book_ts": 1_000, "first_up_mid": 0.50,
            "latest_book_ts": 6_000, "latest_up_mid": 0.623,
            "trade_count": 4, "notional_usd": 12.75,
            "last_trade_ts": 5_000, "up_midpoints": [[0, 0.50], [1, 0.623]],
        }
        with patch.object(cockpit, "q", return_value=[row]) as query:
            result = cockpit.api_v5_market_motion("condition-1")
        self.assertAlmostEqual(result["markets"][0]["move_cents"], 12.3)
        self.assertEqual(result["markets"][0]["book_span_s"], 5.0)
        self.assertEqual(result["markets"][0]["trade_count"], 4)
        self.assertEqual(result["markets"][0]["notional_usd"], 12.75)
        self.assertEqual(result["markets"][0]["up_midpoints"], [[0, 0.50], [1, 0.623]])
        self.assertIn("ts >= m.window_open", query.call_args.args[0])
        self.assertIn("ts <= LEAST(now(), m.window_close)", query.call_args.args[0])

    def test_missing_samples_remain_unavailable(self):
        row = {
            "condition_id": "condition-1", "asset": "ETH", "horizon_seconds": 900,
            "first_book_ts": None, "first_up_mid": None,
            "latest_book_ts": None, "latest_up_mid": None,
            "trade_count": 0, "notional_usd": 0.0,
            "last_trade_ts": None, "up_midpoints": None,
        }
        with patch.object(cockpit, "q", return_value=[row]):
            result = cockpit.api_v5_market_motion("condition-1")["markets"][0]
        self.assertIsNone(result["move_cents"])
        self.assertIsNone(result["book_span_s"])
        self.assertIsNone(result["book_age_s"])
        self.assertEqual(result["up_midpoints"], [])


if __name__ == "__main__":
    unittest.main()
