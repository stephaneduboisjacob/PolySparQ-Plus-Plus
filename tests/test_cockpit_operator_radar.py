"""Operator Radar must fail closed when its alert evidence is missing or stale."""
from datetime import datetime, timedelta
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import cockpit


class OperatorRadarTests(unittest.TestCase):
    def setUp(self):
        self.status = {
            "mode": "paper",
            "running": True,
            "health_stale": 0,
            "health_total": 4,
            "health_age_s": 12.5,
        }
        self.positions = {"positions": []}
        self.markets = {"markets": []}
        self.health = {"oracle_source": "chainlink", "parsefail": 0}

    def test_fresh_complete_healthy_snapshot_can_have_no_alerts(self):
        self.assertEqual(
            cockpit.build_operator_radar(self.status, self.positions, self.markets, self.health),
            [],
        )

    def test_missing_or_invalid_book_health_is_an_explicit_warning(self):
        for changes in (
            {"health_stale": None, "health_total": None, "health_age_s": None},
            {"health_stale": 0, "health_total": 0},
            {"health_stale": 5, "health_total": 4},
        ):
            with self.subTest(changes=changes):
                status = {**self.status, **changes}
                alerts = cockpit.build_operator_radar(status, self.positions, self.markets, self.health)
                self.assertTrue(any(a["title"] == "BOOK HEALTH UNKNOWN" for a in alerts))

    def test_old_book_health_sample_is_not_reported_as_current(self):
        status = {**self.status, "health_age_s": 91.0}
        alerts = cockpit.build_operator_radar(status, self.positions, self.markets, self.health)
        health = next(a for a in alerts if a["title"] == "BOOK HEALTH UNKNOWN")
        self.assertIn("91s old", health["detail"])

    def test_fresh_stale_books_keep_their_warn_or_critical_level(self):
        status = {**self.status, "health_stale": 2}
        alerts = cockpit.build_operator_radar(status, self.positions, self.markets, self.health)
        health = next(a for a in alerts if a["title"] == "STALE BOOKS")
        self.assertEqual(health["level"], "warn")
        self.assertIn("2/4 stale", health["detail"])

        status = {**self.status, "health_stale": 4}
        alerts = cockpit.build_operator_radar(status, self.positions, self.markets, self.health)
        health = next(a for a in alerts if a["title"] == "STALE BOOKS")
        self.assertEqual(health["level"], "critical")

    def test_book_health_staleness_boundary_is_explicit(self):
        at_limit = {**self.status, "health_age_s": 90.0}
        self.assertFalse(any(
            a["title"] == "BOOK HEALTH UNKNOWN"
            for a in cockpit.build_operator_radar(at_limit, self.positions, self.markets, self.health)
        ))
        beyond_limit = {**self.status, "health_age_s": 90.1}
        self.assertTrue(any(
            a["title"] == "BOOK HEALTH UNKNOWN"
            for a in cockpit.build_operator_radar(beyond_limit, self.positions, self.markets, self.health)
        ))

    def test_stopped_engine_is_visible_even_when_no_other_radar_condition_fires(self):
        status = {**self.status, "running": False}
        alerts = cockpit.build_operator_radar(status, self.positions, self.markets, self.health)
        service = next(a for a in alerts if a["title"] == "BOT ENGINE STOPPED")
        self.assertEqual(service["level"], "info")

    def test_log_health_age_parser_uses_the_local_log_timestamp(self):
        now = datetime(2026, 10, 26, 12, 1, 0)
        line = "[2026-10-26 12:00:30.250] [info] orderbook health: 4/4 fresh"
        self.assertAlmostEqual(cockpit.log_line_age_seconds(line, now), 29.75)
        self.assertIsNone(cockpit.log_line_age_seconds("orderbook health: 4/4 fresh", now))

    def test_status_log_to_radar_preserves_sample_age_and_counts(self):
        logged_at = datetime.now() - timedelta(seconds=13, milliseconds=400)
        stamp = logged_at.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        line = f"[{stamp}] [info] orderbook health: 2/4 stale"
        positions = {"positions": [], "accounting_errors": []}
        bot = {"running": True, "pid": 123, "instances_running": []}
        with patch.object(cockpit, "current_mode", return_value="paper"), \
             patch.object(cockpit, "bot_status_detail", return_value=bot), \
             patch.object(cockpit, "accounting_since", return_value=None), \
             patch.object(cockpit, "q", return_value=None), \
             patch.object(cockpit, "tail", return_value=[line]), \
             patch.object(cockpit, "starting_balance_usd", return_value=100.0):
            status = cockpit.api_status(position_snapshot=positions)

        self.assertEqual((status["health_stale"], status["health_total"]), (2, 4))
        self.assertGreaterEqual(status["health_age_s"], 13.0)
        self.assertLess(status["health_age_s"], 15.0)
        alerts = cockpit.build_operator_radar(status, positions, self.markets, self.health)
        books = next(alert for alert in alerts if alert["kind"] == "books")
        self.assertEqual(books["title"], "STALE BOOKS")
        self.assertIn("2/4 stale", books["detail"])
        self.assertIn("health sample 13s old", books["detail"])

    def test_root_radar_names_missing_and_stale_data_as_unknown(self):
        page = (ROOT / "scripts" / "cockpit_ui.html").read_text()
        self.assertIn("function updateOperatorRadarFreshness(now=Date.now())", page)
        self.assertIn("OPERATOR_RADAR_SNAPSHOT_STALE_MS=5000", page)
        self.assertIn("Alert snapshot unavailable", page)
        self.assertIn("Last snapshot was clear", page)
        self.assertIn("No configured conditions fired in the latest complete snapshot.", page)
        self.assertIn("font:700 10px/1.4 var(--mono)", page)
        self.assertIn("@media(max-width:580px){.radar-item{grid-template-columns:minmax(0,1fr) auto", page)
        self.assertIn('id="mobileMoreAlert"', page)
        self.assertIn("function updateMobileRadarIndicator(state,count=0,level='info')", page)
        self.assertIn("${n} operator alert${n===1?'':'s'}, ${severityLabel} ${level}, snapshot ${state}", page)
        self.assertIn('onclick="${esc(action)}">OPEN</button>', page)
        self.assertIn('id="mobileRadarDigest"', page)
        self.assertIn("function updateMobileRadarDigest(state,alerts=[],ageSeconds=null)", page)
        self.assertIn('id="mobileRadarDigestAge" class="mobile-radar-digest-age" aria-live="off" hidden', page)
        self.assertIn("These alerts are from a stale snapshot; current alert state is unknown.", page)
        self.assertIn("`${state==='stale'?'SNAPSHOT STALE':'SNAPSHOT AGE'} · ${age}s`", page)
        self.assertIn("title.textContent='Last snapshot was clear';", page)
        self.assertIn('id="mobileMoreSearch" type="search" aria-label="Search cockpit sections"', page)
        self.assertNotIn('<span class="pred-sr-only">Search cockpit sections</span>', page)
        self.assertIn("font:650 14px/1.25 Inter,system-ui,sans-serif", page)
        self.assertIn("font:550 9px/1.1 Inter,system-ui,sans-serif!important", page)
        self.assertIn(".mobile-more-item b{font:600 12px Inter,system-ui,sans-serif}", page)
        self.assertIn("updateMobileRadarIndicator('stale',count,level);", page)
        self.assertIn("updateOperatorRadarFreshness();", page)
        self.assertNotIn("Feeds, positions, and markets are inside normal bounds", page)


if __name__ == "__main__":
    unittest.main()
