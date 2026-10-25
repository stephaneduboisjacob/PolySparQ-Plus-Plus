"""Profile-file capture status is visible without exposing unrelated config."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class CaptureProfileStatusTests(unittest.TestCase):
    def setUp(self):
        cockpit._capture_profiles_cache[:] = [0.0, None]

    def test_reports_only_active_profile_capture_flags(self):
        with tempfile.TemporaryDirectory() as temp:
            profile = Path(temp) / "cb.toml"
            profile.write_text(
                '[data_collection]\nbook_snapshots_enabled = false\n'
                'pm_trades_enabled = true\n\n[wallet]\nprivate_key = "never return"\n',
                encoding="utf-8")
            with patch.object(cockpit, "bot_status_detail", return_value={
                    "instances_running": ["cb", "inactive", "../../invalid"]}), \
                 patch.object(cockpit, "_instance_config_file", side_effect=lambda name: str(profile)
                              if name == "cb" else str(Path(temp) / "missing.toml")):
                profiles = cockpit.api_v3_capture_profiles()

        self.assertEqual(len(profiles), 2)
        by_name = {row["instance"]: row for row in profiles}
        self.assertEqual(by_name["cb"]["book_snapshots_enabled"], False)
        self.assertEqual(by_name["cb"]["pm_trades_enabled"], True)
        self.assertTrue(by_name["cb"]["config_readable"])
        self.assertIsNone(by_name["inactive"]["book_snapshots_enabled"])
        self.assertFalse(by_name["inactive"]["config_readable"])
        self.assertEqual(set(by_name["cb"]), {
            "instance", "config_readable", "book_snapshots_enabled", "pm_trades_enabled"})

    def test_empty_active_fleet_reports_no_profiles(self):
        with patch.object(cockpit, "bot_status_detail", return_value={"instances_running": []}):
            self.assertEqual(cockpit.api_v3_capture_profiles(), [])

    def test_cache_is_copy_safe_and_runtime_invalidation_refreshes(self):
        with tempfile.TemporaryDirectory() as temp:
            profile = Path(temp) / "cb.toml"
            profile.write_text(
                '[data_collection]\nbook_snapshots_enabled = true\n'
                'pm_trades_enabled = false\n', encoding="utf-8")
            with patch.object(cockpit, "bot_status_detail", return_value={
                    "instances_running": ["cb"]}) as status, \
                 patch.object(cockpit, "_instance_config_file", return_value=str(profile)):
                first = cockpit.api_v3_capture_profiles()
                first[0]["instance"] = "mutated"
                second = cockpit.api_v3_capture_profiles()
                self.assertEqual(status.call_count, 1)
                self.assertEqual(second[0]["instance"], "cb")
                cockpit.invalidate_runtime_caches()
                refreshed = cockpit.api_v3_capture_profiles()
                self.assertEqual(status.call_count, 2)
        self.assertEqual(refreshed[0]["book_snapshots_enabled"], True)

    def test_malformed_profile_is_unknown_not_a_server_error(self):
        with tempfile.TemporaryDirectory() as temp:
            profile = Path(temp) / "cb.toml"
            profile.write_text("[data_collection\n", encoding="utf-8")
            with patch.object(cockpit, "bot_status_detail", return_value={
                    "instances_running": ["cb"]}), \
                 patch.object(cockpit, "_instance_config_file", return_value=str(profile)):
                row = cockpit.api_v3_capture_profiles()[0]
        self.assertFalse(row["config_readable"])
        self.assertIsNone(row["book_snapshots_enabled"])
        self.assertIsNone(row["pm_trades_enabled"])

    def test_feed_endpoint_includes_capture_profiles(self):
        capture = [{"instance": "cb", "config_readable": True,
                    "book_snapshots_enabled": False, "pm_trades_enabled": False}]
        with patch.object(cockpit, "q", return_value=[]), \
             patch.object(cockpit, "api_v3_capture_profiles", return_value=capture):
            result = cockpit.api_v3_feeds()
        self.assertEqual(result["capture_profiles"], capture)
        self.assertEqual(result["feeds"], [])


if __name__ == "__main__":
    unittest.main()
