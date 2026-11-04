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
            self.assertEqual(cockpit.api_v5_market_motion("x" * 2576)["error"],
                             "condition_ids query is too long")
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
            "window_open_ms": 0, "window_close_ms": 300_000,
            "first_book_ts": 1_000, "first_up_mid": 0.50,
            "latest_book_ts": 6_000, "latest_up_mid": 0.623,
            "first_down_book_ts": 1_000, "first_down_mid": 0.47,
            "latest_down_book_ts": 5_500, "latest_down_mid": 0.38,
            "trade_count": 4, "notional_usd": 12.75,
            "size_source_known_count": 3, "size_missing_count": 1,
            "timestamp_source_known_count": 3,
            "timestamp_receive_fallback_count": 1,
            "timestamp_database_fallback_count": 0,
            "classified_trade_count": 3, "unclassified_trade_count": 1,
            "classified_notional_usd": 8.5, "net_up_flow_usd": 4.25,
            "reported_side_trade_count": 3, "reported_side_unclassified_count": 1,
            "reported_side_notional_usd": 9.25, "reported_net_up_flow_usd": -1.75,
            "reported_side_match_count": 2, "reported_side_comparable_count": 3,
            "flow_buckets": [[0, 4.25, 6.5, 2.25, 3, 1]],
            "last_trade_ts": 5_000, "up_midpoints": [[0, 0.50], [1, 0.623]],
            "down_midpoints": [[0, 0.47], [1, 0.38]],
            "up_spread_cents": [[0, 2.0], [1, 3.0]],
            "down_spread_cents": [[0, 4.0], [1, 5.0]],
            "up_imbalance_pct": [[0, 5.0], [1, 20.0]],
            "down_imbalance_pct": [[0, 10.0], [1, -10.0]],
        }
        with patch.object(cockpit, "q", return_value=[row]) as query:
            result = cockpit.api_v5_market_motion("condition-1")
        self.assertAlmostEqual(result["markets"][0]["move_cents"], 12.3)
        self.assertAlmostEqual(result["markets"][0]["down_move_cents"], -9.0)
        self.assertEqual(result["markets"][0]["book_span_s"], 5.0)
        self.assertEqual(result["markets"][0]["down_book_span_s"], 4.5)
        self.assertGreaterEqual(result["markets"][0]["down_book_age_s"], 0.0)
        self.assertEqual(result["markets"][0]["trade_count"], 4)
        self.assertEqual(result["markets"][0]["notional_usd"], 12.75)
        self.assertEqual(result["markets"][0]["size_source_known_count"], 3)
        self.assertEqual(result["markets"][0]["size_missing_count"], 1)
        self.assertEqual(result["markets"][0]["size_source_unknown_count"], 1)
        self.assertEqual(result["markets"][0]["timestamp_source_known_count"], 3)
        self.assertEqual(result["markets"][0]["timestamp_receive_fallback_count"], 1)
        self.assertEqual(result["markets"][0]["timestamp_database_fallback_count"], 0)
        self.assertEqual(result["markets"][0]["timestamp_source_unknown_count"], 1)
        self.assertEqual(result["markets"][0]["classified_trade_count"], 3)
        self.assertEqual(result["markets"][0]["unclassified_trade_count"], 1)
        self.assertEqual(result["markets"][0]["classified_notional_usd"], 8.5)
        self.assertEqual(result["markets"][0]["net_up_flow_usd"], 4.25)
        self.assertEqual(result["markets"][0]["reported_side_trade_count"], 3)
        self.assertEqual(result["markets"][0]["reported_side_unclassified_count"], 1)
        self.assertEqual(result["markets"][0]["reported_side_notional_usd"], 9.25)
        self.assertEqual(result["markets"][0]["reported_net_up_flow_usd"], -1.75)
        self.assertEqual(result["markets"][0]["reported_side_match_count"], 2)
        self.assertEqual(result["markets"][0]["reported_side_comparable_count"], 3)
        self.assertEqual(result["markets"][0]["flow_buckets"], [[0, 4.25, 6.5, 2.25, 3, 1]])
        self.assertEqual(result["markets"][0]["up_midpoints"], [[0, 0.50], [1, 0.623]])
        self.assertEqual(result["markets"][0]["down_midpoints"], [[0, 0.47], [1, 0.38]])
        self.assertEqual(result["markets"][0]["up_spread_cents"], [[0, 2.0], [1, 3.0]])
        self.assertEqual(result["markets"][0]["down_spread_cents"], [[0, 4.0], [1, 5.0]])
        self.assertEqual(result["markets"][0]["up_imbalance_pct"], [[0, 5.0], [1, 20.0]])
        self.assertEqual(result["markets"][0]["down_imbalance_pct"], [[0, 10.0], [1, -10.0]])
        self.assertEqual(result["markets"][0]["window_open_ms"], 0)
        self.assertEqual(result["markets"][0]["window_close_ms"], 300_000)
        self.assertIn("window_open_ms", query.call_args.args[0])
        self.assertIn("window_close_ms", query.call_args.args[0])
        self.assertIn("ts >= m.window_open", query.call_args.args[0])
        self.assertIn("ts <= LEAST(now(), m.window_close)", query.call_args.args[0])
        self.assertIn("side_label IN ('UP', 'DOWN')", query.call_args.args[0])
        self.assertIn("AVG(spread)::float8 * 100.0 AS spread_cents", query.call_args.args[0])
        self.assertIn("AVG(imbalance_top1) FILTER (WHERE jsonb_array_length(bids) > 0", query.call_args.args[0])
        self.assertIn("AVG(imbalance_top1)", query.call_args.args[0])
        self.assertIn("(bids->0->>1)::float8 > 0", query.call_args.args[0])
        self.assertIn("(asks->0->>1)::float8 > 0", query.call_args.args[0])
        self.assertIn("(side_label='UP' AND aggressor='buy')", query.call_args.args[0])
        self.assertIn("(side_label='DOWN' AND aggressor='sell')", query.call_args.args[0])
        self.assertIn("aggressor IS NULL", query.call_args.args[0])
        self.assertIn("raw->>'reported_side' IN ('buy','sell')", query.call_args.args[0])
        self.assertIn("raw->>'reported_side'=aggressor::text", query.call_args.args[0])
        self.assertIn("raw->>'timestamp_source'='receive'", query.call_args.args[0])
        self.assertIn("raw->>'timestamp_source'='database'", query.call_args.args[0])
        self.assertIn("raw ? 'size_available'", query.call_args.args[0])
        self.assertIn("raw->>'size_available'='false'", query.call_args.args[0])
        self.assertIn("m.horizon_seconds IN (300, 900)", query.call_args.args[0])
        self.assertIn("m.window_open <= now()", query.call_args.args[0])
        self.assertIn("m.window_close >= now()", query.call_args.args[0])

    def test_missing_samples_remain_unavailable(self):
        row = {
            "condition_id": "condition-1", "asset": "ETH", "horizon_seconds": 900,
            "first_book_ts": None, "first_up_mid": None,
            "latest_book_ts": None, "latest_up_mid": None,
            "first_down_book_ts": None, "first_down_mid": None,
            "latest_down_book_ts": None, "latest_down_mid": None,
            "trade_count": 0, "notional_usd": 0.0,
            "last_trade_ts": None, "up_midpoints": None, "down_midpoints": None,
            "classified_trade_count": 0, "unclassified_trade_count": 0,
            "classified_notional_usd": None, "net_up_flow_usd": None, "flow_buckets": None,
            "up_spread_cents": None, "down_spread_cents": None,
            "up_imbalance_pct": None, "down_imbalance_pct": None,
        }
        with patch.object(cockpit, "q", return_value=[row]):
            result = cockpit.api_v5_market_motion("condition-1")["markets"][0]
        self.assertIsNone(result["move_cents"])
        self.assertIsNone(result["down_move_cents"])
        self.assertIsNone(result["book_span_s"])
        self.assertIsNone(result["book_age_s"])
        self.assertIsNone(result["down_book_span_s"])
        self.assertIsNone(result["down_book_age_s"])
        self.assertEqual(result["up_midpoints"], [])
        self.assertEqual(result["down_midpoints"], [])
        self.assertEqual(result["up_spread_cents"], [])
        self.assertEqual(result["down_imbalance_pct"], [])
        self.assertEqual(result["flow_buckets"], [])
        self.assertIsNone(result["net_up_flow_usd"])
        self.assertEqual(result["timestamp_source_known_count"], 0)
        self.assertEqual(result["timestamp_source_unknown_count"], 0)
        self.assertEqual(result["size_source_known_count"], 0)
        self.assertEqual(result["size_source_unknown_count"], 0)

    def test_root_cockpit_route_shares_the_bounded_motion_endpoint(self):
        with patch.object(cockpit, "api_market_motion",
                          return_value={"markets": [], "server_ts": 1}) as motion:
            result = cockpit.ROUTES["/api/market_motion"](
                {"condition_ids": ["condition-1"]})
        self.assertEqual(result, {"markets": [], "server_ts": 1})
        motion.assert_called_once_with("condition-1")

    def test_root_prediction_cards_expose_tape_and_complete_set_details(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("/api/market_motion?", page)
        self.assertIn("class=\"prediction-tape-btn\"", page)
        self.assertIn("data-tape-mode=\"spread\"", page)
        self.assertIn("data-tape-mode=\"imbalance\"", page)
        self.assertIn("data-tape-mode=\"flow\"", page)
        self.assertIn("function predictionTapeFlowSeries(value,classifiedOnly=true)", page)
        self.assertIn("BBO tick-rule inference", page)
        self.assertIn("UP-supporting inferred notional", page)
        self.assertIn("unclassified prints are excluded", page)
        self.assertIn("TRADE TIME SOURCE", page)
        self.assertIn("TRADE SIZE COVERAGE", page)
        self.assertIn("function setPredictionTapeMode(mode,button)", page)
        self.assertIn("INSPECT STORED 15-SECOND BUCKETS", page)
        self.assertIn("function predictionTapeBuckets(m,mode)", page)
        self.assertIn("predictionTapeReadout", page)
        self.assertIn("top-of-book size tilt", page)
        self.assertIn("mode==='mid'?`${(100*val).toFixed(0)}¢`", page)
        self.assertIn("UP + DOWN COMPLETE SET · VISIBLE SIZE WALK", page)
        self.assertIn("pair.size_scenarios", page)
        self.assertIn("Missing buckets remain disconnected", page)
        self.assertIn("#tabManualpred .pred-hero{display:none}", page)
        self.assertIn("grid-auto-columns:minmax(150px,165px)", page)
        self.assertIn("min-height:40px", page)


if __name__ == "__main__":
    unittest.main()
