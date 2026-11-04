"""Root Predictions surfaces the age of each existing model/feed input."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RootPredictionInputAgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = (ROOT / "scripts" / "cockpit_ui.html").read_text()

    def test_source_ages_use_existing_per_market_fields_and_limits(self):
        for source, field, limit in (
            ("CHAINLINK", "c.reference_age_s", 5),
            ("1m CANDLES", "c.candle_age_s", 120),
            ("UP BOOK", "c.polymarket_up_book_age_s", 3),
            ("DOWN BOOK", "c.polymarket_down_book_age_s", 3),
        ):
            self.assertIn(f"ageChip('{source}',{field},{limit},", self.page)
        self.assertIn("const exchangeAgeLimit=Number(c.horizon_seconds)===300?5:null", self.page)
        self.assertIn("ageChip('COINBASE',c.coinbase_age_s,exchangeAgeLimit,exchangeAgePurpose)", self.page)
        self.assertIn("ageChip('BINANCE',c.binance_age_s,exchangeAgeLimit,exchangeAgePurpose)", self.page)
        self.assertIn("exchange ages on 15m cards are context only", self.page)
        render_start = self.page.index("innerHTML=cards.map(c=>{")
        self.assertGreater(self.page.index("const exchangeAgeLimit=", render_start), render_start)

    def test_age_chips_explain_unknown_clock_and_order_gate_boundaries(self):
        self.assertIn("function tickPredictionSourceAges(now=Date.now())", self.page)
        self.assertIn("data-age-state=\"${state}\"", self.page)
        self.assertIn("CLOCK +${Math.abs(n).toFixed(1)}s", self.page)
        self.assertIn("RECORDED SAMPLE AGE · DIAGNOSTIC ONLY", self.page)
        self.assertIn("not exchange event timestamps or order gates", self.page)
        self.assertIn("Ages follow recorded source timestamps", self.page)
        self.assertIn("data-age-state=\"unknown\"", self.page)
        self.assertIn("Number.isFinite(limit)?age<=limit?'current':'aged':'observed'", self.page)
        self.assertIn("context only, no freshness threshold applies", self.page)

    def test_feature_is_in_the_unversioned_root_prediction_page(self):
        self.assertIn('id="tabManualpred"', self.page)
        self.assertNotIn("/api/v3/manual_predictions", self.page)


if __name__ == "__main__":
    unittest.main()
