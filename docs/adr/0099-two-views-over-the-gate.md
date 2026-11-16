# 0099. Two views over the gate: Bistro's street turn and the city's rooftop, recorded

- **Date:** 2026-11-15
- **Status:** Accepted (the user's word of 2026-11-15: "Write the ADR recording both views as over the
  gate", after the research below found no route to the gate for either that leaves the image as it
  is; records two of the eighteen views of [ADR-0082](0082-gate-at-1440p.md)'s gate as not met, as
  CLAUDE.md §10.4 asks of a budget that proves unreachable; CLAUDE.md §10.7 and §12 point here)

## Context

ADR-0082 gates Phase 14's exit on Bistro, the city and the interior each holding 144 FPS, a 1%-low
of at most 6.94 ms, at native 2560 × 1440 on the RTX 3060 Ti with the phase's features on, and keeps
ADR-0041's rule that no quality is given up to meet a number. The user stated the rule again for this
work on 2026-11-13: "We can not sacrifice any image quality to get to the gate". CLAUDE.md §10.4: "If
any budget proves physically unreachable, we report it with measurements; we never quietly lower
resolution or quality."

**Where the gate stands.** ADR-0098's build (fdf73fb; c03bb30 changes only a Shipping build),
`render_perf --reference-only` in Profile, 240 measured frames a view, a process a scene, the
editor's window idle (docs/benchmarks.md, "The desktop shares the GPU too"); two rounds, the interior
one; the mean of the rounds' means and the worse round's 1%-low, as ADR-0082's table (ms):

| View | Mean | 1%-low | Against 6.94 |
|---|---|---|---|
| `bistro_street` | 5.93 | 6.04 | −0.90 |
| `bistro_street_walk` | 6.10 | 6.35 | −0.59 |
| `bistro_street_turn` | 6.25 | 7.36 | **+0.42** |
| `bistro_terrace` | 6.64 | 6.81 | −0.13 |
| `bistro_hedge` | 5.66 | 5.77 | −1.17 |
| `city_street` | 4.92 | 5.12 | −1.82 |
| `city_street_walk` | 5.04 | 5.33 | −1.61 |
| `city_street_sprint` | 5.15 | 5.49 | −1.45 |
| `city_street_turn` | 5.21 | 5.81 | −1.13 |
| `city_crossing` | 5.27 | 5.53 | −1.41 |
| `city_park` | 6.33 | 6.51 | −0.43 |
| `city_rooftop` | 7.92 | 8.09 | **+1.15** |
| `interior_floor` | 4.62 | 4.66 | −2.28 |
| `interior_floor_walk` | 4.68 | 4.72 | −2.22 |
| `interior_floor_turn` | 4.33 | 4.67 | −2.27 |
| `interior_gallery` | 4.87 | 4.93 | −2.01 |
| `interior_among` | 4.74 | 4.78 | −2.16 |
| `interior_upward` | 4.29 | 4.33 | −2.61 |

Sixteen views hold the gate. A third idle round of the two that do not, on c03bb30, put their 1%-lows
at 7.27 and 7.97 ms, and the budgets' rounds of 2026-11-14 (3716646, 208d10b) at 7.29 to 7.39 and
7.99 to 8.07: the street turn stands 0.33 to 0.45 ms over, the rooftop 1.03 to 1.15.

**How they went over.** ADR-0082 measured every view inside the gate on 2026-11-10, the turn's 1%-low
at 4.44 ms and the rooftop's at 6.05. The user then put the image first: ADR-0083 to ADR-0095, each
accepted on the user's go-ahead or on their report that it answers what they saw in the viewer. The
frame grew with the image. The two views' means in the results each of those pull requests
committed (the rounds before 2026-11-13 were measured with the window busy, which moves a mean about
2 % and a 99th percentile 0.4 to 1.0 ms, so the means):

| Results (`reference_scenes-`) | Commit | ADRs | Turn | Rooftop |
|---|---|---|---|---|
| `2026-11-10-1440p` | f5f9fef | 0082 | 3.80 | 5.40 |
| `2026-11-10-agx` | 22f6e75 | 0083 | 3.86 | 5.40 |
| `2026-11-11-gi-blend` | c0562ea | 0084, 0085 | 4.40 | 5.61 |
| `2026-11-11-motion` | 52bfc3a | 0086, 0087 | 4.18 | 5.61 |
| `2026-11-11-alpha-tested` | 9d8f770 | 0088 | 4.65 | 6.12 |
| `2026-11-11-reflect-rays` | 494ea99 | 0089 | 5.70 | 7.92 |
| `2026-11-13-adr-0095` | f3e66d3 | 0090 to 0095 | 6.35 | 8.57 |
| `2026-11-14-adr-0096` | 3716646 | 0096 | 6.22 | 7.82 |
| `2026-11-14-adr-0097` | 208d10b | 0097 | 6.25 | 7.85 |
| the rounds above | fdf73fb | 0098 | 6.25 | 7.92 |

The largest steps: a ray a frame for every smooth pixel (ADR-0089), +1.05 ms on the turn and +1.80 on
the rooftop; the radiance cache filtered between cells, its table in motion, light off what rays
meet, the pops, the glass and the view in motion as sharp as standing (ADR-0090 to 0095), +0.65 on
each; alpha-tested surfaces that cast and stop rays as they are drawn (ADR-0088), +0.47 and +0.51.
The trimmed cards (ADR-0096) won 0.13 and 0.75 ms back.

## What makes them slow

Measured 2026-11-15 on c03bb30, `render_perf` in Profile with its probe's per-frame pass times: means
with the window busy, worst frames from an idle round. A bound is a variant that changes the image,
run only to size a cost; none is an option. The variants are scratch, not in the tree: each is a few
lines, as the tables describe it.

**Bistro's street turn.** Its 1%-low is its first sixty measured frames: their median is 7.06 ms
idle, the end of the turn's 5.19. There the camera faces the restaurant's corner, and 400,000 to
540,000 pixels are smooth enough to take a reflection ray every frame (ADR-0089), against 37,000 to
110,000 at the turn's end: the painted shopfront beside the restaurant, the awnings and signs, the
metal chairs and tables, the scooter, the railings and the flower pots (seen by a variant that
paints every pixel the gather adds rays' light to). `reflect rays` takes 1.44 ms there, 0.25 at the
end, with 273,000 sun rays from the hits. Hits not shaded would take 0.69 ms from it, no sun ray at
the hits 0.37, every surface opaque to the rays 0.23: the alpha-tested cards matter little here,
0.12 candidates a traversal. Its four slowest frames add the shadow map's pages the turn brings into
view: `vsm raster` 0.06 ms at the median, 0.23 in them.

**The city's rooftop.** Steady: idle mean 7.80, 1%-low 7.97: the whole frame, not its spikes.
`reflect rays` 1.55 ms, `vis resolve` 0.84, the geometry about 3.0 (the rasters 2.28: the voxels
0.69, the software raster 0.59 and 0.30 masked, the hardware raster 0.40 and 0.30 masked; the culls
0.48; the depth pyramids 0.21), `gi update` 0.55, `taa` 0.42, `gi filter` 0.32, `ao` 0.27.

- The reflection rays cross the city's alpha-tested trees. Every surface opaque to the rays would
  take 0.84 ms from `reflect rays` and 0.24 from `gi update`, and most of that is rays rightly passing
  through the gaps between leaves. Of the 237,000 candidates a frame the reflection pass hands its
  shader, 153,500 fail the alpha test, 128,000 of them on cards ADR-0096 trimmed, and 83,500 stop
  their ray. `gi update` meets 22,000.
- The rasters draw 258,000 clusters, 8.6 million triangles, and about a quarter of the clusters win a
  pixel (`--debug-view=clusters`: 58,500 colours of 239,000 clusters). With each cluster's own
  vertices as its occlusion bound and the depth pyramid's second look four levels finer, the
  tightest exact test (a diagnostic, its own cost 3 ms), 14 % fewer clusters and 16.5 % fewer
  triangles are drawn and the rasters take 0.26 ms less. The rest are partly visible: leaves behind
  leaves, which the pyramid sees through, and far clusters of a few pixels.

**Exact changes measured** (the image unchanged; means against the same build's runs):

| Change | Turn | Rooftop |
|---|---|---|
| The shadow ray as two queries: opaque geometry first, then the alpha-tested | `reflect rays` +0.43 | +0.23 |
| A hit's lookups only where used: the parent cell, the emissive texture, the sky's light | +0.02 | +0.03 |
| The software raster reads a pixel before its atomic, the masked one before its alpha test | | the rasters +0.14 |
| The occlusion test's second look three or four levels finer | | frame +0.08, +0.43 (clusters −3 to −3.5 %) |
| The top level built for fast rays (`PREFER_FAST_TRACE`) | `reflect rays` −0.05 | 0; declared +76 and +128 MB |
| Timestamps only at the frame's two ends, none between passes | span −0.06 | −0.05 |

The last is the per-pass profiler's own cost, not the frame's. `reflect rays` runs at 64 registers
with its hits shaded and without, so a pass that traces and one that shades would gain it no
occupancy. Measured on 2026-11-13 and 14 and not kept either: per-geometry opaque flags (opaque
submeshes of mixed instances were at most 2.5 % of the candidates); opacity micromaps (Bistro +0.11
to +0.15 ms, the city −0.06 to +0.04: the RTX 3060 Ti has no hardware for them, which came with the
RTX 40 series); the async compute queue (Bistro's span 6.07 to 6.88 ms); the resolve's registers and
lookups reshaped (slower or equal); a tile's rays packed together (+0.01 to +0.16); the cache's keys
bucketed by block (a gain only while samples were dropped); the top level built for fast rays, which
then cost the city's crossing 23 % of its reflection rays (not measured again since).

**What exact work is left, at most:**

- *The turn:* the shadow map's pages drawn ahead of a turn, so that its slowest frames do not draw
  them all at once (no published scheme keeps the image exact; at most the 0.17 ms they add), and the
  top level built for fast rays (0.05 ms): about 7.0 to 7.1 ms, still over.
- *The rooftop:* cards cut closer than ADR-0096's convex outlines (at most about 0.3 ms: its 128,000
  gap crossings at 2 to 2.5 ns each, the cost a crossing comes to in ADR-0096's own gain) and a box a
  cluster for the occlusion test (at most the 0.26 ms above, before the box test's own cost): about
  7.4 to 7.6 ms.

## Decision

1. **Phase 14 ends with two views over its gate,** recorded here with their measurements as §10.4
   asks: Bistro's street turn, 1%-low 7.27 to 7.39 ms, and the city's rooftop, 7.97 to 8.09 ms. The
   other sixteen hold it.
2. **No image is given up for them.** Their cost is the image the user chose after ADR-0082 (ADR-0083
   to ADR-0095), drawn on the reference scenes as their authors made them (§8.6).
3. **The exact work left is not built for the gate:** at its ceilings neither view reaches 6.94 ms.
   Each item stays open for the game, whose content decides whether it pays.
4. **The gate stands** (ADR-0082): for the sixteen other views at Phase 14's exit, and for every view
   at Phase 15's, in shipping builds, which have no per-pass timestamps. There these two are measured
   again and, if still over, reported against this record.

## Alternatives considered

- **Keep working the two views with the exact changes left:** at their ceilings the turn stays about
  0.1 ms over and the rooftop about 0.5 ms.
- **Give up image for speed:** excluded by the user's rule and ADR-0041; not measured as options.
- **Gate on the mean, or on the turn without its first frames:** §10.1 measures the 1%-low so that
  stutter counts, and ADR-0067 rejected the mean.
- **The gate measured without per-pass timestamps:** the frame is 0.05 to 0.06 ms shorter, which
  changes neither verdict; the nightly keeps them, to report each pass.
- **Move the gate again, as ADR-0082 did:** the user asked for a record, not a new gate.

## Consequences

- Of Phase 14's exit criteria, the gate is met through this record. The others stand: the golden
  tests passing, each §10.4 line reported at native 4K, and the landscape no slower than at the
  phase's start (§10.7).
- The nightly is unchanged: `benchmarks/reference_budgets.txt` holds each view to its own recorded
  mean plus 5 % (§10.7), not to 6.94 ms, so the two views do not fail it and may not get slower.
- The game draws its own content, which has no alpha-tested foliage (§8.6). The rooftop's largest
  cost, the trees' cards in its rays, does not carry over. The turn's does: a ray every frame for
  every smooth pixel (ADR-0089), wherever the game's streets hold as many smooth pixels. The 240 FPS
  target is checked on the game's own content (ADR-0067).
- A run's slowest frames with the window busy are the window's redraws on the longest passes
  (`reflect rays`, `gi update`): a view's slowest frames are read from idle runs only.
