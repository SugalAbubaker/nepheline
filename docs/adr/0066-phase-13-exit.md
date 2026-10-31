# 0066. Phase 13's exit: Bistro's cost of its level hierarchies, won back in Phase 14

- **Date:** 2026-10-30
- **Status:** Accepted (the user's decision of 2026-10-30: record the cost
  and win it back as Phase 14's first task; the gate it names moved by
  [ADR-0067](0067-the-game-is-a-city.md); its first task done by
  [ADR-0070](0070-a-flat-cull-of-the-levels.md))

## Context

CLAUDE.md §12 closes Phase 13 on four criteria: the structure checkpoint's
prototypes measured and recorded (ADR-0059); the open-world test scene
streaming without hitches within 6.5 GB of VRAM; the landscape measured
against every §10.4 line; and no reference scene slower than at the phase's
start. §10.7 lets a phase from 11 to 14 end with a reference scene slower
than when it began only where an ADR shows where the added cost will be won
back. Every figure below is the RTX 3060 Ti at native 4K, render_perf in
Profile, 240 frames a view, with the user's editor windows minimized
(ADR-0059).

**Bistro, against the phase's start.** The phase's start, 8274d5f, built
again and run with the scene cooked as it was then (`S:/nepheline-scenes`),
interleaved with the phase's end, the stack's top 4889e61, with the scene
as Phase 13 cooks it (its meshes' level hierarchies, ADR-0060; the cells,
ADR-0062): four rounds, alternately first.

| View | Start (p99) | End (p99) | Change |
|---|---|---|---|
| street | 6.879 (6.949) | 6.918 (6.978) | +0.039 |
| walk | 6.938 (7.047) | 6.983 (7.095) | +0.045 |
| turn | 7.110 (7.643) | 7.190 (7.718) | +0.080 |
| terrace | 7.415 (7.500) | 7.491 (7.566) | +0.076 |
| hedge | 7.059 (7.138) | 7.123 (7.201) | +0.063 |

Every round put the end behind the start in every view. The cost is the
content's, not the code's:

- **The code is neutral.** The end's build drawing the start's cook measured
  +0.001 to +0.017 ms against the start's build in the session before
  (street 6.880, walk 6.946, turn 7.127, terrace 7.428, hedge 7.062).
- **The hierarchies cost +0.023 to +0.058 ms.** The end's build, the two
  cooks interleaved over four rounds: street +0.024, walk +0.023, turn
  +0.054, terrace +0.052, hedge +0.058. The walk down the hierarchies is
  the cost: `vis cull nodes 0` 0.030 to 0.033 ms and `vis cull nodes 1`
  0.012 to 0.017 ms, three dependent steps a phase on Bistro's 1,548 nodes
  (about 11 µs a step), and the shadow map's walk (`vsm cull nodes`)
  0.008 ms. The instance cull is 0.004 to 0.016 ms cheaper, and the raster
  moves by -0.004 to +0.010 ms: Bistro's views are near, and the levels
  save it almost nothing, where the open world needs them.

**The landscape, against every §10.4 line** (its views in their manifest's
order, the cells' proxies streamed, ADR-0063; the textures streamed within
128 MB, ADR-0065; the lists 16 times their defaults, since at the defaults
every view overflows the 524,288 visible clusters and drops work):

| §10.4 line | Budget | forest | walk | sprint | turn | across | valley |
|---|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 4.84 | 4.85 | 4.89 | 4.62 | 4.86 | 4.27 |
| Visibility buffer raster (HW + SW) | 0.65 | 50.39 | 50.73 | 51.26 | 50.55 | 51.62 | 42.01 |
| Material + shading + direct light | 1.20 | 3.48 | 3.48 | 3.52 | 3.89 | 4.45 | 3.35 |
| Shadows (cached virtual pages) | 0.40 | 0.09 | 0.13 | 0.23 | 1.33 | 0.09 | 0.09 |
| Indirect light (radiance cache, occlusion) | 0.60 | 1.50 | 1.50 | 1.50 | 1.55 | 1.66 | 1.61 |
| TAA + post + tonemap | 0.40 | 1.07 | 1.08 | 1.08 | 1.08 | 1.08 | 1.08 |
| In no line (voxel resolve, cells) | | 0.48 | 0.48 | 0.50 | 0.66 | 0.45 | 0.45 |
| **Frame, mean** | **4.17** | **61.84** | **62.25** | **62.97** | **63.68** | **64.22** | **52.85** |
| **Frame, 99th percentile** | **4.17** | **62.33** | **62.88** | **64.40** | **68.51** | **64.53** | **53.18** |

The raster is most of the frame: the software raster (`vis sw raster 0`,
16.7 to 25.9 ms) and the far field's voxels (`vis voxel raster 0`, 19.7
to 21.8 ms). The walk down the hierarchies (`vis cull nodes 0`, 1.6 to
1.7 ms) and the meshlet cull (1.7 to 2.0 ms) carry the culling line.
Phase 14's exit gates every reference scene at 4.17 ms (ADR-0041).

**Memory.** The landscape held 6.23 GB of device-local memory in its forest
views and 6.30 GB from the turn on, nothing short, loading or failed, with
the renderer's voxel pool sized to what the views hold (`--voxel-pool=1200`:
789 to 1,098 MB of proxy levels). render_perf's default pool with streaming,
1,536 MB reserved at creation, takes it to 6.57 to 6.64 GB, and to 6.72 GB
with the textures whole (272 MiB). The streams' main-thread updates took
0.017 to 0.046 ms a frame, 0.64 ms at most (the turn).

## Decision

1. **Phase 13 ends with Bistro 0.04 to 0.08 ms slower than at its start**,
   its meshes' level hierarchies' cost, which the open world needs.
2. **Phase 14's first task wins it back** by making the walk cheaper,
   measured on Bistro and the landscape:
   - an instance whose every cluster is at level 0 at its distance (its
     hierarchy's smallest maker error too coarse even from its sphere's
     far side, `vis_lod_surely_too_coarse` at the root) skips the walk, its
     level-0 meshlets handed to the meshlet cull as with levels of detail
     off;
   - fewer dependent steps: more levels a step, or a walk in one dispatch,
     against the per-step cost that dominates on a small hierarchy.

   Its target: Bistro at or below the phase's start, with its
   hierarchies, in an interleaved A/B against 8274d5f as here; and the
   landscape's walk cut with it.
3. **The open world's memory is the game's to size:** the voxel pool
   follows the proxy stream's budget, as here, and the textures stream
   within theirs. render_perf's default pool stays as it is, for the
   benchmarks' continuity, and its figure is reported beside the sized one.
4. **Recorded for Phase 14** with the rest of the landscape's overruns:
   the raster's 42 to 52 ms (the software raster and the far field's
   voxels); the visible-cluster lists, which the landscape needs at 16
   times their defaults; the texture stream's main-thread cost in frames
   that install pages (ADR-0065 stage 3: the upload flush's second submit,
   the page copies on the job system).

## Alternatives considered

- **Make the walk cheaper before ending the phase**, an ADR for what
  remained: the recommended option, not chosen. The user preferred to
  record the cost and make it Phase 14's first task, where every reference
  scene's frame is gated (ADR-0041).
- **Cook Bistro's meshes without hierarchies where they do not help**:
  rejected. It would bring the scene back to the start by testing less of
  the pipeline the game uses.

## Consequences

- The guard rail of §10.7 holds through this record: Phase 14 starts with
  Bistro the table's margin behind the phase start, wins it back first,
  and must end with every reference scene inside its §10.4 lines.
- The phase's results are `benchmarks/results/reference_scenes-2026-10-30`
  (Bistro, the end's first round) and `reference_scenes-2026-10-30b`
  (the landscape, as in the table), beside the start's
  `reference_scenes-2026-10-25`.
