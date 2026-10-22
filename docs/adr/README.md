# Architecture Decision Records

Significant decisions are recorded here as numbered Markdown files
(CLAUDE.md §1.7). An accepted ADR is not edited afterwards; a later ADR
supersedes it and both link to each other.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-license.md) | Apache License 2.0 | Accepted |
| [0002](0002-self-hosted-gpu-runner.md) | Self-hosted RTX 3060 Ti performance runner | Accepted; §4 amended by [0028](0028-gpu-runner-service.md) |
| [0003](0003-build-configurations.md) | Build configurations, presets, and sanitizer policy | Accepted |
| [0004](0004-allocator-interface.md) | Allocator interface | Accepted |
| [0005](0005-atomics-in-core.md) | Atomics live in core; the logger thread lives in platform | Accepted |
| [0006](0006-containers.md) | Core containers: representation and the hash map design | Accepted |
| [0007](0007-serialization-formats.md) | Serialization formats: binary documents, JSON, and cvar config | Accepted |
| [0008](0008-math-conventions.md) | Math module: conventions, storage layouts, and SIMD layering | Accepted |
| [0009](0009-platform-threads-and-memory.md) | Platform layer: threads, synchronisation, virtual memory, and the TLSF locking decision | Accepted |
| [0010](0010-fibers.md) | Fibers: assembly context switch, guard-paged stacks, and the Win32 fallback | Accepted |
| [0011](0011-files-and-async-io.md) | Files, memory mapping, and asynchronous I/O backends | Accepted |
| [0012](0012-window-and-input.md) | Window and input: GLFW behind `nph_window_*`, an event queue, and the action layer | Accepted |
| [0013](0013-job-system.md) | Job system: fiber workers, Chase-Lev deques, futex-style counters, and depth-bounded inline waits | Accepted |
| [0014](0014-gpu-module.md) | GPU module: vendored Vulkan headers, volk and VMA, one bindless layout, and a Vulkan-shaped RHI | Accepted |
| [0015](0015-gpu-budget-measurements.md) | GPU budget measurements on the RTX 3060 Ti and the §10.4 table | Accepted |
| [0016](0016-asset-format.md) | Asset runtime: the `.nph` container, pack archives, the VFS, the LZ codec, and the loader | Accepted |
| [0017](0017-cooker.md) | The asset cooker: derived identities, a JSON database, our own mesh and texture pipelines | Accepted |
| [0018](0018-reference-renderer.md) | The reference renderer: forward PBR in physical units, GPU-computed IBL, pre-exposed HDR | Accepted |
| [0019](0019-golden-image-tests.md) | Golden-image tests: our own LDR-FLIP and PNG codec, lavapipe baselines, calibrated thresholds | Accepted |
| [0020](0020-ecs.md) | The ECS: archetype chunks, access-derived scheduling, 64-bit transforms | Accepted |
| [0021](0021-scene-serialization.md) | Scene files: canonical JSON for the editor, archetype snapshots for the runtime, prefab overrides | Accepted |
| [0022](0022-profiling.md) | Profiling: zones recorded in core, collected per fiber by the profile module | Accepted |
| [0023](0023-runtime.md) | The runtime: one engine context and a pipelined fixed-step frame | Accepted |
| [0024](0024-game-modules.md) | Game modules: C shared libraries against a shared engine, reloaded by migrating the world | Accepted |
| [0025](0025-editor-ui.md) | The editor's interface: Dear ImGui through cimgui, on our own backends, as an engine host | Accepted |
| [0026](0026-editor-import.md) | Editor projects and import: sources copied in, cooked on an import thread, placed through the history | Accepted |
| [0027](0027-editable-meshes.md) | Editable meshes: a polygon source the cooker cooks, edited in place, reloaded where it is drawn | Accepted |
| [0028](0028-gpu-runner-service.md) | The GPU runner as a standard runner service: no stored token, no elevated loop | Accepted |
| [0029](0029-gpu-driven-visibility-buffer.md) | The production renderer: GPU-driven two-phase culling into a 64-bit visibility buffer | Accepted |
| [0030](0030-hybrid-raster-and-meshlet-work-lists.md) | The hybrid raster: a compute rasteriser for small triangles, meshlet work lists, and a partial second pyramid | Accepted |
| [0031](0031-light-grid-and-compact-hdr.md) | Fused tile shading: the light grid, the HDR target in B10G11R11, and one resolve pass | Accepted |
| [0032](0032-ray-traced-shadows.md) | Ray-traced shadows: the ground truth for every shadow technique, and the production path's first | Accepted |
| [0033](0033-virtual-shadow-maps.md) | The sun's virtual shadow map: a resolution-matched clipmap of cached pages | Accepted |
| [0034](0034-temporal-anti-aliasing.md) | Temporal anti-aliasing at native resolution, fused with the tonemap | Accepted (its §10.4 line open; the review's options settled by [0041](0041-frame-target-gates.md)) |
| [0035](0035-bloom-and-exposure.md) | Bloom and automatic exposure | Accepted (adds to ADR-0034's open §10.4 line) |
| [0036](0036-motion-blur.md) | Motion blur after the temporal resolve, weighted by the shutter | Accepted (adds to ADR-0034's open §10.4 line) |
| [0037](0037-depth-of-field.md) | Depth of field through a thin lens: slight blur gathered at full resolution, large at half | Accepted (adds to ADR-0034's open §10.4 line) |
| [0038](0038-ambient-occlusion.md) | Ground-truth ambient occlusion at half resolution, from the depth pyramid's reads | Accepted; amended 2026-10-20: full resolution (past §10.4's indirect-light line: [0041](0041-frame-target-gates.md)) |
| [0039](0039-radiance-cache.md) | A hashed world-space radiance cache, kept by a fixed ray budget | Accepted (0.20 to 0.41 ms; with ADR-0038 past §10.4's indirect-light line in the sky-lit field; patent review before release) |
| [0040](0040-reference-scenes.md) | Reference scenes: third-party content outside the repository, pinned by manifest | Accepted (Bistro measures 7.86 ms against the 4.17 ms frame; gated at Phase 14's exit by [0041](0041-frame-target-gates.md)) |
| [0041](0041-frame-target-gates.md) | The frame target's gate: Phase 14's exit for the reference scenes, Phase 15 for the game | Accepted |

## Template

```markdown
# NNNN. Title

- **Date:** YYYY-MM-DD
- **Status:** Proposed | Accepted | Superseded by [NNNN](NNNN-title.md)

## Context
What forces are at play: requirements, constraints, measurements.

## Decision
What we chose, stated so that it can be checked against the code.

## Alternatives considered
Each rejected option and why.

## Consequences
What becomes easier, what becomes harder, what must be revisited later.
```
