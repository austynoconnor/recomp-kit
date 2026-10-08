"""Browser regressions for idle startup, session ownership and worker teardown.

Uses a tiny worker fixture instead of loading any game or WebAssembly module.
"""
from functools import partial
from http.server import ThreadingHTTPServer
import importlib.util
import hashlib
import json
import shutil
from pathlib import Path
import tempfile
import threading
import unittest

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sync_playwright = None

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("player_web_launcher", ROOT / "tools/web_launcher.py")
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)


class QuietHandler(launcher.IsolatedHandler):
    def log_message(self, *args):
        pass


@unittest.skipUnless(sync_playwright, "Install Playwright and Chrome for browser regressions")
class WebPlayerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        directory = Path(self.temp.name) / "stub"
        directory.mkdir()
        shell = (ROOT / "web/player/index.html").read_text().replace("@TITLE@", "Test game")
        (directory / "index.html").write_text(shell)
        # The worker's document owns it. Unloading that document must end its
        # heartbeat, which exercises Stop independently of game cooperation.
        (directory / "runtime.html").write_text('''<!doctype html><script>
          const source = 'setInterval(() => postMessage(1), 50)';
          const worker = new Worker(URL.createObjectURL(new Blob([source], {type:'text/javascript'})));
          worker.onmessage = () => parent.workerBeats = (parent.workerBeats || 0) + 1;
        </script>''')
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=self.temp.name))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.playwright = sync_playwright().start()
        self.browser = self.playwright.chromium.launch(channel="chrome", headless=True)
        self.context = self.browser.new_context(viewport={"width": 3840, "height": 2160})
        self.url = f"http://127.0.0.1:{self.server.server_port}/stub/"

    def tearDown(self):
        self.browser.close()
        self.playwright.stop()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.temp.cleanup()

    def page(self):
        page = self.context.new_page()
        page.goto(self.url)
        return page

    def test_open_and_reload_stay_idle(self):
        page = self.page()
        page.wait_for_timeout(250)
        self.assertEqual(page.locator("iframe").count(), 0)
        page.locator("#start").click()
        page.wait_for_function("window.workerBeats > 1")
        bounds = page.locator("iframe").bounding_box()
        self.assertLessEqual(bounds["width"], 1280)
        self.assertLessEqual(bounds["height"], 720)
        page.reload()
        page.wait_for_timeout(250)
        self.assertEqual(page.locator("iframe").count(), 0)
        self.assertTrue(page.locator("#start").is_enabled())

    def test_duplicate_session_and_stop_release(self):
        first, second = self.page(), self.page()
        first.locator("#start").click()
        first.wait_for_function("window.workerBeats > 1")
        second.locator("#start").click()
        second.wait_for_function("document.querySelector('#message').textContent.includes('another tab')")
        self.assertEqual(second.locator("iframe").count(), 0)
        first.locator("#stop").click()
        first.wait_for_function("document.querySelector('#start').disabled === false")
        first.wait_for_timeout(200)
        beats = first.evaluate("window.workerBeats")
        first.wait_for_timeout(400)
        self.assertEqual(first.evaluate("window.workerBeats"), beats)
        second.locator("#start").click()
        second.wait_for_function("window.workerBeats > 1")

    def test_closed_owner_releases_session(self):
        first, second = self.page(), self.page()
        first.locator("#start").click()
        first.wait_for_function("window.workerBeats > 1")
        first.close()
        second.locator("#start").click()
        second.wait_for_function("window.workerBeats > 1")

    def test_runtime_cannot_bypass_player_as_a_top_level_page(self):
        runtime = Path(self.temp.name) / "stub/runtime.html"
        runtime.write_text((ROOT / "web/player/runtime.html").read_text())
        page = self.context.new_page()
        page.goto(self.url + "runtime.html")
        page.wait_for_function("document.querySelector('#status').textContent.includes('Start game')")
        self.assertEqual(page.locator("script[src]").count(), 0)
        self.assertFalse(page.evaluate("!!window.Module"))

    def hosted_fixture(self, wrong_size=False):
        """Exercise real download, validation and OPFS code with tiny game files."""
        root = Path(self.temp.name)
        for name in ("worker.js", "core.js"):
            shutil.copy(ROOT / "web/launcher" / name, root / name)
        game = {"id": "stub", "title": "Test game", "executable": "GAME.EXE",
                "sha256": hashlib.sha256(b"test executable").hexdigest(),
                "requiredDirs": ["data"], "exclude": [], "minFreeMb": 0,
                "hostedAssets": "./stub/assets.json"}
        assets = {"GAME.EXE": b"test executable", "data/map.bin": b"test map"}
        entries = []
        for name, data in assets.items():
            path = root / "stub" / name
            path.parent.mkdir(exist_ok=True, parents=True)
            path.write_bytes(data)
            size = len(data) + (1 if wrong_size and name == "data/map.bin" else 0)
            entries.append({"path": name, "size": size, "mtime": 1, "isDir": False,
                            "url": "/stub/" + name})
        manifest = root / "stub/assets.json"
        manifest.write_text(json.dumps({"files": entries}))
        source = (ROOT / "web/player/runtime.html").read_text()
        prepare = "async function prepareGame(game) {" + source.split(
            "async function prepareGame(game) {", 1)[1].split("async function start()", 1)[0]
        (root / "stub/download.html").write_text('<p id="status"></p><script>\n'
            'function status(text) { document.querySelector("#status").textContent = text; }\n'
            + prepare + '\nprepareGame(' + json.dumps(game) + ').then(() => window.result="ready")'
            '.catch(error => window.result=error.message);</script>')
        return manifest

    def test_first_visit_downloads_and_cached_visit_needs_no_source(self):
        self.hosted_fixture()
        page = self.context.new_page()
        page.goto(self.url + "download.html")
        page.wait_for_function("window.result !== undefined")
        self.assertEqual(page.evaluate("window.result"), "ready")
        # A repeat visit must use validated OPFS even when the source is offline.
        (Path(self.temp.name) / "stub/GAME.EXE").unlink()
        (Path(self.temp.name) / "stub/data/map.bin").unlink()
        page.reload()
        page.wait_for_function("window.result !== undefined")
        self.assertEqual(page.evaluate("window.result"), "ready")

    def test_short_download_fails_and_can_resume(self):
        manifest = self.hosted_fixture(wrong_size=True)
        page = self.context.new_page()
        page.goto(self.url + "download.html")
        page.wait_for_function("window.result !== undefined")
        self.assertIn("Incomplete download", page.evaluate("window.result"))
        data = json.loads(manifest.read_text())
        data["files"][1]["size"] -= 1
        manifest.write_text(json.dumps(data))
        page.reload()
        page.wait_for_function("window.result !== undefined")
        self.assertEqual(page.evaluate("window.result"), "ready")


if __name__ == "__main__":
    unittest.main()
