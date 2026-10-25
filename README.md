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
| 7 | Renderer v1: forward PBR reference renderer (GGX, energy compensation, punctual lights in physical units), GPU-computed image-based lighting, pre-exposed HDR with ACES and AgX, debug draw, GPU profiler, primitives; golden-image tests | **done** (`v0.2.0`) |
| 8 | Scene/ECS: archetypes in 16 KiB SoA chunks, X-macro reflection, queries with change detection, command buffers, a scheduler that derives a dependency graph from declared access and runs systems on the job system, 64-bit transforms and the hierarchy, prefabs with overrides, text and binary scene files; million-entity benchmarks | **done** (`v0.3.0`) |
| 9 | Runtime + Editor v1: profiling (zones, timeline, memory); the engine context and its pipelined fixed-step frame; the console, cvars, and the command line; the world-to-renderer bridge; C game modules with hot reload that migrates the world; the player; the editor: scene editing with gizmos and undo/redo, drag-and-drop import through the cooker, primitives and mesh editing | **done** (`v0.4.0`) |
| 10 | Renderer v2: the GPU-driven production path (two-phase occlusion culling, a 64-bit visibility buffer, hybrid hardware and software raster); a light grid with exact culling; ray-traced shadows and the sun's virtual shadow map; temporal anti-aliasing at native resolution, bloom and exposure, motion blur, depth of field; full-resolution ground-truth ambient occlusion and a hashed radiance cache; Amazon Lumberyard Bistro as the reference scene, measured nightly at native 4K against every §10.4 line (the 4.17 ms frame gates at Phase 14's exit: ADR-0041) | **done** (`v0.5.0`) |
| 11 | Physics: collision (spheres, capsules, boxes, convex hulls by Quickhull, triangle meshes, heightfields, compounds; separating axes, clipping, GJK and EPA; contact reduction and warm starts kept across steps), a dynamic AABB tree with SAH rebuilds; the world with the substepped soft-step solver (TGS and XPBD kept as options, after a benchmarked comparison), graph-coloured parallel solving, islands and sleep; ball, hinge, fixed, slider, distance, and 6-DOF joints with limits and motors, and ragdolls; continuous collision; ray and shape casts, overlaps; the character controller; in the engine, RigidBody and Collider components and frames drawn between steps; a physics playground sample | **done** (`v0.6.0`) |
| 12–15 | Audio → Streaming → Ray tracing → Hardening | planned |

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
feature and renders it with the PBR reference renderer; `samples/scene_demo`
builds an orrery as an ECS world, moves it with systems on the scheduler,
round-trips it through both scene formats, and renders it through the
GPU-driven production path (P switches to the reference renderer; T, B, O,
and G switch the temporal anti-aliasing, bloom, ambient occlusion, and
global illumination).

`render_perf` measures the renderer's synthetic scenes, most at native 4K,
and with `--scenes=DIR` the third-party reference scenes that `scripts/scenes.py`
fetches and cooks from their pinned manifests in `benchmarks/scenes/`
([`docs/benchmarks.md`](docs/benchmarks.md)); the self-hosted RTX 3060 Ti
runner enforces their budgets.

Games are C shared libraries that `nph_player` (and the editor) run, and
swap in again when they are rebuilt ([`docs/runtime.md`](docs/runtime.md)):

```sh
nph_player --game=build/win-msvc/bin/Debug/game_sandbox.dll \
           --pack=build/win-msvc/samples/asset_viewer/Debug/showcase.npk
```

`samples/game_physics` is a physics playground (crates, balls, stairs,
ramps, a swinging platform) walked by a character: W/A/S/D, Space to jump,
the right mouse button to look, the left one to throw
([`docs/physics.md`](docs/physics.md)):

```sh
nph_player --game=build/win-msvc/bin/Debug/game_physics.dll \
           --pack=build/win-msvc/samples/game_physics/Debug/physics.npk
```

The editor ([`docs/editor.md`](docs/editor.md)) builds scenes in a docked
Dear ImGui interface. It has:
- a viewport with an orbit and fly camera, gizmos, the outliner, and the
  inspector;
- undo and redo for every edit;
- Play (F5) on a copy of the scene;
- drag-and-drop import: dropped files are copied into the project and
  cooked;
- model creation: Create makes primitives, and Tab edits a mesh's vertices
  and faces;
- the console, the profiler's timeline, and the statistics with every cvar;
- bodies from RigidBody and Collider components, their outlines, and the
  physics debug toggles.

```sh
nph_editor editor.assets=my_game/assets editor.cooked=my_game/cooked
nph_editor --pack=build/win-msvc/samples/asset_viewer/Debug/showcase.npk
```

## Repository layout

```
cmake/          build helpers: configurations, warnings, sanitizers, module/test functions
engine/<mod>/   one static library per module: include/nepheline/<mod>/, src/, tests/
                (also built together as nepheline_shared, for game modules and the hosts that load them;
                engine/runtime/player/ is nph_player)
tools/ntest/    the Nepheline test framework
tools/cook/     the asset cooker: nph::cook, the nph_cook tool, and nph::cook_shared for the editor (the only user of cgltf and stb_image)
tools/golden/   the golden-image toolkit: our PNG codec, LDR-FLIP, and the nph_flip tool
editor/         nph_editor: the shell, Dear ImGui backends on the window and the RHI, panels, scene editing, import, mesh editing, tests
benchmarks/     nbench harness, CPU benchmarks per module, GPU microbenchmarks, render_perf, the reference scenes' manifests, recorded results, budgets
shaders/        GLSL sources and the C/GLSL shared headers, compiled with glslc at build time
samples/        one small executable per feature (core_hello, math_demo, platform_window, jobs_demo, gpu_triangle, gpu_mesh, asset_viewer, pbr_viewer, scene_demo) and the game modules game_sandbox and game_physics; samples/common/ holds the cooked-scene bridge and BMP capture they share
tests/fuzz/     libFuzzer targets, their corpus-replay smoke driver, and seed corpora for every parser (scripts/fuzz_seeds.py writes them)
tests/integration/  tests across modules and tools: the cooker against the runtime and the GPU
tests/golden/   golden-image tests of the renderer: scenes, and the approved baselines (Git LFS)
scripts/        dev_env.ps1, install_vulkan_sdk.ps1, install_vulkan_layer.sh, format.py, tidy.py, check_layering.py, gpu_budgets.py, render_budgets.py, scenes.py, fuzz_seeds.py, fuzz_seeds_cook.py, fuzz_seeds_scene.py, nph_hash64.py, runner/install.ps1
third_party/    pinned submodules (GLFW, Vulkan-Headers, volk, VMA, cgltf, stb, cimgui with Dear ImGui), each wrapped by exactly one module or tool
docs/           design docs per subsystem; docs/adr/ holds decision records
.github/        CI workflows, PR template
```

Modules depend strictly left-to-right:
`core → math → platform → jobs → gpu → render`, then `asset`, `scene`,
`physics`, `audio`, `profile` → `runtime` → `editor` / games.

## Documentation

- [`docs/build.md`](docs/build.md) — build system, presets, options, CI
- [`docs/core.md`](docs/core.md) — core module: base, errors, assertions, atomics, allocators, real-time scopes, hashing, containers, strings, logging, cvars, serialization
- [`docs/math.md`](docs/math.md) — math module: conventions, scalar/vector/quaternion/matrix layer, SIMD, geometry, random
- [`docs/platform.md`](docs/platform.md) — platform module: clocks, threads, fibers, virtual memory, files and async I/O, CPU, window and input
- [`docs/jobs.md`](docs/jobs.md) — job system: fiber workers, work-stealing deques, counters, priorities, I/O threads, fiber exhaustion, scaling results
- [`docs/gpu.md`](docs/gpu.md) — GPU module: device and validation gate, bindless resources, frames and submission, uploads, shaders and pipelines, the render graph, the swapchain
- [`docs/render.md`](docs/render.md) — render module: the forward PBR reference renderer, units and conventions, the GPU-driven production path (culling, the visibility buffer and its raster, lights, shadows, temporal anti-aliasing, motion blur, ambient occlusion, global illumination), image-based lighting, tonemapping, depth of field, bloom and exposure, debug draw, the GPU profiler
- [`docs/golden.md`](docs/golden.md) — golden-image tests: the scenes, our LDR-FLIP and PNG codec, baselines and approval, calibration
- [`docs/asset.md`](docs/asset.md) — asset module: the `.nph` container, the LZ codec, GUIDs, pack archives, the VFS, the loader, the typed views
- [`docs/cook.md`](docs/cook.md) — the asset cooker: identities and incremental cooks, glTF import, the mesh pipeline, textures and the BC encoders, fuzzing, benchmarks
- [`docs/profile.md`](docs/profile.md) — profiling: CPU zones recorded per thread in core, the collector that rebuilds them per fiber into frames, statistics, Chrome trace export, tracked allocators
- [`docs/scene.md`](docs/scene.md) — scene module: the ECS (storage, components and reflection, queries and change detection, command buffers), the scheduler, transforms and the hierarchy, prefabs, scene files, copies that keep their handles, benchmarks
- [`docs/physics.md`](docs/physics.md) — physics module: conventions and precision in a 64-bit world, collision geometry, convex hulls and Quickhull, the narrow phase (separating axes, clipping, GJK and EPA), the dynamic AABB tree, the world (bodies, shapes, contacts), the solvers (the soft step, TGS, XPBD) and their threads, joints, islands and sleep, events, meshes and heightfields, queries, continuous collision, the character controller, the engine's bodies, benchmarks
- [`docs/audio.md`](docs/audio.md) — audio module: conventions, real-time scopes, the block kernels per SIMD level, resampling (bandlimited interpolation), filters, delay lines, panning, the codecs (WAV, our lossless codec, our ADPCM), benchmarks
- [`docs/runtime.md`](docs/runtime.md) — runtime module: the engine and its frame, configuration and the console, input for the simulation, the world-to-renderer bridge, physics in the engine, drawing between steps, game modules and hot reload, the player
- [`docs/editor.md`](docs/editor.md) — the editor: running it, the panels, editing and undo, projects and import, model creation, the engine hooks it is built on, the Dear ImGui backends, testing
- [`docs/ntest.md`](docs/ntest.md) — test framework
- [`docs/benchmarks.md`](docs/benchmarks.md) — benchmark harness, methodology, the GPU microbenchmarks, render_perf and the reference scenes, and their budgets
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
