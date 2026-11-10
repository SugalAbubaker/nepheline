# 0084. The radiance cache's history follows its light

- **Date:** 2026-11-10
- **Status:** Accepted (the user's go-ahead of 2026-11-10: "go ahead with step 1")

## Context

With AgX the default (ADR-0083), the user found the city's shade readable, and
named its static noise the next thing to fix. Measured on the city's street,
park and crossing views at 2560 × 1440 (`render_perf --accumulate=64`: how far
a frame strays from the mean of 64, as a share of it), the noise is the
radiance cache's (ADR-0039): without the cache it falls from 9.3, 6.9 and
5.2 % to 1.6, 2.5 and 1.3 %. Adding its parts as variances, the street's
splits into:

| Part | Share | What tells it |
|---|---|---|
| The cells' wander | ~7 % | Without the lookup's jitter, the cells flicker as blocks |
| The lookup's jitter | ~6 % | Each pixel reads a random point of its cell, a neighbouring cell's light, and TAA's clipping lets most of it through |
| The reflection cells | the most in the street's darks | Glass blotches; rough surfaces' specular takes the irradiance cell again |
| The rest | 1.6 % | Geometry's edges under TAA |

and the park's is mostly its foliage, which needs a look of its own.

**Why the cells wander.** A cell blends each frame's samples into its estimate
weighted by their count against its history's, at most 128 samples. Under a
light that holds still, its estimate then wanders by about 1 / sqrt(2 h - 1)
of a sample's spread for a history of h, 1/16 at 128, whatever the rays a
frame: more rays make it wander faster, not further. Four times the rays had
changed nothing (2026-11-10), and a history of 255 cut the street's cell part
by the predicted sqrt(2) (its noise without the jitter 7.2 to 5.3 %).

A longer history alone would be slow to follow a change of light: 1,024
samples, at the 20 or so a cell takes a frame, leave 93 % of a change after
4 frames.

## Decision

1. **A history of up to 1,024 samples** for the irradiance cells
   (`NPH_RENDER_GI_HISTORY`), which the cell's word counts in 11 bits (the
   many lights' far fields' share keeps 13, `NPH_RENDER_GI_COUNT_BITS`).
2. **A change of light cuts it.** Each irradiance cell keeps moments
   (`NphRenderGiView.moments`, 16 bytes a cell): its light's recent mean,
   over about 32 samples, and a sample's variance, estimated from the
   frames' means (k times a frame's squared deviation from the recent mean,
   less the recent mean's own share), averaged over the first 16 frames and
   followed over as many after. Where the recent mean lies further from the
   cell's estimate than 4 of its own deviations and 2 % of the cell's
   luminance, the light changed: each such frame quarters the history, to no
   fewer than 32 samples (`NPH_RENDER_GI_HISTORY_MIN`). The recent mean
   gathers a few frames' evidence, so a noisy cell with few samples a frame
   tells a change within a few, and one outlying frame moves it little.
3. **The test waits for the variance** to have seen 16 frames
   (`NPH_RENDER_GI_VARIANCE_FRAMES`); until then a cell's history is its
   samples, as every young cell's was. A walking camera keeps making cells
   (a surface's level changes with its distance), so young cells must be no
   noisier than before.
4. **The variance grows a fifth a frame at most** once settled, so a change,
   whose frames stray from the recent mean until it catches up, does not
   blind the test that is cutting the history.
5. **The reflection cells keep 128** (`NPH_RENDER_REFLECT_HISTORY`): their
   noise is their few samples (a tile's turn once in 32 frames), not their
   history's length.

## Alternatives considered

- **More rays.** No effect on the wander (above), at their cost.
- **The longer history without the test.** A doubled sky 8 % followed after
  4 frames and 30 % after 16 (`a_cell_follows_a_change_of_its_light`, with
  the test switched off).
- **A test of each frame's mean alone.** The frame that changed fed the
  variance an estimate thousands of times too large, which blinded the test
  for the frames after; and a noisy cell's frame of 8 samples cannot tell a
  change of 100 % from its noise (its mean strays by 35 %).
- **The test from a cell's first frames.** A guessed variance, wrong for
  noisy cells, cut young cells' histories: the sunlit wall's bounce on the
  ground came out with an error of 0.160 against the truth, where the
  128-sample history's was 0.146.
- **One cut to 32 samples** where a change shows, in place of quartering: a
  little faster to follow, but a false alarm would cost a cell 5.6 times its
  wander where quartering costs 2.

## Consequences

- **Noise**, after 600 frames still (two runs of the same views; the old
  history then, this one now):

  | View | Noise | Pixels over 10 % | Dark pixels' wander |
  |---|---|---|---|
  | `city_street` | 8.8 → 6.8 % | 21.8 → 16.8 % | 8.9 → 6.6 % |
  | `city_crossing` | 5.0 → 4.5 % | 15.0 → 13.6 % | 10.7 → 9.3 % |
  | `city_rooftop` | 4.0 → 3.7 % | 13.5 → 12.7 % | 7.2 → 6.4 % |
  | `city_park` | 7.0 → 6.8 % | 19.5 → 19.3 % | 15.5 → 15.2 % |

  The cells' part of the street's noise falls from about 7 % to about 2.5 %,
  as 1,024 samples against 128 predict; what is left is the lookup's jitter,
  the reflection cells and the park's foliage, the next steps. A cell takes
  a second or two of a still view to fill its history; walking, cells stay
  younger.
- **Following a change**: a doubled sky 73 % of the way in 4 frames and
  97 % in 16, where the 128-sample history went 47 % and 88 % (`test_gi.c`,
  run with the old history). The image's levels are unchanged (the street's near-black
  share 37.7 % and 36.9 %).
- **Accuracy** against the ray-cast truths (48 frames): the occlusion's
  error 0.103 → 0.102, the sun's bounce 0.146 → 0.142.
- **Memory**: 4 MB of moments at 2^18 cells: the table's 10 MB become 14
  (its log now counts the far fields' sums too, which its 9 MB left out).
- **Cost** (RTX 3060 Ti, two rounds interleaved with #169's build): the
  cache's resolve takes 3.2 to 4.3 µs more in the 18 reference views at
  2560 × 1440 (22 to 34 µs before) and 2.2 to 3.8 µs more in the synthetic
  scenes at 3840 × 2160; its update is unchanged, and the frames move within
  their rounds' spread. DRAM: a cell that takes samples reads and writes its
  16 bytes of moments, at most 2 MB a frame at 65,536 rays. No budget
  moves: the resolve's entries are under their 50 µs floor.
- **Tests**: `a_cell_follows_a_change_of_its_light`,
  `a_still_light_is_averaged_over_the_whole_history` and
  `a_young_cell_keeps_all_its_samples` (`test_gi.c`); each catches one of
  the alternatives above put back (the test off, a 128-sample history, the
  variance's growth uncapped, the test before the variance settles, an 8-bit
  count).
