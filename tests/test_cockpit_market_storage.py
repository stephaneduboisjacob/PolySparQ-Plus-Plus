"""Read-only market-data footprint telemetry is bounded and cache-safe."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class MarketStorageTests(unittest.TestCase):
    def setUp(self):
        cockpit._market_storage_cache[:] = [0.0, None]

    def test_returns_relation_sizes_and_copy_safe_cached_rows(self):
        rows = [{"relname": "pm_book_snapshots", "approx_rows": 100,
                 "approx_dead_rows": 2, "table_bytes": 700,
                 "index_bytes": 300, "total_bytes": 1100}]
        with patch.object(cockpit, "q", return_value=rows) as query:
            first = cockpit.api_v3_market_storage()
            self.assertTrue(first["available"])
            self.assertGreater(first["sampled_ms"], 0)
            first["relations"][0]["total_bytes"] = 0
            second = cockpit.api_v3_market_storage()

        self.assertEqual(second["relations"][0]["total_bytes"], 1100)
        query.assert_called_once()
        sql = query.call_args.args[0]
        self.assertIn("pg_stat_user_tables", sql)
        self.assertIn("pg_total_relation_size", sql)
        self.assertEqual(query.call_args.args[1],
                         (["pm_book_snapshots", "pm_trades", "pm_markets"],))

    def test_database_failure_is_private_and_briefly_cached(self):
        with patch.object(cockpit, "q", side_effect=RuntimeError("private database detail")) as query:
            first = cockpit.api_v3_market_storage()
            second = cockpit.api_v3_market_storage()
        self.assertFalse(first["available"])
        self.assertEqual(first["relations"], [])
        self.assertNotIn("private database detail", repr(first))
        self.assertEqual(first, second)
        query.assert_called_once()


if __name__ == "__main__":
    unittest.main()
