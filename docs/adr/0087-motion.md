# 0087. The radiance cache in motion: new cells start from their neighbours, and reflections are refilled

- **Date:** 2026-11-11
- **Status:** Accepted (the user's report of 2026-11-11, after testing ADR-0086's build in the viewer: "as I move around
  the world there are concentric rings of 'light' that move with the camera ... The fact that I need to stand still
  for the noise to go away is unacceptable for a game engine"); the new cells' seed is replaced by ADR-0090's
  parents (proposed 2026-11-11)

## Context

ADR-0084, ADR-0085 and ADR-0086 measured still views, and still views had become quiet. A camera that moves was
another matter. In the viewer, the user saw:
- rings of light around the camera, moving with it, across shaded ground;
- noise smeared over the shop windows' glass;
- noise on shaded surfaces such as the bus, which went away only when the camera stood still.

To measure motion, render_perf's moving views (a walk at 1.4 m/s, a sprint at 6 m/s, a turn at 90 degrees a second,
at 240 frames a second) were compared with themselves held still: each view's last frame in motion against the same
pose after 600 still frames, the mean |luma difference| in display levels. The sprint along the city's street was
8.5 levels off on average, 12.7 % of its pixels by more than 24, and 7.1 levels brighter than when settled. The
irradiance debug view showed the rings plainly: bright arcs on the ground, centred on the camera.

One cause ran through all of it. A cell with few samples was trusted in proportion to them, up to 16, and the
environment's irradiance made up the rest. In shade the open sky is many times what arrives. New cells appear
whenever the camera moves:
- **The rings.** The cells' size follows the distance from the camera (a level of cells for each doubling of it),
  so a surface the camera walks toward crosses into a finer level, whose cells there hold nothing. The level's
  boundary is a sphere around the camera, and the new cells behind it glowed until they filled.
- **Leaves.** A tree's leaves face every way, each octant a cell of its own; their new cells, with no neighbouring
  level's cell of the same octant to start from, glowed the longest. The tree beside the camera's path was
  brighter in motion than any other surface.
- **The glass.** A reflection cell is keyed by the mirror direction as well as the place, and a moving camera turns
  every pixel's mirror direction into bins no ray has filled. A tile's rays come one frame in 32. Until then the
  pixel took its fallback, the sky along its direction held to the irradiance cell's light; while moving, that
  cell was young too, so the hold was weak and the sky showed in blocks.

## Decision

1. **A new irradiance cell starts with its neighbouring level's light** (`gi_seed`, in `gi_update.comp`, by the
   invocation whose compare-and-swap took the slot: `gi_claim_new`). The cell around the same point a level up
   (twice its size) or down (half), whichever holds more samples, gives its irradiance and far-field share,
   counted as up to `NPH_RENDER_GI_SEED` (16) samples; the new cell's moments start afresh (ADR-0084). The level
   the surface leaves held its light a moment ago.
2. **A cell with a sample is trusted** (`gi_cell`, `gi_surface_blend` and its CPU twin `render_gi_blend`). Within
   the four-cell blend each cell still weighs by its samples (a young cell counts for little beside settled
   neighbours), but the blend's trust is 1 wherever any cell holds a sample; the environment stands in only where
   no cell does. A young cell's first samples are noisy, but no further from its light than the open sky is in
   shade.
3. **Tiles whose pixels found no reflection cell ask the next frame's rays for them** (`reflect_want`, a bit a tile
   in `NphRenderGiView`, in the place of ADR-0086's padding words, which keeps the vectors after it on 16 bytes).
   `reflect.comp` sets a tile's bit when a pixel's own cell holds no sample and its key holds from frame to frame
   (`ReflectKey.stable`: the surface's plane was found); `reflect_rays.comp` reads and clears it, and in a tile
   that is not having its turn, each such pixel claims its cell and the one that creates it traces a ray, with no
   roulette. One ray a missing cell.
4. **A reflection cell lives while it is looked up.** `reflect.comp` sets the age of each cell it finds back to 0,
   and `NPH_RENDER_REFLECT_MAX_AGE` falls from 60 frames after the last sample to 16 frames after the last lookup:
   a visible cell is kept however long its tile waits for its turn, and a moving camera's cells left behind give
   their slots back four times sooner.

## Alternatives considered

- **A seed counted as 128 samples**, so a new cell keeps its parent's smoother light longer: the sprint's error
  2.26 -> 2.11, the walk's 1.47 -> 1.44, within the measure's noise; 16 kept.
- **Seeding without the trust change** (decision 1 alone): the rings went, but the tree beside the path still
  glowed (no cell of its octants a level away to start from), and the sprint was still 2.7 levels too bright.
- **A neighbourhood for missing reflection cells**: the four direction bins nearest the pixel's and its own
  direction a level of cells up and down, blended to stand in until the refill comes. It moved the city's moving
  views by no more than their noise (the walk 1.47 against 1.48 without it, the turn 2.16 against 2.09): a
  camera's step gives a pixel new spatial cells as often as new directions, and their neighbours are as new. Not
  kept.
- **A wanted tile's pixels making their missing cells whatever their keys** (with the neighbourhood above as the
  gate): `reflect rays` on Bistro's walk went 73 -> 233 µs, 191 % of its budget, against 129 µs with the pixels
  whose keys follow the jitter (edges, leaves) left out, whose cells would be made anew every frame and never found
  again. (At first a tile's every pixel made its missing cell: a surface whose mirror directions scatter, Bistro's
  cobbles, made one for nearly every pixel.)
- **Reflection cells freed 60 frames after their last sample**, as before: with the refill, a moving camera's cells
  filled the table. Rays dropped by full buckets over 240 frames: the city's walk 964,000 against 539,000, the
  sprint 3.4 million against 911,000.

## Consequences

- **Motion** (render_perf's moving views at 2560 × 1440, each view's last frame in motion against the same pose
  held 600 frames; the mean |luma difference| in levels of 255, the share of pixels off by more than 24, and the
  mean signed difference, positive where motion is brighter):

  | View | ADR-0086 | This |
  |---|---|---|
  | City `street_walk` | 3.35, 3.9 %, +2.20 | **1.49, 0.8 %, +0.21** |
  | City `street_sprint` | 8.50, 12.7 %, +7.10 | **2.24, 1.5 %, +0.39** |
  | City `street_turn` | 3.71, 4.2 %, +2.70 | **2.18, 1.1 %, +0.49** |
  | Bistro `street_walk` | 3.89, 2.8 %, +1.63 | **2.72, 0.9 %, +0.56** |
  | Bistro `street_turn` | 5.81, 6.0 %, +3.60 | **3.59, 1.4 %, 0.00** |
  | Interior `floor_walk` | 6.00, 3.5 %, -1.25 | **5.40, 2.4 %, -0.23** |
  | Interior `floor_turn` | 6.78, 5.3 %, -2.21 | **6.26, 4.1 %, -1.36** |

  The rings are gone from the irradiance view, the tree no longer glows, and the windows keep their reflections
  but for thin streaks at their panes' edges. What is left near the camera is cell-sized blotches on the ground:
  cells that came into a finer level start from the coarser level's softer light and take a few frames to reach
  their own. The interior's error is its many lights' cells, which see a wall the first time as the camera turns
  to it, with no neighbouring level to start from and a candle a sample: their first frames are noisy.
- **Still views** (600 frames, the noise of render_perf's 64-frame accumulation): the city's street 3.71 -> 2.88 %,
  crossing 2.15 -> 1.75 %, park 4.24 -> 3.97 %; Bistro's street 4.39 -> 3.71 %, hedge 4.22 -> 3.47 %; the rest within
  0.2 %. On the pixels that take reflection cells, against ADR-0086's truths (`--reflections=2`): the city's street
  1.06 -> 0.66 levels off and its noise 5.2 -> 1.9 %, the crossing 1.01 -> 0.76 and 3.0 -> 1.5 %; Bistro's street
  6.48 -> 4.29, its terrace 6.84 -> 4.02 (the reflections' fallback now holds the sky to an irradiance cell that is
  trusted).
- **The cost** (two rounds interleaved with ADR-0086's build, 240 frames a view, RTX 3060 Ti):

  | Scenes | `reflect rays` | `vis_resolve` | Frame |
  |---|---|---|---|
  | The city (7 views) | +5 to +20 µs | -25 to -48 µs | -14 to -46 µs |
  | Bistro (5 views) | +5 to +56 µs | -220 to -289 µs | -192 to -273 µs |
  | The interior (6 views) | 0 to +4 µs | +4 to +9 µs | 0 to +17 µs |
  | Synthetic, 3840 × 2160 | 0 | -59 to +10 µs | -67 to +17 µs |

  The resolve is faster where it had fetched the sky for cells it partly trusted: Bistro's many small surfaces left
  corners of the blend missing, and every such pixel fetched the sky's harmonics and its cube. `gi update` (the
  seed's two lookups, on a cell's first sample only) and `reflect` (the age it sets) are within their noise.
  Re-based: `bistro_street_walk_production.gpu.reflect_rays`, past its budget (73 -> 129 µs, the budget 122 -> 179
  µs), from `benchmarks/results/reference_scenes-2026-11-11-motion-win-msvc-profile-rtx3060ti.json` and its second
  round. DRAM: the want bits are a bit a tile (7 KB at 2560 × 1440); a refill ray reads what a turn's does.
- **Tests:**
  - `Gi.a_surface_entering_a_finer_level_keeps_its_light`: shaded ground under a roof, settled, the camera then 2 m
    nearer and one frame drawn. The ground's mean stays within 8 % of the same view settled (0.2766 against
    0.2648), and of the cells its near half has come into, 694 of 1,546 pixels' hold a seed's samples (not all:
    at the test's 128 pixels, ground seen at a grazing angle covers several cells along it a pixel, and the
    coarser cells no pixel's point fell in hold nothing to give).
  - `Reflect.a_mirror_shows_its_wall_soon_after_the_camera_moves`: ADR-0077's mirror and red wall, settled, the
    camera a step aside: four frames later the wall's reflection is 0.688 red against 0.704 settled.
  - Four planted faults, each caught: no seed (none of the 1,546 seeded), the old trust ramp (the shaded ground
    0.4165 against 0.3154), no refill (the wall's reflection 0.16 red four frames after the move), and no age set
    back by lookups (three of ADR-0077's and ADR-0086's mirror tests, their cells freed between turns).
  - `Engine.an_offscreen_engine_draws_the_world` compares the two pipelines with the radiance cache off: a few
    frames young, the cache's cells had leaned on the environment's light, which the reference renderer draws, and
    the comparison had passed on that.
- **The reflection table fills while the camera moves.** Rays dropped by full buckets over 240 frames, ADR-0086's
  build against this: the city's walk 7,400 against 496,000, the sprint 7,800 against 765,000 (the turn 250
  against 7,800). A refill that finds its bucket full leaves its pixel the fallback, as before, and the next frame
  asks again. The table is as large as the irradiance's (ADR-0077); a larger one, or a claim that evicts a cell no
  pixel has looked up for some frames, is the next step for it.
- **Left:** the reflection table's size in motion (above); the near ground's blotches while moving (more of a new
  cell's light from its neighbouring level, or more rays for young cells); the interior's cells new to the camera;
  foliage's static under temporal anti-aliasing; Bistro's bright roughness fade (ADR-0077).
