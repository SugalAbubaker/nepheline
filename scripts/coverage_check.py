# SPDX-License-Identifier: Apache-2.0
"""Check an llvm-cov report against per-module line-coverage targets.

    python scripts/coverage_check.py report.txt

Reads the text of `llvm-cov report` (one row per source file plus TOTAL),
groups rows by engine module (engine/<module>/...), and fails if any
module's line coverage is below its target (CLAUDE.md §9: 90 % for core,
math, and jobs; 80 % for every other module). Files under tests/, tools/,
benchmarks/, samples/, and third_party/ are ignored.
"""

import re
import sys

TARGETS = {"core": 90.0, "math": 90.0, "jobs": 90.0}
DEFAULT_TARGET = 80.0
IGNORED = ("tests/", "tools/", "benchmarks/", "samples/", "third_party/")


def parse_rows(text):
    """Yield (path, lines_total, lines_missed) for each file row."""
    for line in text.splitlines():
        parts = line.split()
        # Rows: path regions missed cover% functions missed executed% lines missed cover% [branches...]
        if len(parts) < 10 or parts[0] == "TOTAL" or parts[0] == "Filename":
            continue
        path = parts[0].replace("\\", "/")
        try:
            lines_total = int(parts[7])
            lines_missed = int(parts[8])
        except ValueError:
            continue
        yield path, lines_total, lines_missed


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()

    per_module = {}
    for path, total, missed in parse_rows(text):
        if any(part in path for part in IGNORED):
            continue
        match = re.search(r"engine/([^/]+)/", path)
        if not match:
            continue
        module = match.group(1)
        t, m = per_module.get(module, (0, 0))
        per_module[module] = (t + total, m + missed)

    if not per_module:
        print("coverage: no engine rows found in the report", file=sys.stderr)
        return 2

    failed = False
    for module in sorted(per_module):
        total, missed = per_module[module]
        covered = 100.0 * (total - missed) / total if total else 100.0
        target = TARGETS.get(module, DEFAULT_TARGET)
        status = "ok " if covered >= target else "LOW"
        if covered < target:
            failed = True
        print(f"{status} engine/{module}: {covered:6.2f}% of {total} lines (target {target:.0f}%)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
