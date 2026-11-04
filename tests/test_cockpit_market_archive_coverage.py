"""Recent book-history coverage is bounded, read-only, and copy-safe."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class MarketArchiveCoverageTests(unittest.TestCase):
    def setUp(self):
        cockpit._market_archive_coverage_cache[:] = [0.0, None]

    def test_returns_bounded_per_horizon_coverage_and_caches_copy(self):
        row = {
            "sampled_markets": 2,
            "truncated": False,
            "groups": [{
                "asset": "BTC", "horizon_seconds": 300,
                "markets_sampled": 2, "windows_with_snapshots": 1,
                "windows_with_both_sides": 1, "snapshot_rows": 4,
                "snapshot_buckets_15s": 12, "expected_buckets_15s": 20,
                "both_sides_buckets_15s": 10,
                "windows_with_tracked_time": 2,
                "windows_with_full_15s_coverage": 1,
                "windows": [{"condition_id": "market-1", "slug": "btc-updown-5m-1",
                             "window_open": "2026-10-25T10:00:00+00:00",
                             "window_close": "2026-10-25T10:05:00+00:00",
                             "tracked_start": "2026-10-25T10:00:00+00:00",
                             "snapshot_rows": 4, "sides": 2,
                             "snapshot_buckets_15s": 3,
                             "both_sides_buckets_15s": 2,
                             "expected_buckets_15s": 20,
                             "buckets": [{"index": 0, "up": True, "down": True},
                                         {"index": 2, "up": True, "down": False}]}],
            }],
        }
        with patch.object(cockpit, "q", return_value=row) as query:
            first = cockpit.api_v3_market_archive_coverage()
            self.assertTrue(first["available"])
            self.assertEqual(first["lookback_hours"], 24)
            self.assertEqual(first["market_limit"], 500)
            self.assertGreater(first["sampled_ms"], 0)
            first["groups"][0]["snapshot_rows"] = 999
            first["groups"][0]["windows"][0]["snapshot_rows"] = 999
            first["groups"][0]["windows"][0]["buckets"][0]["up"] = False
            second = cockpit.api_v3_market_archive_coverage()

        self.assertEqual(second["groups"][0]["snapshot_rows"], 4)
        self.assertEqual(second["groups"][0]["windows"][0]["snapshot_rows"], 4)
        self.assertTrue(second["groups"][0]["windows"][0]["buckets"][0]["up"])
        query.assert_called_once()
        sql = query.call_args.args[0]
        self.assertIn("LIMIT 501", sql)
        self.assertIn("window_close > now()-interval '24 hours'", sql)
        self.assertIn("ORDER BY window_close DESC,condition_id", sql)
        self.assertIn("GREATEST(window_open,first_seen)", sql)
        self.assertIn("s.ts>=m.tracked_start AND s.ts<m.window_close", sql)
        self.assertIn("snapshot_buckets_15s", sql)
        self.assertIn("bool_or(side_label='UP'::vp.share_side)", sql)
        self.assertIn("both_sides_buckets_15s", sql)
        self.assertIn("expected_buckets_15s", sql)
        self.assertIn("'windows'", sql)
        self.assertIn("'buckets'", sql)
        self.assertIn("'tracked_start',w.tracked_start", sql)
        self.assertIn("FROM bucket_presence bp", sql)
        self.assertIn("ORDER BY p.window_close DESC,p.condition_id LIMIT 12", sql)
        self.assertTrue(query.call_args.kwargs["one"])

    def test_database_failure_is_private_and_briefly_cached(self):
        with patch.object(cockpit, "q", side_effect=RuntimeError("private database detail")) as query:
            first = cockpit.api_v3_market_archive_coverage()
            second = cockpit.api_v3_market_archive_coverage()
        self.assertFalse(first["available"])
        self.assertEqual(first["groups"], [])
        self.assertNotIn("private database detail", repr(first))
        self.assertEqual(first, second)
        query.assert_called_once()

    def test_force_refresh_bypasses_the_shared_short_cache(self):
        row = {"sampled_markets": 0, "truncated": False, "groups": []}
        with patch.object(cockpit, "q", return_value=row) as query:
            cockpit.api_v3_market_archive_coverage()
            cockpit.api_v3_market_archive_coverage(force=True)
        self.assertEqual(query.call_count, 2)

    def test_root_route_is_unversioned_and_forwards_force_refresh(self):
        result = {"available": True, "groups": []}
        with patch.object(cockpit, "api_market_archive_coverage", return_value=result) as endpoint:
            self.assertEqual(cockpit.ROUTES["/api/market_archive_coverage"](
                {"force": ["1"]}), result)
        endpoint.assert_called_once_with(True)

    def test_root_markets_tab_exposes_local_coverage_panel(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn('id="marketArchiveGrid"', page)
        self.assertIn("/api/market_archive_coverage", page)
        self.assertIn("loadMarketArchiveCoverage(true)", page)
        self.assertIn("15-second buckets measure observed snapshot presence", page)
        self.assertIn("snapshot_buckets_15s", page)
        self.assertIn("both_sides_buckets_15s", page)
        self.assertIn("windows_with_full_15s_coverage", page)
        self.assertIn("had any snapshot", page)
        self.assertIn("covered every expected bucket", page)
        self.assertIn("WINDOW DETAILS", page)
        self.assertIn("LATEST ${windows.length} OF ${markets}", page)
        self.assertIn("gaps first", page)
        self.assertIn("incompleteWindowCount", page)
        self.assertIn("untrackedWindowCount", page)
        self.assertIn("mkt-archive-strip", page)
        self.assertIn("slots both outcomes", page)
        self.assertIn("FIRST TRACKED", page)
        self.assertIn("window close", page)
        self.assertIn("ONE SIDE", page)
        self.assertIn("NO SNAPSHOT", page)
        self.assertIn("polymarket.com/event/", page)
        self.assertNotIn("/api/v3/market_archive_coverage", page)

    def test_feed_response_includes_archive_coverage(self):
        coverage = {"available": True, "groups": [], "sampled_markets": 0}
        with patch.object(cockpit, "q", return_value=[]), \
             patch.object(cockpit, "api_v3_fleet", return_value={"fleet": []}), \
             patch.object(cockpit, "api_v3_capture_profiles", return_value=[]), \
             patch.object(cockpit, "api_v3_market_storage", return_value={"available": True}), \
             patch.object(cockpit, "api_v3_market_archive_coverage", return_value=coverage):
            result = cockpit.api_v3_feeds()
        self.assertEqual(result["market_archive_coverage"], coverage)


if __name__ == "__main__":
    unittest.main()
