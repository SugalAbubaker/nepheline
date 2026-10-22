# 0041. The frame target's gate: Phase 14's exit for the reference scenes, Phase 15 for the game

- **Date:** 2026-10-22
- **Status:** Accepted (the user's decision of 2026-10-22; CLAUDE.md
  §10.1, §10.7, §12 and §13 amended; contact shadows placed at Phase
  10's review, at the end)

## Context

CLAUDE.md §12 closed Phase 10 on two criteria: a CPU cost constant in the
object count, and the reference scenes holding a 1%-low frame time of
4.17 ms at native 4K on the RTX 3060 Ti, every pass inside its §10.4
line. Phase 13 was to keep the target with the landscape, and Phase 14 to
stay within the §10.4 budget with the interior.

Phase 10's features are in: §8.6 stages 3 to 8 (ADR-0029 to ADR-0039),
with Bistro as its reference scene (ADR-0040); contact shadows are
deferred (ADR-0033, decision 9). The CPU criterion holds: the production
path's CPU time is 9.7 to 10.0 µs from 1,000 to 100,000 objects, where
the reference path's grows from 0.20 to 12.2 ms (the `grid_*` scenes,
`render_perf-2026-10-22e`).

Bistro does not hold the frame. Its views on the reference machine
(docs/benchmarks.md "GPU benchmarks"; `reference_scenes-2026-10-22e`,
Profile, 240 frames), each §10.4 line's passes summed, the occlusion
counted with the indirect light (ms):

| §10.4 line | Budget | Street | Walk | Turn | Terrace | Hedge |
|---|---|---|---|---|---|---|
| Culling + depth pyramid | 0.25 | 0.44 | 0.44 | 0.45 | 0.44 | 0.43 |
| Visibility buffer raster (HW + SW) | 0.65 | 0.77 | 0.77 | 1.15 | 1.24 | 1.41 |
| Material classify + fused shading + direct light | 1.20 | 2.93 | 2.95 | 2.76 | 2.96 | 2.50 |
| Shadows (cached virtual pages) | 0.40 | 0.09 | 0.11 | 0.15 | 0.10 | 0.07 |
| Indirect light (radiance cache, occlusion) | 0.60 | 1.53 | 1.53 | 1.52 | 1.54 | 1.50 |
| TAA + post + tonemap | 0.40 | 1.17 | 1.17 | 1.17 | 1.17 | 1.17 |
| **Frame, mean** | **4.17** | **6.92** | **6.97** | **7.15** | **7.45** | **7.07** |
| **Frame, 99th percentile** | **4.17** | **6.99** | **7.07** | **7.71** | **7.53** | **7.14** |

UI and present (0.10 ms) are not in the benchmark, and the headroom
(0.57 ms) is unspent. Three things decide the question:

- **About half of each view's overrun does not depend on what the view
  holds.** TAA and post take 1.17 ms in every view, the indirect light
  1.50 to 1.54 ms, culling and the depth pyramid 0.43 to 0.45 ms: 1.9 ms
  over their lines in every view. Only the raster and the shading move
  with the view: the raster with the alpha-tested foliage that the turn,
  the terrace and the hedge face (their masked pass takes 0.66 to 0.98 ms,
  the street's 0.18), the shading with the lamps and materials in view.
  Any content at native 4K pays the first part, a game made to §8.6's
  content budgets included.
- **Part of that is the passes' shape, not their tuning.** A TAA pass of
  today's shape cannot meet its line: its memory traffic alone takes
  0.48 ms against the line's 0.40 ms (ADR-0034). The levers left are
  structural: folding the history blend into the resolve, the fusion
  §10.3 describes ("material, lighting, and post are fused"), and the
  bloom chain on the async compute queue (ADR-0035). The occlusion at
  full resolution, which the user chose over half (ADR-0038's amendment),
  takes 1.32 to 1.33 ms of the indirect line's 0.60 ms, which the
  radiance cache (0.18 to 0.21 ms) shares.
- **Tuning no longer closes the gap.** The last four optimizations took
  the street from 7.28 to 6.92 ms (`reference_scenes-2026-10-22` to
  `-22e`), 0.01 to 0.14 ms each, against 2.75 to 3.28 ms still to go
  (3.54 ms at the turn's 99th percentile).

Some of the rest belongs to later phases. Phase 14's many-light sampling
(ReSTIR: Bitterli et al., "Spatiotemporal Reservoir Resampling for
Real-Time Ray Tracing with Dynamic Direct Lighting", SIGGRAPH 2020) is
likely to replace the loop over each tile's lights, and Phase 13's
virtualized geometry bounds the triangles a view rasterises by its
pixels. Those phases also add GPU work: virtual texturing's feedback and
lookups, reflections, the sampling itself.

On 2026-10-22 the user proposed moving the target to Phase 15, to be met
by the game built on the engine at the end of the roadmap (§13 leaves the
game open).

## Decision

1. **The target and the table stand.** 1%-low ≤ 4.17 ms at native 4K
   (§10.1), against the §10.4 lines as they are: no line is raised to
   meet a measurement, and no quality is given up to meet a line
   (ADR-0040's review; the occlusion at full resolution). What moves is
   when the target gates.
2. **Phase 10 exits on its features, its CPU criterion, and this
   record:** the table above, and each line's path to its budget:

   | §10.4 line | Over | Path | When |
   |---|---|---|---|
   | Culling + depth pyramid | 0.18–0.20 ms | tuning, chiefly the pyramid's first pass (0.30 ms), which also writes the occlusion's depths and the shadow map's page requests | Phase 14 |
   | Visibility buffer raster | 0.12–0.76 ms | virtualized geometry; the masked pass of Bistro's alpha-tested foliage, kept as authored (ADR-0040) | Phases 13 and 14 |
   | Shading | 1.30–1.76 ms | the light loop through many-light sampling; the resolve's structure, with the fusion below; its register pressure | the checkpoint, Phase 14 |
   | Shadows | inside, 0.25–0.33 ms left | contact shadows and local lights' shadows will spend from it (ADR-0033; placed at the end) | Phase 14 |
   | Indirect light | 0.90–0.94 ms | full-resolution occlusion in a structure that fits its share; ADR-0038's amendment leaves two levers open (the occlusion and its denoise in one pass; fewer special-function instructions in the slices) | the checkpoint |
   | TAA + post + tonemap | 0.77 ms | fused with the shading (§10.3, ADR-0034); the bloom chain on the async compute queue (ADR-0035) | the checkpoint |

3. **Phases 11 and 12 come next.** Physics and audio depend only on
   phases 1 to 4, 8 and 9 (§12).
4. **Phase 13 opens with a structure checkpoint**, before its features:
   - prototypes of TAA and post fused with the shading, and of
     full-resolution occlusion in a form that fits its share of the
     indirect line, measured on the synthetic scenes and Bistro and
     recorded in an ADR;
   - a prototype keeps the image: the neighbourhood clip across tile
     borders and the blended surfaces under TAA (the fold ADR-0034
     sketched would give up both), and `test_taa.c`'s and `test_ao.c`'s
     errors;
   - a line that cannot be met at native resolution and full quality is
     reported with its measurements (§10.4), and the user decides then,
     before Phases 13 and 14 build on its passes;
   - the game's setting, camera, and world density are decided (§13),
     and the reference scenes are checked against them: if none
     resembles the game, one that does joins them.
5. **Guard rails until the gates:**
   - the synthetic scenes on every renderer pull request and the
     reference scenes nightly keep their regression budgets
     (docs/benchmarks.md), and renderer pull requests state their cost
     in milliseconds and DRAM bytes (§10.7);
   - no phase from 11 to 14 ends with a reference scene slower, in mean
     or 99th percentile, than when it began, measured on the reference
     machine as docs/benchmarks.md describes it (a driver change
     re-measures the start), unless an ADR shows where the added cost is
     won back.
6. **The gates:**
   - **Phase 14's exit**, the renderer's last phase, when every feature
     the lines pay for is in: every reference scene (Bistro, the
     landscape from Phase 13, the interior from Phase 14) holds 1%-low
     ≤ 4.17 ms at native 4K on the RTX 3060 Ti, every pass inside its
     §10.4 line;
   - **Phase 15:** the first game holds 1%-low ≤ 4.17 ms at native 4K
     on the reference hardware, inside the §10.4 and §10.5 budgets.
7. **CLAUDE.md says so:** §10.1 (the game measures the target too),
   §10.7 (the gates, the guard rails, the checkpoint), §12 (the exit
   criteria of Phases 10, 13, 14 and 15) and §13 (the game's decision and
   its date).

## Alternatives considered

- **Keep Phase 10 open until Bistro holds the frame.** Each optimization
  found 0.01 to 0.14 ms against 2.75 to 3.28 ms, and the largest
  overruns need new structure or later phases' features. Physics, audio,
  and the rest of the roadmap would wait on it.
- **Gate only the game, in Phase 15**, as first proposed. The 1.9 ms
  that is the same in every view would be the same in the game, and a
  measured part of it is structural, in passes that Phases 13 and 14
  extend: the resolve takes virtual texturing's lookups, the reflections,
  the sampling. Restructuring is cheapest before them. The game does not
  exist yet, so no scene would stand for it while those phases add their
  work, and no phase after 15 absorbs a miss.
- **Gate the reference scenes in Phase 15, with the game.** Phase 15 is
  production hardening. The renderer's performance work belongs with the
  renderer's last phase, so that the game's performance pass is about
  the game.
- **Gate every later phase on the frame**, as Phases 13 and 14 read.
  Unmeetable before the checkpoint's structures exist; the guard rails
  keep the gap from growing instead.
- **Raise or rebalance the lines** (ADR-0034's third option, from the
  headroom), or **cheaper settings** (its second, and the occlusion at
  half resolution): declined at the review of 2026-10-20 (ADR-0040) and
  in ADR-0038's amendment.

## Consequences

- Phase 10 closes at its review, which merges its stack.
- ADR-0034's options for the review are settled: the fold, in a form that
  keeps the image, is the checkpoint's first prototype, and cheaper
  settings and rebalancing are declined. ADR-0035's async chain is the
  checkpoint's to measure. ADR-0040's overruns are scheduled above.
- Phase 13 grows by the checkpoint, and Phase 14 carries the reference
  scenes' performance work and their gate: most of the renderer's
  remaining work sits there.
- The target gates four phases later than it did, but the questions of
  structure are answered before the phases that build on those passes,
  and the guard rails keep the gap from growing meanwhile.
- The game becomes a decision with a date: its setting, camera, and
  density at the checkpoint (§13).
- Contact shadows, left open for the review, go to Phase 14 (below).

## Decided at Phase 10's review (2026-10-22)

- **Contact shadows go to Phase 14**, with the local lights' shadows
  (ADR-0033: decision 9, and its open list). Phase 14's many-light
  sampling traces a shadow ray toward each light it samples, and the
  checkpoint may move the shading that screen-space contact shadows would
  extend, so they are built once, after both, from the shadow line's 0.25
  to 0.33 ms. CLAUDE.md §12's Phase 14 scope lists them.
