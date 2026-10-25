# SPDX-License-Identifier: Apache-2.0
"""Approves rendered golden mixes as baselines (docs/audio.md "Golden mixes").

    python scripts/audio_golden_approve.py --from DIR [NAME ...]
    python scripts/audio_golden_approve.py --from DIR --all

DIR holds the mixes a golden run wrote: <build>/audio_golden/<Config>/
locally, or a CI artifact unpacked with `gh run download <run> -n
audio-golden-<job>`. The mixer renders the same mix on every compiler to
within the tests' tolerance, so a baseline may come from any build.

Each NAME.wav is checked to be the 2-channel 48 kHz 32-bit float WAV the
tests write, then copied to tests/audio/baselines/. The script prints which
baselines are new, changed, or unchanged, and for a changed one the
largest difference from the old baseline. Listen to every new or changed
mix before committing: approving is a review, not a formality (CLAUDE.md
§9).
"""

import argparse
import array
import os
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINES = os.path.join(ROOT, "tests", "audio", "baselines")
RATE = 48000
CHANNELS = 2


def read_wav(path):
    """The samples of a float WAV as an array('f'), or a reason it is not one."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None, "not a WAV file"
    fmt = None
    samples = None
    at = 12
    while at + 8 <= len(data):
        tag, size = struct.unpack("<4sI", data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if tag == b"fmt " and len(body) >= 16:
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            samples = body
        at += 8 + size + (size & 1)
    if fmt is None or samples is None:
        return None, "no 'fmt ' or 'data' chunk"
    code, channels, rate, _, _, bits = fmt
    # 3: IEEE float; 0xFFFE: extensible (whose subformat the writer sets to float).
    if code not in (3, 0xFFFE) or bits != 32 or channels != CHANNELS or rate != RATE:
        return None, (f"format {code:#x}, {channels} channels, {rate} Hz, {bits} bits: "
                      "not a golden mix")
    out = array.array("f")
    out.frombytes(samples[:len(samples) - len(samples) % 4])
    if sys.byteorder != "little":
        out.byteswap()
    return out, None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--from", dest="source", required=True,
                        help="directory with the rendered NAME.wav files")
    parser.add_argument("--all", action="store_true", help="approve every mix in the directory")
    parser.add_argument("names", nargs="*", help="mixes to approve (without .wav)")
    args = parser.parse_args()
    if args.all == bool(args.names):
        parser.error("name the mixes to approve, or pass --all")
    if args.all:
        names = sorted(n[:-4] for n in os.listdir(args.source) if n.endswith(".wav"))
    else:
        names = args.names
    if not names:
        print("audio_golden_approve: no mixes found", file=sys.stderr)
        return 1
    os.makedirs(BASELINES, exist_ok=True)
    status = 0
    for name in names:
        src = os.path.join(args.source, name + ".wav")
        dst = os.path.join(BASELINES, name + ".wav")
        if not os.path.isfile(src):
            print(f"  missing   {name}: {src} does not exist", file=sys.stderr)
            status = 1
            continue
        new, problem = read_wav(src)
        if problem is not None:
            print(f"  refused   {name}: {problem}", file=sys.stderr)
            status = 1
            continue
        note = ""
        if not os.path.exists(dst):
            state = "new"
        else:
            with open(src, "rb") as a, open(dst, "rb") as b:
                state = "unchanged" if a.read() == b.read() else "changed"
            if state == "changed":
                old, problem = read_wav(dst)
                if problem is not None:
                    note = f" (the old baseline: {problem})"
                elif len(old) != len(new):
                    note = f" ({len(old) // CHANNELS} frames before, {len(new) // CHANNELS} now)"
                else:
                    worst = max(abs(x - y) for x, y in zip(old, new))
                    note = f" (largest difference {worst:.3g})"
        if state != "unchanged":
            shutil.copyfile(src, dst)
        peak = max((abs(x) for x in new), default=0.0)
        print(f"  {state:9} {name}: peak {peak:.3f}{note}")
    return status


if __name__ == "__main__":
    sys.exit(main())
