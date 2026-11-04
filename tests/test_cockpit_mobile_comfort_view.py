"""The unversioned root cockpit offers a persistent, phone-friendly reading view."""
from pathlib import Path
import unittest


PAGE = Path(__file__).resolve().parents[1] / "scripts" / "cockpit_ui.html"


class RootMobileComfortViewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = PAGE.read_text(encoding="utf-8")

    def test_more_drawer_exposes_an_accessible_local_layout_toggle(self):
        self.assertIn('id="mobileDensityToggle" aria-pressed="true"', self.page)
        self.assertIn('aria-label="Switch to compact mobile reading"', self.page)
        self.assertIn("onclick=\"toggleMobileDensity()\"", self.page)
        self.assertIn("id=\"mobileDensityState\"", self.page)
        self.assertLess(self.page.index('id="mobileDensityToggle"'),
                        self.page.index('id="mobileMoreSearch"'))

    def test_preference_defaults_to_comfortable_and_persists_only_on_device(self):
        self.assertIn("const MOBILE_DENSITY_KEY='polysparq_mobile_density_v1'", self.page)
        self.assertIn("localStorage.getItem(MOBILE_DENSITY_KEY)!=='compact'", self.page)
        self.assertIn("localStorage.setItem(MOBILE_DENSITY_KEY,enabled?'comfortable':'compact')", self.page)
        self.assertIn("function toggleMobileDensity()", self.page)
        self.assertIn("document.body.classList.toggle('mobile-comfort-view',enabled)", self.page)

    def test_comfortable_styles_apply_only_to_mobile_and_enlarge_dense_text(self):
        start = self.page.index('<style id="mobile-comfort-view">')
        styles = self.page[start:self.page.index("</style>", start)]
        self.assertIn("@media(max-width:850px)", styles)
        self.assertIn("@media(max-width:700px)", styles)
        self.assertIn("font-size:max(9px,1em)!important", styles)
        self.assertIn("#mobileDock .mobile-dock-item{min-height:58px", styles)
        self.assertIn("#statsBar .sb-label{font-size:10px", styles)
        self.assertIn("#tabManualpred .pred-quick-jump label{font-size:10px", styles)


if __name__ == "__main__":
    unittest.main()
