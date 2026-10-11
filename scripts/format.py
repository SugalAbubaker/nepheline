# SPDX-License-Identifier: Apache-2.0
"""Run clang-format over every tracked C source and header.

    python scripts/format.py --check   # exit 1 if anything would change (CI)
    python scripts/format.py --fix     # rewrite files in place

clang-format is located via $CLANG_FORMAT, then PATH (clang-format, clang-format-19),
then the Visual Studio 2022 LLVM directory.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

TRACKED_PATTERNS = ("*.c", "*.h")


def find_clang_format():
    env = os.environ.get("CLANG_FORMAT")
    if env:
        return env
    for name in ("clang-format", "clang-format-19"):
        found = shutil.which(name)
        if found:
            return found
    vs_root = os.environ.get("ProgramFiles", r"C:\Program Files")
    pattern = os.path.join(vs_root, "Microsoft Visual Studio", "2022", "*",
                           "VC", "Tools", "Llvm", "x64", "bin", "clang-format.exe")
    matches = sorted(glob.glob(pattern))
    return matches[0] if matches else None


def tracked_sources(repo_root):
    out = subprocess.run(["git", "ls-files", "--", *TRACKED_PATTERNS],
                         cwd=repo_root, check=True, capture_output=True, text=True)
    return [line for line in out.stdout.splitlines() if line]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="report unformatted files, exit 1")
    mode.add_argument("--fix", action="store_true", help="format files in place")
    args = parser.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    clang_format = find_clang_format()
    if not clang_format:
        print("error: clang-format not found (set CLANG_FORMAT or add it to PATH)", file=sys.stderr)
        return 2

    files = tracked_sources(repo_root)
    if not files:
        print("no tracked C sources")
        return 0

    version = subprocess.run([clang_format, "--version"], capture_output=True, text=True).stdout.strip()
    print(f"{version}: {len(files)} files")

    if args.fix:
        subprocess.run([clang_format, "-i", "--style=file", *files], cwd=repo_root, check=True)
        return 0

    result = subprocess.run([clang_format, "--dry-run", "--Werror", "--style=file", *files],
                            cwd=repo_root, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        bad = sorted({line.split(":", 1)[0] for line in result.stderr.splitlines()
                      if ": error:" in line})
        print(f"\n{len(bad)} file(s) need formatting; run: python scripts/format.py --fix",
              file=sys.stderr)
        return 1
    print("all files formatted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
