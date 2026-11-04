"""Root window-tape export stays local, provenance-labeled, and read-only."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "scripts" / "cockpit_ui.html"


class RootPredictionTapeExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = PAGE.read_text(encoding="utf-8")

    def test_export_is_disabled_until_a_window_snapshot_is_loaded(self):
        self.assertIn(
            'id="predictionTapeExport" aria-label="Download the current local market tape as JSON"',
            self.page,
        )
        self.assertIn('onclick="exportPredictionTapeJson()" disabled', self.page)
        self.assertIn("const exportButton=$('predictionTapeExport');if(exportButton)exportButton.disabled=false", self.page)
        self.assertIn("state.market=null;state.serverTs=0;state.selectedBucket=null", self.page)
        self.assertIn("if(exportButton)exportButton.disabled=true", self.page)

    def test_export_contains_raw_local_window_data_and_explicit_caveats(self):
        self.assertIn("function exportPredictionTapeJson()", self.page)
        self.assertIn("schema:'polysparq.window-tape.v1'", self.page)
        self.assertIn("snapshot_as_of_utc:capturedAt.toISOString()", self.page)
        self.assertIn("sampling_seconds:15", self.page)
        self.assertIn("market:m", self.page)
        self.assertIn("'Reported trade side and BBO tick-rule aggressor inference are distinct fields", self.page)
        self.assertIn("'Book midpoint and flow summaries are descriptive research data, not executable quotes", self.page)
        self.assertIn("'Large-print rows are individual public prints without wallet identity", self.page)
        self.assertIn("link.download=filename", self.page)

    def test_export_reuses_existing_read_route_and_adds_no_execution_route(self):
        self.assertIn("source:'PolySparQ++ local stored market tape via /api/market_motion'", self.page)
        self.assertIn("/api/market_motion?${qs}", self.page)
        self.assertNotIn("/api/market_motion/export", self.page)
        self.assertNotIn("predictionTapeExportOrder", self.page)

    def test_phone_export_actions_are_large_and_fit_a_two_column_footer(self):
        style_start = self.page.index('<style id="prediction-tape-export">')
        styles = self.page[style_start:self.page.index("</style>", style_start)]
        self.assertIn("grid-template-columns:repeat(2,minmax(0,1fr))", styles)
        self.assertIn("min-height:46px", styles)
        self.assertIn("overflow-wrap:anywhere", styles)


if __name__ == "__main__":
    unittest.main()
