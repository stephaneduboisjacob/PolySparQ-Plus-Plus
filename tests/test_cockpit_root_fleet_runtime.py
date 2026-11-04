"""Root Fleet should expose only verified, tenant-scoped runtime state."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cockpit


class RootFleetRuntimeTests(unittest.TestCase):
    def test_fleet_details_distinguish_fresh_runtime_from_unverified_config(self):
        with tempfile.TemporaryDirectory() as directory:
            config_dir = Path(directory) / "config"
            config_dir.mkdir()
            for instance in ("cb", "other"):
                (config_dir / f"polypulse-{instance}.toml").write_text("[runtime]\nmode='paper'\n")

            def query(sql, *_args, **_kwargs):
                return [{"n": 0}] if "COUNT(*)" in sql else []

            runtime_rows = [
                {"instance": "cb", "mode": "PAPER", "mode_verified": True,
                 "risk_state": "LOSS_LIMIT_HALT", "autopilot": False,
                 "daily_kill_fired": True, "runtime_updated_ms": 1234},
                {"instance": "other", "mode": "PAPER", "mode_verified": False,
                 "risk_state": "UNVERIFIED", "autopilot": None,
                 "daily_kill_fired": None, "runtime_updated_ms": None},
            ]
            with (
                patch.object(cockpit, "_fleet_cache", [0.0, None]),
                patch.object(cockpit, "CONFIG_DIR", str(config_dir)),
                patch.object(cockpit, "fleet_instances", return_value=["cb", "other"]),
                patch.object(cockpit, "instance_names", return_value={}),
                patch.object(cockpit, "_models_env", return_value={}),
                patch.object(cockpit, "_fleet_runtime_snapshot", return_value={"fleet": runtime_rows}),
                patch.object(cockpit, "_instance_config_file",
                             side_effect=lambda instance: str(config_dir / f"polypulse-{instance}.toml")),
                patch.object(cockpit, "_supervisor_running", return_value=False),
                patch.object(cockpit, "q", side_effect=query),
                patch.object(cockpit, "_instance_display_name", side_effect=lambda instance, *_: instance),
                patch.object(cockpit, "_read_swap_origin", return_value=None),
                patch.object(cockpit, "_instance_config_is_external", return_value=False),
                patch.object(cockpit, "_strategy_summary", return_value={}),
                patch.object(cockpit, "_wallet_status_for", return_value={}),
                patch.object(cockpit, "instance_running", return_value=True),
            ):
                fleet = cockpit.api_fleet_detail()["fleet"]

        by_instance = {row["instance"]: row["runtime"] for row in fleet}
        self.assertEqual(by_instance["cb"], {
            "verified": True,
            "mode": "PAPER",
            "risk_state": "LOSS_LIMIT_HALT",
            "autopilot": False,
            "daily_kill_fired": True,
            "updated_ms": 1234,
        })
        self.assertEqual(by_instance["other"], {
            "verified": False,
            "mode": None,
            "risk_state": "UNKNOWN",
            "autopilot": None,
            "daily_kill_fired": None,
            "updated_ms": None,
        })

    def test_user_fleet_response_filters_runtime_to_owned_instances(self):
        fleet = [
            {"instance": "cb", "runtime": {"verified": True, "risk_state": "NORMAL"}, "wallet": {}},
            {"instance": "other", "runtime": {"verified": True, "risk_state": "NORMAL"}, "wallet": {}},
        ]
        handler = object.__new__(cockpit.Handler)
        with (
            patch.object(cockpit, "api_fleet_detail", return_value={"fleet": fleet}),
            patch.object(cockpit.Handler, "_plan_name", return_value="standard"),
            patch.object(cockpit.Handler, "_allowed_models", return_value=[]),
            patch.object(cockpit.Handler, "_is_admin", return_value=False),
            patch.object(cockpit.Handler, "_user_instances", return_value={"cb"}),
            patch.object(cockpit.Handler, "_is_demo", return_value=False),
        ):
            result = handler._api_fleet_for_user()

        self.assertEqual([row["instance"] for row in result["fleet"]], ["cb"])
        self.assertEqual(result["fleet"][0]["runtime"], {"verified": True, "risk_state": "NORMAL"})

    def test_root_fleet_presents_scanner_state_without_claiming_entry_eligibility(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("function fleetRuntimePresentation(f,running=!!(f&&f.running))", page)
        self.assertIn("SESSION LOSS HALT", page)
        self.assertIn("ORDER RECONCILIATION HALT", page)
        self.assertIn("ENTRY SCANNER ON", page)
        self.assertIn("ENTRY SCANNER OFF", page)
        self.assertIn("may still block an order", page)


if __name__ == "__main__":
    unittest.main()
