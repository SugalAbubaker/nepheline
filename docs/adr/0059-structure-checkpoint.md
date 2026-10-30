# 0059. The structure checkpoint: the frame keeps its passes; the first game

- **Date:** 2026-10-25
- **Status:** Accepted (the user's decisions of 2026-10-25: the first game,
  the measurement method, and the two §10.4 lines this checkpoint could not
  meet; decisions 3 and 4, the game and its scenes, superseded by
  [ADR-0067](0067-the-game-is-a-city.md))

## Context

ADR-0041 (decision 4) opens Phase 13 with a structure checkpoint, before
the phase's features build on the frame:

- prototypes of TAA and post fused with the shading, and of
  full-resolution occlusion in a form that fits its share of the indirect
  line, measured on the synthetic scenes and Bistro;
- a prototype keeps the image: the neighbourhood clip across tile borders,
  the blended surfaces under TAA, and `test_taa.c`'s and `test_ao.c`'s
  errors;
- a line that cannot be met at native resolution and full quality is
  reported with its measurements, and the user decides;
- the first game's setting, camera, and world density are decided
  (CLAUDE.md §13), and the reference scenes are checked against them.

**Phase 13's start.** No renderer code changed in Phase 12 (6033afb to
8274d5f touched the CPU-side real-time checks in `core` and `platform`
only). Bistro on the reference machine, Profile, 240 frames
(`reference_scenes-2026-10-25`, measured as "Measuring" below says), ms:

| View | Mean | p99 |
|---|---|---|
| `street` | 6.771 | 6.837 |
| `street_walk` | 6.841 | 6.934 |
| `street_turn` | 7.025 | 7.563 |
| `terrace` | 7.337 | 7.437 |
| `hedge` | 6.967 | 7.050 |

These are 0.08 to 0.13 ms under Phase 12's start (the runner's
`reference_scenes-2026-10-24`), in mean and 99th percentile alike, so they
are the numbers ADR-0041's guard rail holds Phase 13 to. By §10.4 line
(each line's passes summed, the occlusion counted with the indirect
light):

| §10.4 line | Budget | Street | Walk | Turn | Terrace | Hedge |
|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 0.43 | 0.44 | 0.44 | 0.44 | 0.42 |
| Visibility buffer raster (HW + SW) | 0.65 | 0.75 | 0.75 | 1.13 | 1.22 | 1.38 |
| Material + shading + direct light | 1.20 | 2.86 | 2.89 | 2.71 | 2.92 | 2.47 |
| Shadows (cached virtual pages) | 0.40 | 0.09 | 0.11 | 0.15 | 0.10 | 0.07 |
| Indirect light (radiance cache, occlusion) | 0.60 | 1.50 | 1.50 | 1.49 | 1.52 | 1.48 |
| TAA + post + tonemap | 0.40 | 1.14 | 1.15 | 1.15 | 1.15 | 1.15 |

The street's TAA line is the temporal resolve (0.86 ms) and the bloom
chain (0.28); its indirect line the occlusion (0.84 ms, its denoise 0.42,
its depth mips 0.03) and the radiance cache's update and blend (0.20).

**What bounds the passes.** Phase 10's GPU traces (docs/render.md "Where
the time goes") found the frame issue-bound under the board's 200 W limit:
the temporal resolve issues on 89% of its cycles, the occlusion on 78%,
its denoise on 73%, and the resolve on 74%, bound there by the latency of
its chains of dependent loads. A fusion pays only where one pass's
instructions can issue in another's idle cycles, or where it removes
work.

## Measurements

Every prototype below was measured in `render_perf` interleaved against
the unchanged renderer (two rounds, each variant built from scratch: the
`ab` method of docs/benchmarks.md), on Bistro's views and the 4K field
(`field_4k_production`), means in ms.

### TAA fused with the shading

A temporal resolve inside `vis resolve` needs each pixel's 3 × 3
neighbourhood of this frame's colour, which other invocations shade.
Three probes measured what such a pass would pay, each appended to the
resolve as it is:

- **Idle issue in the resolve.** Arithmetic appended after the shading
  (independent fused multiply-adds whose result is kept but adds
  nothing to the image) cost the resolve 54 to 69% of its issue time:

  | Scene | Resolve | + 256 a pixel | + 512 a pixel |
  |---|---|---|---|
  | street | 2.794 | 2.935 (+0.141) | 3.092 (+0.298) |
  | hedge | 2.437 | 2.572 (+0.135) | 2.747 (+0.310) |
  | 4K field | 1.042 | 1.202 (+0.160) | 1.388 (+0.346) |

  At 1.74 GHz the GPU issues 256 instructions a 4K pixel in 0.25 ms: about
  40% of the temporal resolve's arithmetic would hide in the resolve's
  memory stalls, the rest would add to it.
- **Tiles large enough to share neighbourhoods.** A tile's border pixels
  need neighbours from other tiles. With the resolve's own 8 × 8
  workgroups, 28 of every 64 pixels lie on that border; at 16 × 16, 60 of
  256. But in workgroups of 16 × 16 the driver compiles the resolve to 48
  registers and spills 19,456 bytes a workgroup to shared memory (against
  64 and 1,792 at 8 × 8): the resolve took 3.624 ms in the street against
  2.810, 1.388 in the field against 1.045.
- **The exchange itself.** Each pixel's colour and depth written to shared
  memory, a barrier, and a neighbour read back: at 16 × 16, 0.42 ms more
  again (4.048 in the street), as each latency-bound workgroup waits for
  its slowest warp; at 8 × 8, 0.03 to 0.09 ms (2.871 against 2.785 in the
  street, 2.464 against 2.438 at the hedge, 1.082 against 1.040 in the
  field).

So the fusion must use 8 × 8 tiles, whose 44% of border pixels take the
temporal resolve's full cost elsewhere while the other 56% take about 60%
of it in the resolve: 0.86 × (0.44 + 0.56 × 0.6) ≈ 0.67 ms, plus the
exchange (about 0.06 ms), plus what the fusion moves in. The blended
surfaces, drawn after the resolve today, would have to be composited
before it (an "over" layer accumulated back to front, which associativity
makes exact, read by every pixel), and the bloom, which the temporal
resolve mixes in from this frame's image, could then come only from the
previous frame's, or after the resolve with a pass of its own to
composite it (the tonemap pass with the glare: 0.13 ms in the field). The
total meets or exceeds today's 0.86 ms; the prototype was not built past
the probes.

### The temporal resolve as a compute pass

The resolve's arithmetic as a compute pass (`taa.comp`, a prototype):
each 16 × 16 tile and its apron fetched once into shared memory and
turned into YCoCg once, the moments, extremes and the Gaussian sum taken
along rows and then down columns (the filter's weights factor into an x
and a y part), the nearest surface searched in the tile's depth words,
and the display colour and the history written as storage images. By
count it removes about 230 of the pass's 782 instructions a pixel. The
images are the fragment pass's: `test_taa.c` gives 0.393, 0.646 and 0.334
levels of error (still, panning, moving cube) and 0.069 after an exposure
change, as before. The times:

| Scene | Fragment pass (today) | Compute pass |
|---|---|---|
| street | 0.872 | 0.872 |
| terrace | 0.878 | 0.865 |
| hedge | 0.879 | 0.871 |
| 4K field | 0.835 | 1.095 |

No faster in Bistro, and 0.26 ms slower in the field. What spends the
instructions saved was not isolated: the driver's statistics give no
instruction counts, and the GPU traces that would need counter access
from an elevated session. ADR-0034 measured a compute form slower too.

### The occlusion in a form that fits

The indirect line's 0.60 ms holds the radiance cache's 0.20, which leaves
the occlusion about 0.40 against 1.30 today:

- **Its denoise inside the resolve.** Each pixel denoising its own 5 × 5
  window of the raw occlusion as the resolve begins, in the denoise's own
  order of sums, the `ao denoise` pass gone. `test_ao.c` gives the same
  errors (a box: 0.163 of the occlusion, bias +0.034; open ground: 0.0033
  and no pixel off by over 0.03; the ground alone: 0.009; the bounce
  within 0.004 and 0.007). But the resolve grew by more than the pass it
  removed: 2.785 to 3.425 ms in the street, 2.741 to 3.382 on the terrace,
  2.434 to 3.017 at the hedge, against 0.43 ms for the denoise; the frames
  took 7.090, 7.599 and 7.178 ms against 6.864, 7.375 and 7.013. In the
  field the two came out even (4.073 against 4.036). The window's 25
  fetches ahead of the resolve's chain of dependent loads delay all of it.
- **The occlusion and its denoise in one pass.** A tile's 5 × 5 windows
  reach two pixels past it, so the occlusion would be computed for an
  apron as well: 27% more for 32 × 32 tiles, 13% for 64 × 64, of a pass of
  0.84 ms, to save the raw occlusion's write and read (8 bytes a pixel) in
  passes that use 18 and 23% of the bus. Not built.

Either way the occlusion's instructions remain: 672 a pixel in its slices
and 315 in its denoise (docs/render.md "Where the time goes"), more than
the line's share at the GPU's issue rate. A form of today's quality that
fits does not exist on this GPU among those measured.

### Measuring

While Claude Code's panel in VS Code and the desktop compositor redraw
(about ten times a second while a tool runs, 1.4% and 1.2% of the 3D
engine by the system's GPU counters), each redraw takes 0.3 to 0.8 ms of
GPU time from whatever pass runs then: one frame in fourteen to sixteen
came out slow, the GPU's clock holding steady at its power limit. Bistro's
street measured 6.865 ms mean and 7.608 at the 99th percentile that way,
and 6.753 and 6.809 with the editor minimized. The runner's own runs share
the GPU with the desktop the same way.

## Decision

1. **The frame keeps its passes.** No fusion of the temporal resolve or of
   the occlusion with the shading, and no compute form of the temporal
   resolve: each measured slower than the pass it would replace, or no
   faster. Phase 13's features build on today's structure.
2. **Two §10.4 lines are not met at native resolution and full quality,
   reported here with their measurements (§10.4):** TAA + post + tonemap
   (0.40 ms; 1.14 to 1.15 in Bistro) and the indirect light (0.60 ms; 1.48
   to 1.52). The user decided (2026-10-25) to proceed with Phase 13 and
   carry both to Phase 14's performance work, where the gate is
   (ADR-0041): instruction-level work from GPU traces, and the many-light
   sampling that will change the resolve. The target and every line stand,
   and nothing is given up to meet a line.
3. **The first game** (§13; the user's decision of 2026-10-25):
   first-person exploration of a forested mountain valley with ruins. The
   eye 1.7 m above the ground, walking and sprinting at 1.4 to 6 m/s;
   modelled vegetation to the horizon (§8.6's content budgets: no
   alpha-tested foliage in our own content); a ruined village whose
   interiors are lamp- and candle-lit at night; a day-night cycle; a world
   of about 4 × 4 km.
4. **The reference scenes against the game.** The open landscape (Phase
   13: Poly Haven CC0 assets in a layout of ours, ADR-0040) stands for the
   forest, and its views are taken from the eye's height, at walking speed
   and at a sprint. Bistro stands for the village's dense, lamp-lit
   streets, the candle-lit interior (Phase 14) for its interiors at night.
   No further scene is needed.
5. **Measuring.** The guard rail's measurements (a phase's start and its
   end) are taken with the user's editor windows minimized for the length
   of the run (the user's decision of 2026-10-25), and day-to-day
   comparisons interleave their variants and compare means, which the
   desktop's redraws shift alike (docs/benchmarks.md "Renderer runs").
   `reference_scenes-2026-10-25` and `render_perf-2026-10-25` are Phase
   13's start.

## Alternatives considered

- **Building the fused temporal resolve in full.** Its decisive costs were
  measured by the probes above, and with them its total meets or exceeds
  today's pass before the blended surfaces' layer and the bloom's
  composite are added.
- **Keeping the checkpoint open** for GPU-trace instruction diets of the
  temporal resolve and the occlusion before the features (the user's other
  option). Diets are tuning, not structure: even the 30% they might find
  leaves both lines over, and Phase 14 holds the performance work.
- **The bloom chain on the async compute queue from the previous frame's
  history** (ADR-0035's lever). The history carries the glare, so the chain
  would bloom the glare again unless the glare left the history for the
  displayed colour only, and the glare would reach the screen a frame
  late: an image change, not a structure that keeps it. Not built.
- **The occlusion accumulated in a history of its own**, with fewer
  directions a frame: a trade of quality (noise where surfaces are
  disoccluded) that the user has declined for the occlusion before
  (ADR-0038's amendment).
- **Rebalancing the §10.4 lines** to the measurements: declined at the
  reviews of 2026-10-20 and 2026-10-22 (ADR-0040, ADR-0041).

## Consequences

- Phase 13 starts its features: virtualized geometry, geometry streaming,
  virtual texturing, world partition with HLOD, and the landscape.
- Phase 14 inherits, besides its own work, the two lines this checkpoint
  measured, with the numbers above as their start, and the frame's
  instruction budget: 4,635 instructions a 4K pixel in Bistro's street
  where the GPU issues at most about 4,290 in 4.17 ms at its clock under
  load (docs/render.md "Where the time goes").
- Phase 13's features add work to passes the gate measures (the resolve
  takes virtual texturing's lookups; the culling takes the levels of
  detail): by ADR-0041's guard rail, the phase must end with no reference
  scene slower than the start above, or with an ADR that shows where the
  added cost is won back.
- `docs/benchmarks.md` describes the quiet measurement; CLAUDE.md §13
  records the first game.
