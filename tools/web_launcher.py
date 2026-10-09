#!/usr/bin/env python3
"""Build the web launcher: the hub page for one or more games.

    tools/web_launcher.py --game-dir /abs/game-a [--game-dir /abs/game-b ...] --out <dir>
        [--web-build <game id>=<build dir> ...] [--serve [port]]

Writes <dir>/index.html, app.js, core.js, worker.js and games.json (each game's
title, executable, digest, required folders, exclusions and store link from its
game.toml). A game's web build, when one exists, goes in <dir>/<game id>/.
--web-build copies a game's web build (build/web/recomp: index.html and the
app's .js/.wasm/.data) there. Serve <dir> over HTTPS or from localhost with
COOP/COEP headers (the game needs a secure, cross-origin isolated context);
--serve runs such a server for local testing."""

import argparse
import functools
import fnmatch
import hashlib
import http.server
import json
import re
from pathlib import Path
import shutil
import sys
from urllib.parse import quote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402

PAGE_FILES = ("index.html", "app.js", "core.js", "worker.js")


def game_entry(cfg):
    game, launcher = cfg["game"], cfg["launcher"]
    return {
        "id": game["id"],
        "title": launcher["title"],
        "executable": game["executable"],
        # The folder under the install root that holds the executable ("" when
        # it sits at the root), as [game] executable_dir.
        "executableDir": game.get("executable_dir", ""),
        "sha256": game["sha256"],
        "requiredDirs": cfg["setup"]["required_dirs"],
        "exclude": cfg["bundle"]["exclude"],
        "store": launcher["store"],
        "installNames": launcher["install_names"],
        "minFreeMb": launcher["min_free_mb"],
        "streamAssets": launcher.get("stream_assets", False),
    }


def build(game_dirs, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    games = [game_entry(game_config.load(d)) for d in game_dirs]
    ids = [g["id"] for g in games]
    if len(set(ids)) != len(ids):
        raise ValueError("two games share an id: %s" % ", ".join(ids))
    for name in PAGE_FILES:
        shutil.copy2(ROOT / "web/launcher" / name, out / name)
    (out / "games.json").write_text(json.dumps(games, indent=2) + "\n")
    return games


WEB_BUILD_SUFFIXES = (".js", ".wasm", ".data")


def copy_web_build(game_id, build, out):
    build, dest = Path(build), Path(out) / game_id
    if not (build / "index.html").is_file():
        raise ValueError("no web build in %s (index.html is missing)" % build)
    dest.mkdir(parents=True, exist_ok=True)
    for f in build.iterdir():
        if f.is_file() and (f.name in ("index.html", "runtime.html") or f.suffix in WEB_BUILD_SUFFIXES):
            shutil.copy2(f, dest / f.name)


def hosted_assets(games, asset_dirs, out, export=False):
    """Expose only configured game files, without copying private inputs into the site.

    With `export`, the files are copied into <out>/<game id>/assets/ instead and
    every URL is relative, so the directory can be uploaded to any static host
    that answers HEAD with Content-Length and byte-range GETs (the streamed path
    needs both; the full-import path needs only GET)."""
    routes = {}
    for game in games:
        if game["id"] not in asset_dirs:
            continue
        root = Path(asset_dirs[game["id"]]).resolve()
        exe = root / game.get("executableDir", "") / game["executable"]
        if not exe.is_file() or hashlib.sha256(exe.read_bytes()).hexdigest() != game["sha256"]:
            raise ValueError("hosted assets need the pinned executable: %s" % exe)
        if any(not (root / d).is_dir() for d in game["requiredDirs"]):
            raise ValueError("hosted assets are missing required game directories")
        entries = []
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            resolved = path.resolve()
            if root not in resolved.parents:
                continue
            parts = rel.split("/")
            if any(fnmatch.fnmatchcase(name.lower(), pattern.lower())
                   for name in (parts[0], parts[-1]) for pattern in game["exclude"]):
                continue
            if parts[-1] in (".stamp", ".manifest.json", ".DS_Store") or parts[0] == "__MACOSX":
                continue
            stat = path.stat()
            if export:
                copy = Path(out) / game["id"] / "assets" / rel
                copy.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(resolved, copy)
                url = "assets/" + quote(rel, safe="/")  # relative to assets.json
            else:
                url = "/_game-assets/%s/%s" % (quote(game["id"], safe=""), quote(rel, safe="/"))
                routes[url] = resolved
            entries.append({"path": rel, "size": stat.st_size, "mtime": int(stat.st_mtime),
                            "isDir": False, "url": url})
        dest = Path(out) / game["id"]
        dest.mkdir(parents=True, exist_ok=True)
        (dest / "assets.json").write_text(json.dumps({"files": entries}) + "\n")
        game["hostedAssets"] = "./%s/assets.json" % game["id"]
        # The runtime page lives in <out>/<game id>/, so a relative base names
        # the exported copy from there.
        game["assetBase"] = ("../%s/assets" % quote(game["id"], safe="") if export
                             else "/_game-assets/%s" % quote(game["id"], safe=""))
    (Path(out) / "games.json").write_text(json.dumps(games, indent=2) + "\n")
    return routes


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript", ".data": "application/octet-stream"}

    def __init__(self, *args, asset_files=None, **kwargs):
        self.asset_files = asset_files or {}
        super().__init__(*args, **kwargs)

    def asset_route(self, path):
        # FetchFS joins its base URL with an already-rooted relative path.
        # Normalize only repeated slashes; dot paths still cannot reach inputs.
        return re.sub(r"/+", "/", urlsplit(path).path)

    def translate_path(self, path):
        route = self.asset_route(path)
        if route in self.asset_files:
            return str(self.asset_files[route])
        return super().translate_path(path)

    def send_head(self):
        route = self.asset_route(self.path)
        self.byte_range = None
        if not route.startswith("/_game-assets/"):
            return super().send_head()
        path = self.asset_files.get(route)
        if path is None:
            self.send_error(404, "Game asset not found")
            return None
        file = open(path, "rb")
        size = path.stat().st_size
        start, end = 0, size - 1
        # HEAD reports the entire file, including when FetchFS sends Range.
        requested = self.headers.get("Range") if self.command == "GET" else None
        if requested:
            match = re.fullmatch(r"bytes=(\d*)-(\d*)", requested)
            if match and any(match.groups()):
                left, right = match.groups()
                if left:
                    start = int(left)
                    end = min(int(right), end) if right else end
                elif int(right):
                    start = max(0, size - int(right))
                else:
                    start = size
            else:
                start = size
            if start > end or start >= size:
                file.close()
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
            self.byte_range = (start, end)
        self.send_response(206 if requested else 200)
        self.send_header("Content-Type", self.guess_type(str(path)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if requested:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        return file

    def copyfile(self, source, outputfile):
        if self.byte_range is None:
            return super().copyfile(source, outputfile)
        start, end = self.byte_range
        source.seek(start)
        remaining = end - start + 1
        while remaining:
            chunk = source.read(min(64 * 1024, remaining))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def serve(out, port, asset_files=None):
    handler = functools.partial(IsolatedHandler, directory=str(out), asset_files=asset_files)
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as httpd:
        print("serving %s at http://localhost:%d/" % (out, port), flush=True)
        httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-dir", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--web-build", action="append", default=[], metavar="ID=DIR")
    parser.add_argument("--serve", type=int, nargs="?", const=8000, metavar="PORT")
    parser.add_argument("--asset-dir", action="append", default=[], metavar="ID=DIR",
                        help="serve game files automatically from a local installation")
    parser.add_argument("--export-assets", action="store_true",
                        help="copy the --asset-dir files into the site for a static host "
                             "instead of serving them (no --serve needed)")
    args = parser.parse_args()
    games = build(args.game_dir, args.out)
    ids = {g["id"] for g in games}
    asset_dirs = {}
    for spec in args.asset_dir:
        game_id, sep, directory = spec.partition("=")
        if not sep or game_id not in ids or (args.serve is None and not args.export_assets):
            parser.error("--asset-dir needs ID=DIR for a known game and --serve or --export-assets")
        asset_dirs[game_id] = directory
    asset_files = (hosted_assets(games, asset_dirs, args.out, export=args.export_assets)
                   if asset_dirs else {})
    for spec in args.web_build:
        game_id, sep, build_dir = spec.partition("=")
        if not sep or game_id not in ids:
            parser.error("--web-build wants <game id>=<dir> for one of: %s" % ", ".join(sorted(ids)))
        copy_web_build(game_id, build_dir, args.out)
    print("web launcher for %s in %s" % (", ".join(g["title"] for g in games), args.out))
    if args.serve is not None:
        serve(args.out, args.serve, asset_files)


if __name__ == "__main__":
    main()
