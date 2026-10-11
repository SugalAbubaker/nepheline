# SPDX-License-Identifier: Apache-2.0
"""Run clang-tidy over every first-party translation unit in a compile database.

    python scripts/tidy.py --build-dir build/linux-clang-tidy [--clang-tidy clang-tidy-19] [--fix]

The build directory must contain compile_commands.json (configure the
linux-clang-tidy preset, or any single-config Ninja build). Files outside
engine/, tools/, editor/, samples/, tests/, and benchmarks/ are skipped, so
third_party/ never gets analysed. Exit status is non-zero if any file produced
diagnostics (.clang-tidy treats every warning as an error).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

FIRST_PARTY_DIRS = ("engine", "tools", "editor", "samples", "tests", "benchmarks")


def find_clang_tidy(requested):
    if requested:
        return requested
    for name in ("clang-tidy", "clang-tidy-19"):
        found = shutil.which(name)
        if found:
            return found
    return None


def first_party_files(repo_root, build_dir):
    with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
        entries = json.load(f)
    allowed = tuple(os.path.join(repo_root, d) + os.sep for d in FIRST_PARTY_DIRS)
    files = set()
    for entry in entries:
        path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
        if path.startswith(allowed) and path.endswith(".c"):
            files.add(path)
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, help="directory holding compile_commands.json")
    parser.add_argument("--clang-tidy", default=None, help="clang-tidy executable")
    parser.add_argument("--fix", action="store_true", help="apply suggested fixes")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    clang_tidy = find_clang_tidy(args.clang_tidy)
    if not clang_tidy:
        print("error: clang-tidy not found (pass --clang-tidy or add it to PATH)", file=sys.stderr)
        return 2

    files = first_party_files(repo_root, args.build_dir)
    if not files:
        print("error: no first-party sources in compile_commands.json", file=sys.stderr)
        return 2

    version = subprocess.run([clang_tidy, "--version"], capture_output=True, text=True).stdout
    print(" ".join(version.split()[:4]) + f": {len(files)} files, {args.jobs} jobs")

    def run(path):
        cmd = [clang_tidy, "-p", args.build_dir, "--quiet"]
        if args.fix:
            cmd.append("--fix")
        cmd.append(path)
        result = subprocess.run(cmd, capture_output=True, text=True)
        return path, result.returncode, result.stdout + result.stderr

    failures = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for path, rc, output in pool.map(run, files):
            rel = os.path.relpath(path, repo_root)
            if rc != 0 or "warning:" in output or "error:" in output:
                failures += 1
                print(f"--- {rel}")
                print(output.strip())
            else:
                print(f"ok  {rel}")

    if failures:
        print(f"\nclang-tidy: {failures} file(s) with diagnostics", file=sys.stderr)
        return 1
    print("clang-tidy: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
