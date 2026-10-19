# 0037. Depth of field through a thin lens: slight blur gathered at full resolution, large at half

- **Date:** 2026-10-19
- **Status:** Accepted (the cost is reported against §10.4's post line: see Consequences)

## Context

CLAUDE.md §8.6 stage 8 lists depth of field among the post effects, fused
into as few passes as possible. §10.1 asks for native resolution: the parts
of an image in focus must stay as sharp as they were drawn. The camera had
only a field of view; a lens needs an f-number, a focus distance, and a
sensor, whose height with the field of view gives the focal length.

A thin lens of focal length f and f-number N, focused at z_f, blurs a point
z away into a circle of confusion of diameter f² |z − z_f| / (N z (z_f − f))
on the sensor (Potmesil & Chakravarty, "A Lens and Aperture Camera Model for
Synthetic Image Generation", SIGGRAPH 1981). Its exact image is the average
of pinhole images seen through every point of the aperture, each with the
plane at z_f held still: the accumulation buffer (Haeberli & Akeley, "The
Accumulation Buffer", SIGGRAPH 1990), which also gets partial occlusion
right, at the cost of a frame per lens point.

Post-process depth of field in production gathers rather than scatters:
each pixel collects the light of the surface points whose circles cover it
("scattering as a gather", Jimenez, "Next Generation Post Processing in
Call of Duty: Advanced Warfare", SIGGRAPH 2014), at half resolution, with
layers in front of and behind the pixel's surface. Unreal's Diaphragm DOF
(Abadie, "A Life of a Bokeh", SIGGRAPH 2018) adds a full-resolution
"slight out of focus" gather, because half resolution is too coarse for
small circles.

Measurements are on the RTX 3060 Ti, Profile build, at 3840 × 2160, in
`render_perf`'s field through a full-frame lens wide open (f/1.4, 24.8 mm at
the camera's 0.9 rad field of view) focused at 5 m, 2.6 m, or 0.6 m;
images are `test_dof.c`'s, 64 × 64 views through a sensor ten times a full
frame (circles of several pixels, as a 4K view shows them), against the
average of 64 frames through points spread over the aperture.

## Decision

1. **API.** `NphRenderView.aperture` (the f-number; 0, the default, is a
   pinhole), `focus_distance` (metres, beyond the focal length),
   `sensor_height` (metres; 0 is full frame, 0.024), and `lens_offset`, a
   point on the lens to see the frame through: the camera moves there and
   the projection shears so the focus plane stays put, which renders the
   accumulation buffer's frames (for captures, and the tests' ground
   truth). `nph_render_view_focal_length` gives f. Both pipelines, lit
   views only; frame statistic `dof`. The runtime gives the world camera's
   lens (the Camera component's `aperture`, `focus_distance`, 10 m by
   default, and `sensor_height`; a lens that cannot focus there is a
   pinhole) and `r.dof` (on) turns it into a pinhole.
2. **The circle** in pixels, negative in front of the focus, is
   K (1 − z_f / z) with K = f² H / (2 N (z_f − f) h) for H pixels and a
   sensor h tall: with the reverse-Z depth d = znear / z it is bias +
   scale · d, one multiply-add. It is cut at 32 pixels, a tile, so no
   circle reaches past the tiles around its own.
3. **Where:** after the scene and before bloom (which then glows from the
   blurred image) and the final pass, whose temporal resolve averages the
   gathers' samples, turned by interleaved gradient noise that moves every
   frame.
4. **Passes:**
   - *Setup* (`dof_setup.comp`, a workgroup per 32-pixel tile): every
     pixel's depth in half floats; the half-resolution image, each texel
     the average colour of its 2 × 2 pixels and the nearest of their
     depths; each tile's nearest and farthest circle.
   - *Tiles* (`dof_tiles.comp`): each tile's 3 × 3 neighbourhood bounds
     every circle that can reach its pixels, and so its class: **sharp**
     (all under half a pixel), **slight** (none over 8 pixels,
     `NPH_RENDER_DOF_SLIGHT`), or **large**; and the largest circle within
     two tiles says whether a tile within one is large.
   - *Gather* (`dof_gather.comp`): the large tiles and the tiles beside
     them at half resolution, a sample per 2 square texels of the disc,
     8 to 48 (texture fetches: its apron of 16 texels in shared memory
     would take 27.6 KiB, past the 16 KiB every Vulkan device has).
   - *Resolve* (`dof_resolve.comp`, a workgroup per quarter tile), into a
     new HDR image that the rest of the frame reads: sharp tiles copied as
     drawn; slight tiles gathered at full resolution, from their pixels
     and 8 more around in shared memory (12 KiB, each texel's colour,
     depth, circle, and density worked out once), a sample per 8 square
     pixels, 8 to 48; large tiles composited from the gather's layers.
5. **The gather** (`dof_gather.glsl`, one source for both): a spiral turned
   by the golden angle at radii sqrt((i + 0.5) / n) of the disc (Vogel's
   equal-area pattern), each sample standing for an equal share of it, the
   disc's radius the largest circle that can reach the pixel. A sample is
   classified by its depth against the pixel's own (wholly in front once 5
   % nearer). Samples in front reach the pixel where their circles cover
   it, each bringing 1 / (π c²) of its light per unit of area: the near
   layer, its coverage the alpha. The others, within the pixel's own
   circle, average into the far layer: the pixel's surface and what its
   circle reveals behind it. (Weighting the level samples and those behind
   apart, then mixing them by their weights, is the same average.)
6. **The composite** of a large tile: a pixel in focus keeps its colour and
   one out of focus takes the far layer, blending as its circle grows from
   half a pixel to a pixel and a half, with the near layer over either.
   Both layers come from the four half-resolution texels around the pixel,
   those of another surface (1 % apart in depth: none of it; 5 %: a
   hundredth) all but left out; where the four are all of the pixel's
   surface, by two bilinear fetches. Beside a blurred surface in front,
   whose texels carry the pixel's near layer only from a texel and more
   further out, the pixel also takes that surface's texels' own colour
   (their far layer under their near) by its bilinear weights and their
   blur, so the blur's edge runs on across the sharp edge under it; a
   sharp surface in front, no blur, leaves the pixel as it is.

## Measurements

**The images** (`test_dof.c`; mean absolute error in 8-bit levels against
the lens; the same on lavapipe within 0.003 levels):

| Case | Pinhole | Half resolution, layers by circle | Half resolution, layers by depth | With the full-resolution slight gather (a sample per 4 px²) | **Kept** |
|---|---|---|---|---|---|
| a box 10 m away behind one in focus at 3.7 m (circles 3.8 px, the sky 6.1) | 7.12 | 3.05 | 3.23 | 1.87 | **2.05** |
| the same on the reference renderer | 7.10 | 2.97 | 3.15 | 1.81 | **1.98** |
| the same, temporally resolved (supersampled truth) | 6.13 | 2.53 | 2.66 | 1.43 | **1.18** |
| a box 1.35 m away over a wall in focus (circles 12 px; half resolution) | 9.35 | 1.37 | 1.37 | 1.37 | **1.14** |

At half resolution a 2 × 2 block across an edge becomes one texel under
its nearest depth with the block's average colour, and the upsample reads
a texel and a half away: the box's light reached 2 pixels further than the
lens's, a dim halo over the black sky that sRGB levels count heavily
(classifying by circle had also let the box's colour into the sky beside
it, its 3.8-pixel circle within the threshold of the sky's 6.1). Gathered
at full resolution, with the near box's 12-pixel circles: 1.22, little
better than half resolution, at four times the samples; the kept 1.14
comes from the front-texel blend of Decision 6, which took a step of 9
levels off the near box's sharp silhouette (half resolution before it:
1.37). The kept slight gather samples half as densely as the one measured
(a sample per 8 px², as the half-resolution gather): single frames are
noisier, and the temporal resolve's image better. A new test renders a
card in focus over a wall in focus while a blur 11 pixels across fills the
view's tiles: the card's edges stay bit for bit as drawn (dropping the
blend's blur factor fails it in 18 rows).

What remains is a post-process's limit: in the first case the far box's
blurred edge lies behind the box in focus, whose body the lens's rays
graze and the image cannot show (46 levels at the meeting column), and the
gather's noise in single frames.

**The cost** in the 4K field (ms; `render_perf-2026-10-19b`):

| Scene | Frame | setup | tiles | gather | resolve |
|---|---|---|---|---|---|
| no depth of field (`field_4k_production`) | 2.969 | | | | |
| focused at 5 m, circles to 4 px (`dof_4k_production`) | 3.943 | 0.170 | 0.004 | 0.023 | 0.762 |
| at 2.6 m, to 7.7 px (`dof_slight_4k_production`) | 4.511 | 0.169 | 0.004 | 0.022 | 1.342 |
| at 0.6 m, at the 32-px limit (`dof_large_4k_production`) | 4.290 | 0.171 | 0.004 | 0.770 | 0.357 |

The setup reads 99.5 MB (the HDR image and the visibility buffer) and
writes 33.2 (the depths and the half-resolution image); the resolve reads
83.0 MB and writes the new HDR image's 33.2 (declared bytes, upper
bounds). The gather's 0.02 ms in the first two scenes is its workgroups
finding nothing large around them; the resolve's 0.36 ms in the third, the
composite with its bilinear fast path.

**What shaped it**, the depth of field's passes together in the three
scenes (focused at 5 m, 2.6 m, 0.6 m; single runs):

| Step | 5 m | 2.6 m | 0.6 m |
|---|---|---|---|
| half resolution, the slight gather at full resolution by texture fetches, a composite in place | 1.34 | 3.43 | 1.96 |
| the slight gather from shared memory | 1.40 | 3.00 | 1.78 |
| the far layer as one average; the depth test one multiply-add a sample | 1.22 | 2.51 | 1.66 |
| the slight gather a sample per 8 px² instead of 4 | 1.10 | 1.66 | 1.66 |
| the composite's bilinear fast path | 1.09 | 1.65 | 1.61 |
| one resolve into a new image instead of the slight image and the composite in place | 0.96 | 1.53 | 1.54 |
| the half-resolution gather at most 48 samples instead of 64 | **0.96** | **1.53** | **1.28** |

Shared memory alone hardly helped: the gathers are bound by the
instructions they issue, about 49 a sample (180 G samples a second, 128
lanes on each of 38 multiprocessors). Leaving out the colour's half-float
conversions took the slight gather from 2.11 to 1.99 ms, and the
distance's square root to 2.08: neither fetches, special functions, nor
conversions lead, and the sample count is the lever. The composite in
place needed the slight blur in an image of its own, a round trip of 12
bytes a pixel; the resolve reads and writes 8 for every tile, copies
included.

## Alternatives considered

- **Half resolution throughout** (Jimenez 2014): measured above, its edges
  carry 2 pixels of light too far, too much of a 4- to 8-pixel circle.
- **Full resolution throughout:** four times the half-resolution gather's
  samples for the large circles, for 11 % less error at 12 pixels.
- **Layers by circle instead of depth:** surfaces at different depths but
  similar circles (anything far behind the focus) count as one.
- **A composite in place**, rewriting the HDR image: measured above.
- **Shared memory for the half-resolution gather:** 27.6 KiB; Vulkan
  guarantees 16. The device layer does not report the limit yet; when it
  does, the gather can take the slight path's way where the device allows.
- **Scattered sprites for bright highlights** (Abadie 2018): shaped bokeh,
  a sprite a bright texel; the gather's bokeh is a uniform disc.
- **Half-float sums:** the gather would issue fewer instructions, but sums
  of pre-exposed highlights overflow half floats.
- **A smaller slight class** (circles to 6 pixels): half resolution at
  6 to 8 pixels; the sky of the first case would cross back.

## Consequences

- **The cost when on is reported, not hidden:** a full-frame lens wide
  open (f/1.4) adds 0.97 ms to a 4K frame with circles up to 4 pixels,
  1.54 ms up to 7.7 (the most the full-resolution gather takes), and
  1.32 ms at the 32-pixel limit: past §10.4's post line, which ADR-0034 to
  ADR-0036 report exceeded already. A pinhole (the default) costs nothing,
  and the other scenes moved by no more than their run-to-run noise.
- `render_perf` gains `dof_4k_production`, `dof_slight_4k_production`, and
  `dof_large_4k_production`, budgeted like the other production scenes;
  every scene is re-baselined (`render_perf-2026-10-19b`).
- New: `NphRenderView.aperture`, `focus_distance`, `sensor_height`,
  `lens_offset`, `nph_render_view_focal_length`,
  `NphRenderFrameStats.dof`, the Camera component's `aperture`,
  `focus_distance`, and `sensor_height`, the `r.dof` cvar, and the passes
  `dof setup`, `dof tiles`, `dof gather`, and `dof resolve`.
- Open: circles are cut at 32 pixels (a full-frame 50 mm lens at f/1.4
  focused at 2 m gives 41 at 4K); what a sharp surface in front hides from
  the image cannot show through its blur; bokeh is a uniform disc (no
  blades, no vignetting's cat's eyes); blended surfaces take the depth of
  the opaque surface behind them; the gather's noise is left to the
  temporal resolve and the eye.
