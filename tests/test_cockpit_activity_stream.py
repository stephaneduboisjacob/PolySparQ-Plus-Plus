"""The root activity tape includes bounded, bot-scoped fill events."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class ActivityFillEventTests(unittest.TestCase):
    def test_fill_lookup_uses_recent_and_open_order_ids_with_instance_scope(self):
        order_id = "11111111-1111-4111-8111-111111111111"
        fill = {
            "id": 41, "client_order_id": order_id, "ts": "2026-10-26T03:20:00Z",
            "price": 0.61, "size_shares": 8.19, "fee_usd": 0.02,
            "instance": "cb", "mode": "paper", "direction": "buy",
            "side_label": "UP", "source": "CB_EDGE", "execution_mode": "maker",
        }
        with patch.object(cockpit, "q", return_value=[fill]) as query:
            rows = cockpit.api_activity_fill_events(
                [{"client_order_id": order_id}, {"client_order_id": order_id}],
                "cb", limit=900)

        self.assertEqual(rows, [fill])
        sql, args = query.call_args.args
        self.assertEqual(args, ([order_id], "cb", "cb", 200))
        self.assertIn("SELECT unnest(%s::uuid[])", sql)
        self.assertIn("o.status IN ('submitted','acked','partial','uncertain')", sql)
        self.assertIn("r.ended_at IS NULL", sql)
        self.assertIn("f.client_order_id IN (SELECT client_order_id FROM candidate_orders)", sql)
        self.assertIn("ORDER BY f.ts DESC,f.id DESC LIMIT %s", sql)
        self.assertEqual(sql.count("AND o.instance=%s"), 2)
        self.assertNotIn("exchange_order_id", sql)
        self.assertNotIn("f.raw", sql)

    def test_unscoped_admin_lookup_is_bounded_and_keeps_paper_session_guard(self):
        with patch.object(cockpit, "q", return_value=[]) as query:
            self.assertEqual(cockpit.api_activity_fill_events([], limit=0), [])

        sql, args = query.call_args.args
        self.assertEqual(args, ([], 1))
        self.assertNotIn("AND o.instance=%s", sql)
        self.assertIn("o.mode::text <> 'paper' OR EXISTS", sql)
        self.assertIn("LIMIT 200", sql)

    def test_activity_includes_fill_events_from_the_activity_snapshot(self):
        order_rows = [{"client_order_id": "11111111-1111-4111-8111-111111111111"}]
        fills = [{"id": 41, "ts": "2026-10-26T03:20:00Z", "price": 0.61,
                  "size_shares": 8.19, "fee_usd": 0.02}]
        with patch.object(cockpit, "_activity_cache", {}), \
             patch.object(cockpit, "api_exit_performance", return_value={"groups": []}), \
             patch.object(cockpit, "api_orders", return_value={"orders": order_rows}), \
             patch.object(cockpit, "api_activity_fill_events", return_value=fills) as fill_query, \
             patch.object(cockpit, "api_settles", return_value={"settles": []}), \
             patch.object(cockpit, "api_orderstats", return_value={}), \
             patch.object(cockpit, "api_source_pnl", return_value={"sources": []}):
            result = cockpit.api_activity("cb")

        self.assertEqual(result["fill_events"], fills)
        self.assertTrue(result["fill_events_available"])
        fill_query.assert_called_once_with(order_rows, "cb")

    def test_fill_query_failure_is_localized_to_fill_events(self):
        with patch.object(cockpit, "_activity_cache", {}), \
             patch.object(cockpit, "api_exit_performance", return_value={"groups": []}), \
             patch.object(cockpit, "api_orders", return_value={"orders": []}), \
             patch.object(cockpit, "api_activity_fill_events", side_effect=RuntimeError("db internals")), \
             patch.object(cockpit, "api_settles", return_value={"settles": []}), \
             patch.object(cockpit, "api_orderstats", return_value={}), \
             patch.object(cockpit, "api_source_pnl", return_value={"sources": []}):
            result = cockpit.api_activity("cb")

        self.assertEqual(result["fill_events"], [])
        self.assertFalse(result["fill_events_available"])
        self.assertNotIn("db internals", repr(result))


if __name__ == "__main__":
    unittest.main()
