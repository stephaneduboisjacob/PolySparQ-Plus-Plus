"""Root cockpit past-snapshot lookup is bounded and read-only."""
from pathlib import Path
import json
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class RootStateAtTests(unittest.TestCase):
    def test_returns_latest_snapshot_and_prior_order_creations_with_scope(self):
        now_ms = 1_798_000_000_000
        as_of_ms = now_ms - 90_000
        positions = [{"asset": "BTC", "side": "UP", "shares": i + 1}
                     for i in range(51)]
        snapshot = {
            "instance": "cb", "mode": "paper", "ts": as_of_ms - 15_000,
            "cash_usd": 7.5, "equity_usd": 12.0, "realized_pnl": 1.0,
            "unrealized_pnl": 0.5, "deployed_usd": 4.5,
            "open_positions": json.dumps(positions),
        }
        order = {
            "client_order_id": "order-1", "instance": "cb", "mode": "paper",
            "ts_created": as_of_ms - 20_000, "condition_id": "condition-1",
            "side_label": "UP", "direction": "buy", "execution_mode": "maker",
            "post_only": True, "source": "RULE", "price": 0.61,
            "size_shares": 8.0, "notional_usd": 4.88, "asset": "BTC",
            "horizon_seconds": 300, "question": "Will BTC close up?",
        }

        with patch.object(cockpit.time, "time", return_value=now_ms / 1000), \
             patch.object(cockpit, "q", side_effect=[[snapshot], [order]]) as query:
            result = cockpit.api_root_state_at(as_of_ms, "cb", "paper")

        self.assertEqual(result["as_of_ms"], as_of_ms)
        self.assertEqual(result["instance"], "cb")
        self.assertEqual(result["mode"], "paper")
        saved = result["snapshots"][0]
        self.assertEqual(saved["open_position_count"], 51)
        self.assertTrue(saved["open_positions_truncated"])
        self.assertEqual(len(saved["open_positions"]), 50)
        self.assertEqual(result["orders"], [order])
        self.assertNotIn("status", result["orders"][0])

        snapshot_sql, snapshot_args = query.call_args_list[0].args
        self.assertIn("DISTINCT ON (h.instance)", snapshot_sql)
        self.assertIn("h.ts DESC,h.id DESC", snapshot_sql)
        self.assertIn("h.ts<=to_timestamp(%s/1000.0)", snapshot_sql)
        self.assertIn("AND h.instance=%s", snapshot_sql)
        self.assertIn("LIMIT 101", snapshot_sql)
        self.assertNotIn("session_id", snapshot_sql)
        self.assertEqual(snapshot_args, (as_of_ms, "paper", "cb"))

        order_sql, order_args = query.call_args_list[1].args
        self.assertIn("o.ts_created<=to_timestamp(%s/1000.0)", order_sql)
        self.assertIn("interval '30 minutes'", order_sql)
        self.assertIn("AND o.instance=%s", order_sql)
        self.assertIn("LIMIT 101", order_sql)
        self.assertNotIn("o.status", order_sql)
        self.assertEqual(order_args, (as_of_ms, as_of_ms, "paper", "cb"))

    def test_caps_snapshot_and_order_results_and_marks_truncation(self):
        now_ms = 1_798_000_000_000
        as_of_ms = now_ms - 1000
        snapshots = [{"instance": f"bot-{i}", "mode": "paper", "ts": as_of_ms,
                      "open_positions": []} for i in range(101)]
        orders = [{"client_order_id": str(i)} for i in range(101)]
        with patch.object(cockpit.time, "time", return_value=now_ms / 1000), \
             patch.object(cockpit, "q", side_effect=[snapshots, orders]):
            result = cockpit.api_root_state_at(as_of_ms, mode="paper")
        self.assertEqual(len(result["snapshots"]), 100)
        self.assertEqual(len(result["orders"]), 100)
        self.assertTrue(result["snapshots_truncated"])
        self.assertTrue(result["orders_truncated"])

    def test_database_error_does_not_expose_internal_details(self):
        with patch.object(cockpit.time, "time", return_value=1_798_000_001), \
             patch.object(cockpit, "q", side_effect=RuntimeError("private db detail")) as query:
            result = cockpit.api_root_state_at(1_798_000_000_000, "cb", "paper")
        self.assertEqual(result, {"error": "Stored position snapshots are temporarily unavailable."})
        self.assertNotIn("private db detail", repr(result))
        query.assert_called_once()

    def test_rejects_invalid_time_mode_and_instance_without_querying(self):
        with patch.object(cockpit.time, "time", return_value=1_798_000_000), \
             patch.object(cockpit, "q") as query:
            self.assertIn("valid timestamp", cockpit.api_root_state_at("bad")["error"])
            self.assertIn("past timestamp", cockpit.api_root_state_at(1_798_000_001_000)["error"])
            self.assertIn("Invalid bot instance", cockpit.api_root_state_at(
                1_798_000_000_000, "../cb", "paper")["error"])
            self.assertIn("Mode must be", cockpit.api_root_state_at(
                1_798_000_000_000, "cb", "auto")["error"])
        query.assert_not_called()

    def test_root_route_forwards_lookup_parameters(self):
        expected = {"as_of_ms": 123, "instance": "cb", "mode": "paper"}
        with patch.object(cockpit, "api_root_state_at", return_value=expected) as endpoint:
            result = cockpit.ROUTES["/api/root_state_at"]({
                "as_of_ms": ["123"], "instance": ["cb"], "mode": ["paper"]})
        self.assertEqual(result, expected)
        endpoint.assert_called_once_with("123", "cb", "paper")

    def test_unassigned_non_admin_cannot_query_all_bot_snapshots(self):
        handler = object.__new__(cockpit.Handler)
        handler.path = "/api/root_state_at?as_of_ms=123&mode=paper"
        with patch.object(cockpit.Handler, "_require_auth", return_value=True), \
             patch.object(cockpit.Handler, "_is_admin", return_value=False), \
             patch.object(cockpit.Handler, "_enforce_instance", return_value=None), \
             patch.object(cockpit.Handler, "_send", return_value="sent") as send:
            result = handler.do_GET()
        self.assertEqual(result, "sent")
        send.assert_called_once_with(
            403, {"error": "No bot instance is assigned to this account."})

    def test_root_activity_tab_exposes_the_unversioned_lookup(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn('id="rootStateAtPanel"', page)
        self.assertIn("/api/root_state_at?", page)
        self.assertIn("LOOK UP A PAST SNAPSHOT", page)
        self.assertIn("strategy configuration are unavailable", page)


if __name__ == "__main__":
    unittest.main()
