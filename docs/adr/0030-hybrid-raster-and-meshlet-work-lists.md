# 0030. The hybrid raster: a compute rasteriser for small triangles, meshlet work lists, and a partial second pyramid

- **Date:** 2026-10-17
- **Status:** Accepted (small masked clusters joined the software raster by
  [ADR-0071](0071-small-masked-clusters-in-software.md))

## Context

CLAUDE.md §8.6 stage 4 asks for hybrid rasterisation: "large triangles use
the hardware rasterizer; micro-triangles use a compute software rasterizer
with 64-bit image atomics". [ADR-0029](0029-gpu-driven-visibility-buffer.md)
built the visibility buffer so that both rasters can write it the same
way, and left two measured problems against the §10.4 budgets:

- Meshlet culling took a 64-lane workgroup per batch of an instance's
  meshlets, so instances of one or two meshlets left most lanes idle:
  102,400 one-meshlet cubes took 0.90 ms to cull.
- Culling with the depth pyramids took 0.42 ms at 4K against 0.25 ms,
  each pyramid reading the whole 66 MB visibility buffer.

Measurements are `render_perf` on the RTX 3060 Ti, Profile build (the
4K field: 16,385 instances, 5.3 million triangles in the visible clusters).

## Decision

1. **Meshlet work lists.** The instance cull writes one (instance,
   meshlet) item per meshlet of every instance it keeps. A subgroup's
   instances write together: one atomic for the subgroup's total, then
   each invocation writes an equal share of the items, finding each item's
   instance by a binary search over the subgroup's inclusive prefix sums
   (subgroup shuffles, now a requirement of the production path). The
   meshlet cull runs an invocation per item. Phase 1's candidates, the
   meshlets phase 0 deferred and those of deferred instances found
   visible, share one list. `max_cull_batches` becomes
   `max_cull_meshlets` (2^20).
2. **The software raster** (`vis_sw_raster.comp`) takes an opaque
   cluster when its bounding sphere projects in front of the near plane
   into at most 64 pixels across and its triangles average at most
   `sw_raster_pixels` (default 3) pixels across (the square root of the
   projected box's area over the triangle count). A 32-thread workgroup
   per cluster snaps the vertices to 1/256 pixel, culls back faces, and
   walks each triangle's pixel centres with 32-bit integer edge functions
   relative to the triangle's box under the top-left rule (Pineda 1988;
   Direct3D's tie-breaking), writing the atomic max the hardware path's
   fragment shader writes. Masked materials stay in hardware.
3. **The pyramid after phase 1 rebuilds only where phase 1 drew.** Phase
   1's meshlet cull marks the 32-pixel tiles each kept cluster's sphere
   projects onto, or asks for a full rebuild when the sphere reaches the
   near plane or covers more than 64 tiles; the pyramid after phase 1
   rebuilds mip 0 in the marked tiles alone, then every coarser mip.

## Measurements

| | Before | After |
|---|---|---|
| meshlet cull, 102,400 one-meshlet cubes | 0.898 ms | 0.059 ms |
| meshlet cull, 4K field (phase 0) | 0.098 ms | 0.038 ms |
| raster, 4K field | 0.555 ms (hardware) | 0.504 ms (0.226 hardware + 0.278 software) |
| raster, 102,400 cubes 1.6 px across | 0.436 ms (hardware) | 0.392 ms (software) |
| pyramid after phase 1, 4K field | 0.147 ms | 0.066 ms |
| GPU frame, 4K field | 2.056 ms | 1.885 ms |
| GPU frame, 102,400 cubes | 1.512 ms | 0.638 ms |

How the software raster's parameters were chosen, on the 4K field's
raster (hardware only: 0.545 ms):

| Variant | Raster |
|---|---|
| clusters up to 16 / 32 / 64 px, 64-bit edge functions, 64 threads | 0.531 / 0.547 / 0.615 ms |
| average triangle up to 2 / 3 / 5 px, 32-bit, 64 threads | 0.515 / 0.508 / 0.498 ms |
| average triangle up to 1 / 2 / 3 px, 32-bit, 32 threads | 0.539 / 0.511 / 0.494 ms |

32 threads at 3 pixels is the choice. Larger thresholds gain little on
the field (0.498 ms at 5 pixels with 64 threads) and already at 3 a grid
of 10,240 cubes of 12 triangles 5 pixels across, which the hardware
rasterises in 0.045 ms, goes to the software raster at 0.049 ms (0.081 ms
with 64 threads and 64-bit arithmetic).

Correctness: a scene whose 273 clusters all went to the software raster
gives the hardware raster's image to the bit on the RTX 3060 Ti, so the
snapping, the top-left rule, and the depth interpolation match NVIDIA's; a
finely divided plane drawn by both shows no background through it; the
golden scenes score the same through the production path as before.

Culling with the pyramids is now 0.31 ms at 4K against the §10.4 budget
of 0.25 ms. The first pyramid's 0.145 ms is the visibility buffer's 66 MB
at the bus's bandwidth; what remains to cut is the second pyramid's
coarser mips (rebuilt in full) and the six argument passes (0.015 ms).
The §10.4 table's 30 MB for this line cannot hold with an 8-byte
visibility buffer read once per pyramid at 4K: the record that lands the
reference scenes revisits the table with measurements, as ADR-0015
anticipated.

## Alternatives considered

- **Clusters by size alone** (Nanite's criterion, where the LOD system
  keeps triangles near a pixel): without LOD (Phase 13), a small cluster
  can hold a few large triangles that one thread walks for hundreds of
  pixels; measured slower above.
- **64-bit edge functions**: exact for any box, but integer 64-bit
  arithmetic is emulated on the reference GPU; the 64-pixel bound makes 32
  bits exact.
- **A prefix-sum pass over per-instance meshlet counts** instead of the
  subgroup expansion: one more pass and barrier; the subgroup version
  balances within a subgroup, which is where the imbalance hurt.
- **Rebuilding the coarser mips per tile too**: exact only away from the
  image's right and bottom edges, where Vulkan's rounded-down mips give the
  last texel an extra row or column; left for when the remaining 0.06 ms
  matters more than the complexity.
- **A 32-bit depth copy written by the raster**, halving the pyramid's
  reads: it doubles the raster's writes and both rasters' atomics; not
  pursued while the pyramid after phase 1 can be skipped instead.

## Consequences

- The production path now requires subgroup shuffles (the RTX 3060 Ti and
  lavapipe have them).
- `NPH_RENDER_VIS_NO_SW_RASTER` and the `r.sw_raster` cvar switch the
  software raster off; the frame statistics count its clusters.
- With the DAG LOD of Phase 13, clusters will carry about a triangle per
  two to four pixels, which is where the software raster wins; its
  threshold is revisited then.

## Amendment (2026-10-20): phase 0's opaque clusters through the ROP

**Context.** Measured on Bistro, the opaque fragments' 64-bit atomics cost
0.16 to 0.24 ms of `vis raster 0` (a build without them), and the transfer
clear of the visibility buffer 0.085 ms (67 MB) before it.

**Decision.** On the mesh path, whose early depth test writes the opaque
clusters' depth, the last opaque fragment of a pixel to pass the test is
its nearest, so phase 0's opaque clusters write the visibility buffer
through the ROP: a pass of their own whose colour attachment is the buffer
viewed as R32G32_UINT (`NphGpuImageDesc.sampled_format` now names the
colour-attachment views' format too), cleared as the pass begins. The
masked clusters (which discard, so cannot write depth early) follow in a
pass of their own with their atomics, then the software raster; the atomic
max merges them with what the ROP wrote. Phase 1 keeps the atomics: its
depth attachment lacks phase 0's masked and software surfaces.

**Measurements** (interleaved, three rounds): the clear went, the opaque
pass drew faster, and the frame took 7.38 ms against 7.50 in the street,
8.19 against 8.30 on the terrace, 7.88 against 7.98 at the hedge and 4.12
against 4.22 in the 4K field.

**Alternatives.** A 32-bit identifier target beside the depth attachment,
merged into the 64-bit buffer afterwards: the merge reads 16 bytes a pixel
and writes 8 (0.3 to 0.45 ms at 4K). The ROP for phase 1 too: it could
cover the nearer masked and software surfaces of phase 0.

**Consequences.** Equal depths of two opaque triangles keep the last drawn
where the atomic kept the larger index, both arbitrary (the clusters'
indices come from atomics in the cull). The vertex path, without a depth
attachment, keeps the atomics and the transfer clear.

## Amendment (2026-10-21): the masked skin

**Context.** In Bistro's foliage views most of the raster is masked
fragments: layer after layer of leaf cards, each running its alpha test
(an anisotropic texture sample) and its atomic, since a masked fragment
cannot write depth early (a discarded one would occlude). Writing it after
the alpha test instead, for every masked cluster, measured slower (2.60 ms
against 1.85 at the hedge): the late test lost the early rejection behind
opaque surfaces.

**Decision.** On the mesh path, the masked clusters at the front of their
instance, the skin, draw first with the alpha test and a late depth write;
the other masked clusters follow with the early test against it. A
cluster is skin where its centre lies within the nearest share of the
chord its view ray cuts through the instance's box
(`NphRendererDesc.masked_skin`, 0.35, swept on Bistro). The skin's list is
a fourth raster kind (`NPH_VIS_RASTER_SKIN`); the vertex path, without a
depth attachment, has none.

**Measurements** (interleaved, two rounds): the hedge's frame 7.85 →
7.38 ms (masked pass 1.46 → 0.97), the terrace 8.20 → 7.84, the turn
7.84 → 7.53, the street unchanged. The image is unchanged, pixel for
pixel (the depth written is that of a fragment that passed its alpha
test).

**Alternatives.** Classifying alpha per region offline (certainly opaque
or transparent texels per mip, as opacity micromaps do for rays), clipping
cards to their opaque hull, and a software raster for small masked
triangles: each more machinery for part of the same layers. A depth-only
masked pre-pass: every layer's alpha test twice.

**Consequences.** `NphRendererDesc.masked_skin`, `NPH_RENDER_VIS_NO_SKIN`,
`NPH_VIS_VIEW_SKIN` and the pipeline `vis raster (mesh, masked skin)`. The
share suits Bistro's hedges and cypresses; content of other depths may
want another.
