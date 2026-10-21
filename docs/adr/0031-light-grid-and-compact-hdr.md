# 0031. Fused tile shading: the light grid, the HDR target in B10G11R11, and one resolve pass

- **Date:** 2026-10-17
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 5 asks for fused tile shading: screen tiles
classified into material bins, one compute pass per bin evaluating the
material and clustered direct lighting (Olsson et al., "Clustered
Deferred and Forward Shading", HPG 2012), intermediates kept on chip, and
FP16 math where precision allows; §10.3.5 asks for R11G11B10 or RGB9E5
HDR targets.

The production path's resolve ([ADR-0029](0029-gpu-driven-visibility-buffer.md))
already evaluated the material and all of its lighting in one compute
pass per pixel, with nothing written between them. What it lacked was a
light structure: every pixel evaluated every light, the reference's way,
and the lights were CPU records rewritten every frame, a main-thread cost
that grew with the light count.

Measurements are `render_perf` on the RTX 3060 Ti, Profile build, at
3840 × 2160 unless stated. Registers and spills are the driver's own
figures (`VK_KHR_pipeline_executable_properties`, which the RHI now
exposes as `nph_gpu_pipeline_statistics`; the renderer logs them for every
compute pass when the device keeps them).

## Decision

1. **Lights live in a GPU table** (`NphVisLight` per pool slot, the
   position split into high and low floats like an instance's
   translation), and a light's record goes up only in the frame after it
   was created, changed, or destroyed. The main thread's cost per frame
   depends on neither the instance count nor the light count.
2. **The light grid**, built on the GPU every frame from the table
   (`NphRenderLightGrid`; Drobot, "Improved Culling for Tiled and
   Clustered Rendering", SIGGRAPH 2017):
   - `vis lights`: each light made camera relative. Directional lights and
     lights of unlimited range go to a global list. The others are bounded
     by the sphere around what they reach (the range; for a spot light the
     smallest sphere around its cone, after Wronski, "Cull that Cone!",
     2017), kept when the sphere meets the frustum, and given the distance
     bins of its nearest and farthest points (1,024 bins evenly spaced in
     log2 distance over 20 octaves from the near plane) and the
     16-pixel tiles its clipped screen rectangle covers (Mara & McGuire,
     JCGT 2013, with the near-plane clipping the culling version omits).
   - `vis light bins`, one workgroup: a counting sort of those lights by
     nearest bin; a running maximum of their farthest bins; and for each
     bin the range of sorted lights that may reach it, from the first
     whose running maximum reaches the bin (a binary search) to the last
     whose nearest bin is at most it.
   - `vis light tiles`: a bit per light and tile, a word of 32 lights at a
     time; the word's lights whose rectangles miss a workgroup's block of
     8 × 8 tiles are dropped together first, and the masks are stored a
     word at a time over every tile so a subgroup's writes are contiguous.

   A pixel evaluates the global lights, then the sorted lights in its
   bin's range whose bit its tile's mask sets. Culling is exact: a light
   left out contributes zero, its windowed falloff or its cone being zero
   at the pixel, and the bounds carry a 10^-4 slack for rounding. Each
   invocation walks its own lights. The view's lights are at most 4,096
   (16-bit indices; the sort's 13 KB of shared memory, under the 16 KB
   every device has).
3. **The shading evaluates the environment and the emission before the
   punctual lights**, so that across the light loop, where the resolve's
   registers peak, only the point's description and one sum stay live;
   the resolve compiles the grid's loop alone (the reference's
   every-light loop stays in the forward shaders).
4. **The production path's HDR target is B10G11R11** where the device can
   store to it, sample it, and blend into it (RGBA16F otherwise; the
   reference keeps RGBA16F). The resolve rounds each colour to the format's
   grid itself, up or down at random with the odds that keep its expected
   value (interleaved gradient noise; Gjøl & Svendsen, "The Rendering of
   INSIDE", GDC 2016), so the coarse steps show as noise rather than bands
   and the value stored is exact whatever rounding the device applies.
5. **One resolve pass over every tile.** Tile classification was built
   and measured, and is not adopted (below).
6. **No FP16 arithmetic** in the shading on this GPU (below).

## Measurements

The light grid, the night field (16,385 instances, lamps of 3 to 8 m range
among the nearer rows):

| Scene | GPU frame | Resolve | Light passes | CPU |
|---|---|---|---|---|
| 1,024 lights, reference renderer | 54.0 ms | (forward 53.9 ms) | | 3.63 ms |
| 1,024 lights, production, every light for every pixel | 47.6 ms | 46.6 ms | 0.013 ms | 0.011 ms |
| 1,024 lights, production, the grid | 2.23 ms | 1.18 ms | 0.047 ms | 0.007 ms |
| 4,000 lights, production, the grid | 3.99 ms | 2.86 ms | 0.131 ms | 0.007 ms |
| the day field (the sun alone), the grid | 1.896 ms | 0.883 ms | 0.013 ms | 0.007 ms |

Before this record the day field took 1.885 ms (resolve 0.87 ms). The
light-tile pass took 0.164 ms for 1,024 lights with its masks stored a
tile at a time (every write in its own cache line) and 0.025 ms with
them stored a word at a time behind the block test.

The resolve's lit tiles in the day field, while the grid code took shape
(the first four rows with the tile bins of decision 5 in place):

| Variant | Registers | Spill space | Lit resolve |
|---|---|---|---|
| grid loop and subgroup walk, lights before the environment | (not read yet) | | 1.26 ms |
| one evaluation site for every light source | 72 | 6.9 KB shared + 16 B local | 1.02 ms |
| the grid's loop compiled out (the reference's loop alone) | 72 | 1.8 KB | 0.82 ms |
| environment first, grid only, subgroup walk | 64 | 3.3 KB | 0.91 ms |
| environment first, grid only, per invocation | 64 | 0.8 KB | 0.82 ms |

With 1,024 lights the subgroup walk took 1.26 ms against 1.12 ms per
invocation: on this GPU invocations loading the same light share the load
anyway, and the walk's bookkeeping costs registers.

Tile classification (decision 5): the two depth pyramids classified each
8 × 8 tile (any surface, or background alone) at no measurable cost, a pass
sorted the tiles into two bins, and the resolve ran once per bin, the
background bin's pipeline (32 registers) never reading the visibility
buffer:

| | Day field | 1,024 lights |
|---|---|---|
| two bins (background + surfaces) | 0.949 ms (0.116 + 0.833) | 1.211 ms (0.089 + 1.122)* |
| one pass | 0.888 ms | 1.164 ms |

\* measured before decision 4, with RGBA16F.

The background bin's own dispatch cost more than its pixels did inside the
full resolve, and reading tiles from a list cost the lit pipeline
registers (1.8 KB of spills against 0.8 KB).

The HDR target (decision 4), in the day field and the golden scenes
(256 × 256, FLIP against the lavapipe baselines; ADR-0019's threshold is
a mean of 0.03):

| | Tonemap | Golden FLIP mean, production (worst scene) |
|---|---|---|
| RGBA16F | 0.127 ms | the reference's to 10^-4 (cooked_scene_normals 0.021) |
| B10G11R11, rounded by the device | 0.109 ms | 0.036, debug_draw (fails); material_grid 0.026 against 0.008 |
| B10G11R11, rounded in the shader | 0.109 ms | 0.0255, cooked_scene_normals; material_grid 0.015 |

The RTX 3060 Ti truncates when converting to the format (Vulkan leaves
the rounding to the implementation), which is what the device-rounded
row shows. The rounding costs the resolve about 0.015 ms, about what the
format saves in the tonemap's read; the format's gain is in the passes of
stage 8 that will read the HDR target (temporal antialiasing, the fused
post), each at half the bytes.

## Alternatives considered

- **3D clusters with a light list each** (Olsson et al. 2012): a list per
  cell and lights × cells of construction work; the distance bins and the
  tile masks each bound one dimension, a word per bin and a bit per light
  and tile, and neither needs the visibility buffer's depth, so the grid
  can be built before the raster (or on the async compute queue).
- **Sorting and binning on the CPU** (as Drobot describes): main-thread
  work that grows with the light count, which Phase 10's exit criterion
  rules out.
- **The subgroup walk of Drobot 2017** (the subgroup's union of lights,
  each light loaded once as a scalar): slower on this GPU, measured above;
  it may pay on GPUs with a scalar register file (AMD), to be measured
  when one is in the matrix.
- **Tile classification** (Garawany, "Deferred Lighting in Uncharted 4",
  SIGGRAPH 2016): measured slower with today's one shading model. It is the
  way to bring a second shading model (clear coat, cloth, hair), whose
  pipeline then holds code the others do not; the specialisation constants
  it needs are in the RHI.
- **RGBA16F**: 0.02 ms more on the tonemap today and twice the bytes for
  every later pass. **RGB9E5** cannot be stored to, rendered to, or
  blended into on the RTX 3060 Ti (vulkaninfo lists sampling and transfers
  alone for it).
- **FP16 arithmetic** (CLAUDE.md §8.6.5): the RTX 3060 Ti's GA104 runs
  FP16 at the FP32 rate outside its tensor cores, so only registers could
  gain, and GGX in half precision needs the minimum roughness raised from
  0.045 to 0.089 (Filament's mediump floor), a visible change to smooth
  materials. Revisited on a GPU with double-rate FP16.

## Consequences

- `max_lights` is at most 4,096; `NphRenderFrameStats.vis_lights` counts
  the lights in view; `NPH_RENDER_VIS_NO_LIGHT_GRID` makes every light
  global, for comparisons.
- The production path's blended surfaces use the grid too (their forward
  shader walks it per fragment).
- Against §10.4's shading line (1.20 ms, 250 MB): the day field takes
  0.90 ms with the light passes, 140 MB declared; 1,024 lights take
  1.23 ms, 2 % over, in a scene where, by the lamps' density and ranges,
  about fifteen reach each point of the lit ground. The reference scenes
  will say what is typical.
- The dither pattern is fixed until temporal antialiasing (stage 8)
  varies it per frame and averages it out.
- The light passes are the async compute queue's first candidates.

## Amendment (2026-10-20): the tile masks culled against the tiles' depths

**Context.** In Bistro's street the grid handed each pixel 11.8 lights,
of which 5.0 reached it: a light's rectangle and its distance bins bound
the box around its sphere, and a pixel near the rim of its disc, at a
distance within its bins, is outside the sphere. Each such light cost a
load and a range test, and, where the lanes of a subgroup walk different
lights, often a whole evaluation in which no lane takes part.

**Decision.** The tile masks wait for the frame's depths. The depth
pyramid's passes (`vis_hzb.comp` modes 0 and 2, which read every pixel's
final depth anyway) keep each light tile's nearest and farthest surface
depths and whether the background shows (`NphVisViewData.light_depths`),
and `vis light tiles` runs after phase 1. A light stays in a tile's mask
for opaque surfaces where its sphere meets the box around the tile's part
of the view between those distances (Harada, McKee & Yang, "Forward+",
Eurographics 2012, for the bounds; the box as Olsson & Assarsson, "Tiled
Shading", JGT 2011, bound tiles); a tile without surfaces keeps none. A
second mask (`NphRenderLightGrid.tiles_blended`) serves blended surfaces,
which lie in front of the opaque ones: from the near plane to the
farthest surface, or every light whose rectangle covers the tile where
the background shows. The box carries the grid's 10^-4 slack, so the
image is unchanged, bit for bit.

**Measurements** (interleaved, three rounds): the lights a pixel tests
fell from 11.8 to 5.7 in the street (5.0 in range), from 10.8 to 4.8 on
the terrace and from 8.8 to 3.1 at the hedge. The resolve took 3.34 ms
against 3.53, 3.25 against 3.45 and 2.84 against 3.02; `vis hzb 0` took
0.024 ms more and `vis light tiles` 0.013 ms more; the frame 0.14 to
0.16 ms less. The 1,024 lamps' resolve took 1.36 ms against 1.42, the
frame the same (the masks' pass 0.025 ms more there); the 4,096 lamps'
frame took 4.80 ms against 4.95 (the resolve 2.57 against 2.83, the masks'
pass 0.18 against 0.09: a light's record is read only by the blocks of
tiles its rectangle reaches). A frame without local lights in view skips
the bounds, whose pass then costs what it did. The scenes that make no use
of the grid moved by up to 0.06 ms (the 100,000-cube grid's culls and
software raster, 8 %) with the allocation of the masks' buffers alone, as
a dummy allocation of the same size showed: where later resources land in
memory, not work.

**Alternatives.**
- Culling per subgroup in the resolve against the subgroup's own depth
  range (8 × 4 pixels, tighter than a tile's): registers in the pass
  that peaks, and the tiles leave 0.7 lights a pixel to win.
- A depth mask per tile (Harada 2012's 2.5D culling) for tiles that span
  an edge: the boxes leave little.
- The tile's bounds reduced per subgroup, each tile's blocks in
  subgroups of their own: 0.008 ms slower in the pyramid's pass than the
  shared-memory atomics, for its memory order.

**Consequences.** The masks need the visibility buffer's depth, so the
grid can no longer be finished before the raster; its lights and bins
still can. Blended and forward-drawn surfaces walk their own mask
(`NPH_SURFACE_GRID_TILES`), twice the masks' memory.
