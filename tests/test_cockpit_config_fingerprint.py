"""The root Strategy editor fingerprints only redacted visible settings."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cockpit


class ConfigFingerprintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config_dir = self.root / "config"
        self.config_dir.mkdir()
        self.profile = self.config_dir / "polypulse-fingerprint.toml"
        self.profile.write_text(
            '[runtime]\nmode="paper"\n'
            '[strategy.hot]\nper_click_size_usd=5.0\n'
            '[wallet]\nprivate_key="secret-one"\napi_key="key-one"\n'
            '[[tokens]]\nname="btc"\nprivate_key="array-secret-one"\n'
            'created=2026-10-26\n'
        )
        for name, value in (
            ("CONFIG_DIR", str(self.config_dir)),
            ("USER_BOT_CONFIG_DIR", str(self.root / "external")),
        ):
            p = patch.object(cockpit, name, value)
            p.start()
            self.addCleanup(p.stop)

    def test_fingerprint_matches_canonical_redacted_config_and_hides_secrets(self):
        result = cockpit.api_config_read("fingerprint")
        visible = cockpit._redact_config_tree(tomllib.loads(self.profile.read_text()))
        expected = hashlib.sha256(
            json.dumps(visible, sort_keys=True, separators=(",", ":"),
                       ensure_ascii=False, default=str).encode("utf-8")
        ).hexdigest()

        self.assertEqual(result["redacted_config_sha256"], expected)
        self.assertEqual(len(result["redacted_config_sha256"]), 64)
        self.assertEqual(result["config"]["wallet"]["private_key"], "")
        self.assertEqual(result["config"]["tokens"][0]["private_key"], "")
        self.assertNotIn("secret-one", result["raw"])
        self.assertNotIn("key-one", result["raw"])
        self.assertNotIn("array-secret-one", result["raw"])

    def test_credential_rotation_does_not_change_fingerprint_but_settings_do(self):
        first = cockpit.api_config_read("fingerprint")["redacted_config_sha256"]
        self.profile.write_text(self.profile.read_text().replace(
            'private_key="secret-one"', 'private_key="secret-two"'))
        self.assertEqual(cockpit.api_config_read("fingerprint")["redacted_config_sha256"], first)

        self.profile.write_text(self.profile.read_text().replace(
            "array-secret-one", "array-secret-two"))
        self.assertEqual(cockpit.api_config_read("fingerprint")["redacted_config_sha256"], first)

        self.profile.write_text(self.profile.read_text().replace("5.0", "7.0"))
        self.assertNotEqual(cockpit.api_config_read("fingerprint")["redacted_config_sha256"], first)

    def test_root_strategy_editor_explains_and_copies_fingerprint_scope(self):
        page = (ROOT / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("REDACTED SHA-256", page)
        self.assertIn("copyStrategyFingerprint()", page)
        self.assertIn("not effective runtime state", page)
        self.assertIn("runtime defaults and overrides are not included", page)


if __name__ == "__main__":
    unittest.main()
