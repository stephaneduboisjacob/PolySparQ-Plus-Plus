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


class ExternalStrategyProfilesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config_dir = self.root / "config"
        self.user_config_dir = self.root / "user" / "bots"
        self.config_dir.mkdir()
        self.user_config_dir.mkdir(parents=True)
        self.profile = self.user_config_dir / "endgame-never-loses.toml"
        self.profile.write_text(
            '[runtime]\nmode="paper"\n'
            '[strategy.hot]\nlate_window_sniper_order_usd=5.0\n'
            'prediction_2m_order_usd=15.0\n'
            '[wallet]\nprivate_key="DO_NOT_EXPOSE"\n'
        )
        for name, value in (("ROOT", str(self.root)),
                            ("CONFIG_DIR", str(self.config_dir)),
                            ("USER_BOT_CONFIG_DIR", str(self.user_config_dir))):
            p = patch.object(cockpit, name, value)
            p.start()
            self.addCleanup(p.stop)

    def test_assigned_profile_list_includes_external_but_hides_unassigned(self):
        (self.user_config_dir / "unassigned.toml").write_text('[runtime]\nmode="paper"\n')
        result = cockpit.api_configs(["endgame-never-loses", "../bad"])
        self.assertEqual([x["name"] for x in result["configs"]], ["endgame-never-loses"])
        self.assertTrue(result["configs"][0]["external"])

    def test_external_editor_redacts_credentials_and_exposes_usd_controls(self):
        result = cockpit.api_config_read("endgame-never-loses", full=False)
        self.assertTrue(result["external_profile"])
        self.assertIn("strategy.hot.prediction_2m_order_usd", result["tunable_keys"])
        self.assertEqual(result["config"]["wallet"]["private_key"], "")
        self.assertNotIn("DO_NOT_EXPOSE", json.dumps(result))

    def test_strategy_editor_accepts_requested_clips_on_external_profiles(self):
        code, result = cockpit.api_config_write("endgame-never-loses", {
            "strategy.hot.late_window_sniper_order_usd": 25.0,
            "strategy.hot.prediction_2m_order_usd": 25.0,
        })
        self.assertEqual(code, 200, result)
        saved = tomllib.loads(self.profile.read_text())
        hot = saved["strategy"]["hot"]
        self.assertEqual(hot["late_window_sniper_order_usd"], 25.0)
        self.assertEqual(hot["prediction_2m_order_usd"], 25.0)

    def test_strategy_editor_rejects_clips_above_the_order_cap(self):
        before = self.profile.read_bytes()
        code, result = cockpit.api_config_write("endgame-never-loses", {
            "strategy.hot.late_window_sniper_order_usd": 26.0,
        })
        self.assertEqual(code, 422)
        self.assertEqual(before, self.profile.read_bytes())

    def test_fleet_profile_button_opens_strategy_editor(self):
        page = (ROOT / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("EXTERNAL PROFILE</span><button", page)
        self.assertIn("editStrategyProfile('${esc(inst)}')", page)


if __name__ == "__main__":
    unittest.main()
