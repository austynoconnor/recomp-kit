"""tools/web_launcher.py writes the hub page and each game's launcher data."""

import importlib.util
import gzip
import json
import hashlib
from functools import partial
from http.server import ThreadingHTTPServer
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import threading
from urllib.request import urlopen, Request
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("web_launcher", ROOT / "tools/web_launcher.py")
web_launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web_launcher)


class WebLauncherTests(unittest.TestCase):
    def test_hosted_assets_validate_and_expose_only_game_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "game"
            root.mkdir()
            (root / "data").mkdir()
            (root / "data/a b.dat").write_bytes(b"asset data")
            (root / "STUB.EXE").write_bytes(b"test executable")
            (root / "secret.txt").write_text("excluded")
            game = {"id": "stub", "executable": "STUB.EXE",
                    "sha256": hashlib.sha256(b"test executable").hexdigest(),
                    "requiredDirs": ["data"], "exclude": ["*.txt"]}
            out = Path(tmp) / "site"
            routes = web_launcher.hosted_assets([game], {"stub": root}, out)
            self.assertEqual(len(routes), 2)
            manifest = json.loads((out / "stub/assets.json").read_text())
            self.assertEqual(sum(e["size"] for e in manifest["files"]), 25)
            self.assertTrue(game["hostedAssets"].endswith("stub/assets.json"))
            server = ThreadingHTTPServer(("127.0.0.1", 0), partial(
                web_launcher.IsolatedHandler, directory=str(out), asset_files=routes))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                with urlopen(base + "/_game-assets/stub/data/a%20b.dat") as response:
                    self.assertEqual(response.read(), b"asset data")
                    self.assertEqual(response.headers["Cross-Origin-Embedder-Policy"], "require-corp")
                for route in ("/_game-assets/stub/secret.txt", "/_game-assets/stub/../secret.txt"):
                    with self.assertRaises(HTTPError) as error:
                        urlopen(base + route)
                    self.assertEqual(error.exception.code, 404)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
            game["sha256"] = "0" * 64
            with self.assertRaises(ValueError):
                web_launcher.hosted_assets([game], {"stub": root}, out)

    def test_exported_assets_are_copied_with_relative_urls(self):
        # --export-assets: the site carries the game files itself, for a static
        # host; URLs are relative to assets.json and nothing is routed.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "game"
            (root / "data").mkdir(parents=True)
            (root / "data/a b.dat").write_bytes(b"asset data")
            (root / "STUB.EXE").write_bytes(b"test executable")
            (root / "secret.txt").write_text("excluded")
            game = {"id": "stub", "executable": "STUB.EXE",
                    "sha256": hashlib.sha256(b"test executable").hexdigest(),
                    "requiredDirs": ["data"], "exclude": ["*.txt"]}
            out = Path(tmp) / "site"
            routes = web_launcher.hosted_assets([game], {"stub": root}, out, export=True)
            self.assertEqual(routes, {})
            manifest = json.loads((out / "stub/assets.json").read_text())
            urls = sorted(e["url"] for e in manifest["files"])
            self.assertEqual(urls, ["assets/STUB.EXE", "assets/data/a%20b.dat"])
            self.assertEqual((out / "stub/assets/data/a b.dat").read_bytes(), b"asset data")
            self.assertFalse((out / "stub/assets/secret.txt").exists())
            self.assertEqual(game["assetBase"], "../stub/assets")

    def test_range_reads_head_and_invalid_ranges(self):
        with tempfile.TemporaryDirectory() as tmp:
            asset = Path(tmp) / "archive.bin"
            asset.write_bytes(bytes(range(256)))
            route = "/_game-assets/stub/archive.bin"
            server = ThreadingHTTPServer(("127.0.0.1", 0), partial(
                web_launcher.IsolatedHandler, directory=tmp, asset_files={route: asset}))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            url = f"http://127.0.0.1:{server.server_port}" + route
            try:
                for request, expected in (("bytes=10-19", bytes(range(10, 20))),
                                          ("bytes=250-999", bytes(range(250, 256))),
                                          ("bytes=-3", bytes(range(253, 256))),
                                          ("bytes=253-", bytes(range(253, 256)))):
                    with urlopen(Request(url, headers={"Range": request})) as response:
                        self.assertEqual(response.status, 206)
                        self.assertEqual(response.read(), expected)
                        self.assertEqual(int(response.headers["Content-Length"]), len(expected))
                with urlopen(Request(url, method="HEAD", headers={"Range": "bytes=0-"})) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.headers["Content-Length"], "256")
                    self.assertEqual(response.headers["Accept-Ranges"], "bytes")
                    self.assertEqual(response.read(), b"")
                # FetchFS's URL join can contain two slashes before a file.
                with urlopen(url.replace("stub/", "stub//")) as response:
                    self.assertEqual(response.read(), bytes(range(256)))
                for requested in ("bytes=256-", "bytes=20-10", "bytes=-0", "bytes=0-2,4-5", "bad"):
                    with self.assertRaises(HTTPError) as error:
                        urlopen(Request(url, headers={"Range": requested}))
                    self.assertEqual(error.exception.code, 416)
                    self.assertEqual(error.exception.headers["Content-Range"], "bytes */256")
            finally:
                server.shutdown()
                server.server_close()
                thread.join()

    def test_hub_for_two_games(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            other = tmp / "other"
            other.mkdir()
            text = (ROOT / "games/stub/game.toml").read_text().replace('id = "stub"', 'id = "other"')
            (other / "game.toml").write_text(text + '\n[launcher]\ntitle = "Other"\nstore = "https://x.test"\n'
                                                    'input_hints = { keyboard = "WASD: drive" }\n'
                                                    '\n[setup]\nrequired_dirs = ["data"]\n')
            shutil.copy(ROOT / "games/stub/globals.toml", other / "globals.toml")
            out = tmp / "site"
            games = web_launcher.build([ROOT / "games/stub", other], out)
            self.assertEqual([g["id"] for g in games], ["stub", "other"])
            for name in web_launcher.PAGE_FILES:
                self.assertTrue((out / name).is_file(), name)
            data = json.loads((out / "games.json").read_text())
            self.assertEqual(data[1]["title"], "Other")
            self.assertEqual(data[1]["requiredDirs"], ["data"])
            self.assertEqual(data[0]["executable"], "STUB.EXE")
            self.assertEqual(data[0]["sha256"], "0" * 64)
            self.assertEqual(data[1]["inputHints"], {"keyboard": "WASD: drive"})
            self.assertIsNone(data[0]["inputHints"])
            with self.assertRaises(ValueError):
                web_launcher.build([ROOT / "games/stub", ROOT / "games/stub"], tmp / "dup")

    def test_web_build_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            build = tmp / "build"
            build.mkdir()
            with self.assertRaises(ValueError):
                web_launcher.copy_web_build("stub", build, tmp / "site")
            for name in ("index.html", "runtime.html", "App.js", "App.wasm", "App.data", "libgen.a"):
                (build / name).write_text(name)
            web_launcher.copy_web_build("stub", build, tmp / "site")
            copied = sorted(f.name for f in (tmp / "site/stub").iterdir())
            self.assertEqual(copied, ["App.data", "App.js", "App.wasm", "index.html", "runtime.html"])

    def test_program_files_are_sent_compressed_and_assets_carry_an_etag(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            build = tmp / "build"
            build.mkdir()
            (build / "index.html").write_text("page")
            program = b"(module)" * 4096
            (build / "App.wasm").write_bytes(program)
            site = tmp / "site"
            web_launcher.copy_web_build("stub", build, site)
            self.assertEqual(gzip.decompress((site / "stub/App.wasm.gz").read_bytes()), program)
            asset = tmp / "archive.bin"
            asset.write_bytes(b"x" * 64)
            route = "/_game-assets/stub/archive.bin"
            server = ThreadingHTTPServer(("127.0.0.1", 0), partial(
                web_launcher.IsolatedHandler, directory=str(site), asset_files={route: asset}))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                with urlopen(Request(base + "/stub/App.wasm", headers={"Accept-Encoding": "gzip"})) as response:
                    self.assertEqual(response.headers["Content-Encoding"], "gzip")
                    self.assertEqual(response.headers["Content-Type"], "application/wasm")
                    self.assertEqual(gzip.decompress(response.read()), program)
                    modified = response.headers["Last-Modified"]
                with self.assertRaises(HTTPError) as error:
                    urlopen(Request(base + "/stub/App.wasm", headers={
                        "Accept-Encoding": "gzip", "If-Modified-Since": modified}))
                self.assertEqual(error.exception.code, 304)
                with urlopen(base + "/stub/App.wasm") as response:  # no Accept-Encoding
                    self.assertIsNone(response.headers["Content-Encoding"])
                    self.assertEqual(response.read(), program)
                with urlopen(Request(base + route, method="HEAD")) as response:
                    first = response.headers["ETag"]
                    self.assertTrue(first)
                asset.write_bytes(b"y" * 65)
                with urlopen(Request(base + route, method="HEAD")) as response:
                    self.assertNotEqual(response.headers["ETag"], first)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()
    def test_core_suite(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        suites = sorted((ROOT / "web/launcher/tests").glob("*.test.mjs"))
        run = subprocess.run([node, "--test", *map(str, suites)],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
