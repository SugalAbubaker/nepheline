# 0035. Bloom and automatic exposure

- **Date:** 2026-10-18
- **Status:** Accepted (the §10.4 line ADR-0034 reports over grows: see Consequences)

## Context

CLAUDE.md §8.6 stage 8 asks for bloom and auto-exposure among the post
effects, fused into as few passes as possible, the last one writing the
swapchain image. §10.4 gives TAA, post, and the tonemap together 0.40 ms
and 170 MB per 4K frame; ADR-0034 measured the temporal resolve with the
tonemap alone at 0.79 ms and reported that line unreachable for a pass of
that shape.

Exposure so far was the view's EV100 (Lagarde & de Rousiers, "Moving
Frostbite to PBR", SIGGRAPH 2014): every shading pass writes pre-exposed
colour (radiance / (1.2 · 2^EV100)) into the HDR image, which is
B10G11R11 on the production path and RGBA16F on the reference's. The
production path's final pass is the temporal resolve (`taa.frag`,
fused with the tonemap) on a temporally anti-aliased view and the tonemap
pass otherwise; the reference's is always the tonemap pass. The
temporal history is rescaled by this frame's exposure over the history's,
which the CPU computes.

Measurements are on the RTX 3060 Ti, Profile build, at 3840 × 2160, in
`render_perf`'s 4K field (`field_4k_production` against
`field_4k_no_bloom`, `field_4k_no_taa`, `field_4k_auto_exposure`); the
bandwidth floors are `gpu_bench`'s (432 GB/s read, 423 GB/s write,
402 GB/s copy).

## Decision

1. **The bloom chain** (`bloom.comp`; Jimenez, "Next Generation Post
   Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014): a
   transient image of at most six mips, mip 0 half the frame's size
   (rounded up), in B10G11R11 where the device can store to and filter it
   (else RGBA16F), stochastically rounded as the HDR image is.
   - Down: each mip is the 13-tap filter of the one before (five
     overlapping 2 × 2 boxes of bilinear taps, the inner box 1/2 and the
     outer four 1/8 each); the first, from the HDR image, weights each box
     by 1 / (1 + luma), the partial Karis average, so a sub-pixel highlight
     that the jitter finds and loses does not pulse.
   - Up: coarsest first, mip k += a 3 × 3 tent of mip k + 1, so mip 0 ends
     as the sum of every level's blur, in place.
   - Each level halves the one before exactly (a texel covers the 2 × 2
     texels under it), so neighbouring destination texels share taps: one
     downsample invocation writes 2 × 2 texels from 25 taps (52 apart), and
     the tent's nine taps fold, per axis, into two bilinear taps whose
     positions carry the weights (all positive), four taps in all with the
     same sum. One pipeline per kernel (a specialisation constant), so the
     upsample holds 31 registers where the downsample needs 64.
2. **The composite** mixes the glare into the image as a lerp,
   `mix(colour, mip0 / levels, bloom)`, which keeps the image's energy
   (Jimenez 2014): in the tonemap pass, and in the temporal resolve before
   the history meets the frame, so the history carries the glare and steadies
   it. There the glare is the same for the 3 × 3 neighbourhood (the pixel's
   own; it varies slowly), so it is applied to the neighbourhood's moments,
   extremes, and filtered colour after the loop (`taa_neighbourhood_mix`)
   rather than to each sample. Mip 0 is read at (p + 0.5) / 2 of its texels
   (`bloom_uv` in the push constants).
3. **Metering** (`exposure.comp`): when the view's exposure is
   `NPH_RENDER_EXPOSURE_AUTO`, the first downsample also counts each
   half-resolution texel's plain 2 × 2 box into a 128-bin histogram of
   log2 pre-exposed luminance over 16 stops ([-12, 4), an eighth of a stop a
   bin; shared-memory counts, one global add per bin per workgroup), and a
   one-workgroup pass takes the mean of the bins between the 50th and the
   95th percentiles, so neither the darkest half of the frame nor its
   brightest highlights steer it (as Unreal Engine 4's histogram eye
   adaptation does with its own percentiles). A copy puts it in a
   readback buffer per frame in flight.
4. **Adaptation on the CPU.** `NPH_GPU_FRAMES_IN_FLIGHT` frames later,
   when its slot comes round again, the CPU reads the metering, undoes the
   exposure it was metered at, and takes the EV100 an averaging meter would
   give it, EV100 = log2(L · 100 / 12.5) (Lagarde & de Rousiers 2014,
   §5.1), less the view's compensation, within [-6, 20]. The frame's
   exposure moves toward it exponentially, `ev += (target - ev) (1 -
   exp(-dt · speed))` (the time course of Pattanaik et al., "Time-Dependent
   Visual Adaptation for Fast Realistic Image Display", SIGGRAPH 2000),
   with the view's `frame_seconds` and `exposure_speed` (default 1/240 s
   and 1.5 e-foldings a second). Every shader keeps reading the exposure
   from the view record, and the temporal history's rescale follows it.
5. **Scope.** Both pipelines. Debug views have neither (they show data as
   they are); their frames keep the automatic exposure moving toward the
   last metering, so the lit view returns as it left. A manual view resets
   the adaptation.
6. **API and runtime.** `NphRenderView.bloom` (0 to 1, default 0),
   `exposure` (`NPH_RENDER_EXPOSURE_MANUAL` by default),
   `exposure_compensation`, `exposure_speed`, `frame_seconds`; statistics
   `ev100`, `metered_ev100`, `bloom`. The runtime's cvars: `r.bloom`
   (0.04), `r.auto_exposure` (off: a camera's EV100 is the scene author's),
   `r.exposure_compensation`, `r.exposure_speed`; `frame_seconds` is the
   frame's time, one simulation step under `rt.lockstep` so runs repeat.

## Measurements

**The chain**, ms per 4K frame:

| | down 0 | down 1 | down 2–5 | up 4–1 | up 0 | in all |
|---|---|---|---|---|---|---|
| 13 taps a texel, nine-tap tent, one pipeline | 0.139 | 0.037 | 0.023 | 0.039 | 0.089 | 0.327 |
| shared taps, four-tap tent | 0.132 | 0.036 | 0.024 | 0.026 | 0.058 | 0.276 |
| and a pipeline per kernel (kept; the baseline) | 0.134 | 0.037 | 0.023 | 0.025 | 0.057 | 0.276 |

The first downsample moves 41.5 MB (the HDR image's 33.2 MB read, mip
0's 8.3 MB written), 0.10 ms at the bus's rates: it runs at about 75 % of
that, and halving its taps barely moved it, so it is bound by memory, not
the texture units. On the reference path, whose HDR image is RGBA16F
(bilinear at half rate), the shared taps took it from 0.265 to 0.198 ms.
The four-tap tent gives the nine taps' result to within the hardware's
interpolation precision; up 0 moves 18.7 MB, a 0.045 ms floor. The four
smallest levels cost about 3 µs each, the launch and the barrier. A
pipeline per kernel changed nothing measurable; it is kept because the
upsample then holds 31 registers instead of the downsample's 64.

**The composite.** The temporal resolve took 0.789 ms before; with the
glare mixed into each of the nine samples it took 0.876 ms with bloom on
and off alike, and with the analytic mix 0.799 ms off and 0.871 on: one
bilinear tap of mip 0, in a pass bound by its texture fetches, costs
0.07 to 0.09 ms across the scenes. The tonemap pass went from 0.113 to
0.172 ms when it found mip 0's size with a texture query per pixel, and
to 0.126 ms with the step in its push constants.

**Metering** adds nothing measurable to the first downsample; the
average takes 0.004 ms, the clear 0.001 ms, the copy 0.002 ms.

**The frame** in the 4K field: 2.674 ms without bloom, 3.018 ms with
(+0.344 ms: the chain and the glare's tap in the temporal resolve); every
production scene grows by 0.34 to 0.40 ms, while the passes bloom does
not touch move from the previous baseline by under 2 % (the long ones) to
5 % (3 µs on the shortest).

**The images** (`test_post.c`, on the RTX and on lavapipe): a uniform image
keeps its value within one level on both pipelines (two under temporal
anti-aliasing, whose history and chain round at random); a six-pixel
highlight keeps the image's energy within 2.6 % (the coarse levels'
tails leave the frame) and glows alike in the four directions, dimmer
with distance; a background of 2,000 cd/m² meters at EV100 13.996 on both
pipelines against 13.966 for its exact luminance (the bin is an eighth of
a stop), shows at 1/9.6 of the scale as an averaging meter's grey does,
and a compensation of +1 gives one stop less.

## Alternatives considered

- **Bloom from the resolved image** (after the history), composited by a
  tonemap pass of its own: the glare of the converged image, but a
  separate tonemap re-reads the HDR image and writes the target again
  (0.11 ms and 66 MB). Mixing the glare in before the history meets the
  frame steadies it as well.
- **Starting the chain at a quarter of the frame:** about 0.04 ms less,
  without the tightest level of the glow, the one small highlights show.
- **A separable Gaussian per level, or convolution bloom** (an FFT with an
  artist's kernel): more passes, or far more cost, for a look the
  13-tap chain gives.
- **Thirteen taps per texel, the nine-tap tent, one pipeline:** measured
  above.
- **The glare mixed into each neighbourhood sample:** measured above.
- **Adaptation on the GPU** (the exposure in a buffer the shaders read):
  no readback latency, but the latency is two frames, 8 ms at 240 Hz,
  against a 0.67 s time constant, and the CPU would no longer know the
  exposure a frame was drawn with (statistics, the history's rescale).
- **The log-average luminance** (Reinhard et al., "Photographic Tone
  Reproduction for Digital Images", SIGGRAPH 2002) by a reduction: a sky
  or a lamp in view would pull it; the histogram's percentiles leave them
  out.

## Consequences

- **Post costs more against §10.4.** The temporal resolve with the glare's
  tap and the tonemap takes 0.871 ms and the chain 0.276 ms: 1.15 ms against
  the 0.40 ms of TAA, post, and the tonemap together. The chain is within
  about 0.08 ms of its bandwidth floor. Two levers remain: ADR-0034's
  folding of the history blend into the resolve, and running the chain on
  the async compute queue under the next frame's raster, from the
  previous frame's history (the glare a frame late, which the history's
  own sixteen frames dwarf), once the render graph schedules that queue
  (§8.5). Reported for the phase's review with ADR-0034's options.
- Every `render_perf` scene draws the runtime's bloom; the scenes are
  re-baselined (`render_perf-2026-10-18`), `field_4k_no_bloom` keeps the
  frame without it, and `field_4k_auto_exposure` measures the metering.
- The golden scenes and the comparisons between pipelines render without
  bloom (the default view); the runtime's comparisons render with it on
  both pipelines alike.
- New: `NphRenderExposureMode`, `NphRenderView.bloom`, `exposure`,
  `exposure_compensation`, `exposure_speed`, `frame_seconds`; statistics
  `ev100`, `metered_ev100`, `bloom`; the cvars `r.bloom`,
  `r.auto_exposure`, `r.exposure_compensation`, `r.exposure_speed`; the
  passes `bloom down N`, `bloom up N`, `exposure clear`, `exposure
  average`, `exposure readback`.
- Open: lens effects (dirt, flares, streaks) and a local exposure are not
  there; the metering weighs every pixel alike (no centre weighting); with
  bloom off, the automatic exposure still runs the first downsample to
  count its histogram.
