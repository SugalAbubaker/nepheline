# 0036. Motion blur after the temporal resolve, weighted by the shutter

- **Date:** 2026-10-19
- **Status:** Accepted (the cost is reported against §10.4's post line: see Consequences)

## Context

CLAUDE.md §8.6 stage 8 asks for motion blur among the post effects, fused
into as few passes as possible, the last one writing the swapchain image.
§10.1 counts every frame as real: motion blur adds no frames and lowers no
resolution, it only spreads each pixel over what crosses it while a
shutter is open. At 240 frames a second a 180-degree shutter is open for
1/480 s, so the motions of play (a turning camera, a strafe, a passing
object) blur by a few pixels; a fast flick by a few tens.

The production path has no velocity buffer: the temporal resolve
(ADR-0034) rebuilds the motion of the nearest surface around each pixel
from the visibility buffer, taking still surfaces with the camera and moved
instances through their previous transforms. ADR-0034 and ADR-0035 report
the §10.4 line for TAA, post, and the tonemap (0.40 ms) already exceeded.

The reconstruction filters in use are gathers over tiles of dominant motion:
McGuire, Hennessy, Bukowski & Osman, "A Reconstruction Filter for Plausible
Motion Blur" (I3D 2012), and its successor by Guertin, McGuire &
Nowrouzezahrai, "A Fast and Stable Feature-Aware Motion Blur Filter" (HPG
2014), which samples a second direction, weighs samples by how their motion
lines up, compares depths relatively, and jitters tile edges.

Measurements are on the RTX 3060 Ti, Profile build, at 3840 × 2160, in
`render_perf`'s 4K field (`field_4k_production` and the
`motion_*_4k_production` scenes: the camera turning 60 degrees a second,
strafing at 5 m/s, or flicking at 360 degrees a second, each with a
180-degree shutter at 240 frames a second); images are `test_motion_blur.c`'s,
against the ground truth a shutter integrates: the average of 32 sharp frames
at instants across the time it is open.

## Decision

1. **API.** `NphRenderView.shutter`, how long the shutter is open in
   seconds (0: none). A pixel blurs over its surface's motion since the
   previous frame times `shutter / frame_seconds`, centred on the frame's
   instant (half before, half after). Only the production path blurs, only
   lit views, and only a frame that follows a production frame of the same
   extent without a cut (`RenderVis.prev_frame`): the motion is measured
   against it. The runtime gives the world camera's `shutter` (a new Camera
   field, 0 by default) and closes it with `r.motion_blur` (on by default).
   Frame statistic `motion_blur`.
2. **After the temporal resolve, fused with the tonemap.** On a frame that
   blurs, the temporal resolve writes only its history (`taa_history.frag`,
   the same source as `taa.frag` without the display colour), and the blur
   reads that history and writes the target as a colour attachment, with
   the tonemap (`motion_blur.frag`). The resolve then sees the frame's
   surfaces sharp, as its reprojection and clipping assume, and no frame's
   blur reaches the next one's history. Without temporal anti-aliasing the
   blur reads the HDR image and mixes the bloom in itself. The debug lines
   follow either final pass.
3. **Motion** (`motion.comp`): a workgroup per 32-pixel tile, an invocation
   per 2 × 2 pixels (two gathers of the visibility buffer: the cluster
   words, the depths). A pixel's motion is its surface's
   (`vis_previous_uv`, now shared with the temporal resolve); a still
   point goes from this frame's clip space to the previous frame's through
   one matrix, `NphVisViewData.reproject` = previous view-projection ×
   the camera's move (in doubles on the CPU) × this frame's inverse. The
   blur's half-extent is cut to a tile (31.94 pixels), so no blur reaches
   past the tiles around its own. The motion image packs the half-extent
   (sixteenths of a pixel) and the view distance (log2 in 64ths of an
   octave, 1.1 % steps) into one word (`motion_common.glsl`). Each tile
   keeps its longest blur and its spread, how far any of its pixels' blurs
   lies from that one.
4. **Neighbourhoods** (`motion_tiles.comp`): each tile's dominant blur is
   the longest in its 3 × 3 tiles ("NeighborMax", McGuire et al. 2012), a
   diagonal tile counting only when its blur runs along a diagonal of the
   same sense, the only way it reaches past the corner (Guertin et al. 2014
   §4.4); the neighbourhood's spread bounds every pixel's distance from it.
5. **The gather** (`motion_blur.frag`), at each pixel, from the dominant
   blur of its tile, or near a tile's edge at random of the tile across it
   (Guertin et al. 2014 §4.2):
   - *Uniform:* where the spread is under half a pixel, or a tenth of the
     blur's length (a turning camera's field varies smoothly, by 73 % from
     the centre to the edge of a 90-degree view), the pixel is the average
     of the colour along the dominant blur: bilinear taps a pixel apart (up
     to two for the longest), 4 to 16.
   - *Layered:* elsewhere, samples along the dominant blur and, every other
     one, a second direction, the pixel's own blur's or, where it barely
     moves, the dominant one's perpendicular (Guertin et al. 2014 Eq. 1),
     classified against the pixel by a relative depth test (5 % of the
     nearer distance; Guertin et al. 2014 §5 blend over the whole of it).
     The weights are the shutter's: while it is open a surface point sweeps
     the segment its blur spans and covers a pixel on it for 1 / (2 reach)
     of the time per pixel of segment, so a sample, standing for its share
     of the line sampled, covers the pixel for that long. Samples in front
     cover the pixel for the time their sweeps cross it; samples behind
     show through for the time the pixel's own surface, sweeping its own
     segment, has left it; samples of the pixel's own surface give the
     colours that pass over it. The pixel is the front layer over its own
     surface over the background it reveals. Four samples per pixel of
     reach (two per direction), 4 to 32.
   Interleaved gradient noise (Jimenez 2014), moving every frame, jitters
   the samples.

## Measurements

**The images** (`test_motion_blur.c`; mean absolute error in 8-bit levels
against the shutter's average; the same on lavapipe within 0.002 levels;
the comparison columns measured before the motion image was packed, which
moved the kept one by 0.007 levels at most):

| Case | Sharp | Guertin et al.'s weights, their softness (1) | Theirs, softness 0.05 | The shutter's, softness 1 | **The shutter's, softness 0.05 (kept)** |
|---|---|---|---|---|---|
| a box moving 8 px a frame, the shutter open the whole frame | 4.258 | 1.829 | 1.829 | 0.730 | **0.733** |
| the same, half open, against its own average | 4.015 | 1.499 | 1.499 | 0.801 | **0.801** |
| the camera turning 6 px a frame (uniform) | 4.339 | 0.769 | 0.763 | 0.559 | **0.590** |
| a box moving behind a still one | 4.529 | 2.022 | 1.961 | 0.896 | **0.841** |
| — of the still box's 121 pixels, kept within a level | | 106 | 121 | 121 | **121** |
| a box moving 4 px a frame under temporal anti-aliasing (supersampled truth) | 1.665 | 1.227 | 1.227 | 0.777 | **0.776** |

Guertin et al.'s cone weights give a moving box over a still background
too much coverage near its edge (0.58 one pixel out where the shutter gives
0.375) and too little background through its own edge, and their
whole-distance softness let a box 60 % farther count 40 % in front of the
still box. What remains is the input's own aliasing: one sample per pixel
places an edge at 23.6 pixels on pixel 24, so the ramp's outermost pixel is
lost. The 64 × 64 views hold 2 × 2 tiles, so every test runs the layered
path where anything moves; the turning camera's runs the uniform one.

**The cost** in the 4K field (ms; `render_perf-2026-10-19`):

| Scene | Frame | TAA | motion | tiles | blur |
|---|---|---|---|---|---|
| no motion blur (`field_4k_production`) | 2.950 | 0.825 | | | |
| turning, 60°/s (`motion_turn_4k_production`) | 3.548 | 0.768 | 0.226 | 0.005 | 0.425 |
| strafing, 5 m/s (`motion_strafe_4k_production`) | 3.530 | 0.760 | 0.229 | 0.005 | 0.390 |
| flicking, 360°/s (`motion_flick_4k_production`) | 3.460 | 0.769 | 0.226 | 0.005 | 0.580 |

The temporal resolve writing its history alone saves about 0.06 ms. What
shaped it, measured in the turning scene in single runs unless said:

- *The motion image:* RGBA16F (8 bytes a pixel) to a packed word (4):
  the motion pass 0.406 → 0.340 ms, and every layered sample reads half the
  bytes.
- *Gathers* for the 2 × 2 pixels' records instead of four fetches: 0.340 →
  0.333 ms, in the noise. Skipping the cluster's flags (treating every
  surface as still) gave 0.313 ms and skipping the motion's arithmetic
  0.179 ms: the pass was paying for the matrices and camera positions every
  pixel loaded through the view's buffer reference, about 200 bytes of L1
  traffic per pixel.
- *The joined matrix* (`reproject`): one matrix-vector product for a still
  point instead of two and the camera's move: 0.333 → 0.225 ms. The
  temporal resolve shares the function: between the baselines
  (`render_perf-2026-10-18` and `-19`) it went from 0.863–0.885 to
  0.821–0.832 ms in every 4K production scene, and their frames from 0.055
  to 0.075 ms faster.
- *The uniform test:* at half a pixel of spread, a 360-degree-a-second turn
  took the layered path at the view's sides, whose field varies by 4.3
  pixels a frame across a neighbourhood: the blur took 2.619 ms. With a
  tenth of the blur's length allowed: 1.037 ms.
- *The uniform taps* capped at 16 (two pixels apart at the flick's 14.7
  pixels of reach): 1.037 → 0.570 ms.

## Alternatives considered

- **Blurring before the temporal resolve**, into the HDR image it reads:
  the history would denoise the gather's samples and no history-only
  variant would be needed, but the history would carry the blur. In a
  moving object's blur beyond the 3 × 3 nearest-surface search the motion
  is the background's, which points the history at where the blur was a
  frame ago, and the clip box of a smooth blurred neighbourhood holds it: a
  trail.
- **A velocity image written by the resolve or the temporal resolve:** it
  would save the motion pass's 66 MB read of the visibility buffer, but
  the resolve is the most expensive pass and bound by its registers
  (ADR-0031, ADR-0033), and the temporal resolve by its texture fetches
  (ADR-0034). The motion pass costs 0.23 ms; the lever is left for later.
- **Guertin et al.'s or McGuire et al.'s weights:** measured above.
- **One direction only** (McGuire et al. 2012): Guertin et al. 2014 show
  its seams between tiles and its misses where neighbouring objects move in
  different directions; the second direction costs nothing where the pixel
  moves with the dominant blur (both directions then coincide).
- **Half resolution:** a motion blur is one-dimensional, and the detail
  across the motion must stay sharp.
- **Larger tiles:** a 32-pixel tile limits a blur to 32 pixels either way
  (64 in all, 1.7 % of a 4K view's width): 15,000 pixels a second at a
  1/480 s shutter, more than a flick. Larger tiles dilate the dominant blur
  over more still pixels.

## Consequences

- **The cost when on is reported, not hidden:** with typical motions at
  240 frames a second, motion blur adds 0.51 to 0.60 ms to a 4K frame
  (the motion 0.23 ms, the blur 0.39 to 0.58 ms, less about 0.06 ms the
  temporal resolve no longer writes), past §10.4's post line, which
  ADR-0034 and ADR-0035 already report exceeded. A closed shutter costs
  nothing (the default). The joined matrix takes 0.04 to 0.05 ms off every
  temporally resolved 4K frame.
- `render_perf` gains `motion_turn_4k_production`, `motion_strafe_4k_production`,
  and `motion_flick_4k_production`, budgeted like the other production
  scenes; every scene is re-baselined (`render_perf-2026-10-19`).
- New: `NphRenderView.shutter`, `NphRenderFrameStats.motion_blur`, the
  Camera component's `shutter`, the `r.motion_blur` cvar,
  `NphVisViewData.reproject`, the passes `motion`, `motion tiles`, and
  `motion blur`; the temporal resolve's body moved to `taa_resolve.glsl`
  and its motion to `vis_common.glsl`; the debug lines' pass to `frame.c`.
- Open: blended surfaces blur with the motion of the opaque surface behind
  them (they have no visibility-buffer record, as for ADR-0034); motion is
  taken as linear across the shutter (an accelerating or turning object
  blurs along its tangent); the gather's noise is not denoised (it moves
  every frame, which at 240 frames a second averages in the eye); the
  first frame after a cut or a change of extent does not blur; the
  reference renderer has no motion blur (its ground truth is the shutter's
  average of sharp frames, as the tests take it).
