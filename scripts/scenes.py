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
alone. `cook` runs the cooker on the manifest's document, packing what it
cooked; a second cook only redoes what changed. No names: every manifest.
"""

import argparse
import glob
import json
import os
import subprocess
import sys

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


def fetch(m, root):
    src = m["source"]
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


def cook(m, root, cooker):
    path = os.path.join(root, m["name"])
    document = os.path.join(path, m["source"]["document"])
    if not os.path.isfile(document):
        sys.exit(f"{m['name']}: {document} is missing (run `fetch` first)")
    out = os.path.join(root, "cooked", m["name"])
    pack = os.path.join(root, m["pack"])
    print(f"{m['name']}: cooking {document} into {pack}")
    r = subprocess.run([cooker, document, "--root", path, "-o", out, "--pack", pack])
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
