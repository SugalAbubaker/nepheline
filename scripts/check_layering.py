# SPDX-License-Identifier: Apache-2.0
"""Enforce the module dependency rule and the third-party include policy.

    python scripts/check_layering.py        # exit 1 on any violation (CI)

CLAUDE.md §4: modules depend strictly left to right,
core -> math -> platform -> jobs -> gpu -> render, then asset, scene, physics,
audio, profile -> runtime -> editor / games. A module's headers and sources
may include only modules at or before its own position.

CLAUDE.md §1.8: engine code never includes a third-party header directly;
only the wrapper module that owns the library may. Vulkan headers and volk
are owned by engine/gpu (plus its escape hatch vk.h, which only the gpu
tests, the GPU benchmarks, and the editor may include); GLFW by
engine/platform; VMA by engine/gpu/src/gpu_vma.cpp.
"""

import os
import re
import subprocess
import sys

RANK = {
    "core": 0,
    "math": 1,
    "platform": 2,
    "jobs": 3,
    "gpu": 4,
    "render": 5,
    "asset": 6,
    "scene": 6,
    "physics": 6,
    "audio": 6,
    "profile": 6,
    "runtime": 7,
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

# Third-party headers and who may include them.
THIRD_PARTY = [
    (re.compile(r"^(volk\.h|vulkan/.*)$"),
     ("engine/gpu/src/", "engine/gpu/include/nepheline/gpu/vk.h", "engine/gpu/tests/",
      "benchmarks/gpu/", "editor/")),
    (re.compile(r"^vk_mem_alloc\.h$"), ("engine/gpu/src/gpu_vma.cpp",)),
    (re.compile(r"^GLFW/.*$"), ("engine/platform/src/window_glfw.c",)),
    (re.compile(r"^(cimgui|imgui).*$"), ("editor/",)),
    (re.compile(r"^(cgltf|stb_image).*$"), ("tools/cook/",)),
    (re.compile(r"^miniaudio.*$"), ("engine/audio/src/",)),
]

# The gpu escape hatch: only these may include it.
VK_H_ALLOWED = ("engine/gpu/src/", "engine/gpu/tests/", "benchmarks/gpu/", "editor/")


def tracked_sources(repo_root):
    out = subprocess.run(["git", "ls-files", "--", "*.c", "*.h", "*.cpp"], cwd=repo_root,
                         check=True, capture_output=True, text=True)
    return [line for line in out.stdout.splitlines() if line and not line.startswith("third_party/")]


def module_of(path):
    m = re.match(r"^engine/([a-z_]+)/(include|src)/", path)
    return m.group(1) if m else None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    violations = []
    for rel in tracked_sources(repo_root):
        with open(os.path.join(repo_root, rel), encoding="utf-8", errors="replace") as f:
            text = f.read()
        module = module_of(rel)
        for inc in INCLUDE_RE.findall(text):
            m = re.match(r"^nepheline/([a-z_]+)/", inc)
            if m:
                target = m.group(1)
                if module is not None and target in RANK and module in RANK:
                    if RANK[target] > RANK[module] or (RANK[target] == RANK[module] and target != module):
                        violations.append(f"{rel}: module '{module}' includes <{inc}> ({target} is to its right)")
                if inc == "nepheline/gpu/vk.h" and not rel.startswith(VK_H_ALLOWED):
                    violations.append(f"{rel}: includes the Vulkan escape hatch <{inc}>")
                continue
            for pattern, allowed in THIRD_PARTY:
                if pattern.match(inc) and not rel.startswith(allowed):
                    violations.append(f"{rel}: includes third-party header <{inc}>")
    if violations:
        print("\n".join(violations), file=sys.stderr)
        print(f"\ncheck_layering: {len(violations)} violation(s)", file=sys.stderr)
        return 1
    print("check_layering: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
