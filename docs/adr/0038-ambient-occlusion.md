# 0038. Ground-truth ambient occlusion at half resolution, from the depth pyramid's reads

- **Date:** 2026-10-19
- **Status:** Accepted (the cost is reported against §10.4's indirect-light line: see Consequences)

## Context

CLAUDE.md §8.6 stage 7 lists GTAO beside the radiance cache under indirect
lighting (Jimenez, Wu, Pesce & Jarabo, "Practical Realtime Strategies for
Accurate Indirect Occlusion", ATVI-TR-16-01, 2016). Until now the
environment's light reached every surface whole except where a material's
occlusion texture darkened it: corners, contacts, and crevices glowed.

Ambient occlusion is the cosine-weighted visibility of a point's hemisphere,
A = (1/π) ∫ V(ω) ⟨n, ω⟩⁺ dω (Eq. 2). GTAO integrates it in slices through
the view vector: in each, the visible arc lies between the two horizons the
depth buffer shows, and its cosine-weighted measure has a closed form
(Eq. 7) that, scaled by the length of the normal projected into the slice
and averaged over the slices' angles (Eq. 8), is the integral itself. The
paper's implementation runs at half resolution with one slice per pixel,
16 directions spread over 4 × 4 pixels for a bilateral filter, and 6
rotations for the temporal filter: 96 directions a pixel, 0.5 ms on a PS4
at 1080p (§4.1). Its Eq. 10 fits the light that occluders of the surface's
albedo reflect back as a cubic in A. XeGTAO (Intel, 2021) takes the far
samples from mips of the depth.

The production path's visibility buffer holds every pixel's reverse-Z
depth; §10.4 gives indirect light (the radiance cache's sampling and update)
0.60 ms. Measurements are on the RTX 3060 Ti, Profile build, at 3840 ×
2160 in `render_perf`'s field (`field_4k_production`, the sky lighting it);
images are `test_ao.c`'s 128 × 128 views against a ground truth the test
casts itself: each pixel's surface found by its ray, then 576 stratified
cosine-weighted rays a pixel against the scene's boxes, occluders fading
over the last tenth of the radius as the shader's do.

## Decision

1. **API.** `NphRenderView.ao_radius`: the radius in metres within which
   surfaces occlude the environment's light (0, the default: none). The
   production path makes the occlusion for lit views and the indirect
   debug view when the environment lights the view (not with
   `disable_ibl` or no environment), and for the occlusion debug view; the
   reference renderer has none. Frame statistic `ao`. The runtime's `r.ao`
   (0.5 m, as XeGTAO's default radius; saved) sets it.
2. **Half resolution** (§4.1): texel (i, j) stands for pixel (2i, 2j).
   The depth pyramid's passes, which read every pixel's depth anyway, also
   write each 2 × 2 block's top-left depth (`vis_hzb.comp`: phase 1's pass
   rewrites the tiles it draws into), and `ao depth` (`ao_depth.comp`)
   averages 4 more mips. Averaging reverse-Z depth, which is affine in
   screen position across a plane, keeps every mip's points on the planes
   they come from; the extent is rounded up to 16 texels so every mip
   halves it exactly and one uv addresses a point in all of them.
3. **The slices** (`ao.comp`): each texel takes one, its angle one of 16
   given by a 4 × 4 Bayer matrix (Bayer 1973), turned by a sixth of a
   step each frame under temporal anti-aliasing (the resolve averages the
   rotations; without it they stay put). The angles are spread evenly
   around the view vector, as Eq. 8 integrates them, and each slice's
   screen direction is the projection's derivative along it, so the
   samples along that screen line lie in the slice.
4. **The horizons:** 4 steps a side, evenly out to the radius (at most a
   quarter of the image's height), jittered by the transposed matrix and a
   golden-ratio step a frame. A step 32 pixels out or more reads mip 1,
   twice as far mip 2, and so on (hardware bilinear). Nearer ones gather
   the 4 texels of mip 0 around the point, at the corner they share, and
   interpolate them exactly where they lie on one plane (the
   parallelogram rule), else take the nearest: a sample along the slice
   then lies in the slice's plane, where a texel's centre a little aside
   would rise above a plane seen at a grazing angle and occlude it. An
   occluder's weight falls from 1 to 0 over the last tenth of the radius
   (§4.1's attenuation), samples off the image count nothing, and the
   horizons stay within the hemisphere around the projected normal.
5. **The slice's math** in closed form: the normal from the depth's
   screen gradient (reverse-Z depth is m · r for the plane's vector m and
   the ray r, so m.xy is the gradient over the unprojection and the
   normal is −m normalised; on each axis the side nearer in depth, the
   image's own at its edges); each sample's offset split along the view
   vector and the slice's other axis without rebuilding a point
   ((x − d) r + x s w, with r and w in the slice's plane); the visible
   arc by Eq. 7 with acos to within 6.8 × 10⁻⁵ (Abramowitz & Stegun
   4.4.45). Each texel stores its slice's visible share |n_p| a and its
   open share |n_p| (cos γ + γ sin γ), Eq. 7 with the horizons at the
   hemisphere's edges, 8 bits each, with its distance as a half float.
6. **The denoise** (`ao_denoise.comp`): each texel weighs the 5 × 5 texels
   around it with a tent (the outer rows and columns at half), which holds
   each of the 16 directions equally and is centred; a neighbour counts as
   it lies on the texel's tangent plane, nothing once off it by 5 % of the
   distance (tested exactly from inverse distances, which are affine
   across the plane). Taps beyond the image take the texel 4 inside, whose
   slice runs the same way. The occlusion is the window's visible shares
   over its open shares: a slice's open share is 1 only on average over
   the directions, so where an edge cuts some out, a plain average biases
   an open surface; the ratio keeps it open and equals the average where
   the window is whole. Out: the occlusion and the depth, half floats in
   a word.
7. **Shading** (`vis_resolve.glsl`, `surface.glsl`): the resolve takes
   the four texels around each pixel, weighted bilinearly and by depth
   (another surface, 5 % apart: a hundredth). The environment's specular
   light takes specular occlusion from the material's occlusion times the
   screen's (Lagarde & de Rousiers, "Moving Frostbite to PBR", 2014), and
   its diffuse light the screen occlusion through Eq. 10, per channel of
   the albedo, within [A, 1]; direct light is untouched.

## Measurements

**The images** (`test_ao.c`; NVIDIA, lavapipe within 0.002):

| Case | Kept |
|---|---|
| a unit box on the ground, radius 0.5 m: where the truth is occluded (A < 0.9, 1,107 pixels), the error as a share of the occlusion | 0.20 (bias +0.049: a little light) |
| the same, ground nothing occludes (12,989 pixels): mean error, and pixels off by over 0.03 | 0.0031, 0.5 % |
| the ground alone, out to its horizon: the most occluded pixel | 0.009 (lavapipe 0.000) |
| the diffuse light's bounce against `nph_render_ref_ao_bounce`: albedo 0.8, 0.2 | within 0.004, 0.007 |

What remains in the box's case is screen space's limit: behind the box's
silhouette the depth cannot tell how thick it is, and a few pixels there
are occluded as if it went on. Before the denoise's ratio and the gather at
the shared corner, the open ground was off by 0.0075 on average, 3.5 % of
it by over 0.03, and the lone ground's far edge 0.044: the edge cut
directions out of the windows there, and slices that run along a row of
texels had the hardware pick the gather's texels by its own rounding, one
row from those the weights were for, which put points above the plane.
With 6 steps a side instead of 4 the occluded share was 0.16.

**The cost** at 4K with the runtime's 0.5 m (`render_perf-2026-10-19c`; ms,
and the declared megabytes read and written, upper bounds):

| Pass | Time | Declared |
|---|---|---|
| `vis hzb 0` and `vis hzb 1`: the depths' mip 0 | +0.016 | +8.3 written by each |
| `ao depth` | 0.025 | 8.4 / 2.8 |
| `ao` | 0.166 | 11.1 / 8.3 |
| `ao denoise` | 0.112 | 8.3 / 8.3 |
| `vis resolve`: the upsample and the shading | +0.063 | +8.3 read |
| **the frame** (`field_4k_production` against `field_4k_no_ao`) | **+0.378** (3.360 against 2.982) | |

**What shaped it**, the occlusion's passes with the pyramid's share (ms at
4K, a 1 m radius but in the last row; single runs; the resolve's upsample
adds 0.05 to 0.07 throughout):

| Step | The passes |
|---|---|
| full resolution, on the visibility buffer | 2.66 |
| half resolution with the depth's mips, 6 steps a side | 0.50 |
| 4 steps a side | 0.45 |
| the occlusion and the distance packed in a word; normals from shared memory | 0.44 |
| the denoise's windows inside the image unrolled; fewer special functions; one uv for every mip | 0.37 |
| mip 0 written by the depth pyramid's passes; mip 1 a 2 × 2 block an invocation | 0.30 |
| samples in the slice's coordinates | 0.29 |
| the denoise's ratio of shares (the image fixes above), at 0.5 m | **0.32** |

The occlusion pass alone, without a single step, takes 0.07 ms: its depth
reads and packed writes at the bandwidth these passes reach. Each step a
side adds 0.022 ms.

## Alternatives considered

- **Full resolution** (XeGTAO's default): 2.66 ms at 4K, the slices
  bound by the visibility buffer's 8 bytes a pixel.
- **6 steps a side:** the occluded share 0.16 for 0.05 ms more.
- **Samples at texel centres:** a centre beside the slice rises above a
  plane seen at a grazing angle and occludes it.
- **The paper's average of the slices:** biased where an edge cuts
  directions out of a window (measured above).
- **The thickness heuristic** (Eq. 9): it would brighten what remains
  behind silhouettes, at the cost of occlusion that thick occluders do
  cast; the box's case already leans light (+0.049).
- **A depth pass of its own:** reading the visibility buffer again cost
  0.100 ms; the pyramid's passes add 0.016.
- **Async compute:** after the second pyramid this path runs only compute
  until the frame ends, so the occlusion could overlap only another
  frame's raster; left to the render graph's scheduling.

## Consequences

- **The cost when on is reported:** the occlusion adds 0.38 ms to a 4K
  frame the sky lights, 63 % of §10.4's 0.60 ms indirect-light line,
  which the radiance cache (stage 7's other part) shares: unless the cache
  fits in the remaining 0.22 ms, the line is past its budget. Frames
  without the environment's light (the night scenes) pay nothing, and
  `r.ao 0` turns it off.
- `render_perf`'s production scenes take the runtime's 0.5 m, and
  `field_4k_no_ao` shows what it costs; every scene is re-baselined
  (`render_perf-2026-10-19c`).
- The depth pyramid's passes write the occlusion's depths (`NphVisPush`
  gains `arg4`).
- New: `NphRenderView.ao_radius`, `NphRenderFrameStats.ao`, the `r.ao`
  cvar, `nph_render_ref_ao_bounce`, and the passes `ao depth`, `ao`, and
  `ao denoise`.
- Open: the thickness heuristic; occlusion of the environment's specular
  light from the bent normal (the paper's §5 cones) rather than from A;
  the occlusion is not accumulated on its own (the temporal resolve
  averages the rotations in the image); blended surfaces and the
  reference renderer get none.
