"""Root Predictions keeps a bounded, read-only browser-session forecast trail."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PredictionSessionTrailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = (ROOT / "scripts" / "cockpit_ui.html").read_text()

    def test_trail_is_captured_only_from_fresh_distinct_server_boards(self):
        self.assertIn("capturePredictionBoardTrail(r);", self.page)
        self.assertIn("board.stale||board.warming_up||S.predFeedError", self.page)
        self.assertIn("generated>now+1500", self.page)
        self.assertIn("generated-last.ts<PREDICTION_TRAIL_SAMPLE_MS", self.page)
        self.assertIn("PREDICTION_TRAIL_MAX_POINTS=300", self.page)

    def test_model_and_fresh_clob_midpoint_remain_separate_series(self):
        self.assertIn("independent_up_probability??card.up_probability??card.settlement_model_probability", self.page)
        self.assertIn("bookAge>=0&&bookAge<=3?rawClob:null", self.page)
        self.assertIn("class=\"prediction-trail-model\"", self.page)
        self.assertIn("class=\"prediction-trail-clob\"", self.page)
        self.assertIn("CLOB MID UP", self.page)

    def test_long_observation_gaps_break_both_chart_series(self):
        self.assertIn("const PREDICTION_TRAIL_GAP_MS=12000", self.page)
        self.assertIn("point.ts-modelPreviousTs>PREDICTION_TRAIL_GAP_MS)modelSegment=false", self.page)
        self.assertIn("point.ts-previousTs>PREDICTION_TRAIL_GAP_MS", self.page)

    def test_phone_chart_uses_large_stacked_legend_rows(self):
        self.assertIn("#tabManualpred .prediction-trail-legend{grid-template-columns:minmax(0,1fr);gap:4px;margin-top:7px}", self.page)
        self.assertIn("#tabManualpred .prediction-trail-legend span{font-size:10px}", self.page)
        self.assertIn("#tabManualpred .prediction-trail-body>p{font-size:10px}", self.page)

    def test_session_scope_and_root_page_only_disclosure_are_explicit(self):
        self.assertIn("PAGE-SESSION PROBABILITY HISTORY", self.page)
        self.assertIn("clears on reload", self.page)
        self.assertIn("descriptive only", self.page)
        self.assertIn("data-pred-detail=\"session-trail\"", self.page)
        self.assertNotIn("localStorage.setItem('predictionTrails'", self.page)


    def test_inline_shift_uses_only_a_recent_fresh_session_comparison(self):
        self.assertIn("function predictionSessionShiftHtml(card,currentModel,ready)", self.page)
        self.assertIn("if(!ready||!board||board.stale||board.warming_up||S.predFeedError)return''", self.page)
        self.assertIn("boardAge>freshLimit", self.page)
        self.assertIn("elapsed>PREDICTION_TRAIL_GAP_MS/1000", self.page)
        self.assertIn("versus the previous fresh page-session sample", self.page)
        self.assertIn("const probabilityShift=predictionSessionShiftHtml(c,modelUp,ready)", self.page)
        self.assertIn('class="forecast-shift ${tone}"', self.page)

    def test_phone_probability_line_can_wrap_the_delta_chip_cleanly(self):
        self.assertIn("#tabManualpred .forecast-prob-line{display:flex;flex-wrap:wrap", self.page)
        self.assertIn("#tabManualpred .forecast-prob-line .forecast-shift{display:inline-flex", self.page)


if __name__ == "__main__":
    unittest.main()
