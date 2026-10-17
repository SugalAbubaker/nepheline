# SPDX-License-Identifier: Apache-2.0
"""Approves rendered golden images as baselines (docs/golden.md).

    python scripts/golden_approve.py --from DIR [NAME ...]
    python scripts/golden_approve.py --from DIR --all

DIR holds the images a golden run wrote: <build>/golden/<Config>/ locally,
or a CI artifact unpacked with `gh run download <run> -n golden-<job>`.
Baselines are the CI reference renders (lavapipe, ADR-0019), so approve
from the Linux job's artifact, not from a local GPU, unless the change is
being made for every device at once.

Each NAME.png (FLIP maps, *.flip.png, are skipped, and so are the
production path's renders, *.production.png, which are held to the
reference's baseline rather than approved as their own) is checked to be
the 256 x 256 8-bit RGB PNG the tests write, then copied to
tests/golden/baselines/. The script prints which baselines are new,
changed, or unchanged. Look at every new or changed image, and at its FLIP
map against the old baseline, before committing: approving is a review,
not a formality (CLAUDE.md §9).
"""

import argparse
import os
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINES = os.path.join(ROOT, "tests", "golden", "baselines")
SIZE = 256


def check_png(path):
    with open(path, "rb") as f:
        head = f.read(33)
    if len(head) < 33 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return "not a PNG"
    width, height, depth, color = struct.unpack(">IIBB", head[16:26])
    if (width, height) != (SIZE, SIZE) or depth != 8 or color != 2:
        return f"{width}x{height}, depth {depth}, colour type {color}: not a golden image"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--from", dest="source", required=True,
                        help="directory with the rendered NAME.png files")
    parser.add_argument("--all", action="store_true", help="approve every image in the directory")
    parser.add_argument("names", nargs="*", help="images to approve (without .png)")
    args = parser.parse_args()
    if args.all == bool(args.names):
        parser.error("name the images to approve, or pass --all")
    if args.all:
        names = sorted(n[:-4] for n in os.listdir(args.source)
                       if n.endswith(".png") and not n.endswith(".flip.png")
                       and not n.endswith(".production.png"))
    else:
        names = args.names
        for name in names:
            if name.endswith(".production"):
                parser.error(f"{name}: the production path is held to the reference's "
                             "baseline; approve the reference image")
    if not names:
        print("golden_approve: no images found", file=sys.stderr)
        return 1
    os.makedirs(BASELINES, exist_ok=True)
    status = 0
    for name in names:
        src = os.path.join(args.source, name + ".png")
        dst = os.path.join(BASELINES, name + ".png")
        if not os.path.isfile(src):
            print(f"  missing  {name}: {src} does not exist", file=sys.stderr)
            status = 1
            continue
        problem = check_png(src)
        if problem is not None:
            print(f"  refused  {name}: {problem}", file=sys.stderr)
            status = 1
            continue
        if not os.path.exists(dst):
            state = "new"
        else:
            with open(src, "rb") as a, open(dst, "rb") as b:
                state = "unchanged" if a.read() == b.read() else "changed"
        if state != "unchanged":
            shutil.copyfile(src, dst)
        print(f"  {state:9} {name}")
    return status


if __name__ == "__main__":
    sys.exit(main())
