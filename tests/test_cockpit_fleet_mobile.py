"""Regression checks for the unversioned root Fleet phone layout."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RootFleetMobileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = (ROOT / "scripts" / "cockpit_ui.html").read_text()

    def test_phone_cards_prioritize_net_pnl_and_thumb_sized_controls(self):
        self.assertIn('id="fleet-mobile-operator-cards"', self.page)
        self.assertIn("#tabFleet #fleetGrid{grid-template-columns:minmax(0,1fr)", self.page)
        self.assertIn("#tabFleet #fleetGrid .fleet-stats>div:first-child{grid-column:1/-1", self.page)
        self.assertIn(".fleet-controls .fleet-btn{display:inline-flex;width:100%;min-width:0;min-height:46px", self.page)
        self.assertIn("font-size:11px!important;line-height:1.35;text-align:center", self.page)
        self.assertIn("#tabFleet #fleetGrid .fleet-wallet .fw-keys{grid-column:4;grid-row:2;min-height:44px", self.page)
        self.assertIn("#tabFleet #fleetGrid .bot-mode-btn{min-width:52px;min-height:44px;padding:7px 9px;font-size:10px", self.page)

    def test_wallet_and_strategy_selectors_reflow_without_changing_handlers(self):
        self.assertIn('class="fleet-subcontrol fleet-strategy-control"', self.page)
        self.assertIn('class="fleet-subcontrol fleet-config-control"', self.page)
        self.assertIn(".fleet-subcontrol .fleet-btn{min-height:44px", self.page)
        self.assertIn("setBotWalletMode('${esc(inst)}','live',this)", self.page)
        self.assertIn("Enable LIVE real-money trading for ${inst}?", self.page)

    def test_running_controls_and_accounting_scope_remain_explicit(self):
        self.assertIn("botCtl('${esc(inst)}','start')", self.page)
        self.assertIn("botCtl('${esc(inst)}','stop')", self.page)
        self.assertIn("botCtl('${esc(inst)}','restart')", self.page)
        self.assertIn("Current/last session realized net after fees; open-position market movement is excluded.", self.page)


if __name__ == "__main__":
    unittest.main()
