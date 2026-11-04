"""Prediction focus sharing exposes only the selected root-cockpit state."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class PredictionFocusShareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = (ROOT / "scripts" / "cockpit_ui.html").read_text(encoding="utf-8")
        start = cls.page.index("async function sharePredictionFocus(){")
        end = cls.page.index("\n}\nwindow.addEventListener('popstate'", start) + 2
        cls.share = cls.page[start:end]

    def test_focus_share_builds_clean_unversioned_root_link(self):
        self.assertIn("const url=new URL('/',location.origin)", self.share)
        self.assertIn("url.searchParams.set(PREDICTION_FOCUS_PARAM,`${asset}:${horizon}:${period}`)", self.share)
        self.assertNotIn("location.href", self.share)
        self.assertNotIn("location.pathname", self.share)

    def test_focus_share_stays_whitelisted_and_uses_existing_share_fallbacks(self):
        self.assertIn("['BTC','ETH','SOL','XRP'].includes(asset)", self.share)
        self.assertIn("![300,900].includes(horizon)", self.share)
        self.assertIn("!['CURRENT','NEXT'].includes(period)", self.share)
        self.assertIn("navigator.share({title,text:", self.share)
        self.assertIn("navigator.clipboard.writeText(url.href)", self.share)

    def test_prediction_focus_controls_meet_phone_touch_target(self):
        self.assertIn("#tabManualpred .pred-focus-actions .pred-share-focus,#tabManualpred .pred-focus-actions .pred-show-all{min-width:0;min-height:44px", self.page)

    def test_saved_prediction_filters_sync_before_first_network_response(self):
        init = self.page.index("// ── Init ──")
        sync = self.page.index("syncPredictionFilters();", init)
        stream = self.page.index("connectSSE();", init)

        self.assertLess(sync, stream)
        self.assertIn("if(quickAsset)quickAsset.value=S.predAsset", self.page)
        self.assertIn("if(quickWindow)quickWindow.value=", self.page)


if __name__ == "__main__":
    unittest.main()
