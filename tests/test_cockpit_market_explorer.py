"""The unversioned root market explorer stays read-only and label-safe."""
from pathlib import Path
from email.message import Message
import sys
import unittest
from urllib.error import HTTPError
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class RootMarketExplorerTests(unittest.TestCase):
    def setUp(self):
        with cockpit._mobile_gamma_lock:
            cockpit._mobile_gamma_cache.clear()
            cockpit._mobile_book_cache.clear()
            cockpit._mobile_book_batch_cache.clear()
            cockpit._mobile_public_trades_cache.clear()
            cockpit._mobile_book_retry_at = 0.0
            cockpit._mobile_book_retry_keys.clear()
            cockpit._mobile_book_failures.clear()
            cockpit._mobile_gamma_retry_at.clear()
            cockpit._mobile_gamma_failures.clear()
            cockpit._mobile_gamma_host_retry_at = 0.0
        with cockpit._mobile_public_trades_lock:
            cockpit._mobile_public_trades_retry_at = 0.0
            cockpit._mobile_public_trades_retry_keys.clear()
            cockpit._mobile_public_trades_failures.clear()

    def test_normalizer_preserves_bounded_resolution_text_and_source(self):
        raw = {
            "conditionId": "0x" + "a" * 64,
            "outcomes": '["Yes", "No"]',
            "clobTokenIds": '["yes-token", "no-token"]',
            "outcomePrices": '["0.4", "0.6"]',
            "description": "Resolves Yes if the stated event occurs.",
            "resolutionSource": "https://example.com/official-rules",
        }
        market = cockpit._mobile_normalize_market(raw)
        self.assertEqual(market["description"], "Resolves Yes if the stated event occurs.")
        self.assertEqual(market["resolution_source"], "https://example.com/official-rules")

    def test_normalizer_bounds_resolution_payload_fields(self):
        raw = {
            "conditionId": "0x" + "b" * 64,
            "outcomes": '["Yes", "No"]',
            "clobTokenIds": '["yes-token", "no-token"]',
            "description": "d" * 13000,
            "resolutionSource": "s" * 1200,
        }
        market = cockpit._mobile_normalize_market(raw)
        self.assertEqual(len(market["description"]), 12000)
        self.assertEqual(len(market["resolution_source"]), 1000)

    def test_market_catalog_route_forwards_bounded_search_parameters(self):
        expected = (200, {"markets": [], "query": "bitcoin", "stale": False})
        with patch.object(cockpit, "api_mobile_market_search", return_value=expected) as search:
            result = cockpit.ROUTES["/api/mobile/markets"]({
                "q": ["bitcoin"], "limit": ["12"], "offset": ["24"]})
        self.assertEqual(result, expected)
        search.assert_called_once_with("bitcoin", "12", "24")

    def test_public_trades_reject_invalid_market_without_upstream_request(self):
        with patch.object(cockpit, "_polymarket_data_v2_get") as request:
            status, payload = cockpit.api_mobile_public_trades("not-a-condition", 20)
        self.assertEqual(status, 400)
        self.assertIn("condition", payload["error"])
        request.assert_not_called()

    def test_public_trades_route_forwards_only_condition_and_capped_page_size(self):
        condition = "0x" + "c" * 64
        expected = (200, {"trades": [], "condition_id": condition, "limit": 20})
        with patch.object(cockpit, "api_mobile_public_trades", return_value=expected) as trades:
            result = cockpit.ROUTES["/api/mobile/trades"]({
                "condition_id": [condition], "limit": ["20"]})
        self.assertEqual(result, expected)
        trades.assert_called_once_with(condition, "20", "")

    def test_batch_book_route_forwards_only_validated_market_condition(self):
        condition = "0x" + "a" * 64
        expected = (200, {"condition_id": condition, "books": []})
        with patch.object(cockpit, "api_mobile_market_books", return_value=expected) as books:
            result = cockpit.ROUTES["/api/mobile/books"]({"condition_id": [condition]})
        self.assertEqual(result, expected)
        books.assert_called_once_with(condition)

    def test_watchlist_book_route_forwards_bounded_condition_list(self):
        conditions = ["0x" + "a" * 64, "0x" + "b" * 64]
        raw = ",".join(conditions)
        expected = (200, {"markets": []})
        with patch.object(cockpit, "api_mobile_market_books_watchlist", return_value=expected) as books:
            result = cockpit.ROUTES["/api/mobile/books/watchlist"]({"condition_ids": [raw]})
        self.assertEqual(result, expected)
        books.assert_called_once_with(raw)

    def test_watchlist_books_reject_invalid_or_oversized_list_before_gamma(self):
        valid = "0x" + "a" * 64
        with patch.object(cockpit, "_mobile_market_for_condition") as lookup:
            invalid_status, invalid_payload = cockpit.api_mobile_market_books_watchlist(valid + ",bad")
            oversized_status, oversized_payload = cockpit.api_mobile_market_books_watchlist(
                ",".join([valid] * 13))
        self.assertEqual(invalid_status, 400)
        self.assertIn("condition ID", invalid_payload["error"])
        self.assertEqual(oversized_status, 400)
        self.assertIn("1 to 12", oversized_payload["error"])
        lookup.assert_not_called()

    def test_watchlist_books_fetch_multiple_markets_in_one_clob_batch_and_cache(self):
        conditions = ["0x" + "b" * 64, "0x" + "c" * 64]
        markets = {
            conditions[0]: {"condition_id": conditions[0], "outcomes": [
                {"token_id": "101"}, {"token_id": "102"}]},
            conditions[1]: {"condition_id": conditions[1], "outcomes": [
                {"token_id": "201"}, {"token_id": "202"}]},
        }
        stamp = cockpit.time.time() * 1000
        response = [
            {"asset_id": token, "market": condition, "timestamp": stamp,
             "bids": [{"price": "0.49", "size": "2"}],
             "asks": [{"price": "0.51", "size": "3"}]}
            for condition, tokens in ((conditions[0], ("101", "102")),
                                      (conditions[1], ("201", "202")))
            for token in tokens
        ]
        with patch.object(cockpit, "_mobile_market_for_condition",
                          side_effect=lambda condition: (markets[condition], False)) as lookup, \
                patch.object(cockpit, "_public_json", return_value=response) as request:
            status, payload = cockpit.api_mobile_market_books_watchlist(",".join(conditions))
            cached_status, cached_payload = cockpit.api_mobile_market_books_watchlist(",".join(conditions))

        self.assertEqual(status, 200)
        self.assertEqual(cached_status, 200)
        self.assertEqual(payload, cached_payload)
        self.assertEqual([row["condition_id"] for row in payload["markets"]], conditions)
        self.assertEqual([[book["best_bid"] for book in row["books"]]
                          for row in payload["markets"]], [[0.49, 0.49], [0.49, 0.49]])
        self.assertTrue(all(book["fresh"] for row in payload["markets"] for book in row["books"]))
        request.assert_called_once_with(
            "https://clob.polymarket.com/books", timeout=6.0,
            data=[{"token_id": token} for token in ("101", "102", "201", "202")])
        self.assertEqual(lookup.call_count, 2)

    def test_watchlist_books_uses_stale_single_market_cache_during_batch_cooldown(self):
        conditions = ["0x" + "d" * 64, "0x" + "e" * 64]
        cached_book = {"condition_id": conditions[0], "outcome_index": 0,
                       "best_bid": 0.48, "best_ask": 0.52, "fresh": True,
                       "stale": False, "book_age_s": 3.0,
                       "server_ts": int(cockpit.time.time() * 1000)}
        with cockpit._mobile_gamma_lock:
            cockpit._mobile_book_batch_cache["books:" + conditions[0]] = (
                cockpit.time.monotonic() - 5.0,
                {"condition_id": conditions[0], "books": [cached_book, None]})
            cockpit._mobile_book_retry_at = cockpit.time.monotonic() + 30.0
        with patch.object(cockpit, "_mobile_market_for_condition") as lookup, \
                patch.object(cockpit, "_public_json") as request:
            status, payload = cockpit.api_mobile_market_books_watchlist(",".join(conditions))

        self.assertEqual(status, 200)
        self.assertTrue(payload["stale"])
        self.assertEqual(payload["retry_after_seconds"], 30)
        self.assertEqual(payload["markets"][0]["books"][0]["best_bid"], 0.48)
        self.assertFalse(payload["markets"][0]["books"][0]["fresh"])
        self.assertTrue(payload["markets"][0]["books"][0]["stale"])
        self.assertEqual(payload["markets"][1]["books"], [None, None])
        self.assertIn("No cached book", payload["markets"][1]["error"])
        lookup.assert_not_called()
        request.assert_not_called()

    def test_watchlist_books_returns_stale_pair_when_clob_rate_limits_refresh(self):
        condition = "0x" + "f" * 64
        market = {"condition_id": condition, "outcomes": [
            {"token_id": "301"}, {"token_id": "302"}]}
        stamp = int(cockpit.time.time() * 1000)
        books = [
            {"condition_id": condition, "outcome_index": index,
             "best_bid": 0.47 + index / 100, "best_ask": 0.52 + index / 100,
             "fresh": True, "stale": False, "book_age_s": 2.0, "server_ts": stamp}
            for index in range(2)
        ]
        with cockpit._mobile_gamma_lock:
            cockpit._mobile_book_batch_cache["books:" + condition] = (
                cockpit.time.monotonic() - 10.0,
                {"condition_id": condition, "books": books})
        headers = Message()
        headers["Retry-After"] = "60"
        rate_limited = HTTPError("https://clob.polymarket.com/books", 429,
                                 "rate limited", headers, None)
        with patch.object(cockpit, "_mobile_market_for_condition", return_value=(market, False)), \
                patch.object(cockpit, "_public_json", side_effect=rate_limited) as request:
            status, payload = cockpit.api_mobile_market_books_watchlist(condition)

        self.assertEqual(status, 200)
        self.assertTrue(payload["stale"])
        self.assertEqual(payload["retry_after_seconds"], 60)
        self.assertTrue(all(book["stale"] and not book["fresh"]
                            for book in payload["markets"][0]["books"]))
        request.assert_called_once()

    def test_batch_book_rejects_invalid_condition_before_gamma_lookup(self):
        with patch.object(cockpit, "_mobile_market_for_condition") as lookup:
            status, payload = cockpit.api_mobile_market_books("invalid")
        self.assertEqual(status, 400)
        self.assertIn("condition", payload["error"])
        lookup.assert_not_called()

    def test_batch_book_rejects_invalid_gamma_token_ids_before_clob_request(self):
        condition = "0x" + "a" * 64
        market = {"condition_id": condition, "outcomes": [
            {"token_id": "123"}, {"token_id": "not-a-token"}]}
        with patch.object(cockpit, "_mobile_market_for_condition", return_value=(market, False)), \
                patch.object(cockpit, "_public_json") as request:
            status, payload = cockpit.api_mobile_market_books(condition)
        self.assertEqual(status, 502)
        self.assertIn("token IDs", payload["error"])
        request.assert_not_called()

    def test_market_explorer_batch_fetches_both_outcomes_in_one_clob_call(self):
        condition = "0x" + "b" * 64
        market = {"condition_id": condition, "outcomes": [
            {"token_id": "123456"}, {"token_id": "789012"}]}
        stamp = cockpit.time.time() * 1000
        response = [
            {"asset_id": "789012", "market": condition, "timestamp": str(stamp),
             "bids": [{"price": "0.47", "size": "8"}],
             "asks": [{"price": "0.52", "size": "9"}]},
            {"asset_id": "123456", "market": condition, "timestamp": str(stamp),
             "bids": [{"price": "0.48", "size": "10"}],
             "asks": [{"price": "0.51", "size": "12"}]},
        ]
        with patch.object(cockpit, "_mobile_market_for_condition", return_value=(market, False)), \
                patch.object(cockpit, "_public_json", return_value=response) as request:
            status, payload = cockpit.api_mobile_market_books(condition)

        self.assertEqual(status, 200)
        self.assertEqual([book["outcome_index"] for book in payload["books"]], [0, 1])
        self.assertEqual([book["best_bid"] for book in payload["books"]], [0.48, 0.47])
        self.assertEqual([book["best_ask"] for book in payload["books"]], [0.51, 0.52])
        self.assertEqual([book["best_bid_size"] for book in payload["books"]], [10.0, 8.0])
        self.assertEqual([book["best_ask_size"] for book in payload["books"]], [12.0, 9.0])
        self.assertEqual(payload["books"][0]["bid_levels"], [{"price": 0.48, "size": 10.0}])
        self.assertEqual(payload["books"][0]["ask_levels"], [{"price": 0.51, "size": 12.0}])
        self.assertAlmostEqual(payload["books"][0]["top_imbalance_pct"], -100.0 / 11.0)
        self.assertAlmostEqual(payload["books"][0]["microprice"], 0.49363636363636365)
        self.assertTrue(all(book["fresh"] for book in payload["books"]))
        self.assertFalse(payload["stale"])
        request.assert_called_once_with(
            "https://clob.polymarket.com/books", timeout=4.0,
            data=[{"token_id": "123456"}, {"token_id": "789012"}])

    def test_batch_book_depth_zero_and_missing_sizes_remain_distinct(self):
        condition = "0x" + "f" * 64
        now_ms = cockpit.time.time() * 1000
        zero = cockpit._mobile_book_summary(condition, 0, "123", {
            "asset_id": "123", "market": condition, "timestamp": now_ms,
            "bids": [{"price": "0.50", "size": "0"}],
            "asks": [{"price": "0.60", "size": "0"}],
        })
        missing = cockpit._mobile_book_summary(condition, 0, "123", {
            "asset_id": "123", "market": condition, "timestamp": now_ms,
            "bids": [{"price": "0.50"}], "asks": [{"price": "0.60"}],
        })
        self.assertEqual((zero["best_bid_size"], zero["best_ask_size"]), (0.0, 0.0))
        self.assertIsNone(zero["top_imbalance_pct"])
        self.assertIsNone(zero["microprice"])
        self.assertIsNone(missing["best_bid_size"])
        self.assertIsNone(missing["best_ask_size"])

    def test_batch_book_depth_ladder_is_sorted_validated_and_bounded(self):
        condition = "0x" + "e" * 64
        bids = [{"price": str(price / 100), "size": str(price)}
                for price in range(40, 51)]
        bids.extend([{"price": "0.50", "size": "2"},
                     {"price": "0.00", "size": "99"},
                     {"price": "0.49", "size": "-1"}])
        asks = [{"price": str(price / 100), "size": str(price)}
                for price in range(51, 62)]
        summary = cockpit._mobile_book_summary(condition, 0, "123", {
            "asset_id": "123", "market": condition,
            "bids": bids, "asks": asks,
        })

        self.assertEqual(len(summary["bid_levels"]), 8)
        self.assertEqual(len(summary["ask_levels"]), 8)
        self.assertEqual([row["price"] for row in summary["bid_levels"]],
                         [0.50, 0.49, 0.48, 0.47, 0.46, 0.45, 0.44, 0.43])
        self.assertEqual(summary["bid_levels"][0]["size"], 52.0)
        self.assertEqual([row["price"] for row in summary["ask_levels"]],
                         [0.51, 0.52, 0.53, 0.54, 0.55, 0.56, 0.57, 0.58])

    def test_market_explorer_batch_cache_avoids_repeat_clob_call(self):
        condition = "0x" + "c" * 64
        market = {"condition_id": condition, "outcomes": [
            {"token_id": "123"}, {"token_id": "456"}]}
        response = []
        with patch.object(cockpit, "_mobile_market_for_condition", return_value=(market, False)), \
                patch.object(cockpit, "_public_json", return_value=response) as request:
            first = cockpit.api_mobile_market_books(condition)
            second = cockpit.api_mobile_market_books(condition)
        self.assertEqual(first, second)
        request.assert_called_once()

    def test_market_explorer_batch_429_serves_stale_and_suppresses_repeat(self):
        condition = "0x" + "d" * 64
        market = {"condition_id": condition, "outcomes": [
            {"token_id": "123"}, {"token_id": "456"}]}
        headers = Message()
        headers["Retry-After"] = "5"
        rate_limited = HTTPError(
            "https://clob.example/books", 429, "Too Many Requests", headers, None)
        self.addCleanup(rate_limited.close)
        cached_payload = {"condition_id": condition, "books": [
            {"condition_id": condition, "outcome_index": 0, "fresh": True, "stale": False},
            {"condition_id": condition, "outcome_index": 1, "fresh": True, "stale": False}],
            "server_ts": 10, "stale": False, "retry_after_seconds": 0}
        with cockpit._mobile_gamma_lock:
            cockpit._mobile_book_batch_cache["books:" + condition] = (0.0, cached_payload)
        with patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit, "_mobile_market_for_condition", return_value=(market, False)), \
                patch.object(cockpit, "_public_json", side_effect=rate_limited) as request:
            status, stale = cockpit.api_mobile_market_books(condition)
            cached, again = cockpit.api_mobile_market_books(condition)

        self.assertEqual((status, cached), (200, 200))
        self.assertTrue(stale["stale"])
        self.assertEqual(stale["retry_after_seconds"], 5)
        self.assertTrue(all(not book["fresh"] and book["stale"] for book in stale["books"]))
        self.assertEqual(again["retry_after_seconds"], 5)
        request.assert_called_once()

    def test_market_explorer_batch_5xx_uses_market_cooldown(self):
        condition = "0x" + "e" * 64
        market = {"condition_id": condition, "outcomes": [
            {"token_id": "123"}, {"token_id": "456"}]}
        unavailable = HTTPError(
            "https://clob.example/books", 503, "Unavailable", Message(), None)
        self.addCleanup(unavailable.close)
        with patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit, "_mobile_market_for_condition", return_value=(market, False)), \
                patch.object(cockpit, "_public_json", side_effect=unavailable) as request:
            first_status, first = cockpit.api_mobile_market_books(condition)
            second_status, second = cockpit.api_mobile_market_books(condition)

        self.assertEqual(first_status, 502)
        self.assertEqual(first["retry_after_seconds"], 1)
        self.assertEqual(second_status, 503)
        self.assertEqual(second["retry_after_seconds"], 1)
        request.assert_called_once()

    def test_public_trades_reject_invalid_cursor_without_upstream_request(self):
        condition = "0x" + "c" * 64
        with patch.object(cockpit, "_polymarket_data_v2_get") as request:
            status, payload = cockpit.api_mobile_public_trades(condition, 20, "bad\ncursor")
        self.assertEqual(status, 400)
        self.assertIn("cursor", payload["error"])
        request.assert_not_called()

    def test_public_trades_normalize_fields_drop_mismatches_and_cap_limit(self):
        condition = "0x" + "d" * 64
        now = cockpit.time.time()
        wallet = "0x" + "a" * 40
        tx_hash = "0x" + "b" * 64
        response = {"data": [
            {"condition_id": condition, "side": "buy", "outcome": "Up", "price": "0.55",
             "size": "12.5", "timestamp": now, "proxy_wallet": wallet,
             "pseudonym": "<img src=x>", "transaction_hash": tx_hash},
            {"condition_id": "0x" + "e" * 64, "side": "SELL", "outcome": "Down",
             "price": 0.4, "size": 10, "timestamp": now},
            {"condition_id": condition, "side": "SELL", "outcome": "Bad price",
             "price": 1.1, "size": 10, "timestamp": now},
        ]}
        with patch.object(cockpit, "_polymarket_data_v2_get", return_value=response) as request:
            status, payload = cockpit.api_mobile_public_trades(condition.upper(), 200)
        self.assertEqual(status, 200)
        self.assertEqual(payload["condition_id"], condition)
        self.assertEqual(payload["limit"], 20)
        self.assertEqual(len(payload["trades"]), 1)
        trade = payload["trades"][0]
        self.assertEqual(trade["side"], "BUY")
        self.assertEqual(trade["outcome"], "Up")
        self.assertAlmostEqual(trade["gross_notional"], 6.875)
        self.assertEqual(trade["proxy_wallet"], wallet)
        self.assertEqual(trade["transaction_hash"], tx_hash)
        self.assertFalse(payload["stale"])
        self.assertEqual(payload["retry_after_seconds"], 0)
        request.assert_called_once_with("trades", {"condition": condition, "limit": 20})

    def test_public_trades_follow_signed_keyset_cursor_with_same_filter(self):
        condition = "0x" + "9" * 64
        now = cockpit.time.time()
        cursor = "opaque.signed_cursor-value"
        first_response = {
            "data": [{"condition_id": condition, "side": "BUY", "outcome": "Up",
                      "price": 0.6, "size": 2, "timestamp": now}],
            "pagination": {"has_more": True, "next_cursor": cursor},
        }
        second_response = {
            "data": [{"condition_id": condition, "side": "SELL", "outcome": "Up",
                      "price": 0.7, "size": 3, "timestamp": now - 1}],
            "pagination": {"has_more": False, "next_cursor": None},
        }
        with patch.object(cockpit, "_polymarket_data_v2_get",
                          side_effect=[first_response, second_response]) as request:
            first_status, first = cockpit.api_mobile_public_trades(condition, 20)
            second_status, second = cockpit.api_mobile_public_trades(
                condition, 20, first["next_cursor"])
        self.assertEqual((first_status, second_status), (200, 200))
        self.assertTrue(first["has_more"])
        self.assertEqual(first["next_cursor"], cursor)
        self.assertFalse(second["has_more"])
        self.assertIsNone(second["next_cursor"])
        self.assertEqual(request.call_args_list[0].args,
                         ("trades", {"condition": condition, "limit": 20}))
        self.assertEqual(request.call_args_list[1].args,
                         ("trades", {"condition": condition, "limit": 20, "cursor": cursor}))

    def test_public_trades_cache_avoids_duplicate_upstream_requests(self):
        condition = "0x" + "f" * 64
        response = {"data": []}
        with patch.object(cockpit, "_polymarket_data_v2_get", return_value=response) as request:
            first = cockpit.api_mobile_public_trades(condition, 20)
            second = cockpit.api_mobile_public_trades(condition, 20)
        self.assertEqual(first, second)
        request.assert_called_once_with("trades", {"condition": condition, "limit": 20})

    def test_public_trades_upstream_failure_returns_safe_error(self):
        condition = "0x" + "1" * 64
        with patch.object(cockpit, "_polymarket_data_v2_get", side_effect=OSError("private detail")):
            status, payload = cockpit.api_mobile_public_trades(condition, 20)
        self.assertEqual(status, 502)
        self.assertEqual(payload, {"error": "Polymarket public trades are temporarily unavailable"})

    def test_public_trades_rate_limit_honors_retry_after_and_serves_stale_page(self):
        condition = "0x" + "2" * 64
        headers = Message()
        headers["Retry-After"] = "5"
        rate_limited = HTTPError(
            "https://data-api.example/trades", 429, "Too Many Requests", headers, None)
        self.addCleanup(rate_limited.close)
        cache_key = (condition, 20, cockpit.hashlib.sha256(b"").hexdigest())
        old_payload = {"trades": [{"timestamp": 10}], "condition_id": condition,
                       "limit": 20, "has_more": False, "next_cursor": None,
                       "server_ts": 10, "stale": False, "retry_after_seconds": 0}
        with cockpit._mobile_public_trades_lock:
            cockpit._mobile_public_trades_cache[cache_key] = (0.0, old_payload)

        with patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit, "_polymarket_data_v2_get", side_effect=rate_limited) as request:
            stale_status, stale = cockpit.api_mobile_public_trades(condition, 20)
            limited_status, limited = cockpit.api_mobile_public_trades("0x" + "3" * 64, 20)

        self.assertEqual(stale_status, 200)
        self.assertTrue(stale["stale"])
        self.assertEqual(stale["retry_after_seconds"], 5)
        self.assertEqual(stale["trades"], old_payload["trades"])
        self.assertEqual(limited_status, 429)
        self.assertEqual(limited["retry_after_seconds"], 5)
        self.assertIn("rate limiting", limited["error"])
        request.assert_called_once()

    def test_public_trades_transient_failure_uses_page_cooldown(self):
        condition = "0x" + "4" * 64
        unavailable = HTTPError(
            "https://data-api.example/trades", 503, "Unavailable", Message(), None)
        self.addCleanup(unavailable.close)
        with patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit, "_polymarket_data_v2_get", side_effect=unavailable) as request:
            first_status, first = cockpit.api_mobile_public_trades(condition, 20)
            second_status, second = cockpit.api_mobile_public_trades(condition, 20)

        self.assertEqual(first_status, 502)
        self.assertEqual(first["retry_after_seconds"], 1)
        self.assertEqual(second_status, 503)
        self.assertEqual(second["retry_after_seconds"], 1)
        request.assert_called_once()

    def test_rate_limit_cooldown_serves_stale_cache_without_repeating_request(self):
        headers = Message()
        headers["Retry-After"] = "5"
        rate_limited = HTTPError("https://gamma.example/markets", 429, "Too Many Requests", headers, None)
        self.addCleanup(rate_limited.close)
        with cockpit._mobile_gamma_lock:
            cockpit._mobile_gamma_cache["catalog:test"] = (0.0, {"markets": ["cached"]})
            cockpit._mobile_gamma_cache["catalog:other"] = (0.0, {"markets": ["other cached"]})

        with patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit, "_public_json", side_effect=rate_limited) as request:
            first = cockpit._mobile_cached_json("catalog:test", "https://gamma.example/markets", ttl=1.0)
            second = cockpit._mobile_cached_json("catalog:test", "https://gamma.example/markets", ttl=1.0)
            other_key = cockpit._mobile_cached_json("catalog:other", "https://gamma.example/markets", ttl=1.0)
            with self.assertRaises(cockpit._MobileUpstreamCooldown):
                cockpit._mobile_cached_json("catalog:empty", "https://gamma.example/markets", ttl=1.0)

        self.assertEqual(first, ({"markets": ["cached"]}, True))
        self.assertEqual(second, ({"markets": ["cached"]}, True))
        self.assertEqual(other_key, ({"markets": ["other cached"]}, True))
        request.assert_called_once()
        self.assertEqual(cockpit._mobile_gamma_retry_at["catalog:test"], 105.0)
        self.assertEqual(cockpit._mobile_gamma_host_retry_at, 105.0)

    def test_cooldown_without_cached_data_fails_fast_until_retry_time(self):
        headers = Message()
        headers["Retry-After"] = "4"
        rate_limited = HTTPError("https://gamma.example/markets", 429, "Too Many Requests", headers, None)
        self.addCleanup(rate_limited.close)
        with patch.object(cockpit.time, "monotonic", return_value=100.0), \
                patch.object(cockpit, "_public_json", side_effect=rate_limited) as request:
            with self.assertRaises(HTTPError):
                cockpit._mobile_cached_json("catalog:empty", "https://gamma.example/markets")
            with self.assertRaises(cockpit._MobileUpstreamCooldown):
                cockpit._mobile_cached_json("catalog:empty", "https://gamma.example/markets")

        request.assert_called_once()

    def test_retry_after_date_and_transient_backoff_are_bounded(self):
        headers = Message()
        headers["Retry-After"] = "Thu, 01 Jan 1970 00:01:50 GMT"
        rate_limited = HTTPError("https://gamma.example/markets", 429, "Too Many Requests", headers, None)
        unavailable = HTTPError("https://gamma.example/markets", 503, "Unavailable", Message(), None)
        self.addCleanup(rate_limited.close)
        self.addCleanup(unavailable.close)

        with patch.object(cockpit.time, "time", return_value=100.0):
            self.assertEqual(cockpit._mobile_gamma_retry_delay(rate_limited, 1), 10.0)
        self.assertEqual(cockpit._mobile_gamma_retry_delay(unavailable, 1), 0.5)
        self.assertEqual(cockpit._mobile_gamma_retry_delay(unavailable, 2), 1.0)
        headers.replace_header("Retry-After", "9999")
        self.assertEqual(cockpit._mobile_gamma_retry_delay(rate_limited, 1), 300.0)

    def test_unversioned_markets_tab_uses_safe_read_only_catalog_presentation(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn('id="marketExplorerQuery"', page)
        self.assertIn('onsubmit="event.preventDefault();searchMarketExplorer(true)"', page)
        self.assertIn("/api/mobile/markets?${params}", page)
        self.assertIn('aria-label="Gamma-reported last trade prices"', page)
        self.assertIn("esc(String(m.question||", page)
        self.assertIn("https://polymarket.com/event/${encodeURIComponent(slug)}", page)
        self.assertIn("READ-ONLY DISCOVERY", page)
        self.assertIn('class="mkt-explorer-rules"', page)
        self.assertIn("Venue-provided metadata shown verbatim", page)
        self.assertIn("new URL(rawResolutionSource)", page)
        self.assertIn("resolution_source", page)
        self.assertIn("/api/mobile/trades?${query.toString()}", page)
        self.assertIn("/api/mobile/books?${query.toString()}", page)
        self.assertIn("Fetching both outcomes in one CLOB request", page)
        self.assertIn("TOP SHARES · BID / ASK", page)
        self.assertIn("MICROPRICE", page)
        self.assertIn("Top-level imbalance and microprice use best-level shares", page)
        self.assertIn("book?.best_bid_size==null?NaN", page)
        self.assertIn("book?.top_imbalance_pct==null?NaN", page)
        self.assertIn("book?.book_age_s==null?NaN", page)
        self.assertIn("loadOlderMarketExplorerTrades", page)
        self.assertIn("LOAD OLDER TRADES", page)
        self.assertIn("_fetchMarketExplorerTrades", page)
        self.assertIn("cached trade", page)
        self.assertIn("Retry in about", page)
        self.assertIn("POLYMARKET DATA API · PUBLIC TRADES", page)
        self.assertIn("not this bot’s fills", page)
        self.assertIn("Polygonscan", page)
        self.assertIn("tradeCache:Object.create(null)", page)
        self.assertNotIn("marketExplorerOrder", page)

    def test_public_trade_rollup_is_bounded_to_loaded_reported_sides(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("LOADED ROW SIDE SPLIT", page)
        self.assertIn("trade?.gross_notional==null?NaN:Number(trade.gross_notional)", page)
        self.assertIn("Gross share × price across the loaded rows", page)
        self.assertIn("it is not labeled as aggressor flow", page)
        self.assertIn("mkt-explorer-trade-split-bar", page)
        self.assertIn(".slice(0,200)", page)
        self.assertIn("Showing the newest 200 loaded rows", page)


if __name__ == "__main__":
    unittest.main()
