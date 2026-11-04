"""Polymarket editorial headlines stay descriptive and outside forecast signals."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cockpit
import prediction_board


class PredictionNewsContextTests(unittest.TestCase):
    def test_oracle_rss_is_registered_as_context_only(self):
        self.assertIn(
            ("The Oracle by Polymarket", "https://news.polymarket.com/feed", True),
            cockpit.PREDICTION_NEWS_FEEDS,
        )

    def test_editorial_rss_parses_headline_but_forces_neutral_sentiment(self):
        payload = b"""<?xml version='1.0'?>
        <rss version='2.0'><channel><item>
          <title>Bitcoin crash</title>
          <link>https://news.polymarket.com/p/bitcoin-outlook</link>
          <pubDate>Fri, 23 Oct 2026 14:31:44 GMT</pubDate>
        </item></channel></rss>"""

        editorial, = cockpit._parse_prediction_rss(
            "The Oracle by Polymarket", payload, context_only=True,
            now_s=2_000_000_000,
        )
        crypto_feed, = cockpit._parse_prediction_rss(
            "CoinDesk", payload, context_only=False, now_s=2_000_000_000,
        )

        self.assertEqual(editorial["sentiment"], 0)
        self.assertTrue(editorial["context_only"])
        self.assertNotEqual(crypto_feed["sentiment"], 0)
        self.assertFalse(crypto_feed["context_only"])

    def test_editorial_story_does_not_change_news_signal(self):
        now = 2_000_000_000
        editorial = [{
            "title": "Bitcoin crash warning",
            "published": now,
            "sentiment": -2,
            "context_only": True,
        }]
        eligible = [{**editorial[0], "context_only": False}]

        self.assertEqual(prediction_board._news_signal("BTC", editorial, now), (0.0, 0))
        score, matched = prediction_board._news_signal("BTC", eligible, now)
        self.assertLess(score, 0)
        self.assertEqual(matched, 1)

    def test_recent_editorial_gets_one_display_slot_and_old_items_are_omitted(self):
        now = 2_000_000_000
        regular = [
            {"source": "CoinDesk", "title": f"Bitcoin market update {i}",
             "published": now - i, "sentiment": 0, "context_only": False}
            for i in range(30)
        ]
        recent_editorial = {
            "source": "The Oracle by Polymarket", "title": "Bitcoin policy outlook",
            "published": now - 3 * 86400, "sentiment": 0, "context_only": True,
        }
        stale_editorial = {
            "source": "The Oracle by Polymarket", "title": "Old Bitcoin outlook",
            "published": now - 15 * 86400, "sentiment": 0, "context_only": True,
        }

        bounded = cockpit._bounded_prediction_news(
            regular + [recent_editorial, stale_editorial], now
        )
        self.assertEqual(len(bounded), 24)
        self.assertEqual(sum(bool(row.get("context_only")) for row in bounded), 1)
        self.assertIn("Bitcoin policy outlook", {row["title"] for row in bounded})
        self.assertNotIn("Old Bitcoin outlook", {row["title"] for row in bounded})

        visible = prediction_board._news_for_display(bounded, now)
        self.assertEqual(len(visible), 8)
        self.assertEqual(sum(bool(row.get("context_only")) for row in visible), 1)
        self.assertEqual(visible[0]["title"], "Bitcoin market update 0")

    def test_malformed_publication_time_does_not_break_news_bounding(self):
        rows = [
            {"title": "Regular headline", "published": "not-a-time", "context_only": False},
            {"title": "Editorial headline", "published": "not-a-time", "context_only": True},
        ]
        bounded = cockpit._bounded_prediction_news(rows, 2_000_000_000)
        self.assertEqual([row["title"] for row in bounded], ["Regular headline"])
        self.assertEqual(
            [row["title"] for row in prediction_board._news_for_display(rows, 2_000_000_000)],
            ["Regular headline"],
        )

    def test_ui_labels_editorial_and_news_context_as_non_predictive(self):
        page = (Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("EDITORIAL CONTEXT ONLY", page)
        self.assertIn("HEADLINES · NOT MODEL INPUTS", page)


if __name__ == "__main__":
    unittest.main()
