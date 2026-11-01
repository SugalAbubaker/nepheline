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
2. **In the opaque clusters' own list and dispatch,** with a branch a cluster. The material's alpha test
   is read once a cluster, as the mesh stage reads it, and the vertices' UV and alpha go to shared memory.
3. **Each covered pixel runs the test before its atomic,** with `textureGrad` and the derivatives of the
   triangle's UV plane in screen space, which are what the hardware takes across a quad of the same
   plane. So a pixel takes the same level and the same footprint as in hardware.
4. **The UV is interpolated in screen space,** without perspective's correction. The 64-pixel bound
   keeps a cluster's w within about 2% of its middle at 4K, so the shift is under 2% of a small
   triangle's UV span.

## What it does

The RTX 3060 Ti at native 4K, Profile, the editor minimized, the median of three rounds, interleaved,
against the commit before. Declared traffic and GPU memory are unchanged: no list, no pass, no buffer.

**The city** (ms):

| View | Mean | 1%-low | `vis raster 0 masked` | `vis sw raster 0` | Clusters in software |
|---|---|---|---|---|---|
| street | 6.30 -> 6.30 | 6.38 -> 6.35 | 0.52 -> 0.49 | 0.05 -> 0.08 | 5,017 -> 7,312 |
| walk | 6.35 -> 6.35 | 6.52 -> 6.50 | 0.55 -> 0.52 | 0.05 -> 0.08 | 5,116 -> 7,604 |
| sprint | 6.50 -> 6.50 | 6.74 -> 6.72 | 0.56 -> 0.54 | 0.04 -> 0.06 | 1,526 -> 2,221 |
| turn | 6.32 -> 6.29 | 7.12 -> 7.13 | 0.34 -> 0.33 | 0.03 -> 0.03 | 1,193 -> 2,356 |
| crossing | 6.40 -> 6.34 | 6.55 -> 6.48 | 0.49 -> 0.34 | 0.15 -> 0.26 | 23,668 -> 35,390 |
| park | 7.53 -> 7.43 | 7.59 -> 7.50 | 1.46 -> 1.21 | 0.04 -> 0.21 | 3,172 -> 26,267 |
| rooftop | 9.83 -> 9.41 | 9.92 -> 9.52 | 2.63 -> 1.20 | 0.89 -> 1.93 | 157,014 -> 274,198 |

**Bistro** (ms), whose foliage is near:

| View | Mean | 1%-low |
|---|---|---|
| street | 6.91 -> 6.91 | 6.99 -> 6.98 |
| walk | 6.96 -> 6.99 | 7.08 -> 7.08 |
| turn | 7.16 -> 7.18 | 7.68 -> 7.70 |
| terrace | 7.46 -> 7.48 | 7.53 -> 7.55 |
| hedge | 7.11 -> 7.13 | 7.17 -> 7.20 |

It has about a thousand such clusters a view, which take as long in either raster, so it gains nothing.
Its +0.00 to +0.02 ms is about each round's spread; every view moved the same way, so it is taken as a
small real cost, kept for the far views' gains.

**The images.** A test compares the two rasters over a texture whose levels hold different holes, so
that a pixel's alpha says which level it sampled: on the RTX 3060 Ti, 99.98% of a field of spheres' pixels
and 99.27% of a slanted sheet's are within 3 levels (lavapipe 99.90% and 99.15%). Dropping the alpha test,
the derivatives of either axis or the interpolation, or scaling the derivatives, fails it.

## Alternatives

Each measured on the city, one round, except where it says otherwise.

- **A list and a dispatch of their own,** which keep the opaque path's code as it was: the same gains on
  the city, a seventh list of `max_visible_clusters` entries, and a pass that costs Bistro its floor and its
  barrier (ADR-0070) in every view. The branch costs nothing that shows: the rooftop's two dispatches took
  0.89 and 1.05 ms, and the one dispatch 1.94.
- **Larger triangles in software** (with that dispatch of their own). Up to 4 pixels: the rooftop -0.29 ms,
  against -0.42 ms at 3, and the streets +0.04 to +0.09. Up to 6: the rooftop -0.14, the park +0.15. Up to
  12: the rooftop +0.14, the park +0.57. The compute path pays for its fetch and its atomic pixel by pixel,
  where the hardware rejects fragments behind the depth attachment before their shader. A smaller limit, 2
  pixels: the rooftop -0.25.
- **Perspective's correction,** which the hardware does: 0.02 ms more on the rooftop, and invisible by
  point 4.
- **Masked clusters through the ROP** stays rejected (docs/render.md "Masked clusters"): discarding and
  writing depth loses the early test on this GPU.
- **Clusters of up to 128 vertices rather than 64,** since a far view's clusters average 41 triangles of
  their 124: foliage's cards are four vertices to two triangles, so a 64-vertex meshlet holds 32 of them,
  and flat-shaded walls are not much better. Recooked with that limit, the rooftop's clusters fall by 37%
  (490,000 to 310,000) and its culling is 0.23 ms cheaper, but a larger cluster culls less and its levels
  are coarser steps, so its triangles rise by 20% (20.1 to 24.2 million), the raster goes with them
  (`vis raster 0` 1.12 to 1.58 ms, `vis sw raster 0` 1.94 to 2.26), and the frame is 9.40 -> 10.03 ms. Every
  other view is slower too: the street +0.05, the park +0.18. The raster's cost follows triangles, not
  clusters.

## Consequences

- The rooftop is 2.6 ms from the gate, not 3.0, and the park 0.56, not 0.65. This does not reach it.
- What remains on the rooftop goes with its 20 million triangles, against the 4 million of §8.6 stage 9, so
  it is a matter for the levels of detail, not the raster.
- `reference_scenes-2026-11-01` re-baselines the budgets: masked time moved into `vis sw raster`.
