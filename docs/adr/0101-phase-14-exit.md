# 0101. Phase 14's exit: the phase's light kept at its cost, the landscape faster, its memory over

- **Date:** 2026-11-16
- **Status:** Accepted (the user's decision of 2026-11-16, "Accept, memory to Phase 15", on the exit measured that
  day on main's 62bc9fb); decision 3 moved to Phase 16 by [ADR-0102](0102-in-house-tools-then-the-game.md)

## Context

CLAUDE.md §12 closes Phase 14 on four criteria: the golden tests pass; Bistro, the city and the interior each hold
1%-low ≤ 6.94 ms at native 1440p on the RTX 3060 Ti with the phase's features on (ADR-0082), Bistro's street turn and
the city's rooftop recorded over it (ADR-0099); each §10.4 line reported at native 4K; and the landscape no slower
than at the phase's start. §10.7 lets no phase from 11 to 15 end with a reference scene slower than when it began
unless an ADR shows where the added cost will be won back, and ADR-0082 reads that guard rail at 2560 × 1440. The
city and the interior joined in this phase, so the guard rail holds Bistro and the landscape.

**How it was measured.** The end is main's 62bc9fb (ADR-0100 its last record). The start is v0.8.0 (cf65132, Phase
13's end), built again in Profile in a worktree of its own. Each build draws its scenes as its own tree cooks them:
the phase changed the mesh and material formats (`NPH_MESH_TYPE_VERSION` 6 to 7, `NPH_MATERIAL_TYPE_VERSION` 1 to 2),
so the landscape was cooked by each cooker and Bistro again by v0.8.0's, from the same sources; v0.8.0's landscape
by hand with `--voxels --hlod`, as Phase 13's exit cooked it (its `scripts/scenes.py` knew only the voxels; a403e38
put the cells' proxies in the manifest since). `render_perf --reference-only` in Profile, 240 measured frames a
view, a process a scene, driver 610.74. The gate's rounds, the 4K lines and the comparisons at 4K ran from the user's
own terminal with the editor's windows minimized (ADR-0059); the comparisons at 1440p ran the same evening as a
background job with Claude Code's panel idle (docs/benchmarks.md, "The desktop shares the GPU too"). The comparisons
interleave the two builds over two rounds, alternately first. Measured again so, the start reproduces ADR-0066's
record: Bistro's views at 4K within 0.015 ms of its end column, the landscape's within 0.01 to 0.22 ms of its table.

**The gate** (2560 × 1440, two rounds; the mean of the rounds' means and the worse round's 1%-low, as ADR-0082's
table, ms):

| View | Mean | 1%-low | Against 6.94 |
|---|---|---|---|
| `bistro_street` | 5.95 | 6.10 | −0.84 |
| `bistro_street_walk` | 6.13 | 6.38 | −0.56 |
| `bistro_street_turn` | 6.29 | 7.39 | **+0.45** |
| `bistro_terrace` | 6.66 | 6.81 | −0.13 |
| `bistro_hedge` | 5.68 | 5.81 | −1.13 |
| `city_street` | 4.89 | 5.11 | −1.83 |
| `city_street_walk` | 5.01 | 5.34 | −1.60 |
| `city_street_sprint` | 5.10 | 5.46 | −1.48 |
| `city_street_turn` | 5.16 | 5.84 | −1.10 |
| `city_crossing` | 5.23 | 5.44 | −1.50 |
| `city_park` | 6.29 | 6.58 | −0.36 |
| `city_rooftop` | 7.84 | 8.03 | **+1.09** |
| `interior_floor` | 4.61 | 4.68 | −2.26 |
| `interior_floor_walk` | 4.65 | 4.72 | −2.22 |
| `interior_floor_turn` | 4.33 | 4.68 | −2.26 |
| `interior_gallery` | 4.87 | 4.93 | −2.01 |
| `interior_among` | 4.75 | 4.81 | −2.13 |
| `interior_upward` | 4.29 | 4.34 | −2.60 |

Sixteen views hold it. The street turn's 1%-low was 7.27 and 7.39 ms in the two rounds and the rooftop's 7.94 and
8.03, inside what ADR-0099 recorded (7.27 to 7.39, 7.97 to 8.09).

**Each §10.4 line at native 4K** (3840 × 2160, a round; each pass's mean summed into its line: the occlusion and the
reflections with the indirect light, as ADR-0041's table had the occlusion; the many lights' sampling, the blended
surfaces and the voxels' resolve with the shading; the cells' cull with the culling; ms):

| Bistro | Budget | street | walk | turn | terrace | hedge |
|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 0.47 | 0.47 | 0.49 | 0.47 | 0.46 |
| Visibility buffer raster (HW + SW) | 0.65 | 0.67 | 0.67 | 1.07 | 1.16 | 1.36 |
| Material + shading + direct light | 1.20 | 4.18 | 4.23 | 4.49 | 4.50 | 4.19 |
| Shadows (cached virtual pages) | 0.40 | 0.14 | 0.20 | 0.26 | 0.15 | 0.13 |
| Indirect light (cache, occlusion, reflections) | 0.60 | 4.43 | 4.60 | 4.71 | 5.08 | 3.18 |
| TAA + post + tonemap | 0.40 | 1.24 | 1.24 | 1.24 | 1.25 | 1.25 |
| **Frame, mean** | **4.17** | **11.14** | **11.42** | **12.26** | **12.60** | **10.57** |
| **Frame, 99th percentile** | **4.17** | **11.25** | **11.76** | **13.64** | **12.73** | **10.70** |

| The city | Budget | street | walk | sprint | turn | crossing | park | rooftop |
|---|---|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 0.69 | 0.69 | 0.67 | 0.72 | 0.66 | 0.70 | 1.13 |
| Visibility buffer raster (HW + SW) | 0.65 | 0.85 | 0.87 | 0.92 | 0.69 | 0.87 | 1.68 | 3.15 |
| Material + shading + direct light | 1.20 | 2.62 | 2.65 | 2.76 | 2.84 | 2.83 | 2.90 | 2.22 |
| Shadows (cached virtual pages) | 0.40 | 0.10 | 0.15 | 0.20 | 0.21 | 0.10 | 0.10 | 0.09 |
| Indirect light (cache, occlusion, reflections) | 0.60 | 3.25 | 3.33 | 3.52 | 3.74 | 3.71 | 3.95 | 5.32 |
| TAA + post + tonemap | 0.40 | 1.24 | 1.25 | 1.24 | 1.25 | 1.24 | 1.25 | 1.21 |
| **Frame, mean** | **4.17** | **8.77** | **8.95** | **9.32** | **9.44** | **9.41** | **10.57** | **13.12** |
| **Frame, 99th percentile** | **4.17** | **9.01** | **9.31** | **9.89** | **10.10** | **9.60** | **10.81** | **13.33** |

| The interior | Budget | floor | walk | turn | gallery | among | upward |
|---|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 0.52 | 0.51 | 0.48 | 0.53 | 0.49 | 0.45 |
| Visibility buffer raster (HW + SW) | 0.65 | 0.69 | 0.68 | 0.48 | 0.82 | 0.66 | 0.53 |
| Material + shading + direct light | 1.20 | 4.07 | 4.13 | 4.07 | 4.33 | 4.36 | 3.57 |
| Shadows (cached virtual pages) | 0.40 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| Indirect light (cache, occlusion, reflections) | 0.60 | 2.07 | 2.10 | 2.16 | 2.05 | 2.09 | 1.94 |
| TAA + post + tonemap | 0.40 | 1.27 | 1.27 | 1.27 | 1.27 | 1.27 | 1.27 |
| **Frame, mean** | **4.17** | **8.62** | **8.69** | **8.46** | **9.01** | **8.86** | **7.76** |
| **Frame, 99th percentile** | **4.17** | **8.70** | **8.77** | **8.82** | **9.09** | **8.92** | **7.83** |

Every view takes 1.9 to 3.1 times the 4.17 ms frame. The shadows hold their line in every view (the interior has no
sun: its lamps' shadows are their rays, on the shading line, ADR-0078), and the raster holds its line or comes within
0.04 ms of it in eight. The culling (0.45 to 1.13 ms against 0.25), the shading (2.22 to 4.50 against 1.20), the
indirect light (1.94 to 5.32 against 0.60) and TAA with post (1.21 to 1.27 against 0.40) are over in every view. The
largest passes: Bistro's terrace, the resolve 3.78 ms, `reflect rays` 2.41, `taa` 0.97, `ao` 0.93; the city's
rooftop, `reflect rays` 3.03, the resolve 1.94, `taa` 0.94, the voxels' raster 0.76; the interior's gallery, the
resolve 3.33, `taa` 0.99, `ao` 0.78, `transparent` 0.70. The passes in no line (the rays' structures, the
statistics, the texture stream's feedback) took 0.01 ms at most. The DRAM column is not measured:
`render_perf` reports each pass's declared footprint, an upper bound (docs/benchmarks.md, "Renderer runs"; Bistro's
street declares 5,803 MB a frame), as at Phase 13's exit.

**Bistro against the phase's start** (2560 × 1440, two rounds; the mean of the rounds' means and of their 99th
percentiles, ms):

| View | Start (p99) | End (p99) | Change | Each round |
|---|---|---|---|---|
| street | 3.491 (3.648) | 5.979 (6.086) | +2.487 | +2.48, +2.49 |
| walk | 3.524 (3.607) | 6.143 (6.340) | +2.619 | +2.61, +2.63 |
| turn | 3.428 (3.749) | 6.265 (7.320) | +2.837 | +2.80, +2.87 |
| terrace | 3.726 (3.778) | 6.649 (6.763) | +2.923 | +2.89, +2.95 |
| hedge | 3.499 (3.537) | 5.657 (5.758) | +2.158 | +2.13, +2.18 |

At 4K, interleaved the same way: +3.50 to +5.16 ms (the street 6.924 to 11.221, the walk 6.983 to 11.492, the turn
7.180 to 12.337, the terrace 7.489 to 12.649, the hedge 7.108 to 10.612), each round within 0.06 ms of the other.
The cost is the light the phase added, by pass at 1440p:

- the reflections (ADR-0077, ADR-0089): `reflect rays` 0.43 to 1.17 ms and `reflect` 0.06 to 0.15, new;
- the radiance cache (ADR-0084 to ADR-0092): `gi filter` 0.32 to 0.37, new, and `gi update` +0.33 to +0.45;
- the resolve, +0.34 to +0.51, which now also reads the many lights' reservoirs (ADR-0074), the contact shadows'
  answer (ADR-0076) and the cache's cells blended (ADR-0085); and the many lights' `many turns`, 0.14 to 0.25, new;
- the blended surfaces, +0.09 to +0.16 (ADR-0075, ADR-0094, ADR-0097; new in the hedge's view, 0.12);
- `taa` +0.04 (ADR-0093, ADR-0095) and `ao` +0.04, whose pass now marches the contact shadows (ADR-0076).

The walk down the hierarchies that Phase 13 left is gone (`vis cull nodes 0`, 0.03 ms, ADR-0070).

**The landscape against the phase's start** (two rounds at each size, with ADR-0066's options: the cells' proxies
streamed, the textures within 128 MB, the voxel pool 1,200 MB, the visible-cluster lists 16 times their defaults;
the mean of the rounds' means and of their 99th percentiles, ms):

| View | 1440p start (p99) | 1440p end (p99) | Change | 4K start (p99) | 4K end (p99) | Change |
|---|---|---|---|---|---|---|
| forest | 31.005 (31.179) | 24.280 (24.454) | −6.724 | 61.853 (62.342) | 47.594 (47.836) | −14.259 |
| walk | 31.102 (31.806) | 24.638 (27.278) | −6.463 | 62.095 (62.711) | 48.239 (51.253) | −13.855 |
| sprint | 31.378 (32.300) | 24.705 (26.523) | −6.673 | 62.748 (64.065) | 48.424 (50.540) | −14.324 |
| turn | 32.384 (35.284) | 28.051 (31.936) | −4.333 | 63.456 (68.190) | 53.975 (60.150) | −9.481 |
| across | 34.697 (34.857) | 27.805 (27.943) | −6.893 | 64.104 (64.484) | 50.575 (50.829) | −13.529 |
| valley | 26.818 (26.950) | 21.231 (21.335) | −5.588 | 52.757 (53.130) | 40.600 (40.809) | −12.157 |

Faster in every view at both sizes, in every round. At 4K the far field's voxels (`vis voxel raster 0`) went from
18.5 to 21.9 ms to 7.0 to 9.0, by ADR-0061's amendments of 2026-11-05 to 2026-11-08 (the raster fills its lanes
and skips what is drawn over it, a cluster a subgroup, bricks hidden behind what is drawn keep no voxels), and the
software raster (`vis sw raster 0`) from 16.7 to 26.9 ms to 13.5 to 20.5. Against that the end pays the resolve,
+1.20 to +1.46 ms; the cache's filter, 0.60 to 0.75, new; the meshlet culls, +0.44 to +0.61 (`vis cull meshlets 0`)
and +0.40 to +0.57 (`1`); the second software raster, +0.39 to +0.79; and in the turn the sun's map, `vsm raster`
1.13 to 3.86 ms at 4K (0.59 to 1.88 at 1440p), drawing the pages the turn brings into view as the phase's map
draws them: a level finer where the sun grazes (ADR-0080), the alpha-tested casters as drawn (ADR-0088), a band
between levels (ADR-0093). Its lines at native 4K (the end; the mean of the rounds, the worse round's
99th percentile; the start's raster was 41.9 to 51.5 ms of its frame):

| The landscape | Budget | forest | walk | sprint | turn | across | valley |
|---|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 5.93 | 5.94 | 5.99 | 5.69 | 6.22 | 5.24 |
| Visibility buffer raster (HW + SW) | 0.65 | 32.57 | 32.56 | 32.87 | 33.82 | 33.83 | 26.30 |
| Material + shading + direct light | 1.20 | 5.29 | 5.32 | 5.38 | 5.99 | 6.33 | 5.03 |
| Shadows (cached virtual pages) | 0.40 | 0.10 | 0.71 | 0.48 | 4.52 | 0.10 | 0.10 |
| Indirect light (cache, occlusion, reflections) | 0.60 | 2.40 | 2.39 | 2.40 | 2.65 | 2.75 | 2.61 |
| TAA + post + tonemap | 0.40 | 1.23 | 1.23 | 1.23 | 1.23 | 1.24 | 1.24 |
| **Frame, mean** | **4.17** | **47.59** | **48.24** | **48.42** | **53.97** | **50.57** | **40.60** |
| **Frame, 99th percentile** | **4.17** | **47.87** | **51.26** | **50.59** | **60.27** | **50.93** | **40.85** |

**Memory** (device-local memory in use, as `render_perf` reports it after each view, GB):

| Scene | 1440p start | 1440p end | 4K start | 4K end |
|---|---|---|---|---|
| Bistro | 4.32 | 4.76 | 4.46 | 5.21 |
| The landscape | 6.09 to 6.16 | 6.67 to 7.01 | 6.23 to 6.30 | 6.94 to 7.00 |
| The city | | 3.86 | | 4.17 |
| The interior | | 4.02 | | 4.46 |

The landscape's working set is over §8.8's 6.5 GB at both sizes, where at the start it was under. Of Bistro's 0.75
GB more at 4K, the phase's ADRs record about 0.5 GB: the shadow map's pool, twice as large (134 MB more, ADR-0080);
the many lights' reservoirs, 133 MB, and their tiles' lists, 2 MB (ADR-0074); the reflections' two images, 66 MB
each (ADR-0077, ADR-0089); TAA's unrest, two images of 33 MB (ADR-0093); the radiance cache, 34 MB where it was 9
(ADR-0090, ADR-0091); the rays' structures, 11 MB more. The rest, about 0.25 GB, grows with the frame's pixels (from
1440p to 4K the end's Bistro grows 0.45 GB, the start's 0.14) and is not itemized here. The landscape has no many
lights, so no reservoirs, but the voxel casters' depths grew with the pool, by 134 MB (ADR-0080).

At 1440p the landscape's views hold more proxy levels than at 4K (the forest's 1,007 MB where 4K's holds 868), and
the end's fill the 1,200 MB pool from the turn on (1,202.5, 1,258 and 1,369 MB held; the start's 1,123 to 1,328,
full in the valley's view only), so levels get buffers of their own, made on the main thread. In the turn, whose
levels change while it is measured, the stream's update then took up to 1.88 and 1.98 ms in a frame in the two
rounds, over §10.5's main-thread millisecond, where the start's took 0.40 (at 4K, 0.31 to 0.32 ms at the end and
0.64 to 0.76 at the start).

**The golden tests.** Main's CI on 62bc9fb passed, every one of its 23 jobs (run 43979797349): among them the Linux
jobs that render the golden images on lavapipe (docs/golden.md). So did its GPU job (43979797416), the synthetic
scenes against their budgets on the RTX 3060 Ti.

**The nightly's budgets** (`benchmarks/reference_budgets.txt`, which the nightly holds each view's passes to): the
gate's first round is inside all 1,171 of its rows, its second over one by 0.13 % (`city_park`'s `gi update`,
1.000 ms against 0.999).

## Decision

1. **Phase 14 ends at 62bc9fb**, its criteria met as measured above: the golden tests pass; the gate holds in
   sixteen of the eighteen views, the other two recorded over it by ADR-0099 and measured no worse than there; each
   §10.4 line is reported at native 4K, in milliseconds; the landscape is faster than at the phase's start in every
   view, at the gate's size and at 4K.
2. **Bistro ends slower than at the phase's start, by the light the phase added:** 2.16 to 2.92 ms at the gate's
   size, 3.50 to 5.16 at 4K. The ADRs that added it gave its cost to "Phase 14's last task, the reference scenes
   brought to 144 FPS" (ADR-0076, ADR-0077, ADR-0078, ADR-0089), and that task ends with the gate held in sixteen
   views and two recorded (ADR-0082, ADR-0099). Where the cost is won back, as §10.7 asks:
   - not by giving any of the light up: the gate is measured with the phase's features on, and no image is given
     up for a number (ADR-0041; the user's rule of 2026-11-13, "We can not sacrifice any image quality to get to the
     gate");
   - the lines it sits on at native 4K, the shading, the indirect light, and TAA with post, are the first game's to
     bring inside §10.4 during its development, which ADR-0067 gates at 4.17 ms, from the figures above;
   - Phase 15's guard rail reads against this record: its reference scenes are to be no slower than here.
3. **The landscape's memory goes to Phase 15, recorded here:** 6.67 to 7.01 GB at the gate's size and 6.94 to 7.00
   at 4K, over §8.8's 6.5 GB, with its voxel pool full from the turn on at the gate's size and the stream's
   main-thread update then up to 1.98 ms in a frame. Phase 15, production hardening, brings the landscape back
   under 6.5 GB with the phase's features on at both sizes, and its stream's main-thread time inside §10.5, or
   records with measurements why it cannot. The city, the game's world, holds 3.86 GB at the gate's size and 4.17
   at 4K.
4. **Carried forward, not built in Phase 14** (the user's decision of 2026-11-16), each taken up with its own ADR
   where Phase 15 or the first game first needs it:
   - the landscape's walk down its hierarchies, which its voxel meshes keep (ADR-0070): `vis cull nodes 0` 1.66 ms
     in the forest's view at 4K (1.72 at the start) and 0.90 at 1440p; and its visible-cluster lists at 16 times
     the 524,288 default;
   - each voxel's own visibility sharpness (ADR-0061, "Accuracy"): `NPH_RENDER_VOXEL_VIEW_SHARPNESS` is one
     constant;
   - ADR-0065's texture-stream frames over §10.5's 0.3 ms submission budget (the acquire submit folded into the
     frame's, the page copies on the job system);
   - the far field in the rays (ADR-0060, ADR-0061's stage 6, ADR-0063): rays trace level 0, and the top level is
     built again each time the camera is 16 m from its anchor;
   - moving instances, for which the top level is still built whole on each frame they move (ADR-0100);
   - the voxel pool filled by fast flight (seen in the city's viewer on 2026-11-16) or by what a view holds (the
     landscape at 1440p, above), after which proxy levels get buffers of their own made on the main thread.

## Alternatives considered

- **Give up some of the phase's light to bring Bistro back to its start:** excluded by the user's rule and
  ADR-0041, and the gate measures the phase's features on.
- **Hold the exit until each §10.4 line fits:** ADR-0067 gave the lines to the first game's development; Bistro's
  frame at 4K is 10.6 to 12.6 ms, and ADR-0099 found no exact route even to the gate for two of its views.
- **Read the guard rail at 4K only:** ADR-0082 reads it at the gate's size; both are given, and they agree.
- **Bring the landscape under 6.5 GB before the exit:** the memory is not among Phase 14's criteria, the first
  game's city holds 3.86 to 4.17 GB, and sizing the frame's images and the voxel pool to what each scene uses is
  the residency work Phase 15's soak and shipping tests measure.

## Consequences

- Phase 15 starts at 62bc9fb with this record as its start. Its exit holds the gate in shipping builds, which have
  no per-pass timestamps, and measures ADR-0099's two views again (ADR-0099, decision 4).
- The results are `benchmarks/results/reference_scenes-2026-11-16-exit-1440p` (the gate's first round, Bistro, the
  city and the interior at 2560 × 1440; `-exit-1440p-b` its second), `-exit-4k` (the same at 3840 × 2160, the lines
  above), `-exit-landscape-1440p` and `-exit-landscape-4k` (the landscape at each size, each with a `-b` for its
  second round), beside the start's `reference_scenes-2026-10-30` (Bistro) and `-2026-10-30b` (the landscape).
- The nightly is unchanged: its budgets hold the views to the means of ADR-0097's rounds plus 5 %, and the second
  round's one row 0.13 % over says the margin there is the runs' spread.
- The phase's release tag, v0.9.0 (CLAUDE.md §7), marks the commit that lands this record.
