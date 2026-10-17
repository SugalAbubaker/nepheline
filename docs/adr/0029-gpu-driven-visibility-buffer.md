# 0029. The production renderer: GPU-driven two-phase culling into a 64-bit visibility buffer

- **Date:** 2026-10-17
- **Status:** Accepted

## Context

Phase 10 (CLAUDE.md §12) replaces the reference renderer as the production
path with §8.6 stages 3 to 8, against two exit criteria: a CPU cost that
does not grow with the object count, and the reference scenes holding a
1 %-low frame time of 4.17 ms at native 4K on the RTX 3060 Ti with every
pass inside its §10.4 budget. This record covers the first part, stages 3
and the hardware half of 4: scene data on the GPU, culling and draw
generation on the GPU, and a visibility buffer resolved by one shading
pass. The software rasteriser, tile classification, shadows, global
illumination, and temporal work follow in their own records.

Constraints that shaped it:

- §10.3: no G-buffer, 8 bytes per pixel for visibility; geometry bounded by
  pixels through culling; occlusion culling that reuses the previous
  frame's depth.
- §8.2: 64-bit world positions on the CPU, camera-relative 32-bit on the
  GPU, at any distance from the origin.
- §10.5: zero per-frame allocation, a constant number of command buffers,
  render submission within 0.3 ms.
- The reference renderer stays the ground truth (ADR-0018): the production
  images must match it, so both must shade with the same code.
- CI renders on lavapipe, which has mesh shaders, 64-bit image atomics, and
  subgroups of 8 but differs from the RTX 3060 Ti in limits and, as found
  here, in one bug.

## Decision

1. **Persistent GPU scene tables indexed by pool slot**: instances (a
   camera-relative transform as high and low floats, Cozzi & Ring 2011),
   last frame's transforms, meshes, and a material map whose per-instance
   ranges come from a TLSF allocator over offsets (`core/range_alloc.h`;
   Masmano et al. 2004, with the 8-bit floating-point classes of
   Aaltonen's OffsetAllocator). Changes travel as at most
   `max_instance_updates` records per frame, scattered by compute. The CPU
   never walks the instances per frame: its cost is the view, the changed
   records, and the pass declarations (5 to 6 µs from 1,024 to 102,400
   instances in `render_perf`, against 0.2 to 13.4 ms for the reference).
2. **Meshlets of 64 vertices and 124 triangles** with bounding spheres and
   normal cones, built by the render module (moved from the cooker, which
   wraps it with byte-identical output) so run-time meshes get them too.
3. **Two-phase occlusion culling** (Haar & Aaltonen, SIGGRAPH 2015; Karis
   et al., "Nanite", SIGGRAPH 2021): phase 0 tests instances and meshlets
   against last frame's depth pyramid with last frame's transforms and
   camera and draws the survivors; a pyramid of that depth then tests what
   phase 0 deferred, and phase 1 draws what disocclusion revealed. Meshlets
   are also culled by frustum and by normal cone (similarity transforms
   only). Sphere projection follows Mara & McGuire (JCGT 2013); the pyramid
   is the farthest depth per texel, its mips sized as Vulkan sizes them,
   with the last texel of an odd row or column taking the extra source
   texel, which keeps every query conservative.
4. **A 64-bit visibility buffer** (R64_UINT: depth bits, then a 25-bit
   cluster and a 7-bit triangle) written with `imageAtomicMax`, with no
   depth attachment, so the software rasteriser of stage 4 writes the same
   buffer the same way. The hardware raster uses mesh shaders (32 threads
   per cluster, back-face and small-primitive culling per triangle,
   Wihlidal GDC 2016) and falls back to `vkCmdDrawIndexedIndirectCount`
   over a pool of 16-bit indices where mesh shaders are missing or broken.
5. **One resolve pass** rebuilding attributes from the visibility buffer
   with analytic barycentrics and their screen derivatives (Schied &
   Dachsbacher, HPG 2015) and shading with `surface.glsl`, the file the
   forward pass includes. Blended surfaces stay forward, over the resolved
   image and the depth the visibility buffer holds.
6. **Correctness gates**: images against the reference (100 % of pixels
   within 3 levels); culling switched off changes nothing; mesh-shader and
   indirect raster give the same image; every culling test against a
   double-precision C twin on thousands of random cases; the built pyramid
   read back and checked texel by texel.
7. **The engine renders with this path by default** (`r.pipeline=1`); the
   reference stays a cvar and a view field away.

The RHI gained what this needed, each with its own test: overlapping
uploads within one batch are ordered (a barrier when a copy overlaps what
the batch already wrote to the buffer); the render graph orders every read
after the last write and every write after every read, per resource and
mip, instead of keeping one stage set per resource; consecutive passes of
a batch share a command buffer, one per recording thread, instead of one
per pass; `NphGpuDeviceInfo.quirks` records driver bugs, the first being
lavapipe's mesh pipelines handing the fragment stage zeros for push
constants (so lavapipe takes the indirect raster, and the RTX runner now
runs the render tests too).

## Measurements

`render_perf`, Profile, RTX 3060 Ti, Ryzen 7 5800X
(`benchmarks/results/render_perf-2026-10-17-win-msvc-profile-rtx3060ti.json`):

| Scene | CPU, reference | CPU, production | GPU, reference | GPU, production |
|---|---|---|---|---|
| grid of 1,024 cubes, 512² | 0.20 ms | 0.005 ms | 0.05 ms | 0.15 ms |
| grid of 10,240 cubes, 512² | 1.26 ms | 0.005 ms | 0.17 ms | 0.27 ms |
| grid of 102,400 cubes, 512² | 13.4 ms | 0.006 ms | 3.0 ms | 1.51 ms |
| field of 16,385 instances, 3840 × 2160 | 2.87 ms | 0.006 ms | 2.69 ms | 2.06 ms (p99 2.47) |

The 4K field per pass, against §10.4:

| Passes | Time | Declared bytes | §10.4 budget |
|---|---|---|---|
| culling (both phases, with argument passes) + two pyramids | 0.16 + 0.26 ms | 184 MB + 155 MB | 0.25 ms, 30 MB |
| clear + raster (both phases) | 0.08 + 0.56 ms | 66 MB + 186 MB | 0.65 ms, 200 MB |
| resolve (all shading, one sun, image-based light) | 0.88 ms | 156 MB | 1.20 ms, 250 MB |
| tonemap | 0.12 ms | 100 MB | part of 0.40 ms, 170 MB |

Declared bytes are the render graph's upper bound (whole resources as
declared), not measured traffic: the culling passes declare buffers sized
for 524,288 clusters and 119,041 instances while this scene touches a
fraction of them. Measured DRAM traffic awaits performance counters.

What is over budget, and the plan:

- **Culling and pyramids, 0.42 ms against 0.25 ms.** Each pyramid build
  reads the whole visibility buffer (66 MB at 4K, 0.13 ms). A single-pass
  downsampler that keeps mip 0 in on-chip memory and writes every mip in
  one dispatch, and a 32-bit depth copy written by the raster, are the two
  candidates; they get their own measurements.
- **Meshlet culling of tiny instances.** Each instance gets its own
  batches of 64 meshlets, so 102,400 one-meshlet cubes leave 63 lanes in 64
  idle (0.90 ms). Packing batches across instances fixes it.
- **The resolve** fits now, but evaluates every material and light for
  every pixel in one shader; tile classification (stage 5) replaces it.

## Alternatives considered

- **Keep CPU culling and only move draws to indirect commands.** The CPU
  would still walk every instance per frame, which the exit criterion
  forbids, and occlusion culling needs the GPU's depth anyway.
- **A 32-bit visibility buffer with a hardware depth attachment.** Smaller
  and with early depth testing for the hardware raster, but the software
  rasteriser cannot share a depth attachment; Nanite's 64-bit atomics keep
  one buffer for both. The pyramid cost above may still argue for writing
  a 32-bit depth copy alongside, which the measurement will decide.
- **Single-phase occlusion culling against last frame's pyramid only.**
  Cheaper, but objects revealed by motion disappear for a frame; the
  second phase costs 0.17 ms here, most of it the extra pyramid (0.13 ms),
  which a cheaper pyramid build shrinks too.
- **A G-buffer.** §10.3 rules it out on bandwidth: 20 to 30 bytes per pixel
  written and read again, against 8.
- **Per-pass command buffers in the render graph.** Simple and maximally
  parallel, but their number grows with the passes, and a 40-pass frame
  exhausted a thread's pool.
- **Working around lavapipe's push constants with per-primitive outputs**
  in the mesh path. Correct everywhere, but it costs the production
  hardware attribute memory to fix a software driver; the indirect path
  covers lavapipe and the RTX runner covers mesh shaders.

## Consequences

- The production path is the default in the engine, the editor, and the
  samples; the reference stays the comparison every production change is
  tested against.
- The software rasteriser (stage 4) plugs into the same visibility buffer
  and lists; tile classification (stage 5) replaces the resolve;
  shadows, global illumination, and TAA read the visibility buffer and the
  pyramid.
- `render_perf` and `benchmarks/render_budgets.txt` hold every production
  pass to its recorded time plus 5 % on the RTX runner (§10.7); the §10.4
  table is revisited by the record that lands the reference scenes.
- Capacities are fixed per renderer (`NphRendererDesc`); a list that fills
  drops work visibly (`vis_overflow`, logged), never silently.
