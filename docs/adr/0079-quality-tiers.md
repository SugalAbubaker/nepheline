# 0079. Quality tiers: the reference hardware's frame, and a high tier of rays

- **Date:** 2026-11-04
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 10 enables the ray-traced effects "on the reference hardware only within the §10 budget. Higher
quality tiers are for high-end GPUs", and Phase 14's scope names "high-end quality tiers". The renderer already has the
variants such a tier would take, each a field of `NphRenderView` and each measured against a truth when it was made:

- **The sun's shadows by rays** (`NPH_RENDER_VIS_RT_SHADOWS`): ADR-0032 made them the truth every shadow technique is
  measured against, "and a quality tier". The reference hardware takes the sun's virtual shadow map (ADR-0033) with
  contact shadows (ADR-0076).
- **Reflections by a ray a smooth pixel** (`NPH_RENDER_REFLECTIONS_RAYS`, ADR-0077), "for GPUs with rays to spare"; the
  reference hardware takes reflection cells.
- **The many lights' reservoirs for all their light** (`NPH_RENDER_RESTIR_ALL`, ADR-0074), "for the tiers" by ADR-0078's
  night measurement; the reference hardware takes their highlights.
- **The radiance cache's rays** (`NphRendererDesc.gi_rays`, 65,536 a frame whatever the resolution, ADR-0039).

What was missing is a name for the set a GPU with rays to spare should take, decided by what each variant costs on the
reference hardware and what it brings against the truth.

## Decision

1. **`NphRenderQuality`**, two tiers: `NPH_RENDER_QUALITY_REFERENCE`, the frame the RTX 3060 Ti is to hold at 144 FPS at
   native 4K (ADR-0067), which is `nph_render_view_default`'s; and `NPH_RENDER_QUALITY_HIGH`.
   `nph_render_view_quality(view, quality)` sets the fields a tier decides, `reflections`, `gi_rays` and the
   `RT_SHADOWS` bit of `vis_flags`, and leaves every other field as it was; a view may change any of them after.
2. **The high tier** takes the sun's shadows by a ray a pixel, reflections by a ray a smooth pixel every frame, and four
   times the cache's rays (262,144). Each measured below brings the image nearer its truth.
3. **The cache's rays are a view's** (`NphRenderView.gi_rays`, 0 the renderer's): they only set the lattice the update
   traces over the target, which nothing is sized by. `NphRenderFrameStats.gi_rays` counts what a frame traced.
4. **In the runtime the tier is a command, not a cvar**: `quality <reference|high>` (`nph_engine_set_quality`) sets
   `r.reflections`, `r.virtual_shadows` and the new `r.gi_rays`, and alone says which tier they match, or `custom`
   (`nph_engine_quality`). A cvar of its own would be saved beside the cvars it sets, and a configuration could hold a
   tier and other values beside it; as a command, what is saved is what is drawn.
5. **The reservoirs for all the light stay out of the tier**, for the bias measured below, and are mended for what they
   lacked: the cache now gathers the many lights' far fields in every mode, and where the reservoirs take all their
   direct light, a pixel's diffuse light takes the rest of its cell, set aside by the far share ADR-0078 keeps. Before,
   with `ALL` the cache held none of them: the lamps' light bounced nowhere, and blended surfaces, which no reservoir
   holds for, took none of it (Bistro's shop windows went dark at night).

## What each brings

**The sun's shadows by rays** are the truth the map is measured against (ADR-0032). With contact shadows the map leaves
1.99 % of Bistro's street too bright by over 16 levels against them, 1.26 % of the terrace, 1.35 % of the hedge
(ADR-0076), and stair-steps where the sun grazes a wall (its texels stretch); with rays, none.

**Reflections by rays**, against the truth (every surface traced along its lobe, ADR-0077's), 64 frames averaged: pixels
too bright by over 16 levels, and the mean.

| View | Cells (reference) | Rays (high) |
|---|---|---|
| Bistro `street` | 14.1 %, 7.0 levels | **11.9 %, 6.1 levels** |
| City `street` | 0.93 %, 1.75 levels | **0.69 %, 1.61 levels** |
| Interior `floor` | 0.45 %, 0.0 levels | 0.45 %, 0.0 levels (nothing smooth) |

What is left is rough surfaces', the cells' in both tiers. The rays are no noisier after the temporal resolve: a pixel
varies 8.7 % from frame to frame on Bistro's street, 9.0 % with cells (the truth 7.3 %).

**Four times the cache's rays**, on views that walk, against sixteen times (the stand-in for a cache that has caught up),
the last frame: pixels off by over 16 levels, and the mean.

| View | 65,536 rays (reference) | 262,144 (high) |
|---|---|---|
| Bistro `street_walk` | 21.8 %, 1.1 levels | **18.0 %, 0.3 levels** |
| City `street_walk` | 7.5 %, 1.4 levels | **4.7 %, 0.5 levels** |
| Interior `floor_walk` | 24.4 %, 1.4 levels | **13.0 %, 0.4 levels** |

New cells fill four times sooner as the camera walks, and a cell's noise settles sooner after a change of the light.

**The reservoirs for all the light**, at night against the lamps with a shadow ray each (ADR-0078's truth, which has no
bounced light at all): pixels too bright by over 16 levels, too dark by over 16, and the mean green level against the
truth's.

| View | Highlights (reference) | All, mended (not taken) |
|---|---|---|
| Bistro `street` | 2.6 %, 4.4 %, 12.4 of 13.7 | 1.6 %, 5.2 %, 11.6 |
| Bistro `terrace` | 2.0 %, 3.6 %, 10.6 of 11.6 | 0.8 %, 4.4 %, 9.7 |
| Bistro `hedge` | 2.5 %, 3.2 %, 13.5 of 13.5 | 0.2 %, 3.3 %, 11.5 |

They leak no light past shadows' edges, and a pixel varies 4 to 6 % from frame to frame where the cache's far fields
vary 15 to 18 %. But their mean level is about 15 % below the truth's, where with bounces it should be above: their
estimate of the direct light is biased dark in a scene of Bistro's complexity, though `Restir.*` holds them to their
truth within 0.5 to 2 % in the tests' simple scenes. Raising what they may show from four times white to 64 times
changed nothing (before the mend, 2.75 levels darker on the street against 2.77), so it is not their limit; the
estimator itself is to be looked into, and with it the highlights the reference tier takes from it. One lead: in a test
where the camera moved 100 m to four lights its tiles' lists did not hold, the reservoirs showed half the highlights 96
frames later that they show where the lists held them from the start: perhaps what a reservoir has weighed stays in its
count long after the candidates it weighed were the wrong ones (not yet shown). A tier that darkens the image is not a
higher one.

## What it costs

Frame in ms, mean, native 4K on the RTX 3060 Ti, Profile, the high tier against the reference, interleaved, two
rounds of 240 frames a scene, a scene a process (2026-11-04); the passes' columns are what the high tier adds:

| View | Reference | High | resolve | `reflect rays` | `gi update` |
|---|---|---|---|---|---|
| Bistro `street` | 7.49 | **13.22** (+5.72) | +3.70 | +2.24 | +0.37 |
| Bistro `street_walk` | 7.60 | **13.48** (+5.88) | +3.82 | +2.31 | +0.37 |
| Bistro `street_turn` | 7.99 | **14.79** (+6.80) | +4.60 | +2.30 | +0.36 |
| Bistro `terrace` | 8.15 | **15.00** (+6.85) | +4.43 | +2.41 | +0.37 |
| Bistro `hedge` | 7.73 | **12.36** (+4.63) | +3.94 | +0.93 | +0.31 |
| City `street` | 6.59 | **7.64** (+1.04) | +0.90 | +0.45 | +0.12 |
| City `street_walk` | 6.68 | **7.71** (+1.04) | +0.87 | +0.51 | +0.13 |
| City `street_sprint` | 6.86 | **7.89** (+1.03) | +0.87 | +0.59 | +0.14 |
| City `street_turn` | 6.78 | **8.05** (+1.27) | +1.15 | +0.66 | +0.12 |
| City `crossing` | 6.74 | **7.86** (+1.12) | +0.82 | +0.65 | +0.12 |
| City `park` | 7.84 | **9.14** (+1.30) | +1.15 | +0.33 | +0.29 |
| City `rooftop` | 10.00 | **11.86** (+1.86) | +0.72 | +1.47 | +0.15 |
| Interior `floor` | 7.16 | **7.55** (+0.39) | +0.00 | +0.03 | +0.37 |
| Interior `floor_walk` | 7.21 | **7.59** (+0.39) | -0.00 | +0.03 | +0.37 |
| Interior `floor_turn` | 6.73 | **7.20** (+0.47) | +0.01 | +0.16 | +0.34 |
| Interior `gallery` | 7.31 | **7.67** (+0.36) | +0.02 | +0.03 | +0.32 |
| Interior `among` | 7.16 | **7.55** (+0.39) | +0.01 | +0.00 | +0.38 |
| Interior `upward` | 6.25 | **6.56** (+0.31) | +0.01 | +0.01 | +0.29 |

The sun's rays cost most where its light falls through Bistro's foliage, which the rays meet as triangles: 3.7 to 4.6 ms in
the resolve, where the city's streets take 0.7 to 1.2; the contact shadows' march no longer runs (`ao` 0.2 ms less), and
the blended surfaces' shadows are rays too (`transparent` 0.2 ms more). The reflections' rays follow the smooth pixels:
2.2 to 2.4 ms on Bistro's street and terrace, 0.3 to 1.5 in the city, nothing in the interior. The cache's rays cost 0.12 to
0.37 ms, most where the many lights' far fields are sampled with each. Each variant alone (2026-11-04, the same way): the
sun's rays +3.44 to +4.39 ms on Bistro, +0.43 to +0.80 in the city; the reflections' +0.77 to +2.12 and +0.20 to +1.18; the
cache's +0.34 to +0.43, +0.14 to +0.29 and +0.30 to +0.39 in the interior.

**DRAM**, as the render graph declares it (whole resources, an upper bound), is less: the sun's map is not drawn or read.
Bistro's street declares 3,316 MB where it declared 4,829, the city's 4,282 where 7,273; the resolve reads 235 MB where it
read 374, `reflect rays` writes 105 MB where it wrote 72. What the rays read is the scene's structures where they go:
Bistro's bottom levels hold 54 MB, the city's 86 MB. Device memory is the same: nothing is sized by a tier, and the map's
pool (128 MB) stays.

## Alternatives

- **A cvar `r.quality`**, as many engines have: either it overrides the cvars it covers, and a configuration's own values
  of them are lost at start, or it does not, and a configuration that holds only `r.quality = 1` draws the reference tier.
- **A third tier** above the high one for the sun's rays, which cost most of the tier on Bistro: not until a GPU of
  the kind the tiers are for is measured; split here, the tiers would follow what the reference hardware can do, which
  the reference tier already does.
- **The sun's shadows traced into a mask in a pass of their own** (ADR-0032's alternative, "worth measuring for the sun
  alone if ray-traced shadows become a production tier on their own"): not measured yet; the resolve's own rays are what
  the tier takes until a mask shows it is cheaper.
- **The reservoirs for all the light in the tier** for their sharper, steadier shadows: not while they darken the image.

## Consequences

- The reference tier is the default, and what Phase 14's gates measure; the high tier is not gated.
- `render_perf --quality=N` draws the views in a tier, `--gi-rays=N` sets the cache's rays; `docs/benchmarks.md`.
- `NPH_RENDER_RESTIR_ALL`'s frames sample the far fields in the cache's update again (the interior's update took 0.05 ms
  less without), and its pixels take the cell's diffuse light less its far share, a luminance share like ADR-0078's.
- `quality.*`, `Quality.the_high_tier_draws_what_the_reference_does`, `Gi.a_view_takes_its_own_budget_of_rays`,
  `Engine.a_quality_tier_sets_its_cvars_and_the_view_takes_them`, and for the mend
  `Many.a_blended_surface_takes_them_with_reservoirs_for_all_the_light` and
  `Many.a_pixel_takes_the_rest_of_its_cell_with_reservoirs_for_all` (the lights moved away, the reservoirs start again
  while the cells still hold their light: a rough metal shows the sky's alone, 0.1838 and 0.1838); 15 planted faults,
  each caught, are listed in the PR.
