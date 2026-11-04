"""Root cockpit exposes a browser-native mobile install action when available."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PAGE = ROOT / "scripts" / "cockpit_ui.html"


class MobileInstallPromptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.page = PAGE.read_text(encoding="utf-8")

    def test_install_action_is_hidden_until_browser_offers_install(self):
        self.assertIn('id="mobileInstallApp" data-mobile-action="install"', self.page)
        self.assertIn("id=\"mobileInstallApp\" data-mobile-action=\"install\" onclick=\"mobileNavAction('install')\" hidden", self.page)
        self.assertIn("window.addEventListener('beforeinstallprompt',event=>{", self.page)
        self.assertIn("event.preventDefault();\n  _deferredCockpitInstallPrompt=event", self.page)
        self.assertIn("const button=$('mobileInstallApp');if(button){button.hidden=false;filterMobileMoreTabs();}", self.page)
        self.assertIn("button.dataset.mobileAction!=='install'||!!_deferredCockpitInstallPrompt", self.page)
        self.assertIn("button.hidden=false;filterMobileMoreTabs()", self.page)

    def test_install_prompt_and_browser_installed_state_are_handled(self):
        self.assertIn("async function installCockpitPwa()", self.page)
        self.assertIn("await event.prompt()", self.page)
        self.assertIn("const choice=await event.userChoice", self.page)
        self.assertIn("window.addEventListener('appinstalled'", self.page)
        self.assertIn("if(action==='install')void installCockpitPwa()", self.page)

    def test_manual_android_and_ios_fallback_remains_available(self):
        self.assertIn("if(!event){if(help)help.open=true", self.page)
        self.assertIn("Android Chrome: open the browser menu", self.page)
        self.assertIn("iPhone Safari: use <strong>Share → Add to Home Screen</strong>", self.page)
        self.assertNotIn("serviceWorker.register", self.page)


if __name__ == "__main__":
    unittest.main()
