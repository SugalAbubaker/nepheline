# 0071. Small masked clusters in the software raster

- **Date:** 2026-11-01
- **Status:** Accepted

## Context

[ADR-0030](0030-hybrid-raster-and-meshlet-work-lists.md) left masked materials in hardware, because
the software raster did not sample their alpha. On the city that is now the largest line of its far
views, and what holds them over the 144 FPS gate of [ADR-0067](0067-the-game-is-a-city.md)
([ADR-0070](0070-a-flat-cull-of-the-levels.md), "Consequences"): `vis raster 0 masked` took 2.63 ms on
the rooftop, 1.46 ms in the park, 0.49 ms at the crossing. On the rooftop, of 490,000 clusters and 20
million triangles, 186,000 masked clusters ran at 14 ns each, against 9 ns for an opaque cluster in
hardware and 6 ns in software.

A far leaf's triangle costs little to shade and a lot to reach a pixel: vertex outputs through the mesh
stage, a 2 x 2 quad of fragments with its helpers for a triangle of about a pixel, and a texture fetch
for each of the four. Neither needs doing in a compute raster, which already walks such triangles.

## Decision

1. **Masked clusters take the software raster's rule:** a bounding sphere projecting in front of the
   near plane into at most 64 pixels across, and triangles averaging at most `sw_raster_pixels` (3)
   across. Larger ones stay in hardware, skin and all; a software cluster is never skin.
2. **A list, a pipeline and a pass of their own,** from the opaque raster's source (`vis_sw_raster.glsl`,
   `VIS_SW_MASKED`), so the opaque one is as it was. The material's alpha test is read once a cluster, as
   the mesh stage reads it, and the vertices' UV and alpha go to shared memory.
3. **Each covered pixel runs the test before its atomic,** with `textureGrad` and the derivatives of the
   triangle's UV plane in screen space, which are what the hardware takes across a quad of the same
   plane. So a pixel takes the same level and the same footprint as in hardware.
4. **The UV is interpolated in screen space,** without perspective's correction. The 64-pixel bound
   keeps a cluster's w within about 2% of its middle at 4K, so the shift is under 2% of a small
   triangle's UV span.
5. **Too few go to hardware after all.** Fewer than `sw_masked_clusters` (8,192) a phase do not pay for
   the dispatch's floor and its barrier (ADR-0070). `vis args` knows the count before the raster, so it
   hands them to the masked pass as mesh tasks and leaves the compute dispatch empty, which takes 1 µs.
   Either raster draws them alike, so a frame may go either way. The indirect path has no mesh tasks to
   give them, and always takes the compute raster.

## What it does

The RTX 3060 Ti at native 4K, Profile, the editor minimized, the median of three rounds, interleaved,
against the commit before.

**The city** (ms):

| View | Mean | 1%-low | `vis raster 0 masked` | Software, phase 0 (masked pass) |
|---|---|---|---|---|
| street | 6.31 -> 6.31 | 6.37 -> 6.38 | 0.52 -> 0.53 | 0.05 |
| walk | 6.40 -> 6.39 | 6.58 -> 6.56 | 0.55 -> 0.56 | 0.05 |
| sprint | 6.53 -> 6.53 | 6.75 -> 6.74 | 0.56 -> 0.57 | 0.04 |
| turn | 6.33 -> 6.31 | 7.12 -> 7.12 | 0.35 -> 0.34 | 0.03 |
| crossing | 6.42 -> 6.40 | 6.55 -> 6.54 | 0.49 -> 0.34 | 0.15 (0.12) |
| park | 7.56 -> 7.49 | 7.63 -> 7.58 | 1.47 -> 1.21 | 0.04 (0.18) |
| rooftop | 9.86 -> 9.47 | 9.92 -> 9.51 | 2.63 -> 1.21 | 0.89 (1.03) |

**Bistro** (ms), whose foliage is near and whose views hold about a thousand such clusters, which stay in
hardware:

| View | Mean | 1%-low |
|---|---|---|
| street | 6.91 -> 6.90 | 7.00 -> 6.95 |
| walk | 6.99 -> 6.98 | 7.10 -> 7.07 |
| turn | 7.16 -> 7.16 | 7.68 -> 7.67 |
| terrace | 7.46 -> 7.47 | 7.52 -> 7.55 |
| hedge | 7.11 -> 7.12 | 7.17 -> 7.18 |

**Memory and traffic.** One more list of `max_visible_clusters` entries a phase: 4 MB at the defaults, 16 MB
at the city's lists, which `gpu memory` reads as 3.15 -> 3.17 GB. The two passes declare 255 MB more in
Bistro and 561 MB more in the city (their reads and writes of whole buffers: upper bounds). What they touch
is what the masked pass touched before: the same clusters' vertices and pixels.

**The images.** A test compares the two rasters over a texture whose levels hold different holes, so
that a pixel's alpha says which level it sampled: on the RTX 3060 Ti, 99.98% of a field of spheres' pixels
and 99.27% of a slanted sheet's are within 3 levels (lavapipe 99.90% and 99.15%). Dropping the alpha test,
the derivatives of either axis or the interpolation, or scaling the derivatives, fails it. Another holds
that too few clusters take hardware, and draw its image to the bit.

## Alternatives

Each measured on the city, one round, except where it says otherwise.

- **The opaque raster's own dispatch, with a branch a cluster:** no new list or pass, and the same gains on
  the city and Bistro. But the opaque clusters of a scene with no masked ones at all pay for it: the GPU job
  caught `vis sw raster 0` 7.5% over in every synthetic scene (`field_4k` 0.315 ms against a budget of 0.308),
  where the extra shared memory and the branches slow the walk. My own A/B had looked only at the reference
  scenes, where masked clusters hide it.
- **Two dispatches in the opaque raster's one pass, no barrier between,** since atomics do not care about
  order: the rooftop's phase 0 takes 2.38 ms rather than the 1.93 of two passes, and the gain is gone. The
  dispatches overlap, and one pass does worse than the two in turn.
- **Every small masked cluster in compute, however few:** Bistro +0.02 to +0.05 ms in every view, for two
  non-empty dispatches' floors and barriers.
- **Larger triangles in software.** Up to 4 pixels: the rooftop -0.29 ms, against -0.42 ms at 3, and the
  streets +0.04 to +0.09. Up to 6: the rooftop -0.14, the park +0.15. Up to 12: the rooftop +0.14, the park
  +0.57. The compute path pays for its fetch and its atomic pixel by pixel, where the hardware rejects
  fragments behind the depth attachment before their shader. A smaller limit, 2 pixels: the rooftop -0.25.
- **Perspective's correction,** which the hardware does: 0.02 ms more on the rooftop, and invisible by
  point 4.
- **Masked clusters through the ROP** stays rejected (docs/render.md "Masked clusters"): discarding and
  writing depth loses the early test on this GPU.
- **Clusters of up to 128 vertices rather than 64,** since a far view's clusters average 41 triangles of
  their 124: foliage's cards are four vertices to two triangles, so a 64-vertex meshlet holds 32 of them,
  and flat-shaded walls are not much better. Recooked with that limit, the rooftop's clusters fall by 37%
  (490,000 to 310,000) and its culling is 0.23 ms cheaper, but a larger cluster culls less and its levels
  are coarser steps, so its triangles rise by 20% (20.1 to 24.2 million), the raster goes with them, and the
  frame is 0.63 ms slower. Every other view is slower too: the street +0.05, the park +0.18. The raster's cost
  follows triangles, not clusters.

## Consequences

- The rooftop is 2.57 ms from the gate, not 2.98, and the park 0.64, not 0.69. This does not reach it, and
  it leaves the streets and Bistro as they were.
- What remains on the rooftop goes with its 20 million triangles, against the 4 million of §8.6 stage 9, so
  it is a matter for the levels of detail, not the raster.
- `sw_masked_clusters` is a fixed count from these scenes' frames on this GPU: the crossing's 11,700 clusters
  gain 0.03 ms, Bistro's 1,000 lose 0.02 to 0.05.
- `reference_scenes-2026-11-01` re-baselines the budgets: masked time moved into the new pass.
