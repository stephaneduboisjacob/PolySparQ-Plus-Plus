"""The root Markets API exposes distinct local snapshot times for each outcome."""
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class MarketsTests(unittest.TestCase):
    def test_market_rows_keep_up_and_down_snapshot_times_separate(self):
        now_ms = int(time.time() * 1000)
        rows = [{
            "condition_id": "market-1",
            "asset": "BTC",
            "horizon_seconds": 300,
            "window_open": now_ms - 60_000,
            "window_close": now_ms + 240_000,
            "up_bid": 0.61,
            "down_bid": 0.38,
            "up_book_ts": now_ms - 700,
            "down_book_ts": now_ms - 4_200,
            "book_ts": now_ms - 700,
        }, {
            "condition_id": "market-2",
            "asset": "ETH",
            "horizon_seconds": 900,
            "window_open": now_ms - 60_000,
            "window_close": now_ms + 840_000,
            "up_bid": None,
            "down_bid": 0.49,
            "up_book_ts": None,
            "down_book_ts": now_ms - 1_300,
            "book_ts": now_ms - 1_300,
        }]
        with patch.object(cockpit, "q", return_value=rows) as query:
            result = cockpit.api_markets(limit=2)

        self.assertEqual(len(result["markets"]), 2)
        first, second = result["markets"]
        self.assertEqual(first["up_book_ts"], now_ms - 700)
        self.assertEqual(first["down_book_ts"], now_ms - 4_200)
        self.assertEqual(first["book_ts"], now_ms - 700)
        self.assertIsNone(second["up_book_ts"])
        self.assertEqual(second["down_book_ts"], now_ms - 1_300)
        self.assertEqual(first["phase"], "live")
        sql, args = query.call_args.args
        self.assertIn("up.ts AS up_book_ts", sql)
        self.assertIn("dn.ts AS down_book_ts", sql)
        self.assertIn("GREATEST(up.ts, dn.ts) AS book_ts", sql)
        self.assertEqual(args, (2,))


if __name__ == "__main__":
    unittest.main()
