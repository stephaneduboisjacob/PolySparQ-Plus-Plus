"""The root Execution API exposes a scoped, redacted rejection history."""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class ExecutionRejectionTests(unittest.TestCase):
    def test_recent_rejections_are_scoped_limited_and_redacted(self):
        row = {
            "client_order_id": "order-123",
            "ts_created": 1_798_000_000_000,
            "instance": "cb",
            "execution_mode": "maker",
            "post_only": True,
            "source": "RULE",
            "direction": "buy",
            "side_label": "UP",
            "price": 0.58,
            "notional_usd": 5.0,
            "reject_reason": "invalid signature; private_key=" + "a" * 64,
            "condition_id": "condition-1",
            "asset": "BTC",
            "horizon_seconds": 300,
            "question": "Will BTC close up?",
        }
        with patch.object(cockpit, "current_mode", return_value="paper"), \
             patch.object(cockpit, "api_config_read", return_value={"config": {}}), \
             patch.object(cockpit, "q", side_effect=[[], [], [row]]) as query:
            result = cockpit.api_execution("cb")

        self.assertEqual(result["recent_rejections"][0]["client_order_id"], "order-123")
        self.assertIn("[REDACTED CREDENTIAL LINE]", result["recent_rejections"][0]["reject_reason"])
        self.assertNotIn("a" * 64, result["recent_rejections"][0]["reject_reason"])
        self.assertNotIn("exchange_order_id", result["recent_rejections"][0])
        sql, args = query.call_args_list[2].args
        self.assertIn("o.status='rejected'", sql)
        self.assertIn("now()-interval '24 hours'", sql)
        self.assertIn("ORDER BY o.ts_created DESC LIMIT 12", sql)
        self.assertIn("AND o.instance=%s", sql)
        self.assertEqual(args, ("paper", "cb"))


if __name__ == "__main__":
    unittest.main()
