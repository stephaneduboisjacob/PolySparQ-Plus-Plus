"""The root Predictions page keeps one-market and multi-select controls clear."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PredictionFilterModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = (ROOT / "scripts" / "cockpit_ui.html").read_text(encoding="utf-8")

    def test_mobile_quick_selectors_are_distinguished_from_multi_select_filters(self):
        toolbar_start = self.page.index('<div class="pred-toolbar">')
        toolbar_end = self.page.index('<div id="predictionQuickJump"', toolbar_start)
        toolbar = self.page[toolbar_start:toolbar_end]

        self.assertIn('id="predictionMultiFilters"', toolbar)
        self.assertIn('id="predictionMultiFilterToggle" aria-expanded="false" aria-controls="predictionMultiFilterBody"', toolbar)
        self.assertIn('id="predictionMultiFilterBody"', toolbar)
        self.assertIn('role="group" aria-label="Filter predictions by asset"', toolbar)
        self.assertIn('role="group" aria-label="Toggle prediction windows"', toolbar)
        self.assertIn('aria-label="Quick-select one asset or all assets"', self.page)
        self.assertIn('aria-label="Quick-select one window or all windows"', self.page)

    def test_phone_filter_details_collapse_without_removing_multi_select(self):
        self.assertIn("#tabManualpred .pred-multi-filter-body{display:none;min-width:0}", self.page)
        self.assertIn("#tabManualpred .pred-multi-filters.open .pred-multi-filter-body{display:grid;", self.page)
        self.assertIn("#tabManualpred .pred-multi-filter-toggle{display:grid;", self.page)
        self.assertIn("min-height:44px", self.page[self.page.index("#tabManualpred .pred-multi-filter-toggle{display:grid;"):])
        self.assertIn("#tabManualpred .pred-multi-filter-body{display:flex;align-items:flex-end;", self.page)

    def test_filter_summary_and_accessibility_state_follow_saved_selection(self):
        start = self.page.index("function togglePredictionMultiFilters(){")
        end = self.page.index("\nfunction focusPredictionMarket", start)
        filter_logic = self.page[start:end]

        self.assertIn("panel.classList.toggle('open')", filter_logic)
        self.assertIn("toggle.setAttribute('aria-expanded',String(expanded))", filter_logic)
        self.assertIn("if(icon)icon.textContent=expanded?'⌃':'⌄'", filter_logic)
        self.assertIn("if(summary)summary.textContent=`${assetLabel} · ${windowLabel}`", filter_logic)
        self.assertIn("if(quickAsset)quickAsset.value=S.predAsset", filter_logic)
        self.assertIn("if(quickWindow)quickWindow.value=", filter_logic)
        self.assertIn("const actions=$('predictionFocusActions');", filter_logic)


if __name__ == "__main__":
    unittest.main()
