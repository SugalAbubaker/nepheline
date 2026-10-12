# Nepheline Engine

[![CI](https://github.com/SugalAbubaker/nepheline/actions/workflows/ci.yml/badge.svg)](https://github.com/SugalAbubaker/nepheline/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

Nepheline is a production-grade 3D game engine written in C17 on Vulkan 1.3.
It is data-oriented, GPU-driven, and designed bandwidth-first for one target:
**240 FPS at native 3840×2160 on mid-range hardware** (NVIDIA RTX 3060 Ti),
rendering a large, detailed open world — no upscaling, no frame generation,
every frame from fresh input and a fresh simulation step.

The complete plan — engineering principles, subsystem designs with the
research they implement, performance budgets, and the phased roadmap — lives
in [`CLAUDE.md`](CLAUDE.md). It is the project's source of truth.

## Status

| Phase | Scope | State |
|---|---|---|
| 0 | Foundation: repo, CMake skeleton, presets, warnings, sanitizers, ntest, CI, ADRs | **done** |
| 1 | Core: types, asserts, logging, allocators, containers, strings, hashing, handles, cvars, serialization | **done** |
| 2 | Math: scalar + SIMD types, wide SoA kernels, geometry, packing, random | **done** |
| 3 | Platform: threads, fibers, virtual memory, files + async I/O, CPU detection, window/input (GLFW) | **done** |
| 4–15 | Jobs → GPU/RHI → Assets → Renderer → ECS → Runtime/Editor → Renderer v2 → Physics → Audio → Streaming → Ray tracing → Hardening | planned |

## Building

Windows (Visual Studio 2022) and Linux (GCC ≥ 12 / Clang ≥ 16), CMake ≥ 3.25,
Ninja. Full details, options, and the configuration matrix: [`docs/build.md`](docs/build.md).

```powershell
# Windows (after: git submodule update --init --recursive)
. .\scripts\dev_env.ps1
cmake --preset win-msvc
cmake --build --preset win-msvc-debug
ctest --preset win-msvc-debug
```

```sh
# Linux
cmake --preset linux-gcc
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Configurations: **Debug** (asserts, validation, ASan/UBSan), **Development**
(optimised, asserts, validation, profiler), **Profile** (optimised, profiler),
**Shipping** (LTO, everything compiled out).

## Repository layout

```
cmake/          build helpers: configurations, warnings, sanitizers, module/test functions
engine/<mod>/   one static library per module: include/nepheline/<mod>/, src/, tests/
tools/ntest/    the Nepheline test framework
benchmarks/     nbench harness, benchmarks per module, recorded results, budgets
samples/        one small executable per feature (core_hello, math_demo, platform_window)
tests/fuzz/     libFuzzer targets and seed corpora for every parser
scripts/        dev_env.ps1, format.py, tidy.py
third_party/    pinned submodules (GLFW), each wrapped by exactly one engine module
docs/           design docs per subsystem; docs/adr/ holds decision records
.github/        CI workflows, PR template
```

Modules depend strictly left-to-right:
`core → math → platform → jobs → gpu → render`, then `asset`, `scene`,
`physics`, `audio`, `profile` → `runtime` → `editor` / games.

## Documentation

- [`docs/build.md`](docs/build.md) — build system, presets, options, CI
- [`docs/core.md`](docs/core.md) — core module: base, errors, assertions, atomics, allocators, hashing, containers, strings, logging, cvars, serialization
- [`docs/math.md`](docs/math.md) — math module: conventions, scalar/vector/quaternion/matrix layer, SIMD, geometry, random
- [`docs/platform.md`](docs/platform.md) — platform module: clocks, threads, fibers, virtual memory, files and async I/O, CPU, window and input
- [`docs/ntest.md`](docs/ntest.md) — test framework
- [`docs/benchmarks.md`](docs/benchmarks.md) — benchmark harness and methodology
- [`docs/adr/`](docs/adr/README.md) — architecture decision records

## Contributing

Work happens on `feature/<module>-<topic>` branches and reaches `main` only
through a pull request with green CI. Commit messages follow
[Conventional Commits](https://www.conventionalcommits.org/)
(`feat(math): add quaternion slerp`). Every change ships with tests, zero
warnings on all toolchains, and formatting per `.clang-format`
(`python scripts/format.py --check`). See the working rules in `CLAUDE.md` §1.

## License

Apache License 2.0 — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE).
Decision record: [ADR-0001](docs/adr/0001-license.md).
