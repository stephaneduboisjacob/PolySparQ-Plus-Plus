"""Protect the readable phone-sized type in root Predictions cards."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PredictionMobileReadabilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        page = (ROOT / "scripts" / "cockpit_ui.html").read_text(encoding="utf-8")
        start = page.index('<style id="prediction-mobile-comfort">')
        end = page.index("</style>", start)
        cls.mobile_styles = page[start:end]
        pulse_start = page.index(".prediction-pulse-horizons button{min-width:54px}")
        pulse_end = page.index("#tabManualpred .pred-pair-caveat", pulse_start)
        cls.pulse_styles = page[pulse_start:pulse_end]

    def test_market_benchmarks_keep_large_values_and_wrapping_labels(self):
        self.assertIn(".benchmark-cell small{font-size:10px;line-height:1.4;white-space:normal;overflow:visible}", self.mobile_styles)
        self.assertIn(".benchmark-cell strong{font-size:15px}", self.mobile_styles)
        self.assertIn(".benchmark-cell.benchmark-bbo{grid-column:1/-1}", self.mobile_styles)
        self.assertIn(".benchmark-book-row strong{font-size:13px}", self.mobile_styles)

    def test_input_trace_labels_and_values_do_not_fall_back_to_dense_type(self):
        self.assertIn(".forecast-input small{font-size:9px;line-height:1.4;white-space:normal;overflow:visible;overflow-wrap:anywhere}", self.mobile_styles)
        self.assertIn(".forecast-input strong{font-size:12px;line-height:1.4;white-space:normal;overflow:visible;overflow-wrap:anywhere}", self.mobile_styles)
        self.assertIn(".forecast-input em{font-size:9px;line-height:1.4;white-space:normal}", self.mobile_styles)

    def test_live_window_pulse_cards_use_full_width_on_phones(self):
        self.assertIn("#tabManualpred .prediction-pulse-grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:7px}", self.pulse_styles)
        self.assertIn("@media(max-width:480px){\n  #tabManualpred .prediction-pulse-grid{grid-template-columns:minmax(0,1fr);gap:8px}", self.pulse_styles)
        self.assertIn("#tabManualpred .prediction-pulse-controls label{font-size:10px}", self.pulse_styles)
        self.assertIn("#tabManualpred .prediction-pulse-controls select{font-size:11px}", self.pulse_styles)


if __name__ == "__main__":
    unittest.main()
