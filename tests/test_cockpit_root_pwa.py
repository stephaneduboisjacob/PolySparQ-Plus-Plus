"""The main cockpit install assets stay same-origin and behind cockpit auth."""
import base64
import hashlib
import json
from http.server import ThreadingHTTPServer
from pathlib import Path
import struct
import sys
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import cockpit


class RootCockpitPwaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        password_hash = hashlib.sha256(b"test-password").hexdigest()
        cls.users_patch = patch.object(cockpit, "_load_users", return_value={
            "tester": {"active": True, "pw_hash": password_hash, "role": "admin"}
        })
        cls.hash_patch = patch.object(cockpit, "COCKPIT_PW_HASH", "")
        cls.users_patch.start()
        cls.hash_patch.start()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), cockpit.Handler)
        cls.server.daemon_threads = True
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base_url = f"http://127.0.0.1:{cls.server.server_port}"
        credentials = base64.b64encode(b"tester:test-password").decode("ascii")
        cls.auth_header = f"Basic {credentials}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.hash_patch.stop()
        cls.users_patch.stop()

    def request(self, path, authenticated=True):
        headers = {"Authorization": self.auth_header} if authenticated else {}
        with urlopen(Request(self.base_url + path, headers=headers), timeout=3) as response:
            return response.status, response.headers, response.read()

    def test_manifest_and_icons_are_served_only_after_authentication(self):
        with self.assertRaises(HTTPError) as unauthenticated:
            urlopen(self.base_url + "/manifest.webmanifest", timeout=3)
        self.assertEqual(unauthenticated.exception.code, 401)
        unauthenticated.exception.close()

        status, headers, body = self.request("/manifest.webmanifest")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get_content_type(), "application/manifest+json")
        manifest = json.loads(body)
        self.assertEqual(manifest["start_url"], "/")
        self.assertEqual(manifest["scope"], "/")
        self.assertEqual(manifest["display"], "standalone")
        self.assertEqual([icon["sizes"] for icon in manifest["icons"]], ["192x192", "512x512"])
        self.assertTrue(all(icon["src"].startswith("/cockpit-assets/") for icon in manifest["icons"]))

        for path, size in (("/cockpit-assets/icon-192.png", (192, 192)),
                           ("/cockpit-assets/icon-512.png", (512, 512)),
                           ("/cockpit-assets/apple-touch-icon.png", (180, 180))):
            with self.subTest(path=path):
                status, headers, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertEqual(headers.get_content_type(), "image/png")
                self.assertTrue(body.startswith(b"\x89PNG\r\n\x1a\n"))
                self.assertEqual(struct.unpack(">II", body[16:24]), size)

    def test_unknown_root_asset_is_not_a_generic_file_route(self):
        with self.assertRaises(HTTPError) as response:
            urlopen(Request(self.base_url + "/cockpit-assets/../../cockpit.py",
                            headers={"Authorization": self.auth_header}), timeout=3)
        self.assertEqual(response.exception.code, 404)
        response.exception.close()

    def test_shell_declares_root_install_metadata(self):
        html = Path(cockpit.UI_FILE).read_text(encoding="utf-8")
        self.assertIn('<link rel="manifest" href="/manifest.webmanifest">', html)
        self.assertIn('<link rel="icon" type="image/png" sizes="192x192" href="/cockpit-assets/icon-192.png">', html)
        self.assertIn('<link rel="apple-touch-icon" href="/cockpit-assets/apple-touch-icon.png">', html)
        self.assertIn("Android Chrome: open the browser menu", html)
        self.assertIn("private market, account, and bot data are not saved for offline use", html)
        self.assertNotIn("serviceWorker.register", html)


if __name__ == "__main__":
    unittest.main()
