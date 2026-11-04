"""Read-only Polymarket account snapshot stays separate from bot accounting."""
from pathlib import Path
import io
import json
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cockpit


WALLET = "0x1234567890abcdef1234567890abcdef12345678"


def position(**overrides):
    row = {
        "proxy_wallet": WALLET,
        "condition_id": "condition-1",
        "token_id": "token-1",
        "title": "Will the market resolve UP?",
        "slug": "market-up",
        "outcome": "UP",
        "status": "OPEN",
        "current_size": 12.5,
        "avg_price": 0.48,
        "entry_cost_usdc": 6.0,
        "entry_fees_usdc": 0.03,
        "total_cost_usdc": 6.03,
        "current_price": 0.51,
        "current_value": 6.375,
        "realized_pnl": -0.125,
        "unrealized_pnl": 0.375,
        "total_pnl": 0.25,
        "redeemable": False,
        "mergeable": False,
        "archived": False,
        "first_entry_at": 1792958400,
    }
    row.update(overrides)
    return row


def positions_page(rows=None, has_more=False, cursor=None):
    return {"data": rows if rows is not None else [position()],
            "pagination": {"offset": 0, "has_more": has_more,
                           "next_cursor": cursor}}


class PolymarketAccountSnapshotTests(unittest.TestCase):
    def _post_handler(self, payload, admin=True):
        handler = object.__new__(cockpit.Handler)
        raw = json.dumps(payload).encode()
        handler.path = "/api/polymarket_account"
        handler._username = None
        handler._user = {}
        handler.headers = {"Content-Length": str(len(raw))}
        handler.rfile = io.BytesIO(raw)
        handler.wfile = io.BytesIO()
        handler._require_auth = lambda: True
        handler._is_admin = lambda: admin
        handler._is_demo = lambda: False
        handler.send_response = Mock()
        handler.send_header = Mock()
        handler.end_headers = Mock()
        return handler

    def test_invalid_wallet_and_cursor_are_rejected_before_network(self):
        with patch.object(cockpit, "_polymarket_data_v2_get") as fetch:
            status, result = cockpit.api_polymarket_account("0x1234")
            self.assertEqual(status, 400)
            self.assertIn("valid", result["error"])
            status, result = cockpit.api_polymarket_account("0x" + "0" * 40)
            self.assertEqual(status, 400)
            status, result = cockpit.api_polymarket_account(WALLET, "bad\ncursor")
            self.assertEqual(status, 400)
            fetch.assert_not_called()

    def test_first_page_uses_fixed_read_only_api_and_sanitizes_fields(self):
        calls = []

        def fetch(route, params):
            calls.append((route, params))
            if route == "value":
                return {"data": {"proxy_wallet": WALLET, "value": 31.2578,
                                 "private": "discard"}}
            return positions_page([position(current_size="12.5", current_price=float("nan"),
                                             profile_image="https://invalid.example/secret")],
                                  True, "opaque-next")

        with patch.object(cockpit, "_polymarket_data_v2_get", side_effect=fetch):
            result = cockpit.api_polymarket_account(WALLET)

        self.assertEqual(result["source"], "Polymarket Data API v2")
        self.assertTrue(result["read_only"])
        self.assertEqual(result["portfolio_value_usdc"], 31.2578)
        self.assertIn("not free USDC cash", result["portfolio_value_includes"])
        self.assertTrue(result["has_more"])
        self.assertEqual(result["next_cursor"], "opaque-next")
        self.assertEqual(result["page_count"], 1)
        self.assertEqual(result["positions"][0]["current_size"], 12.5)
        self.assertIsNone(result["positions"][0]["current_price"])
        self.assertEqual(result["positions"][0]["entry_cost_usdc"], 6.0)
        self.assertEqual(result["positions"][0]["entry_fees_usdc"], 0.03)
        self.assertEqual(result["positions"][0]["total_cost_usdc"], 6.03)
        self.assertEqual(result["positions"][0]["realized_pnl"], -0.125)
        self.assertNotIn("profile_image", result["positions"][0])
        self.assertEqual({route for route, _ in calls}, {"value", "positions"})
        params = next(params for route, params in calls if route == "positions")
        self.assertEqual(params["user"], WALLET)
        self.assertEqual(params["status"], "OPEN")
        self.assertEqual(params["limit"], cockpit.POLYMARKET_ACCOUNT_PAGE_SIZE)
        self.assertEqual(params["sort_by"], "CURRENT_VALUE")
        self.assertEqual(params["include_archived"], "true")

    def test_http_helper_only_targets_official_data_api_and_bounds_body(self):
        response = Mock()
        response.__enter__ = Mock(return_value=response)
        response.__exit__ = Mock(return_value=False)
        response.read.return_value = b'{"data":[]}'
        with patch("urllib.request.urlopen", return_value=response) as open_url:
            self.assertEqual(cockpit._polymarket_data_v2_get(
                "positions", {"user": WALLET, "limit": 100,
                              "cursor": "opaque&status=CLOSED"}), {"data": []})
        request = open_url.call_args.args[0]
        self.assertTrue(request.full_url.startswith(
            "https://data-api.polymarket.com/v2/positions?"))
        self.assertIn("user=" + WALLET, request.full_url)
        self.assertIn("cursor=opaque%26status%3DCLOSED", request.full_url)
        self.assertEqual(open_url.call_args.kwargs["timeout"], 6.0)
        self.assertEqual(response.read.call_args.args[0],
                         cockpit.POLYMARKET_ACCOUNT_MAX_RESPONSE_BYTES + 1)

        response.read.return_value = b"x" * (cockpit.POLYMARKET_ACCOUNT_MAX_RESPONSE_BYTES + 1)
        with patch("urllib.request.urlopen", return_value=response):
            with self.assertRaisesRegex(ValueError, "4 MB safety limit"):
                cockpit._polymarket_data_v2_get("positions", {"user": WALLET})

    def test_cursor_page_does_not_repeat_portfolio_value_call(self):
        with patch.object(cockpit, "_polymarket_data_v2_get",
                          return_value=positions_page([], False)) as fetch:
            result = cockpit.api_polymarket_account(WALLET, "opaque-cursor")
        self.assertEqual(fetch.call_count, 1)
        route, params = fetch.call_args.args
        self.assertEqual(route, "positions")
        self.assertEqual(params["cursor"], "opaque-cursor")
        self.assertIsNone(result["portfolio_value_usdc"])
        self.assertEqual(result["positions"], [])
        self.assertFalse(result["has_more"])

    def test_mismatched_wallet_and_invalid_envelopes_fail_closed(self):
        with patch.object(cockpit, "_polymarket_data_v2_get", side_effect=lambda route, params:
                          {"data": {"proxy_wallet": "0x" + "1" * 40, "value": 4}}
                          if route == "value" else positions_page()):
            status, result = cockpit.api_polymarket_account(WALLET)
        self.assertEqual(status, 502)
        self.assertIn("different wallet", result["error"])

        with patch.object(cockpit, "_polymarket_data_v2_get", side_effect=lambda route, params:
                          {"data": {"proxy_wallet": WALLET, "value": 4}}
                          if route == "value" else {"data": []}):
            status, result = cockpit.api_polymarket_account(WALLET)
        self.assertEqual(status, 502)
        self.assertIn("positions page", result["error"])

    def test_rate_limit_is_reported_as_retryable_service_unavailable(self):
        from urllib.error import HTTPError

        def fail(route, params):
            raise HTTPError("https://data-api.polymarket.com", 429, "limited", {}, None)

        with patch.object(cockpit, "_polymarket_data_v2_get", side_effect=fail):
            status, result = cockpit.api_polymarket_account(WALLET)
        self.assertEqual(status, 503)
        self.assertIn("rate-limiting", result["error"])

    def test_post_route_requires_admin_and_does_not_put_wallet_in_request_path(self):
        body = {"user": WALLET, "cursor": "cursor"}
        handler = self._post_handler(body, admin=True)
        response = {"source": "Polymarket Data API v2", "read_only": True}
        with patch.object(cockpit, "api_polymarket_account", return_value=response) as api:
            handler.do_POST()
        self.assertEqual(handler.send_response.call_args.args[0], 200)
        self.assertEqual(handler.path, "/api/polymarket_account")
        api.assert_called_once_with(WALLET, "cursor")
        self.assertEqual(json.loads(handler.wfile.getvalue()), response)

        handler = self._post_handler(body, admin=False)
        with patch.object(cockpit, "api_polymarket_account") as api:
            handler.do_POST()
        self.assertEqual(handler.send_response.call_args.args[0], 403)
        api.assert_not_called()

    def test_positions_ui_is_admin_gated_manual_and_credential_free(self):
        html = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn('id="polymarketAccountCard" style="display:none"', html)
        self.assertIn("accountCard.style.display=r.role==='admin'?'':'none'", html)
        self.assertIn("onclick=\"loadPolymarketAccount(false)\"", html)
        self.assertIn("No API key or private key is requested or stored here.", html)
        self.assertIn("not bot P&amp;L", html)
        self.assertIn("ENTRY BASIS · EX FEES", html)
        self.assertIn("BUY FEES · DISCLOSURE", html)
        self.assertIn("ENTRY COST &amp; REALIZED P&amp;L", html)
        self.assertIn("do not subtract them a second time", html)
        self.assertIn("Marked value is not guaranteed sale proceeds.", html)
        self.assertIn("fetch('/api/polymarket_account',{method:'POST'", html)
        self.assertIn("/api/polymarket_account", cockpit.Handler.ADMIN_ONLY_ROUTES)


if __name__ == "__main__":
    unittest.main()
