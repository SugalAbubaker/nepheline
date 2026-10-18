# 0034. Temporal anti-aliasing at native resolution, fused with the tonemap

- **Date:** 2026-10-18
- **Status:** Accepted (the §10.4 line it exceeds is open: see Consequences)

## Context

CLAUDE.md §8.6 stage 8 asks for TAA at native resolution, used only for
anti-aliasing and never for upscaling (Karis, "High Quality Temporal
Supersampling", SIGGRAPH 2014), and for the post passes to be fused into
as few passes as possible, the last one writing the swapchain image.
§10.1 counts TAA among what does not fake frames: every frame is rendered
from fresh input at every pixel, and only the history is reused. §10.4
gives TAA, post, and the tonemap together 0.40 ms and 170 MB of DRAM
traffic per 4K frame. ADR-0015 asks that a pass's primitive be measured
before the pass is written.

The production path has no G-buffer: what the resolve shades is a 64-bit
visibility buffer (depth, cluster, triangle) and a B10G11R11 HDR image, and
blended surfaces are drawn into the HDR image after the resolve. The
tonemap pass was a full-screen fragment pass reading the HDR image and
writing the caller's target as a colour attachment (0.11 ms at 4K).

Measurements are on the RTX 3060 Ti, Profile build, at 3840 × 2160:
`gpu_bench` for the microbenchmarks, `render_perf`'s 4K field
(`field_4k_production` against `field_4k_no_taa`) for the pass, and
`test_taa.c` for the images.

## Decision

1. **Jitter.** A view with `NPH_RENDER_AA_TEMPORAL` on the production path
   renders with its projection moved by the next of eight Halton (2, 3)
   points (two to each quarter of the pixel; Karis 2014), through the
   column that multiplies view z, so every pass draws and rebuilds
   positions at the jittered samples alike. Culling's occlusion tests, the
   depth pyramid's dirty tiles, and the light grid move their projected
   rectangles by the jitter of the frame they are tested against
   (`NphVisViewData.jitter`, this frame's and the previous one's), so
   nothing is culled or binned against a place the raster does not draw.
   Debug views and the reference renderer are never jittered; debug lines
   are drawn after the resolve with the jitter taken off.
2. **One fused pass** (`taa.frag`): a full-screen fragment pass with two
   colour attachments, the caller's target and the next history, so the
   HDR image is read once and the target stays a colour attachment (its
   framebuffer compression kept). It replaces the tonemap pass on such
   frames.
3. **Motion from the visibility buffer.** The nearest surface in the
   pixel's 3 × 3 neighbourhood (Karis 2014) is found by four gathers of the
   visibility buffer's depth words, through an R32G32_UINT sampled view of
   the R64_UINT image (`NphGpuImageDesc.sampled_format`, new in the RHI),
   then one fetch of its record. Its point is rebuilt in camera-relative
   space through this frame's jittered inverse projection and taken through
   the previous frame's view and projection without its jitter: a still
   surface moves with the camera alone (the camera's motion from the high
   and low floats of both positions), an instance whose transform changed
   (`NPH_VIS_INSTANCE_MOVED`, kept by the scatter pass and copied to its
   clusters) through its previous transform, and the sky's points at
   infinity with the rotation alone. The history lies that motion back
   from the pixel's centre.
4. **The resolve** (`taa_common.glsl`):
   - the current frame filtered over the 3 × 3 neighbourhood with a
     Gaussian of a box filter's spread (sigma 1/sqrt(12) px), weighted by
     each sample's distance from the pixel's centre under the jitter
     (weights from the CPU in the push constants);
   - the history read through a five-tap Catmull-Rom filter (Jimenez,
     "Filmic SMAA", SIGGRAPH 2016) and rescaled by this frame's exposure
     over the history's;
   - clipped in YCoCg toward the centre of the neighbourhood's variance box
     within its extremes (Salvi, "An Excursion in Temporal Supersampling",
     GDC 2016; Pedersen, "Temporal Reprojection Anti-Aliasing in INSIDE",
     GDC 2016), the box at least 1/32 of its luma wide;
   - blended with weights divided by 1 + luma (Karis 2014), the current
     frame's weight 1/16 still and growing to 1/4 at a pixel a frame of
     motion (Yang, Liu & Salvi, "A Survey of Temporal Antialiasing
     Techniques", 2020);
   - a non-finite colour replaced by black, then the tonemap.
5. **The history** is two images of the HDR format (B10G11R11 where it can
   be rendered to and sampled), written in turn, stochastically rounded to
   the format's grid with interleaved gradient noise moved by 5.588238
   pixels a frame (so the rounding differs every frame and the average
   does not drift). It continues only from the frame just before, resolved
   temporally into the same extent, without `NphRenderView.camera_cut`;
   otherwise the frame stands alone.
6. **API and runtime.** `NphRenderView.anti_aliasing` (default
   `NPH_RENDER_AA_NONE`: every frame stands alone, as tests and tools want)
   and `camera_cut`; frame statistics `taa` and `taa_history`. The runtime
   turns it on (`r.taa`, default on), and a switch between the world's
   camera and the host's override is a cut.
7. **Measured before written.** `gpu_bench` gains the pass's primitive in
   each candidate form, and the tests hold its images to a supersampled
   reference (below).

## Measurements

**The primitive** (`gpu_bench`, over noise; ms per 4K frame):

| Form | B10G11R11 history | RGBA16F history |
|---|---|---|
| tonemap alone | 0.161 | |
| fused fragment, 3 × 3 neighbourhoods by nine fetches | 0.756 | 0.780 |
| fused fragment, the cross (five fetches) | 0.624 | 0.673 |
| fused fragment, 3 × 3 shared across each quad (four fetches and quad swaps) | 0.932 | 0.922 |
| compute pass with shared-memory neighbourhoods, then the tonemap | 0.990 | 1.015 |

Switching parts off one at a time put the history's four extra taps at
0.12 ms, the colour neighbourhood's eight extra fetches at 0.11, the
depth's at 0.07, the tonemap at 0.05, and per-pixel filter weights at
0.02. The quad form trades fetches for shuffles and 71 registers; the
compute form pays a second pass and was slower even stripped. The fused
fragment form is the one kept.

**The pass** in the 4K field: 0.79 ms (`taa`), against 0.11 ms for the
tonemap it replaces, and the resolve 0.03 ms slower with the jitter
(0.93 against 0.90 ms): the frame goes from 1.907 to 2.663 ms. It reads
the HDR image, the visibility buffer, and the history and writes the
target and the history: 24 bytes a pixel, 199 MB a frame, whose transfer
alone is 0.48 ms at the 415 GB/s the bus sustains (ADR-0015). With every
optional part switched off the pass measured 0.49 ms, that floor; on top
of it, before the gathers: the 3 × 3 search for the nearest surface by
64-bit storage loads 0.16 ms (0.09 after: four gathers and a fetch),
the Catmull-Rom taps 0.10, the 3 × 3 colour neighbourhood 0.09, reading
moving instances' transforms 0.05, the guard and the motion weight
0.04. The pipeline holds 48 registers, without spills.

**The images** (`test_taa.c`; mean absolute error in 8-bit levels against
the reference renderer at 4 × 4 the resolution averaged in linear light,
64 × 64 views of rotated cubes over black, after 32 frames):

| Case | One sample per pixel | Temporal |
|---|---|---|
| still | 1.222 | 0.393 |
| camera panning 0.37 px a frame, cubes at two depths | 1.242 | 0.646 |
| cube moving 1.3 px a frame | 0.618 | 0.334 |

No case leaves anything two pixels away from an edge (a trail would). The
same on lavapipe within 0.002 levels. What shaped it:

- *The clipping box's floor.* A flat neighbourhood's box has no chroma
  extent, and the history's per-channel rounding (blue keeps a mantissa bit
  fewer) gives a grey history a little chroma: divided by a near-zero
  extent, the clip pulled the whole history, luma included, to the box's
  centre every frame, blurring edges into a 3 × 3 mean (still: 1.98
  levels, worse than without TAA). The floor of 1/32 of the luma, the
  history's own quantisation, removed it (0.76).
- *The filter's width.* Karis's Gaussian fit of the Blackman-Harris window
  (sigma 0.47 px) steadies a frame's sample but softens edges: still 0.76
  levels with it, 0.47 at sigma 0.35, 0.39 at the box's 0.29 (kept), 0.37
  at 0.25.
- *Motion.* Every frame that reads the history at a fractional offset
  softens its edges a little; a current weight growing to 1/4 at a pixel a
  frame (measured with the sigma-0.25 filter) brings panning from 0.68 to
  0.63 and the moving cube from 0.42 to 0.32, against 0.66 and 0.36 with
  growth to 0.15 over two pixels.
- *The search for the nearest surface.* With the 3 × 3 search the moving
  cube measures 0.33 and panning 0.65; with the cross 0.74 and 0.72; with
  none 1.51 and 1.66, worse than without TAA. The 3 × 3 stays.
- *Exposure.* The first frame after an exposure change differs from the
  converged image of the new exposure by 0.07 levels (9 at most); without
  the rescale, by 1.09 (27).

## Alternatives considered

- **The compute form, or neighbourhoods shared across quads** (above):
  slower here; the compute form also needs a second pass, as a swapchain
  image is not a storage image.
- **RGBA16F history:** finer steps, but 0.02 to 0.05 ms slower and 66 MB
  more a frame; with the stochastic rounding the B10G11R11 history is
  unbiased.
- **The cross for the colour neighbourhood** (0.09 ms less): diagonal
  edges leave the box looser; not taken while the pass's cost question
  below is open.
- **A velocity image written by the resolve:** the resolve knows each
  pixel's motion, but writing it (4 bytes) and reading it back costs what
  the pass's read of the visibility buffer's depth words saves, and the
  search for the nearest surface needs the depths anyway.
- **Folding the history blend into the resolve:** the resolve already
  reads the visibility buffer and writes the HDR image; blending the
  history there would cost only the history's read (4 bytes a pixel
  instead of 16 more), but the neighbourhood clip needs colours from
  other workgroups (shading an apron is out of the question at 0.9 ms of
  shading), and the blended surfaces are drawn after the resolve. It is
  the largest lever left (see Consequences).
- **Upscaling from a lower resolution:** excluded by §10.1.
- **Karis's filter width, a constant blend weight, no search or the
  cross for the nearest surface:** measured above.

## Consequences

- **The §10.4 line is not met, and cannot be by a pass of this shape.**
  TAA + post + tonemap is budgeted 0.40 ms and 170 MB; TAA with the tonemap
  alone takes 0.79 ms and moves 24 bytes a pixel (199 MB), whose transfer
  alone (0.48 ms) exceeds the line. The table's estimate of about 20 bytes
  a pixel had no read for motion. Per §10.4 this is reported with its
  measurements, not hidden: the image keeps native resolution and full
  quality. The options, for the phase's review: fold the history blend
  into the resolve (above; about 0.5 ms back, at the cost of tile-border
  neighbourhoods and blended surfaces outside TAA); cheaper settings
  (bilinear history taps, 0.10 ms; the cross, 0.09 ms) with the errors
  measured above; or rebalance the table from the headroom (0.57 ms).
  Bloom and auto-exposure (stage 8's next part) come on top.
- Every production frame in `render_perf` resolves temporally (the
  runtime's default), so the production scenes' budgets are re-baselined
  (`render_perf-2026-10-18`): about 0.76 ms more each at 4K;
  `field_4k_no_taa` keeps the frame without it. `gpu_bench`'s reference
  run is now `gpu-2026-10-18` (the earlier entries within 1 % of
  `gpu-2026-10-13`), with budgets for the new benchmarks.
- The golden scenes and the image comparisons between pipelines render
  without TAA (the default view); the editor's save-and-reload comparison
  turns it off.
- New: `NphRenderAntiAliasing`, `NphRenderView.anti_aliasing` and
  `camera_cut`, statistics `taa` and `taa_history`, the `r.taa` cvar,
  `NphGpuImageDesc.sampled_format`, `NphGpuFeatures.subgroup_fragment`,
  `NPH_VIS_INSTANCE_MOVED` and `NPH_VIS_CLUSTER_MOVED`, the `taa` and
  `debug lines` passes.
- Open: transparency drawn into the HDR image is resolved with the opaque
  surfaces' motion (blended surfaces have no visibility-buffer record);
  the history is dropped at a cut but not at a teleport the host does not
  announce (the clip limits what shows); there is no sharpening.
