"""Focused cockpit safety and maker telemetry tests; no live services needed."""
import importlib.util
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("cockpit", ROOT / "scripts/cockpit.py")
cockpit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cockpit)


class ConfigWriteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config_dir = self.root / "config"
        self.config_dir.mkdir()
        (self.root / "logs").mkdir()
        self.path = self.config_dir / "polypulse-cb.toml"
        self.path.write_text("""[strategy.hot]
entry_order_mode = "maker"
maker_improve_ticks = 1
min_size_usd = 10.0
max_size_usd = 30.0

[wallet]
api_key = "placeholder"
""")
        capabilities = cockpit.api_v3_capabilities()
        patches = [patch.object(cockpit, "ROOT", str(self.root)),
                   patch.object(cockpit, "CONFIG_DIR", str(self.config_dir)),
                   patch.object(cockpit, "api_v3_capabilities", return_value=capabilities),
                   patch.object(cockpit, "invalidate_runtime_caches", return_value=None)]
        for item in patches:
            item.start()
            self.addCleanup(item.stop)

    def write(self, *changes):
        return cockpit.api_v3_config_write({"instance": "cb", "changes": list(changes), "reason": "test"})

    def test_rejects_secret_and_keeps_entire_batch_unchanged(self):
        before = self.path.read_text()
        code, result = self.write({"key": "strategy.hot.maker_improve_ticks", "value": 2},
                                  {"key": "wallet.api_key", "value": "replacement"})
        self.assertEqual(code, 422)
        self.assertFalse(result["ok"])
        self.assertEqual(self.path.read_text(), before)

    def test_rejects_cross_field_conflict(self):
        code, result = self.write({"key": "strategy.hot.min_size_usd", "value": 50})
        self.assertEqual(code, 422)
        self.assertIn("exceeds", result["error"])

    def test_rejects_duplicate_keys_and_bad_batch_shape(self):
        before = self.path.read_text()
        code, result = self.write({"key": "strategy.hot.maker_improve_ticks", "value": 2},
                                  {"key": "strategy.hot.maker_improve_ticks", "value": 3})
        self.assertEqual(code, 422)
        self.assertIn("duplicate", str(result["results"]))
        self.assertEqual(self.path.read_text(), before)
        code, _ = cockpit.api_v3_config_write({"instance": "cb", "changes": "bad"})
        self.assertEqual(code, 400)

    def test_validated_maker_write_and_audit(self):
        code, result = self.write({"key": "strategy.hot.maker_improve_ticks", "value": 2})
        self.assertEqual(code, 200)
        self.assertEqual(result["changed"], ["strategy.hot.maker_improve_ticks"])
        self.assertIn("maker_improve_ticks = 2", self.path.read_text())
        audit = json.loads((self.root / "logs/config_audit.jsonl").read_text().strip())
        self.assertEqual(audit["changes"][0]["old"], 1)
        self.assertEqual(audit["changes"][0]["new"], 2)

    def test_legacy_write_is_atomic_and_cannot_change_live_mode(self):
        before = self.path.read_text()
        code, result = cockpit.api_config_write(
            "cb", {"maker_improve_ticks": 2, "mode": "live"})
        self.assertEqual(code, 422)
        self.assertIn("mode", result["rejected"])
        self.assertEqual(self.path.read_text(), before)


class MakerTelemetryTests(unittest.TestCase):
    def test_stale_live_quote_does_not_claim_queue_precision(self):
        row = {"quote_id": "intent-a", "exchange_order_id": "venue-a", "instance": "cb",
               "mode": "live", "execution_mode": "maker", "post_only": True,
               "status": "acked", "condition_id": "market-a", "side_label": "UP",
               "price": 0.72, "size_shares": 10.0, "filled_shares": 2.0,
               "ts_submitted": 1, "asset": "BTC", "horizon_seconds": 300}
        with patch.object(cockpit, "q", return_value=[row]), \
             patch.object(cockpit, "load_config", return_value={"strategy": {"maker_max_age_ms": 5000}}):
            result = cockpit.api_v3_maker_fleet()
        quote = result["quotes"][0]
        self.assertEqual(quote["state"], "STALE")
        self.assertTrue(quote["stale"])
        self.assertEqual(quote["remaining_shares"], 8.0)
        self.assertIsNone(quote["queue_ahead"])
        self.assertIsNone(quote["fill_probability"])
        self.assertEqual(quote["post_only_protection"], "REQUESTED")
        self.assertFalse(result["estimates_available"])
        self.assertEqual(result["summary"]["orders_submitted_24h"], 1)
        self.assertEqual(result["summary"]["order_fill_rate_24h"], 100.0)

    def test_order_event_api_reports_unmigrated_state(self):
        with patch.object(cockpit, "q", return_value={"table_name": None}):
            result = cockpit.api_v4_order_events(quote_id="11111111-1111-1111-1111-111111111111")
        self.assertFalse(result["supported"])
        self.assertEqual(result["events"], [])

    def test_uncertain_order_is_not_counted_as_resting(self):
        row = {"quote_id": "intent-unknown", "instance": "cb", "mode": "live",
               "execution_mode": "maker", "post_only": True, "status": "uncertain",
               "price": 0.7, "size_shares": 10, "filled_shares": 0,
               "ts_submitted": int(time.time() * 1000)}
        with patch.object(cockpit, "q", return_value=[row]), \
             patch.object(cockpit, "load_config", return_value={}):
            result = cockpit.api_v3_maker_fleet()
        self.assertEqual(result["quotes"][0]["state"], "UNCERTAIN")
        self.assertEqual(result["summary"]["uncertain_orders"], 1)
        self.assertEqual(result["summary"]["resting_orders"], 0)
        self.assertEqual(result["summary"]["resting_notional"], 0)


class ForceSellRoutingTests(unittest.TestCase):
    def test_same_market_requires_instance_and_targets_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            positions = [{"cid": "market-a", "side": "UP", "shares": 10, "instance": "cb"},
                         {"cid": "market-a", "side": "UP", "shares": 10, "instance": "scalp"}]
            with patch.object(cockpit, "COMMANDS_DIR", directory), \
                 patch.object(cockpit, "cockpit_token", return_value="token"), \
                 patch.object(cockpit, "fleet_instances", return_value=["cb", "scalp"]), \
                 patch.object(cockpit, "_fleet_open_positions", return_value=positions):
                code, _ = cockpit.api_force_sell("market-a", "UP", "token")
                self.assertEqual(code, 409)
                self.assertEqual(list(Path(directory).iterdir()), [])
                code, result = cockpit.api_force_sell("market-a", "UP", "token", "scalp")
                self.assertEqual(code, 200)
                body = json.loads((Path(directory) / result["queued"]).read_text())
                self.assertEqual(body["instance"], "scalp")
                self.assertEqual(body["condition_id"], "market-a")


class PolyTronRedactionTests(unittest.TestCase):
    def test_credential_patterns_removed_before_chat(self):
        raw = "api_key=secret123 Bearer abcdef private_key: 0x" + "a" * 64
        safe = cockpit._polytron_redact(raw)
        for fragment in ("secret123", "abcdef", "a" * 64):
            self.assertNotIn(fragment, safe)
        self.assertIn("[REDACTED", safe)

    def test_log_tail_rejects_path_input(self):
        result = cockpit.api_log_tail("../secrets/api_key", 10)
        self.assertEqual(result["error"], "invalid instance")
        self.assertEqual(result["lines"], [])


class RuntimeStatusTests(unittest.TestCase):
    def test_runtime_mode_requires_fresh_matching_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            logs = Path(directory) / "logs"
            logs.mkdir()
            path = logs / "polypulse-cb.runtime.json"
            status = {"instance": "cb", "pid": 42, "ts_ms": int(time.time() * 1000),
                      "mode": "LIVE", "execution_mode": "maker"}
            path.write_text(json.dumps(status))
            with patch.object(cockpit, "ROOT", directory):
                self.assertEqual(cockpit._observed_runtime_status("cb", 42)["mode"], "LIVE")
                self.assertIsNone(cockpit._observed_runtime_status("cb", 43))
                status["ts_ms"] -= 20_000
                path.write_text(json.dumps(status))
                self.assertIsNone(cockpit._observed_runtime_status("cb", 42))

    def test_unresolved_live_order_marks_running_bot_degraded(self):
        with patch.object(cockpit, "fleet_instances", return_value=["cb"]), \
             patch.object(cockpit, "_systemd_bot_status", return_value={"running": True, "pid": 999999999}), \
             patch.object(cockpit, "_observed_runtime_status", return_value={
                 "mode": "LIVE", "execution_mode": "maker", "ts_ms": 1,
                 "risk_state": "ORDER_UNCERTAIN_HALT"}), \
             patch.object(cockpit, "current_mode", return_value="live"):
            fleet = cockpit.api_v3_fleet()["fleet"]
        self.assertEqual(fleet[0]["status"], "degraded")
        self.assertEqual(fleet[0]["risk_state"], "ORDER_UNCERTAIN_HALT")

    def test_running_process_without_fresh_heartbeat_is_unverified(self):
        with patch.object(cockpit, "fleet_instances", return_value=["cb"]), \
             patch.object(cockpit, "_systemd_bot_status", return_value={"running": True, "pid": 999999999}), \
             patch.object(cockpit, "_observed_runtime_status", return_value=None), \
             patch.object(cockpit, "current_mode", return_value="paper"), \
             patch.object(cockpit, "load_config", return_value={"strategy": {"entry_order_mode": "maker"}}):
            fleet = cockpit.api_v3_fleet()["fleet"]
        self.assertEqual(fleet[0]["status"], "unverified")
        self.assertFalse(fleet[0]["mode_verified"])


if __name__ == "__main__":
    unittest.main()
