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
| 4 | Jobs: fiber workers, work-stealing deques, counters, priorities, I/O threads, deterministic mode | **done** (`v0.1.0`) |
| 5 | GPU/RHI: Vulkan 1.3 device, bindless resources, frames and timelines, uploads, swapchain, SPIR-V reflection, pipelines with a persisted cache and hot reload, render graph with aliasing and async compute, GPU microbenchmarks and budgets on the reference RTX 3060 Ti | **done** |
| 6 | Assets v1: the `.nph` container, LZ codec, pack archives, VFS, loader and typed views; the cooker (glTF import, mesh pipeline with MikkTSpace tangents and meshlets, our BC4/BC5/BC6H/BC7 encoders, incremental database, packs); fuzzing of every parser in every build | **done** |
| 7 | Renderer v1: forward PBR reference renderer (GGX, energy compensation, punctual lights in physical units), GPU-computed image-based lighting, pre-exposed HDR with ACES and AgX, debug draw, GPU profiler, primitives; golden-image tests | in progress |
| 8–15 | ECS → Runtime/Editor → Renderer v2 → Physics → Audio → Streaming → Ray tracing → Hardening | planned |

## Building

Windows (Visual Studio 2022) and Linux (GCC ≥ 12 / Clang ≥ 16), CMake ≥ 3.25,
Ninja, and `glslc` from the Vulkan SDK. Full details, options, and the
configuration matrix: [`docs/build.md`](docs/build.md).

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

Content is cooked offline into packs the runtime maps without parsing
([`docs/cook.md`](docs/cook.md)):

```sh
nph_cook content/ --root content -o build/cooked --pack build/game.npk
```

`samples/asset_viewer` does exactly that at build time and renders the
result; `samples/pbr_viewer` cooks a scene that exercises every material
feature and renders it with the PBR reference renderer.

## Repository layout

```
cmake/          build helpers: configurations, warnings, sanitizers, module/test functions
engine/<mod>/   one static library per module: include/nepheline/<mod>/, src/, tests/
tools/ntest/    the Nepheline test framework
tools/cook/     the offline asset cooker: nph::cook and the nph_cook tool (the only user of cgltf and stb_image)
tools/golden/   the golden-image toolkit: our PNG codec, LDR-FLIP, and the nph_flip tool
benchmarks/     nbench harness, CPU benchmarks per module, GPU microbenchmarks, recorded results, budgets
shaders/        GLSL sources and the C/GLSL shared headers, compiled with glslc at build time
samples/        one small executable per feature (core_hello, math_demo, platform_window, jobs_demo, gpu_triangle, gpu_mesh, asset_viewer, pbr_viewer); samples/common/ holds the cooked-scene bridge and BMP capture they share
tests/fuzz/     libFuzzer targets, their corpus-replay smoke driver, and seed corpora for every parser (scripts/fuzz_seeds.py writes them)
tests/integration/  tests across modules and tools: the cooker against the runtime and the GPU
tests/golden/   golden-image tests of the renderer: scenes, and the approved baselines (Git LFS)
scripts/        dev_env.ps1, install_vulkan_sdk.ps1, install_vulkan_layer.sh, format.py, tidy.py, check_layering.py, gpu_budgets.py, fuzz_seeds.py, fuzz_seeds_cook.py, nph_hash64.py, runner/install.ps1 + loop.ps1
third_party/    pinned submodules (GLFW, Vulkan-Headers, volk, VMA, cgltf, stb), each wrapped by exactly one module or tool
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
- [`docs/jobs.md`](docs/jobs.md) — job system: fiber workers, work-stealing deques, counters, priorities, I/O threads, fiber exhaustion, scaling results
- [`docs/gpu.md`](docs/gpu.md) — GPU module: device and validation gate, bindless resources, frames and submission, uploads, shaders and pipelines, the render graph, the swapchain
- [`docs/render.md`](docs/render.md) — render module: the forward PBR reference renderer, units and conventions, image-based lighting, tonemapping, debug draw, the GPU profiler
- [`docs/golden.md`](docs/golden.md) — golden-image tests: the scenes, our LDR-FLIP and PNG codec, baselines and approval, calibration
- [`docs/asset.md`](docs/asset.md) — asset module: the `.nph` container, the LZ codec, GUIDs, pack archives, the VFS, the loader, the typed views
- [`docs/cook.md`](docs/cook.md) — the asset cooker: identities and incremental cooks, glTF import, the mesh pipeline, textures and the BC encoders, fuzzing, benchmarks
- [`docs/ntest.md`](docs/ntest.md) — test framework
- [`docs/benchmarks.md`](docs/benchmarks.md) — benchmark harness, methodology, the GPU microbenchmarks and their budgets
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
