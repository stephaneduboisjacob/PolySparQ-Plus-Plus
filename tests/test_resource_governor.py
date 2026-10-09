import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "scripts" / "resource_governor.py"
SPEC = importlib.util.spec_from_file_location("resource_governor", MODULE_PATH)
governor = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
sys.modules[SPEC.name] = governor
SPEC.loader.exec_module(governor)


class FakeProbe:
    def __init__(self, active=False, fail=False):
        self.active = active
        self.fail = fail

    def trading_state(self, recent_order_seconds, open_position_shares):
        if self.fail:
            raise RuntimeError("database offline")
        return self.active, {"open_positions": 1 if self.active else 0,
                             "open_shares": 5.0 if self.active else 0.0,
                             "recent_orders": 1 if self.active else 0}


class FakeController:
    def __init__(self):
        self.applied = []

    def apply(self, slice_name, decision, dry_run):
        self.applied.append((slice_name, decision, dry_run))
        return [["fake", str(decision.quota_percent)]]


class DecisionTests(unittest.TestCase):
    def setUp(self):
        self.cfg = governor.GovernorConfig(enabled=True, reserve_cores=2)
        self.host = governor.HostSample(8, 0.2, 60.0, 70.0)

    def test_quiet_host_releases_research_budget(self):
        result = governor.decide(self.cfg, self.host, False, True, None)
        self.assertEqual(result.mode, "research")
        self.assertEqual(result.quota_percent, 480)  # 80% of six usable cores
        self.assertFalse(result.freeze)

    def test_recent_trade_takes_priority(self):
        result = governor.decide(self.cfg, self.host, True, True, 0)
        self.assertEqual(result.mode, "trading")
        self.assertEqual(result.quota_percent, 90)

    def test_pressure_reduces_research(self):
        pressured = governor.HostSample(8, 0.95, 10.0, 70.0)
        result = governor.decide(self.cfg, pressured, False, True, None)
        self.assertEqual(result.mode, "pressure")
        self.assertEqual(set(result.reasons), {"memory", "load"})

    def test_database_failure_is_fail_safe(self):
        result = governor.decide(self.cfg, self.host, True, False, 0)
        self.assertEqual(result.mode, "fail_safe")
        self.assertEqual(result.reasons, ("database_unavailable",))

    def test_cooldown_delays_full_research(self):
        result = governor.decide(self.cfg, self.host, False, True, 12)
        self.assertEqual(result.mode, "cooldown")


class CycleTests(unittest.TestCase):
    def test_disabled_config_always_dry_runs_and_persists_state(self):
        with tempfile.TemporaryDirectory() as temp:
            cfg = governor.GovernorConfig(
                enabled=False,
                state_file=Path(temp) / "state.json",
                reserve_cores=1,
            )
            controller = FakeController()
            host = governor.HostSample(4, 0.1, 80.0, 80.0)
            with mock.patch.object(governor, "read_host_sample", return_value=host):
                payload = governor.run_cycle(cfg, FakeProbe(), controller, now=100.0)
            self.assertTrue(payload["dry_run"])
            self.assertEqual(payload["mode"], "research")
            self.assertTrue(cfg.state_file.exists())
            self.assertTrue(controller.applied[0][2])

    def test_failed_probe_yields_research(self):
        with tempfile.TemporaryDirectory() as temp:
            cfg = governor.GovernorConfig(
                enabled=True,
                state_file=Path(temp) / "state.json",
            )
            controller = FakeController()
            host = governor.HostSample(8, 0.1, 80.0, 80.0)
            with mock.patch.object(governor, "read_host_sample", return_value=host):
                payload = governor.run_cycle(cfg, FakeProbe(fail=True), controller,
                                             dry_run=True, now=100.0)
            self.assertEqual(payload["mode"], "fail_safe")


class ConfigurationTests(unittest.TestCase):
    def test_invalid_quota_order_is_rejected(self):
        with self.assertRaises(ValueError):
            governor.GovernorConfig(quiet_cpu_percent=10,
                                    trading_cpu_percent=20).validate()


if __name__ == "__main__":
    unittest.main()
