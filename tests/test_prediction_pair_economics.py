"""Read-only per-market UP/DOWN complete-set estimate tests."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import prediction_board


class BinaryPairEconomicsTests(unittest.TestCase):
    def test_prediction_card_labels_parameter_and_assumed_taker_fills(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_v5.html").read_text()
        root_page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        board_source = (Path(__file__).resolve().parents[1] / "scripts" / "prediction_board.py").read_text()
        self.assertIn("Taker fee parameter / top ask depth", page)
        self.assertIn("Assumes both shares take liquidity at the current top asks", page)
        self.assertIn("not the effective percent of trade value", page)
        self.assertIn("taker rebates are credited", root_page)
        self.assertIn("VISIBLE-BOOK SIZE WALK", root_page)
        self.assertIn("pair.size_scenarios", root_page)
        self.assertIn("BID/ASK/SPR (¢)", root_page)
        self.assertIn("bookBbo(c.polymarket_up_bid,c.polymarket_up_price,c.book_age_s)", root_page)
        self.assertIn("TOP-OF-BOOK SHARE DEPTH", root_page)
        self.assertIn("MODEL-BASED SIZE WALK · RESEARCH ONLY", root_page)
        self.assertIn("directional_outcome_economics", root_page)
        self.assertIn("polymarket_up_book_age_s", board_source)
        self.assertIn("polymarket_down_book_age_s", board_source)
        self.assertIn("'polymarket_up_book_age_s':current_req.get('up_book_age_s')", board_source)
        self.assertIn("'polymarket_down_book_age_s':next_req.get('down_book_age_s')", board_source)
        self.assertIn("ASK BOOK · ${bookAge.toFixed(1)}s", root_page)
        self.assertIn("STALE · ${bookAge.toFixed(1)}s", root_page)
        self.assertIn("@media(max-width:360px)", root_page)
        self.assertIn("binary_directional_economics", board_source)
        self.assertEqual(board_source.count("_directional_economics_for_request("), 3)
        self.assertIn("polymarket_up_bid_depth':current_req.get('up_bid_depth')", board_source)
        self.assertIn("polymarket_down_ask_depth':next_req.get('down_ask_depth')", board_source)

    def test_top_depth_counts_only_size_at_the_selected_best_price(self):
        book = {
            "bids": [
                {"price": .59, "size": 2}, {"price": .58, "size": 100},
                {"price": .59, "size": 3}, {"price": .59, "size": float("nan")},
                {"price": .59, "size": float("inf")}, {"price": .59, "size": "invalid"},
                {"price": .59, "size": -4},
            ],
            "asks": [{"price": .61, "size": 4}],
        }
        self.assertEqual(prediction_board.top_depth(book, "bids", .59), 5)
        self.assertEqual(prediction_board.top_depth(book, "asks", .61), 4)

    def test_both_taker_fees_and_paired_top_ask_depth_are_included(self):
        result = prediction_board.binary_pair_economics(.45, .50, 8, 10, .07, 1)
        self.assertTrue(result["available"])
        self.assertAlmostEqual(result["gross_cost_per_pair"], .95)
        self.assertAlmostEqual(result["estimated_fee_per_pair"], .03483)
        self.assertAlmostEqual(result["net_cost_per_pair"], .98483)
        self.assertAlmostEqual(result["net_edge_per_pair"], .01517)
        self.assertEqual(result["top_ask_pair_depth"], 8)
        self.assertEqual(result["visible_pair_depth"], 8)
        self.assertTrue(result["size_scenarios"][0]["available"])
        self.assertFalse(result["size_scenarios"][2]["available"])
        self.assertEqual(result["fee_rounding"], "conservative_up_1e-5_usdc")

    def test_size_scenarios_walk_both_ask_books_and_round_each_leg_fee(self):
        result = prediction_board.binary_pair_economics(
            .40, .50, 2, 4, .07, 1,
            up_ask_levels=[{"price": .50, "size": 10}, {"price": .40, "size": 2}],
            down_ask_levels=[{"price": .60, "size": 10}, {"price": .50, "size": 4}],
        )
        one, five, ten, twenty_five = result["size_scenarios"]
        self.assertEqual([s["shares"] for s in result["size_scenarios"]], [1, 5, 10, 25])
        self.assertEqual(result["top_ask_pair_depth"], 2)
        self.assertEqual(result["visible_pair_depth"], 12)
        self.assertTrue(one["available"])
        self.assertAlmostEqual(one["net_cost"], .9343)
        self.assertAlmostEqual(five["gross_cost"], 4.9)
        self.assertAlmostEqual(five["estimated_fees"], .1729)
        self.assertAlmostEqual(five["net_cost"], 5.0729)
        self.assertAlmostEqual(five["total_net_edge"], -.0729)
        self.assertAlmostEqual(ten["net_cost"], 10.7444)
        self.assertFalse(twenty_five["available"])
        self.assertEqual(twenty_five["up_visible_shares"], 12)
        self.assertEqual(twenty_five["down_visible_shares"], 14)

    def test_missing_quotes_or_fee_schedule_fail_closed(self):
        self.assertFalse(prediction_board.binary_pair_economics(
            None, .5, 10, 10, .07, 1)["available"])
        self.assertFalse(prediction_board.binary_pair_economics(
            .4, .5, 10, 10, -1, 1)["available"])
        self.assertFalse(prediction_board.binary_pair_economics(
            .4, .5, 10, 10, .07, 2)["available"])

    def test_directional_size_walk_prices_both_sides_across_visible_asks(self):
        result = prediction_board.binary_directional_economics(
            .70, .50, .40, 5, 10, .02, 1,
            up_ask_levels=[{"price": .60, "size": 3}, {"price": .50, "size": 2}],
            down_ask_levels=[{"price": .40, "size": 10}],
        )
        self.assertTrue(result["available"])
        self.assertEqual(result["sides"]["UP"]["visible_ask_shares"], 5)
        self.assertEqual(result["sides"]["DOWN"]["model_probability"], .3)
        up_one, up_five, _up_ten, _up_twenty_five = result["sides"]["UP"]["scenarios"]
        self.assertAlmostEqual(up_one["net_cost"], .505)
        self.assertAlmostEqual(up_one["expected_net_value"], .195)
        self.assertAlmostEqual(up_five["gross_cost"], 2.8)
        self.assertAlmostEqual(up_five["estimated_fees"], .0244)
        self.assertAlmostEqual(up_five["break_even_probability"], .56488)
        down_five = result["sides"]["DOWN"]["scenarios"][1]
        self.assertAlmostEqual(down_five["expected_net_value"], -.524)
        self.assertFalse(result["sides"]["UP"]["scenarios"][2]["available"])

    def test_directional_size_walk_rejects_bad_model_or_fee_inputs(self):
        for probability, rate, exponent in ((None, .02, 1), (1.01, .02, 1),
                                            (.7, -1, 1), (.7, .02, 2)):
            with self.subTest(probability=probability, rate=rate, exponent=exponent):
                result = prediction_board.binary_directional_economics(
                    probability, .50, .40, 10, 10, rate, exponent,
                    up_ask_levels=[{"price": .5, "size": 10}],
                    down_ask_levels=[{"price": .4, "size": 10}],
                )
                self.assertFalse(result["available"])

    def test_directional_size_walk_uses_only_the_selected_fresh_side_book(self):
        levels = [{"price": .4, "size": 10}]
        for age in (0, .5, 3):
            self.assertEqual(prediction_board._fresh_side_ask_levels(
                {"up_book_age_s": age}, "UP", levels), levels)
        for age in (None, -1, 3.01, "invalid"):
            with self.subTest(age=age):
                self.assertEqual(prediction_board._fresh_side_ask_levels(
                    {"up_book_age_s": age}, "UP", levels), [])
        result = prediction_board.binary_directional_economics(
            .7, .4, None, 10, None, .02, 1,
            up_ask_levels=prediction_board._fresh_side_ask_levels(
                {"up_book_age_s": .5}, "UP", levels),
            down_ask_levels=prediction_board._fresh_side_ask_levels(
                {"down_book_age_s": 3.01}, "DOWN", levels),
        )
        self.assertTrue(result["available"])
        self.assertTrue(result["sides"]["UP"]["scenarios"][0]["available"])
        self.assertFalse(result["sides"]["DOWN"]["scenarios"][0]["available"])
        self.assertIsNone(result["sides"]["DOWN"]["visible_ask_shares"])

    def test_current_and_next_card_size_walks_share_side_freshness_gate(self):
        current_request = {
            "up_ask": .4, "up_ask_depth": 10, "up_book_age_s": .5,
            # A stale quote must be discarded even if a caller still has values.
            "down_ask": .3, "down_ask_depth": 10, "down_book_age_s": 3.01,
            "fee_rate": .02, "fee_exponent": 1,
        }
        next_request = {
            "up_ask": .5, "up_ask_depth": 10, "up_book_age_s": 1.2,
            "down_ask": .4, "down_ask_depth": 10, "down_book_age_s": 1.4,
            "fee_rate": .02, "fee_exponent": 1,
        }
        current = prediction_board._directional_economics_for_request(
            .7, current_request,
            {"UP": [{"price": .4, "size": 10}],
             "DOWN": [{"price": .3, "size": 10}]})
        upcoming = prediction_board._directional_economics_for_request(
            .6, next_request,
            {"UP": [{"price": .5, "size": 10}],
             "DOWN": [{"price": .4, "size": 10}]})

        self.assertTrue(current["sides"]["UP"]["scenarios"][0]["available"])
        self.assertFalse(current["sides"]["DOWN"]["scenarios"][0]["available"])
        self.assertIsNone(current["sides"]["DOWN"]["visible_ask_shares"])
        self.assertTrue(upcoming["sides"]["UP"]["scenarios"][0]["available"])
        self.assertTrue(upcoming["sides"]["DOWN"]["scenarios"][0]["available"])
        self.assertIsNone(prediction_board._directional_economics_for_request(
            .6, next_request, {"UP": [], "DOWN": []}, ready=False))

    def test_pair_estimate_is_cleared_without_fresh_book_age(self):
        for book_age in (None, -1, 3.01, "invalid"):
            with self.subTest(book_age=book_age):
                board = {
                    "generated_at": 100_000,
                    "inputs_at": 100_000,
                    "cards": [{
                        "window_open": 100_000,
                        "window_close": 400_000,
                        "reference_age_s": 0,
                        "candle_age_s": 0,
                        "book_age_s": book_age,
                        "polymarket_up": .6,
                        "polymarket_up_bid": .59,
                        "polymarket_up_bid_depth": 3,
                        "polymarket_up_ask_depth": 4,
                        "polymarket_down_bid_depth": 5,
                        "polymarket_down_ask_depth": 6,
                        "paired_outcome_economics": {"available": True, "net_edge_per_pair": .01},
                        "directional_outcome_economics": {"available": True},
                    }],
                }
                result = prediction_board.age_board(board, now_ms=100_000)
                self.assertIsNone(result["cards"][0]["paired_outcome_economics"])
                self.assertIsNone(result["cards"][0]["directional_outcome_economics"])
                for field in ("polymarket_up", "polymarket_up_bid",
                              "polymarket_up_bid_depth", "polymarket_up_ask_depth",
                              "polymarket_down_bid_depth", "polymarket_down_ask_depth"):
                    self.assertIsNone(result["cards"][0][field], field)

        fresh = {
            "generated_at": 100_000, "inputs_at": 100_000,
            "cards": [{
                "window_open": 100_000, "window_close": 400_000,
                "reference_age_s": 0, "candle_age_s": 0, "book_age_s": .5,
                "polymarket_up_book_age_s": .5,
                "polymarket_down_book_age_s": 3.01,
                "directional_outcome_economics": {"available": True},
            }],
        }
        result = prediction_board.age_board(fresh, now_ms=100_000)
        self.assertEqual(result["cards"][0]["directional_outcome_economics"], {"available": True})
        self.assertEqual(result["cards"][0]["polymarket_up_book_age_s"], .5)
        self.assertEqual(result["cards"][0]["polymarket_down_book_age_s"], 3.01)


if __name__ == "__main__":
    unittest.main()
