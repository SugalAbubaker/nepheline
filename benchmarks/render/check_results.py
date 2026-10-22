#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Runs render_perf with --json and checks the result's shape (CTest bench.render_perf).

    check_results.py <render_perf> <json> [render_perf arguments...]

Every GPU entry, the frame's (`<scene>.gpu`) and each pass's
(`<scene>.gpu.<pass>`, `<scene>.gpu.span`), has its 99th percentile beside
it (`.p99`) over the same frames, and both it and the mean lie within the
entry's minimum and maximum. render_perf's own exit code passes through, so
77 (no device) skips the test.
"""
import json
import os
import subprocess
import sys

TOLERANCE_NS = 0.001  # the JSON prints its values to three decimals


def check(path):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    entries = {e["name"]: e for e in data["benchmarks"]}
    gpu = [n for n in entries if ".gpu" in n and not n.endswith(".p99")]
    problems = [] if gpu else ["no GPU entries"]
    for name in gpu:
        e = entries[name]
        lo = e["min_ns_per_op"] - TOLERANCE_NS
        hi = e["max_ns_per_op"] + TOLERANCE_NS
        if not lo <= e["ns_per_op"] <= hi:
            problems.append(f"{name}: the mean {e['ns_per_op']} lies outside [{lo}, {hi}]")
        p99 = entries.get(name + ".p99")
        if p99 is None:
            problems.append(f"{name}: no {name}.p99")
        elif p99["repeats"] != e["repeats"]:
            problems.append(f"{name}.p99: {p99['repeats']} frames against {e['repeats']}")
        elif not lo <= p99["ns_per_op"] <= hi:
            problems.append(f"{name}.p99: {p99['ns_per_op']} lies outside [{lo}, {hi}]")
    for name in entries:
        if name.endswith(".p99") and name[: -len(".p99")] not in entries:
            problems.append(f"{name}: no entry it is the percentile of")
    return problems


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    exe, path = sys.argv[1], sys.argv[2]
    if os.path.exists(path):
        os.remove(path)
    code = subprocess.run([exe, f"--json={path}", *sys.argv[3:]], check=False).returncode
    if code != 0:
        return code
    problems = check(path)
    for p in problems:
        print(f"check_results: {p}", file=sys.stderr)
    print(f"check_results: {path}: {'FAILED' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
