"""Runtime source matching is read-only and never returns config digests."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cockpit


class RuntimeConfigSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config_dir = self.root / "config"
        self.config_dir.mkdir()
        self.profile = self.config_dir / "polypulse-cb.toml"
        self.profile.write_text('[runtime]\nmode="paper"\n[strategy.hot]\nper_click_size_usd=5.0\n')
        for name, value in (("ROOT", str(self.root)),
                            ("CONFIG_DIR", str(self.config_dir)),
                            ("USER_BOT_CONFIG_DIR", str(self.root / "external"))):
            p = patch.object(cockpit, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.current_digest = hashlib.sha256(self.profile.read_bytes()).hexdigest()
        self.service = {"running": True, "pid": 1234}
        self.observed = {
            "config_source_sha256": self.current_digest,
            "config_source_loaded_ms": 1_800_000_000_000,
            "mode": "PAPER",
            "ts_ms": 1_800_000_000_100,
        }

    def inspect(self):
        return cockpit.api_config_runtime("cb")

    def test_same_source_is_verified_without_returning_either_digest(self):
        with patch.object(cockpit, "_systemd_bot_status", return_value=self.service), \
             patch.object(cockpit, "_observed_runtime_status", return_value=self.observed):
            result = self.inspect()
        self.assertEqual(result["state"], "in_sync")
        self.assertEqual(result["mode"], "PAPER")
        self.assertEqual(result["loaded_at_ms"], self.observed["config_source_loaded_ms"])
        self.assertNotIn("config_source_sha256", result)
        self.assertNotIn(self.current_digest, json.dumps(result))

    def test_profile_edit_reports_file_ahead_without_changing_config(self):
        with patch.object(cockpit, "_systemd_bot_status", return_value=self.service), \
             patch.object(cockpit, "_observed_runtime_status", return_value=self.observed):
            self.profile.write_text(self.profile.read_text().replace("5.0", "7.0"))
            result = self.inspect()
        self.assertEqual(result["state"], "file_ahead")
        self.assertEqual(self.profile.read_text().count("7.0"), 1)
        self.assertNotIn("config_source_sha256", result)

    def test_stopped_bot_does_not_read_or_claim_runtime_state(self):
        with patch.object(cockpit, "_systemd_bot_status", return_value={"running": False}), \
             patch.object(cockpit, "_observed_runtime_status") as observed:
            result = self.inspect()
        self.assertEqual(result["state"], "stopped")
        observed.assert_not_called()

    def test_older_or_missing_runtime_digest_is_unverified_not_a_match(self):
        with patch.object(cockpit, "_systemd_bot_status", return_value=self.service), \
             patch.object(cockpit, "_observed_runtime_status", return_value={"mode": "PAPER"}):
            result = self.inspect()
        self.assertEqual(result["state"], "runtime_unsupported")

    def test_runtime_digest_timestamp_cannot_be_in_the_future(self):
        observed = {**self.observed, "config_source_loaded_ms": self.observed["ts_ms"] + 1001}
        with patch.object(cockpit, "_systemd_bot_status", return_value=self.service), \
             patch.object(cockpit, "_observed_runtime_status", return_value=observed):
            result = self.inspect()
        self.assertEqual(result["state"], "runtime_unsupported")

    def test_invalid_name_and_missing_profile_fail_closed(self):
        with patch.object(cockpit, "_systemd_bot_status") as service:
            self.assertEqual(cockpit.api_config_runtime("../cb")["error"], "invalid instance name")
            self.assertEqual(cockpit.api_config_runtime("missing")["state"], "profile_missing")
        service.assert_not_called()

    def test_root_strategy_editor_discloses_runtime_source_limit(self):
        page = (ROOT / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("/api/config/runtime?name=", page)
        self.assertIn("SAVED FILE DIFFERS FROM LAST LOAD", page)
        self.assertIn("some settings may require restart", page)
        self.assertIn("strategyRuntimeRefresh", page)


if __name__ == "__main__":
    unittest.main()
