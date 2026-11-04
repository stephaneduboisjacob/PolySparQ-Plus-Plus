"""Read-only tests for the root prediction tape's notable public prints."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cockpit


class LargePrintRadarTests(unittest.TestCase):
    def test_returns_only_bounded_known_size_prints_for_the_current_window(self):
        prints = [{
            "ts_ms": 1_735_689_600_000,
            "outcome": "UP",
            "price": 0.62,
            "size_shares": 250.0,
            "notional_usd": 155.0,
            "reported_side": "buy",
            "aggressor": "sell",
            "timestamp_source": "exchange",
        }]
        with patch.object(cockpit, "q", return_value=[{
            "condition_id": "market-1",
            "large_prints": prints,
        }]) as query:
            result = cockpit.api_market_motion("market-1")

        market = result["markets"][0]
        self.assertEqual(market["large_prints"], prints)
        sql = query.call_args.args[0]
        self.assertEqual(query.call_args.args[1], (["market-1"],))
        self.assertIn("t.raw->>'size_available'='true'", sql)
        self.assertIn("t.ts >= m.window_open AND t.ts <= LEAST(now(), m.window_close)", sql)
        self.assertIn("ORDER BY t.price*t.size DESC, t.ts DESC, t.id DESC", sql)
        self.assertIn("LIMIT 40", sql)
        self.assertIn("'reported_side', NULLIF(p.raw->>'reported_side', '')", sql)
        self.assertIn("'aggressor', p.aggressor::text", sql)
        self.assertNotIn("wallet_address", sql)

    def test_missing_print_rows_default_to_an_empty_array(self):
        with patch.object(cockpit, "q", return_value=[{
            "condition_id": "market-1",
            "large_prints": None,
        }]):
            result = cockpit.api_market_motion("market-1")
        self.assertEqual(result["markets"][0]["large_prints"], [])

    def test_root_prediction_tape_labels_prints_without_wallet_or_trade_claims(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn('id="predictionTapeLargePrintDisclosure"', page)
        self.assertIn('id="predictionTapePrintMin"', page)
        self.assertIn('id="predictionTapeLargePrintRows"', page)
        self.assertIn("No wallet identity or combined position is available.", page)
        self.assertIn("function renderPredictionTapeLargePrints(m)", page)
        self.assertIn("within ${largestCount} largest size-known prints", page)
        self.assertIn("data-tape-mode=", page)
        self.assertIn("Research display · not an order signal", page)
        load_fn = page[page.index("async function loadPredictionTape()"):]
        error_path = load_fn[load_fn.index("}catch(error){"):load_fn.index("}finally{")]
        self.assertIn("state.market=null;", error_path)


if __name__ == "__main__":
    unittest.main()
