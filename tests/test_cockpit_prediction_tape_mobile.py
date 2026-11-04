"""Root Prediction Window Tape controls remain legible and thumb-sized on phones."""
from pathlib import Path
import unittest


PAGE = Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html"


class RootPredictionTapeMobileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = PAGE.read_text(encoding="utf-8")

    def test_phone_mode_controls_keep_readable_text_and_large_targets(self):
        start = self.page.index('<style id="prediction-tape-mobile-controls">')
        styles = self.page[start:self.page.index("</style>", start)]
        self.assertIn("@media(max-width:700px)", styles)
        self.assertIn("min-height:46px", styles)
        self.assertIn("font-size:9px!important", styles)
        self.assertIn("overflow-wrap:anywhere", styles)

    def test_all_five_metrics_remain_available_with_accessible_selected_state(self):
        self.assertIn("data-tape-mode=\"mid\" aria-pressed=\"true\"", self.page)
        self.assertIn("data-tape-mode=\"spread\" aria-pressed=\"false\"", self.page)
        self.assertIn("data-tape-mode=\"imbalance\" aria-pressed=\"false\"", self.page)
        self.assertIn("data-tape-mode=\"flow\" aria-pressed=\"false\"", self.page)
        self.assertIn("data-tape-mode=\"parity\" aria-pressed=\"false\"", self.page)
        self.assertIn("el.setAttribute('aria-pressed',String(active))", self.page)


if __name__ == "__main__":
    unittest.main()
