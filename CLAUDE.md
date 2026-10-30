# Nepheline Engine

## Introduction
The goal of this project is to create a 3D game engine that surpasses any game engine on the market. The game engine will be highly performant and optimized. We will be looking at research papers and theoretical computer science concepts to gain the quality that we need. The project will be broken down into manageable parts which will be built one at a time. By the end of this project, the resulting game that will run on this engine will run extremely fast while also being able to render highly detailed graphics. We are doing something that has never been done before and will push the industry forward. This is not a simple hobby project, this is production grade software. No shortcuts should be taken and software must be stable and fully tested.

The game engine will be called Nepheline.

## Components
- C Language
- CMake
- Vulkan graphics API
- GLFW
- Physics engine
- Math Library
- Audio
- Editor to bring in, create models, and test the engine's features
- Create a git repository and commit and push to github

---

## 1. Working Rules (read before every task)
1. **Build one module at a time**, in the order given by the Roadmap (§12). Do not start a phase until the previous phase meets its exit criteria.
2. **Every change ships with tests.** No module is "done" without unit tests, and integration or golden tests where they apply. Run the full test suite before every commit.
3. **Zero warnings, zero validation errors.** Warnings are errors. Vulkan validation layers (including synchronization validation) must report nothing.
4. **No hidden costs.** No allocation, I/O, or locking happens unless the caller can see it. All allocation goes through Nepheline allocators.
5. **Measure before optimizing, and measure after.** Back performance claims with benchmarks in `benchmarks/`.
6. **Cite the research.** When a technique comes from a paper or talk, reference it in the code comment or the design doc.
7. **Record significant decisions** as an ADR in `docs/adr/NNNN-title.md`, covering context, decision, alternatives, and consequences.
8. **Third-party code stays behind our interfaces.** Engine code never includes a third-party header directly. Only the wrapper module that owns the library may include it.
9. **Commit small and often** on feature branches, then merge to `main` through a PR after CI passes.

## 2. Engineering Principles
- **Data-oriented design:** structure-of-arrays where hot, cache-line awareness, batch processing, no per-object virtual dispatch in hot loops (Acton, "Data-Oriented Design and C++", CppCon 2014; Fabian, *Data-Oriented Design*).
- **Explicit memory:** arenas, frame allocators, pools, and a TLSF general-purpose allocator. Every subsystem receives its allocator explicitly.
- **Handles over pointers:** cross-system references use 32-bit generational handles (index + generation) so stale use is detected.
- **Parallel by default:** all per-frame work is expressed as jobs on the job system. The main thread is a scheduler, not a worker bottleneck.
- **GPU-driven:** the CPU submits a small, constant number of draws. Culling, LOD selection, and draw generation happen on the GPU.
- **Bandwidth is the budget:** at native 4K and 240 FPS, memory traffic, not compute, is the binding constraint (§10). Every GPU pass is budgeted in bytes as well as milliseconds.
- **Cost scales with pixels, not world size:** rendering cost is bounded by screen pixels and changed content. World size only affects streaming, never the per-frame budget.
- **Deterministic simulation:** a fixed timestep and reproducible results for physics and gameplay given identical inputs.
- **Fail loudly in development, gracefully in shipping:** `NPH_ASSERT` in debug and development builds, and error codes plus logging in shipping builds.
- **No global mutable state** outside an explicit `NphEngine` context, with the exception of logging and profiling sinks.

## 3. Technology Stack & Third-Party Policy
| Area | Choice |
|---|---|
| Language | C17 (`/std:c17` on MSVC, `-std=c17` on GCC/Clang) |
| Build | CMake ≥ 3.25 with `CMakePresets.json` |
| Graphics | Vulkan 1.3 baseline (dynamic rendering, synchronization2, timeline semaphores, descriptor indexing, buffer device address). Optional: `VK_EXT_mesh_shader`, `VK_KHR_ray_query` / `VK_KHR_acceleration_structure` |
| Shaders | GLSL → SPIR-V, compiled at build time with `glslc` from the Vulkan SDK. Hot reload in the editor |
| Platforms | Windows 10/11 (MSVC 2022, clang-cl), Linux (GCC ≥ 12, Clang ≥ 16; X11 + Wayland). macOS deferred |
| CPU baseline | x86-64 with SSE4.2. AVX2 paths selected at runtime. ARM64/NEON later |
| Reference hardware | NVIDIA RTX 3060 Ti (8 GB, Ampere), with a 6-core CPU (Ryzen 5 7600 / Core i5-13400 class). This is the "mid-range" for the §10 targets |

**Allowed plumbing libraries.** Each is vendored as a pinned git submodule in `third_party/` and wrapped by one module:

| Library | Purpose | Wrapped by | Notes |
|---|---|---|---|
| GLFW | Window, input, monitors | `platform` (`nph_window_*`, `nph_input_*`) | Runtime |
| volk | Vulkan function loading | `gpu` | Runtime |
| VMA | GPU memory allocation | `gpu` (`nph_gpu_alloc_*`) | C++ implementation compiled as one isolated TU. Candidate for later replacement |
| cgltf | glTF parsing | `tools/cook` only | Offline. Never in the shipped runtime |
| stb_image | PNG/JPG/TGA/HDR decode | `tools/cook` only | Offline |
| miniaudio | Audio device output **only** | `audio` (`nph_audio_device_*`) | Mixing, DSP, and spatialization are ours |
| cimgui (+ Dear ImGui) | Editor UI | `editor` only | C++ compiled only for the editor target. Never in the runtime |

Everything else is written from scratch: math, containers, allocators, job system, RHI, renderer, ECS, physics, audio mixer and DSP, asset formats, compression, texture encoding, serialization, profiler, and test framework. Adding any new third-party dependency requires an ADR.

## 4. Repository Layout
```
nepheline/
├── CMakeLists.txt, CMakePresets.json
├── cmake/                 # warnings, sanitizers, shader compile, helper functions
├── engine/
│   ├── core/              # types, asserts, logging, allocators, containers, strings, hashing, handles, cvars
│   ├── math/              # SIMD vectors/matrices/quaternions, geometry, random
│   ├── platform/          # OS abstraction: threads, atomics, fibers, virtual memory, files, timers, window/input (GLFW)
│   ├── jobs/              # fiber-based work-stealing job system
│   ├── gpu/               # Vulkan RHI: device, memory, resources, bindless, command recording, render graph
│   ├── render/            # renderer: GPU-driven pipeline, materials, lighting, shadows, post-processing
│   ├── scene/             # ECS, transforms, scene serialization
│   ├── asset/             # runtime asset format, loading, streaming, virtual file system
│   ├── physics/           # collision + rigid body dynamics
│   ├── audio/             # mixer, voices, DSP, spatialization, device (miniaudio)
│   ├── profile/           # CPU/GPU instrumentation, memory tracking
│   └── runtime/           # engine loop, game module loading (hot reload), config
│   (each module: include/nepheline/<module>/*.h, src/*.c, tests/*.c)
├── shaders/               # GLSL sources + shared headers (shared C/GLSL structs)
├── tools/
│   ├── cook/              # offline asset cooker (glTF/images → .nph)
│   └── ntest/             # Nepheline test framework + runner
├── editor/                # editor executable (cimgui)
├── samples/               # small focused sample apps per feature
├── tests/                 # integration tests, golden images, determinism tests
├── benchmarks/            # performance benchmarks with tracked budgets
├── third_party/           # pinned submodules (see §3)
├── docs/                  # design docs per subsystem + docs/adr/
└── .github/workflows/     # CI
```

**Module dependency rule (strict, acyclic):**
`core → math → platform → jobs → gpu → render`, then `asset`, `scene`, `physics`, `audio`, `profile` → `runtime` → `editor` / games.
A module may only depend on modules to its left. `core` depends only on the C standard library and compiler intrinsics.

## 5. Coding Conventions
- **Naming:** functions `nph_module_verb_noun()` (e.g. `nph_arena_alloc`). Types `NphPascalCase`. Macros and enum values `NPH_UPPER_SNAKE`. Files `snake_case.c/.h`. Locals and fields `snake_case`.
- **Headers:** public headers live in `include/nepheline/<module>/`. Use `#pragma once`. Headers are self-contained and include only what they use. Internal headers stay in `src/`.
- **Types:** fixed-width integers (`u8…u64`, `i8…i64`, `f32`, `f64`, `b32`) from `core/types.h`. Use `size_t` only at stdlib boundaries.
- **Errors:** functions that can fail return `NphResult` (an enum) and use out-parameters. Never return a silently-null pointer.
- **Allocation:** functions that allocate take an `NphAllocator*`. No bare `malloc`/`free` outside `core/alloc`.
- **Formatting:** `.clang-format` (4-space indent, 100 cols). `clang-tidy` runs in CI. `.editorconfig`.
- **Warnings:** MSVC `/W4 /WX /permissive-`. GCC/Clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
- **Comments:** explain *why*, and cite papers. Every public function gets a doc comment covering its contract, ownership, and thread-safety.
- **Thread-safety** is documented per function: `NPH_THREADSAFE`, `NPH_MAIN_THREAD_ONLY`, `NPH_RENDER_THREAD_ONLY`.

## 6. Build System & Configurations
- CMake targets: one static library per engine module (`nph_core`, `nph_math`, …), plus `nepheline` (the aggregate), `nph_editor`, `nph_cook`, test executables, and samples.
- Build configurations:
  - **Debug:** no optimization, all asserts, validation layers, ASan.
  - **Development:** optimized, with asserts, validation, and the profiler.
  - **Profile:** optimized, with the profiler but no asserts.
  - **Shipping:** LTO, no asserts or validation, and the profiler compiled out.
- Shaders are compiled through CMake custom commands. Dependency tracking covers `#include`s. Shared C/GLSL struct headers live in `shaders/shared/`.
- Presets: `win-msvc`, `win-clang`, `linux-gcc`, `linux-clang`, each in every configuration.
- **CI (GitHub Actions):**
  - Matrix of Windows (MSVC, clang-cl) × Linux (GCC, Clang) × Debug/Development.
  - Steps: build, unit tests, sanitizer runs, clang-tidy, format check, and GPU tests on Linux using lavapipe (software Vulkan).
  - Benchmarks run nightly.
  - **A self-hosted performance runner** with the RTX 3060 Ti runs the synthetic performance scenes on every renderer PR, and the third-party reference scenes nightly, cooking them there (ADR-0040). They enforce the §10 budgets, since lavapipe cannot measure GPU performance.

## 7. Git & GitHub Workflow
- Initialize the git repo in Phase 0 with `.gitignore` (build dirs, IDE files), `.gitattributes` (LF normalization; binary assets via Git LFS), and a README.
- Create the GitHub remote with `gh repo create` (name and visibility confirmed with the user at the time).
- Branches: `main` is always green and protected. Work happens on `feature/<module>-<topic>` branches and merges by PR once CI passes.
- Commit messages: Conventional Commits (`feat(math): add quaternion slerp`).
- Releases: semantic versioning tags (`v0.1.0` at the end of Phase 4, and so on).
- License: to be decided by the user (ADR-0001).

## 8. Subsystem Designs

### 8.1 Core (`engine/core`)
- **Base:** `types.h`, `NPH_ASSERT`/`NPH_VERIFY`/`NPH_UNREACHABLE`, compiler/platform detection macros, `NPH_LIKELY`, alignment helpers.
- **Logging:** levels and categories, a lock-free per-thread ring buffer flushed by a logger thread, and sinks for console, file, and the editor.
- **Allocators** (common `NphAllocator` vtable interface):
  - Linear/arena with markers.
  - Per-frame double-buffered arena.
  - Fixed-size pool.
  - Stack.
  - **TLSF** general purpose with O(1) alloc/free (Masmano et al., "TLSF: a New Dynamic Memory Allocator for Real-Time Systems", ECRTS 2004).
  - Debug wrappers add guard bytes, leak tracking with callsites, and use-after-free poisoning.
- **Containers:**
  - Dynamic array.
  - Open-addressing hash map (Robin Hood or Swiss-table style, SIMD probe).
  - Generational handle pool.
  - Ring buffers: SPSC lock-free, MPMC bounded (Vyukov).
  - Bitset and sparse set.
- **Strings:** length-prefixed string views, string builder, interned string IDs (64-bit hash).
- **Hashing:** our own wyhash/xxHash-class 64-bit hash, plus CRC32C for file integrity.
- **CVars:** console variables registered in code and editable at runtime from the editor and config files.
- **Serialization:** a binary writer/reader with versioning, and a simple text format (JSON subset) for editor and scene files.

### 8.2 Math (`engine/math`)
- Types: `NphVec2/3/4`, `NphQuat`, `NphMat3/4`, `NphAffine` (3×4), `NphAABB`, `NphSphere`, `NphPlane`, `NphFrustum`, `NphRay`.
- **Two layers:**
  - Scalar storage types for structs and files.
  - SIMD register types (`__m128` / NEON later) for hot paths.
  - Explicit load/store between the two.
  - SoA "wide" types (`NphVec3x4`, `NphVec3x8`) for batch work such as culling and physics.
- **Conventions:**
  - Right-handed, Y-up, column-major, column vectors.
  - Projection is reverse-Z with an infinite far plane for depth precision (Reed, "Depth Precision Visualized", 2015), using Vulkan clip space (Z in [0,1], Y-down handled in the projection).
- **Operations:**
  - Full linear algebra and decomposition (TRS).
  - Quaternion slerp/nlerp and swing-twist.
  - Frustum extraction, intersection tests (Ericson, *Real-Time Collision Detection*), and half-float conversion.
  - Octahedral normal encoding and quantization helpers.
- **Large-world coordinates:** world positions are 64-bit (`NphDVec3`) on the CPU. The GPU only receives camera-relative 32-bit positions, so precision holds at any distance from the origin.
- **Random:** PCG32 (O'Neill 2014), blue-noise and low-discrepancy sequences (Halton, R2).
- **Tests:** exhaustive property tests (e.g. `inverse(M)·M ≈ I`, quaternion normalization) with ULP-based tolerances. Benchmarks against scalar reference implementations.

### 8.3 Platform (`engine/platform`)
- Threads, mutexes, condition variables, semaphores, thread affinity, thread names.
- **Atomics:** our own `nph_atomic.h` over MSVC `_Interlocked*` intrinsics and GCC/Clang `__atomic` builtins, with explicit memory orders.
- **Fibers:**
  - Hand-written assembly context switch for the Windows x64 and System V x64 ABIs, with guard-paged stacks.
  - Win32 Fibers as a fallback.
- **Memory and files:**
  - Virtual memory: reserve/commit/decommit and large pages.
  - File I/O: sync and async (IOCP on Windows, io_uring with a thread-pool fallback on Linux), memory-mapped files.
- High-resolution timer, CPU feature detection (CPUID), dynamic library loading (for hot-reloading game code).
- **Window/Input:** a GLFW wrapper exposing `nph_window_*` and an event queue. Input is mapped to an action layer (keyboard, mouse, gamepad).

### 8.4 Job System (`engine/jobs`)
- Fiber-based with one worker thread per core, pinned (Gyrling, "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015).
- Work-stealing deques per worker (Chase & Lev, SPAA 2005; with the corrected memory orderings of Lê et al., "Correct and Efficient Work-Stealing for Weak Memory Models", PPoPP 2013).
- Jobs are small POD records. Dependencies are expressed with atomic counters, and `nph_job_wait(counter)` yields the fiber instead of blocking the thread.
- Priorities (high/normal/low) and dedicated I/O threads outside the fiber pool.
- The frame pipeline overlaps work across frames (simulate frame N+1 while rendering N) with a bounded frame latency.
- **Tests:** stress tests with millions of jobs, TSan runs, and a deterministic-schedule mode for debugging.

### 8.5 GPU / RHI (`engine/gpu`)
A thin, Vulkan-specific abstraction. It is not cross-API; it exists to remove boilerplate and enforce correctness.
- Instance and device selection: score physical devices, require the 1.3 features, and detect optional features (mesh shaders, ray tracing).
- Debug utils messenger routed to logging. Object naming for every resource.
- **Memory:** VMA behind `nph_gpu_alloc_*`. Upload via staging ring buffers. Transfer-queue uploads use timeline-semaphore sync.
- **Bindless:** one global descriptor set with descriptor indexing, holding arrays of sampled images, storage images, and samplers. Buffers are accessed via buffer device address. Shaders receive indices and addresses through push constants.
- **Commands:** per-thread command pools per frame in flight, parallel recording via jobs, and secondary command buffers where useful.
- **Render graph** (O'Donnell, "FrameGraph: Extensible Rendering Architecture in Frostbite", GDC 2017):
  - Passes declare reads and writes.
  - The graph computes synchronization2 barriers and layout transitions automatically.
  - Transient resources are aliased in memory.
  - Unused passes are culled.
  - Async compute queue scheduling.
- **Pipelines:** a pipeline cache persisted to disk. Pipeline layouts come from SPIR-V reflection (our own minimal parser). Shaders hot reload.
- **Swapchain:** resize handling, present mode selection, HDR output (later).
- **Tests:** headless device tests on lavapipe, a resource lifetime and leak checker, and a gate requiring zero validation and sync-validation messages.

### 8.6 Renderer (`engine/render`)
Designed around the native-4K, 240 FPS frame architecture in §10. Built in stages. Each stage is a working, tested renderer.
1. **Bring-up:** clear screen → triangle → indexed mesh → depth → textures → camera.
2. **Forward PBR reference renderer:** kept permanently as the ground truth for golden-image comparisons. It is not the production path.
   - Metallic-roughness GGX BRDF (Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013; Burley, "Physically-Based Shading at Disney", 2012).
   - Image-based lighting with the split-sum approximation.
   - HDR target and tonemapping (ACES-fit and AgX options).
3. **GPU-driven pipeline** (Haar & Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015; Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016):
   - Scene data lives in GPU buffers.
   - Per-instance and per-meshlet frustum culling, backface cone culling, and two-phase Hi-Z occlusion culling that reuses the previous frame's depth.
   - Draws are generated with `vkCmdDrawIndexedIndirectCount`.
   - Mesh shader path when available, with a compute plus indirect fallback.
4. **Visibility buffer & hybrid rasterization:**
   - No G-buffer. A 64-bit visibility buffer stores 32-bit depth plus a 32-bit cluster/triangle ID, which is 8 bytes per pixel (Burns & Hunt, "The Visibility Buffer", JCGT 2013).
   - Our own meshlet builder in the cooker.
   - Large triangles use the hardware rasterizer. Micro-triangles use a compute software rasterizer with 64-bit image atomics (`VK_EXT_shader_image_atomic_int64`) (Karis et al., "Nanite: A Deep Dive", SIGGRAPH 2021).
5. **Fused tile shading:**
   - Screen tiles are classified into material bins.
   - One compute pass per material bin evaluates the material and clustered direct lighting (Olsson et al., "Clustered Deferred and Forward Shading", HPG 2012) and writes HDR color.
   - Intermediates stay in on-chip memory or L2 wherever possible. Tile sizes are tuned per GPU so working sets fit the smallest cache (4 MB L2 on the RTX 3060 Ti).
   - FP16 math where precision allows.
6. **Shadows:**
   - Virtual shadow maps with page caching, so only pages invalidated by movement are redrawn.
   - Screen-space contact shadows for fine detail.
7. **Indirect lighting:**
   - A world-space radiance cache updated incrementally within a fixed per-frame ray budget. It uses hardware ray queries where they fit the budget, and software/SDF tracing otherwise.
   - The cache is sampled per pixel at full resolution.
   - GTAO (Jimenez et al., 2016).
   - The final GI technique is chosen via an ADR after research and benchmarks.
8. **Temporal & post:**
   - TAA at native resolution (Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014), used only for anti-aliasing and never for upscaling.
   - Bloom, auto-exposure, depth of field, motion blur, and tonemapping, fused into as few passes as possible. The final pass writes directly to the swapchain image.
9. **Virtualized geometry & streaming:**
   - Cluster-hierarchy (DAG) LOD targeting about one triangle per 2–4 pixels at 4K. This keeps visible triangles at roughly 4M or fewer regardless of world size.
   - Virtual texturing (§8.8).
10. **Ray-traced effects:**
    - Reflections and many-light sampling with ReSTIR (Bitterli et al., SIGGRAPH 2020).
    - Enabled on the reference hardware only within the §10 budget. Higher quality tiers are for high-end GPUs.
- **Content budgets enforced by tools:**
  - Material instruction limits.
  - No alpha-tested foliage in our own content (modeled geometry instead, so visibility-buffer efficiency holds). Third-party reference scenes are measured as their authors made them.
  - A budgeted transparency pass.
  - Light-count limits per cluster.
- Also includes: material system (shader permutations kept minimal through bindless and uber-material parameters), skinned meshes (compute skinning), a debug-draw API, and a GPU timestamp profiler per render-graph pass.

### 8.7 Scene / ECS (`engine/scene`)
- **Archetype-based ECS** with SoA component storage in 16 KB chunks and generational 32-bit entity IDs.
- Systems declare component read/write access. A scheduler builds a dependency graph and runs non-conflicting systems in parallel as jobs.
- **Reflection without C++:** components are defined with X-macros that generate the struct, field metadata (name, type, offset, editor hints), and serialization. The editor and save files use this metadata.
- Transform hierarchy: local→world updated in parallel by hierarchy depth.
- Scene serialization: a text format for the editor (diffable in git), cooked to binary for runtime.
- Prefabs with overrides, plus a change/event stream for the editor and networking (later).

### 8.8 Assets (`engine/asset` + `tools/cook`)
- **Offline cooker** (`nph_cook`) converts source assets into a platform-specific runtime format:
  - Meshes (glTF via cgltf): vertex quantization, vertex cache and overdraw optimization, meshletization, LOD DAG generation, bounds. All written by us.
  - Textures (via stb_image): mip generation in linear space, our own BC7/BC5/BC4/BC6H encoders, normal-map handling.
  - Materials, scenes, audio clips.
- **Runtime format (`.nph`):**
  - Versioned header.
  - Data is offset-based so it can be memory-mapped and used without a parsing step.
  - LZ4-class compression we implement ourselves. GPU decompression is a later research item.
- **Asset database:** 128-bit GUIDs, a dependency graph, content hashes for incremental cooking, and hot reload in the editor.
- **Streaming:** async I/O feeding job-based decompression, a priority queue driven by camera distance and screen size, residency budgets, and texture mip and geometry cluster streaming.
- **Virtual texturing:** textures are split into BC-compressed pages. Residency is driven by GPU feedback (which pages and mips were actually sampled), so the working set fits in 8 GB of VRAM.
- **World partition:** the world is divided into streaming cells, with HLOD proxies for the far field. A residency manager keeps the GPU working set under about 6.5 GB on the reference hardware.
- **Virtual file system:** loose files in development, pack archives in shipping.
- **Robustness:** every parser is fuzzed (libFuzzer on Clang). Malformed input must never crash.

### 8.9 Physics (`engine/physics`)
- **Shapes:** sphere, capsule, box, convex hull, triangle mesh, heightfield, and compounds.
- **Broadphase:** dynamic AABB tree with fat AABBs and incremental refit (as in Box2D/Catto). The tree is rebuilt with SAH when quality degrades.
- **Narrowphase:**
  - GJK with EPA (van den Bergen, *Collision Detection in Interactive 3D Environments*).
  - SAT for box/hull pairs (Gregorius, "The Separating Axis Test between Convex Polyhedra", GDC 2013).
  - Contact manifold reduction and persistence (Gregorius, "Robust Contact Creation for Physics Simulations", GDC 2015).
- **Solver:**
  - Substepped soft-step solver (Macklin et al., "Small Steps in Physics Simulation", SCA 2019; Catto, "Solver2D", 2024). Compared against TGS/XPBD (Müller et al., 2020) via an ADR with benchmarks.
  - Warm starting, graph coloring for parallel solve, island detection and sleeping.
- **Joints:** fixed, hinge, ball-socket, slider, distance, 6-DOF with limits and motors.
- Continuous collision detection (speculative contacts plus conservative advancement for fast bodies).
- Queries: raycast, shape cast, overlap. Character controller (kinematic, with step and slope handling).
- **Fixed 240 Hz timestep** (one step per frame at the target rate), budgeted at ≤ 1.0 ms of job-system time per step. The rate is configurable for other targets, with interpolation when it differs from the frame rate. Determinism across runs on the same platform is guaranteed and tested with state-hash tests.
- Debug visualization through the render debug-draw API.

### 8.10 Audio (`engine/audio`)
- miniaudio is used **only** to open the output device and receive buffer callbacks.
- **Audio thread** runs our mixer. The game thread sends commands through a lock-free SPSC queue. Nothing on the audio thread allocates or locks.
- Voices, buses or submixes, a priority-based virtual voice system, ducking, and snapshots.
- **Decoding:**
  - WAV/PCM and an ADPCM variant, written by us.
  - Streaming for music.
  - The compressed codec (e.g. Opus/Vorbis, ours or wrapped) is chosen via an ADR.
- **3D:** distance attenuation curves, equal-power panning, Doppler, occlusion via physics raycasts, and HRTF/ambisonics later.
- **DSP:** biquad filters, delay, compressor, and reverb (Schroeder 1962; feedback delay network after Jot & Chaigne 1991). SIMD mixing.
- **Tests:** offline rendering of mixes, compared against reference buffers within tolerance.

### 8.11 Profiling & Debugging (`engine/profile`)
- CPU instrumentation zones (`NPH_PROFILE_SCOPE`) recorded per thread into ring buffers. GPU timestamps per render-graph pass.
- Memory tracking per allocator and tag.
- An in-editor timeline viewer. A Chrome trace JSON export for external viewing.
- Crash handler with minidumps (Windows) and backtraces (Linux).
- A debug overlay with frame stats, cvars, and a console.

### 8.12 Runtime (`engine/runtime`)
- `NphEngine` lifecycle: init order follows the module order, and shutdown runs in reverse.
- The main loop: input → simulate (fixed step) → animate → render submit, all as job graphs.
- Game code is built as a shared library loaded by the runtime and **hot-reloaded** in development. State lives in engine-owned memory, so it survives reloads.
- Configuration comes from command line, config files, and cvars.

### 8.13 Editor (`editor/`)
- A separate executable that links the engine. UI is built with cimgui and uses our Vulkan RHI as its backend.
- **Panels:**
  - Viewport with gizmos (our own translate/rotate/scale).
  - Scene outliner.
  - Inspector (driven by ECS reflection metadata).
  - Asset browser with drag-and-drop import (triggers the cooker).
  - Console/log.
  - Profiler.
  - Physics and render debug toggles.
- Undo/redo is a command stack, and every edit is a reversible command.
- **Play-in-editor:** snapshot the world, simulate, then restore.
- **Model creation:**
  - Phase 1: primitives (cube, sphere, plane, cylinder) and basic mesh editing (vertex/face transform, extrude), saved as cooked meshes.
  - Phase 2: more modeling tools.
  - Complex models are imported from DCC tools (e.g. Blender) via glTF.
- Feature test scenes: built-in sample scenes for exercising renderer, physics, and audio features.

## 9. Testing & Quality Strategy
- **Test framework (`tools/ntest`):** our own minimal C framework with test registration macros, fixtures, assertions with diffs, CTest integration, and JUnit XML output for CI.
- **Unit tests** for every module, kept beside the code in `engine/<module>/tests/`. Coverage is measured with llvm-cov. The target is ≥ 90% line coverage for core, math, and jobs, and ≥ 80% elsewhere.
- **Sanitizers in CI:**
  - ASan + UBSan (Linux Clang, MSVC ASan).
  - TSan for jobs, audio, and streaming.
  - Valgrind/memcheck nightly.
- **Static analysis:** clang-tidy, MSVC `/analyze`, cppcheck.
- **Fuzzing:** all file parsers and the cooker inputs, run nightly.
- **GPU golden-image tests:** render test scenes headlessly (lavapipe in CI, real GPUs locally) and compare with NVIDIA FLIP (Andersson et al., HPG 2020) against approved baselines.
- **Determinism tests:** physics and simulation state hashes compared across runs.
- **Performance regression:** `benchmarks/` produces JSON. CI tracks results over time and fails if a budget is exceeded.
- **Soak tests:** long-running scene stress tests checking memory growth and leaks.
- **Definition of Done (per module):**
  - Design doc in `docs/`.
  - Public API documented.
  - Tests passing on all CI configurations.
  - Zero warnings and zero validation errors.
  - Benchmarks recorded.
  - Sample app demonstrating the feature.
  - PR reviewed.

## 10. Performance Targets & Frame Architecture

### 10.1 The target
- **240 FPS at native 3840×2160** on mid-range hardware (**RTX 3060 Ti**, §3), rendering a large, detailed open world.
- **Native** means visibility, materials, and lighting are resolved for all 8.3M pixels every frame:
  - No upscaling.
  - No dynamic resolution.
  - No frame generation counted toward the target.
- **Real frames:** each of the 240 frames is rendered from fresh input and a fresh simulation step.
- **Allowed**, because these neither lower resolution nor fake frames:
  - TAA at native resolution.
  - Caching of slowly changing lighting (GI radiance cache, cached shadow-map pages).
  - Reuse of the previous frame's depth for occlusion culling.
- **Measured as 1%-low frame time ≤ 4.17 ms** in the first game, not average FPS, which it holds during its development, after Phase 15.
- **Until then, the reference scenes gate at 144 FPS:** 1%-low ≤ 6.94 ms, native as above (§10.7, ADR-0067).

### 10.2 The physics of the problem
| Resource | RTX 3060 Ti | Per 4.17 ms frame |
|---|---|---|
| Memory bandwidth | 448 GB/s | ≈ 1.87 GB peak, ~1.5 GB achievable → **~180 bytes per pixel** |
| FP32 compute | ~16.2 TFLOPS | ~60 GFLOP → ~7,000 FLOP per pixel (conservative) |
| Last-level cache | 4 MB L2 | Tile working sets must stay ≤ ~2 MB |
| VRAM | 8 GB | Streaming and virtual texturing are mandatory |

Compute is sufficient. **Bandwidth, cache, and VRAM are the binding constraints**, so the renderer is designed bandwidth-first. The RTX 3060 Ti (Ampere) has reasonable DRAM bandwidth but a tiny 4 MB L2, so fused passes cannot assume large cache-resident working sets. Tile sizes are tuned per GPU at runtime.

The §10.4 DRAM budget (≤ 900 MB) is deliberately held at ~60% of the achievable bandwidth rather than all of it. The margin absorbs measurement error and keeps the design viable on GPUs with less bandwidth than the reference, which most players have.

### 10.3 Core strategies
1. **No G-buffer:** an 8 B/pixel visibility buffer, with everything else derived on demand (§8.6.4).
2. **Geometry bounded by pixels:** virtualized DAG LOD, hybrid hardware/software rasterization, and two-phase Hi-Z culling (§8.6.3–4, §8.6.9).
3. **Fused tile shading:** material, lighting, and post are fused so intermediates stay in on-chip memory or L2 instead of DRAM (§8.6.5, §8.6.8).
4. **Cache slow signals, never pixels:** cached virtual shadow pages and an incrementally updated radiance cache, both sampled at full resolution every frame.
5. **Bandwidth discipline:**
   - R11G11B10/RGB9E5 HDR formats.
   - FP16 math.
   - BC-compressed virtual textures.
   - Quantized vertices.
   - Never disable hardware framebuffer compression through careless image usage flags.
6. **Async compute overlap:** culling, GI cache updates, and shadow pages run on the async compute queue while the graphics queue rasterizes.
7. **Large world:** 64-bit world coordinates with camera-relative rendering (§8.2), streaming cells, HLOD far field, and a ≤ 6.5 GB VRAM working set (§8.8).

### 10.4 GPU frame budget (native 4K, 4.17 ms; times and DRAM traffic measured on the RTX 3060 Ti)
| Pass | Time | DRAM traffic |
|---|---|---|
| Culling + depth pyramid | 0.25 ms | 30 MB |
| Visibility buffer raster (HW + SW) | 0.65 ms | 200 MB |
| Material classify + fused shading + direct light | 1.20 ms | 250 MB |
| Shadows (cached virtual pages) | 0.40 ms | 80 MB |
| Indirect light (cache sample + incremental update) | 0.60 ms | 100 MB |
| TAA + post + tonemap (fused) | 0.40 ms | 170 MB |
| UI + present | 0.10 ms | 20 MB |
| Headroom | 0.57 ms | 50 MB |
| **Total** | **4.17 ms** | **≤ 900 MB** |

These are design estimates. Phase 5 validates them with microbenchmarks that measure achievable bandwidth and throughput on the reference GPU. The table is then updated via ADR and becomes CI-enforced. If any budget proves physically unreachable, we report it with measurements; we never quietly lower resolution or quality. The table is the plan for the 240 FPS target: until the first game's development gates it, each phase's exit reports every line against it (§10.7).

### 10.5 CPU frame budget (4.17 ms, 6 cores)
- Main thread ≤ 1.0 ms (input, scheduling, late-latched camera).
- All per-frame CPU work finishes within the frame via the job system. Simulation of frame N+1 overlaps GPU rendering of frame N.
- Physics: a fixed 240 Hz step, ≤ 1.0 ms of job time (§8.9).
- Render submission: a constant number of command buffers, ≤ 0.3 ms.
- Zero heap allocations per frame in steady state, outside frame arenas.
- Asset loads are memory-mapped with no parsing step. Streaming must never hitch a frame.

### 10.6 Latency
- The CPU runs at most one frame ahead of the GPU. Input-to-photon latency is ≤ 3 frames (~12.5 ms).
- The camera is late-latched just before submission.
- Vendor low-latency extensions are used where available (`VK_NV_low_latency2`, `VK_AMD_anti_lag`).

### 10.7 Enforcement
- A self-hosted runner with the RTX 3060 Ti runs the reference scenes at native 4K nightly, and the synthetic scenes on every renderer PR (§6).
- GPU timestamps per pass are compared to the §10.4 table. A regression of more than 5% fails CI.
- Reference scenes join with the feature each stresses (ADR-0040): dense urban (Amazon Lumberyard Bistro) in Phase 10; the open landscape with foliage in Phase 13, whose virtualized geometry its foliage needs; the interior with many lights (Intel's Sponza and its 10,000 candles) and the big city (NVIDIA Emerald Square, tiled) in Phase 14, for the many-light sampling and the first game's world (ADR-0067). The landscape stays measured, but the first game is a city, so it does not gate.
- **The gates (ADR-0067).** At Phase 14's exit, the renderer's last phase, Bistro, the city, and the interior each hold 144 FPS: 1%-low ≤ 6.94 ms at native 4K on the RTX 3060 Ti with the phase's features on, each §10.4 line reported against its budget. At Phase 15's exit they hold it in shipping builds. During the first game's development, after Phase 15, the game holds the target: 1%-low ≤ 4.17 ms (240 FPS), inside the §10.4 and §10.5 budgets. Until then the target and the table stand unchanged, and no phase from 11 to 15 may end with a reference scene slower than when it began, unless an ADR shows where the added cost will be won back.
- Phase 13 opens with a structure checkpoint (ADR-0041): measured prototypes of TAA and post fused into the shading pass, and of full-resolution GTAO in a form that fits its share of the indirect-light line, so that Phases 13 and 14 build on a frame that can reach the target at full quality. The game's setting, camera, and world density are decided there, and the reference scenes are checked against them (ADR-0059; the game was decided again by ADR-0067).
- Every renderer PR must state its cost in both milliseconds and DRAM bytes.

## 11. Documentation
- `docs/<module>.md`: the design, the data structures, the threading model, and the papers it implements.
- `docs/adr/`: architecture decision records.
- Every public API is documented in its header.
- `README.md`: build instructions for Windows and Linux, plus the project overview.

## 12. Roadmap (build in this order; each phase has exit criteria)
| Phase | Scope | Exit criteria |
|---|---|---|
| **0. Foundation** | git repo + GitHub remote; CMake skeleton and presets; warnings and sanitizer config; `ntest` framework; clang-format/tidy; CI on Windows and Linux; ADR-0001 (license); set up the RTX 3060 Ti (already owned) as a self-hosted performance runner | Empty `nph_core` builds and its tests pass in CI on all configurations |
| **1. Core** | types, asserts, logging, allocators (arena, pool, TLSF), containers, strings, hashing, handles, cvars, serialization | All core tests and sanitizer runs clean. Allocator benchmarks recorded |
| **2. Math** | scalar + SIMD types, full operations, geometry, random | Property tests pass. SIMD paths beat scalar in benchmarks |
| **3. Platform** | threads, atomics, fibers, virtual memory, files (sync/async), timer, CPU detection, window/input via GLFW | A window opens and input is logged on Windows and Linux. Platform tests pass |
| **4. Job system** | fiber scheduler, work stealing, counters, priorities | Stress tests and TSan clean. Scales near-linearly with core count in benchmarks |
| **5. GPU/RHI** | device, swapchain, memory, bindless, commands, render graph, pipeline cache, shader build | Triangle → textured spinning mesh with zero validation and sync errors. Headless tests run on lavapipe. GPU microbenchmarks measure achievable bandwidth and throughput on the RTX 3060 Ti, and the §10.4 budget table is updated via ADR |
| **6. Assets v1** | cooker for meshes and textures, `.nph` format, loader, VFS | glTF scene cooks and loads. Fuzzers run clean |
| **7. Renderer v1** | forward PBR reference renderer, IBL, HDR/tonemap, debug draw, GPU profiler (§8.6 stages 1–2) | Golden-image tests pass for the PBR test scenes |
| **8. Scene/ECS** | archetype ECS, parallel scheduler, reflection, transforms, scene serialization | 1M-entity benchmarks recorded. Scenes round-trip |
| **9. Runtime + Editor v1** | engine loop, game hot reload, editor panels, gizmos, undo/redo, import, primitives | A scene can be built, saved, reloaded, and played in the editor |
| **10. Renderer v2** | GPU-driven culling, visibility buffer + hybrid raster, fused tile shading, virtual shadow maps, radiance cache GI, native TAA + fused post (§8.6 stages 3–8); define the reference scenes | Constant CPU cost vs object count. Bistro measured at native 4K on the RTX 3060 Ti against every §10.4 line, each overrun's path recorded in an ADR (ADR-0041); the frame's gates are §10.7's |
| **11. Physics** | broadphase, narrowphase, solver, joints, CCD, queries, character controller | Stacking, ragdoll, and determinism tests pass. Benchmarks recorded |
| **12. Audio** | device, mixer, voices, DSP, 3D, streaming | Offline mix tests pass. No audio-thread allocations (verified) |
| **13. Streaming & scale** | the structure checkpoint first (§10.7); virtual texturing, geometry streaming, virtualized geometry DAG LOD, world partition + HLOD, 64-bit world coordinates (§8.6 stage 9); the open-landscape reference scene (§10.7) | The checkpoint's prototypes measured and recorded in an ADR. Open-world test scene streams without hitches and stays under 6.5 GB VRAM. The landscape measured against every §10.4 line, and no reference scene slower than at the phase's start (§10.7) |
| **14. The city & ray-traced effects** | the level hierarchies' walk made cheaper (ADR-0066); the big-city reference scene (§10.7), and the world partition's cells and streams in the engine's runtime; the landscape's raster optimized; ray-query reflections, ReSTIR many-light sampling, high-end quality tiers (§8.6 stage 10); the local lights' shadows and screen-space contact shadows (§8.6 stage 6, ADR-0041); the many-light interior reference scene (§10.7); Bistro, the city, and the interior brought to 144 FPS (ADR-0067) | Golden tests pass. Bistro, the city, and the interior each hold 1%-low ≤ 6.94 ms at native 4K on the RTX 3060 Ti with the phase's features on, each §10.4 line reported. The landscape no slower than at the phase's start (§10.7) |
| **15. Production hardening** | shipping builds, packaging, crash reporting, soak tests, docs completion, editor modeling tools v2 | Bistro, the city, and the interior hold 1%-low ≤ 6.94 ms at native 4K on the reference hardware in shipping builds (§10.7). Engine release candidate `v1.0.0` |

**After Phase 15: the first game (§13, ADR-0067).** The game is built on the engine, and what it needs is built as it needs it, each as tested engine work with its own ADR: skeletal animation and compute skinning (§8.6), navigation, NPC AI and crowds, and a crowd reference scene. The engine is optimized throughout against the game's requirements until the game holds 1%-low ≤ 4.17 ms (240 FPS) at native 4K on the reference hardware, inside the §10.4 and §10.5 budgets.

Phases 11 (physics) and 12 (audio) depend only on phases 1–4, 8, and 9, so they may run in parallel with phase 10 if more people join.

## 13. Open Decisions (resolve via ADR when the phase arrives)
- Project license (Phase 0).
- Setting up the RTX 3060 Ti self-hosted performance runner (Phase 0).
- Compressed audio codec: write our own or wrap one (Phase 12).
- Physics solver final choice: soft-step vs TGS/XPBD (Phase 11, based on benchmarks).
- Dynamic GI technique for the radiance cache (Phase 10).
- Gameplay scripting: C hot-reload only, or add a scripting language (Phase 9+).
- First game to showcase the engine. Decided (ADR-0067, replacing ADR-0059's forest valley): a first-person shooter with elements of roleplay, like GTA V, in a big city with many streets and buildings, large indoor facilities with many lights, and many interactible NPCs. It is built after Phase 15 and holds the 240 FPS target during its development (§10.7).
- Whether the first game has vehicles: they would add vehicle physics and streaming at car speed (when its development reaches them).
