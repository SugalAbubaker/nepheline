#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fetches and cooks the reference scenes (benchmarks/scenes/*.json).

    python scripts/scenes.py list
    python scripts/scenes.py fetch [NAME...] --dir DIR
    python scripts/scenes.py cook  [NAME...] --dir DIR --cook PATH/TO/nph_cook

The scenes are third-party content and stay outside the repository (its
Git LFS quota is 1 GB, and their licences ask for attribution where they
are redistributed, which this script does not do). DIR holds, per scene:

    DIR/<name>/            the source at the manifest's pinned commit
    DIR/cooked/<name>/     the cooker's output and database (incremental)
    DIR/<pack>             the pack render_perf loads (--scenes=DIR, or
                           the NPH_SCENES environment variable)

`fetch` checks out exactly the manifest's commit (a shallow fetch of that
commit, so a moved branch changes nothing) and leaves a finished checkout
alone. A Poly Haven source (CC0 assets in a layout of ours) instead names
every file it takes with its md5: `fetch` downloads them into
DIR/<name>/assets/<asset>/ (a file already there with its md5 is kept, a
download that does not match is refused), then lays the scene out with the
manifest's generator (scripts/landscape.py). A zip source names one archive
with its md5, and a zips source several: `fetch` downloads them into DIR/<name>/,
extracts each there once, and runs the manifest's generator (scripts/city.py,
scripts/interior.py). `cook` runs the cooker on
the manifest's document with the manifest's cooker options (`cook`: `voxels`,
`hlod` and `normal_y_down`, each nph_cook's flag of that name), packing what it
cooked; a second cook only redoes what changed (the cooker's settings are in
its outputs' hashes). No names: every manifest.
"""

import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import urllib.request
import zipfile

# Poly Haven asks its API's and downloads' clients to name themselves.
AGENT = {"User-Agent": "nepheline-scenes/1.0 (+https://github.com/SugalAbubaker/nepheline)"}

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFESTS = os.path.join(ROOT, "benchmarks", "scenes")


def manifests(names):
    found = {}
    for path in sorted(glob.glob(os.path.join(MANIFESTS, "*.json"))):
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
        found[m["name"]] = m
    if not names:
        return list(found.values())
    missing = [n for n in names if n not in found]
    if missing:
        sys.exit(f"no manifest named {', '.join(missing)} in {MANIFESTS}")
    return [found[n] for n in names]


def git(args, cwd):
    subprocess.run(["git"] + args, cwd=cwd, check=True)


def head_commit(path):
    r = subprocess.run(["git", "rev-parse", "HEAD"], cwd=path, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url, path, md5):
    """`url` into `path`, unless the file is there with its md5; a download
    whose md5 differs is refused (the pinned file changed upstream)."""
    if os.path.isfile(path) and md5_of(path) == md5:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + ".part"
    request = urllib.request.Request(url, headers=AGENT)
    with urllib.request.urlopen(request, timeout=120) as r, open(part, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    got = md5_of(part)
    if got != md5:
        os.remove(part)
        sys.exit(f"{url}: md5 {got}, the manifest pins {md5}")
    os.replace(part, path)
    return True


def fetch_polyhaven(m, root):
    src = m["source"]["polyhaven"]
    path = os.path.join(root, m["name"])
    fetched = kept = 0
    for asset in src["models"] + src["textures"]:
        for f in asset["files"]:
            target = os.path.join(path, "assets", asset["asset"], f["path"])
            if download(f["url"], target, f["md5"]):
                fetched += 1
                print(f"{m['name']}: {asset['asset']}/{f['path']} ({f['size'] / 1e6:.1f} MB)",
                      flush=True)
            else:
                kept += 1
    print(f"{m['name']}: {fetched} files fetched, {kept} kept")
    run_generator(m, root, "scripts/landscape.py")


def run_generator(m, root, default):
    generator = os.path.join(ROOT, m["layout"].get("generator", default))
    manifest = os.path.join(MANIFESTS, m["name"] + ".json")
    r = subprocess.run([sys.executable, generator, manifest, root])
    if r.returncode != 0:
        sys.exit(f"{m['name']}: the layout failed ({r.returncode})")
    print(f"{m['name']}: licence: {m.get('license', '(see the manifest)')}")


def fetch_zip(m, root):
    source = m["source"]
    path = os.path.join(root, m["name"])
    for src in source["zips"] if "zips" in source else [source["zip"]]:
        archive = os.path.join(path, src["file"])
        if download(src["url"], archive, src["md5"]):
            print(f"{m['name']}: {src['file']} ({src['size'] / 1e6:.1f} MB)", flush=True)
        # The stamp is the last thing written, so an interrupted extraction runs again.
        stamp = archive + ".extracted"
        done = ""
        if os.path.isfile(stamp):
            with open(stamp, encoding="utf-8") as f:
                done = f.read()
        if done != src["md5"]:
            with zipfile.ZipFile(archive) as z:
                z.extractall(path)
            with open(stamp, "w", encoding="utf-8") as f:
                f.write(src["md5"])
            print(f"{m['name']}: extracted into {path}", flush=True)
    run_generator(m, root, "scripts/city.py")


def fetch(m, root):
    src = m["source"]
    if "polyhaven" in src:
        fetch_polyhaven(m, root)
        return
    if "zip" in src or "zips" in src:
        fetch_zip(m, root)
        return
    path = os.path.join(root, m["name"])
    if os.path.isdir(os.path.join(path, ".git")) and head_commit(path) == src["commit"]:
        print(f"{m['name']}: {path} is at {src['commit'][:12]}")
        return
    os.makedirs(path, exist_ok=True)
    if not os.path.isdir(os.path.join(path, ".git")):
        git(["init", "--quiet"], path)
        git(["remote", "add", "origin", src["git"]], path)
    print(f"{m['name']}: fetching {src['git']} at {src['commit'][:12]} into {path}")
    git(["fetch", "--depth", "1", "origin", src["commit"]], path)
    git(["checkout", "--quiet", "--force", "FETCH_HEAD"], path)
    if head_commit(path) != src["commit"]:
        sys.exit(f"{m['name']}: the checkout is not at {src['commit']}")
    print(f"{m['name']}: licence: {m.get('license', '(see the manifest)')}")


# The cooker options a manifest may ask for (its `cook` object) and the
# nph_cook flag each turns on.
COOK_OPTIONS = {"voxels": "--voxels", "hlod": "--hlod", "normal_y_down": "--normal-y-down"}


def cook_flags(m):
    options = m.get("cook", {})
    unknown = sorted(set(options) - set(COOK_OPTIONS))
    if unknown:
        sys.exit(f"{m['name']}: unknown cook options {unknown} (known: {sorted(COOK_OPTIONS)})")
    return [COOK_OPTIONS[k] for k in sorted(options) if options[k]]


def cook(m, root, cooker):
    path = os.path.join(root, m["name"])
    document = os.path.join(path, m["source"]["document"])
    if not os.path.isfile(document):
        sys.exit(f"{m['name']}: {document} is missing (run `fetch` first)")
    out = os.path.join(root, "cooked", m["name"])
    pack = os.path.join(root, m["pack"])
    flags = cook_flags(m)
    print(f"{m['name']}: cooking {document} into {pack}" + (f" ({' '.join(flags)})" if flags else ""))
    r = subprocess.run([cooker, document, "--root", path, "-o", out, "--pack", pack] + flags)
    if r.returncode != 0:
        sys.exit(f"{m['name']}: the cooker failed ({r.returncode})")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=("list", "fetch", "cook"))
    parser.add_argument("names", nargs="*", help="manifest names (default: all)")
    parser.add_argument("--dir", help="where the scenes live (default: $NPH_SCENES)")
    parser.add_argument("--cook", help="the nph_cook executable (cook)")
    args = parser.parse_args()
    selected = manifests(args.names)
    if args.command == "list":
        for m in selected:
            views = ", ".join(v["name"] for v in m["views"])
            print(f"{m['name']}: {m.get('title', '')} (views: {views})")
        return 0
    root = args.dir or os.environ.get("NPH_SCENES")
    if not root:
        sys.exit("--dir or NPH_SCENES names where the scenes live")
    os.makedirs(root, exist_ok=True)
    for m in selected:
        if args.command == "fetch":
            fetch(m, root)
        else:
            if not args.cook:
                sys.exit("cook needs --cook PATH/TO/nph_cook")
            cook(m, root, args.cook)
    return 0


if __name__ == "__main__":
    sys.exit(main())
