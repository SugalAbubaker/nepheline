# 0090. The radiance cache's light: filtered between cells, and right where the camera has not looked

- **Date:** 2026-11-11
- **Status:** Accepted (built and measured; the user's report of 2026-11-11 that it answers: "there is still a lot
  of noise on surfaces such as the bus where there is shade", "we have to focus on getting the lighting, shadows,
  and level of detail working correctly before worrying about optimization", "you need to be moving back and forth
  around the map to see these issues"; of this build they reported the flicker in a sprint that ADR-0091 answers;
  accepted with ADR-0090 to 0095, 2026-11-13: "commit step 3 to 8")

(ADR-0091 changes three things decided here: the filter steps from what each cell was held to be and not from
its mean plus what the filter added; a cell's samples go to four levels of parents, whose own means a cell short
of samples leans on; and a full bucket's oldest cell gives its slot up.)

## Context

After ADR-0084 to ADR-0089 the indirect light was still what the user saw as noise: shaded walls mottled, and the
mottling changing as the camera moved. Shown directly (the irradiance view, a recorded walk replayed frame by frame),
the radiance cache's cells were each a tenth to a whole of their light out, each its own way. Four things were
wrong, and each was measured against a truth: rays cast on the CPU at boxes (`test_gi.c`), and in Bistro a
reference that follows whole paths with no cache at all (built into the update for the measurement, and taken out
again).

1. **A cell's mean is too noisy where light arrives by few paths.** Shade lit by a sunlit wall across the street:
   a sample is nearly always nothing and now and then a hundred times the light. A thousand samples leave such a
   cell 30 to 90 % out. Under Bistro's arch at 4 m/s the walls' 16-pixel blocks stood 44 to 55 % from their
   neighbourhood's mean.
2. **The test for a change of the light (ADR-0084) took the paths for changes.** Its variance remembered 16 frames
   and held each frame's share down: over the dark frames between two paths it fell away, the next path caught
   was many deviations out, the cell cut its history and the path counted four times over. Under a roof open to
   2 % of the sky, the cells' own means were 15 % too bright and 36 % out a pixel; with no test at all, unbiased
   and 16 % out.
3. **What a ray met unseen was taken for open to the whole sky.** The cache holds cells where the camera's pixels
   have put samples. A ray that lands on a surface with no cell (behind the camera, or seen at so low an angle
   that its samples are cells apart) took that surface's indirect light for the environment's, unoccluded. Under a
   roof walled on three sides, with surfaces that reflect 0.7, the ground got 0.70 of the open sky's light; the
   truth is 0.0625.
4. **New cells began with a guess** (ADR-0087's seed: the neighbouring level's cell, counted as 16 samples), which
   took the rings of light off the ground and left a step wherever two levels' estimates met.

## Decision

### 1. A filter between the resolve and the frame (`gi_filter.comp`, the pass `gi filter`)

What the frame looks up is no longer each cell's own mean (`NphRenderGiView.raw_cells`) but the light f that agrees
best with it and with its six face neighbours' (same level, same octant):

    f = (a m + sum of g_j f_j) / (a + sum of g_j)

the posterior mean of a Gaussian Markov random field (Rue & Held, 2005), reached by one damped Jacobi step a frame.

- **`a`, how much the cell's own mean counts**, is its samples over the count that would leave the light 2 % out
  once spread (`NPH_RENDER_GI_SMOOTH_NOISE`), given how a sample spreads about the light; the variance guides the
  filter as in Schied et al., "Spatiotemporal Variance-Guided Filtering" (HPG 2017). A cell whose samples agree keeps
  its light and its detail; one lit by few paths takes its neighbourhood's.
- **The spread is not the cell's own.** Weighted by its own variance, a cell that has caught a path counts for less
  than one that has not, and the shade came out 43 to 63 % darker than its samples' mean. The spread is that of the
  cell's parents' samples, pooled (below), the same for the cells around; and `g` is the same from either side of a
  pair. The cells' light summed by their samples is then what their means' was (0.9997 to 1.003 in the tests): the
  filter moves light between cells and makes none.
- **The reach is three cells at most** (`NPH_RENDER_GI_SMOOTH_REACH`: `a` is never under 1/9 for a cell with 64
  samples). With `a` near nothing a cell takes its light from where the light is another, and keeps for seconds
  whatever it began with: a shaded wall first seen from near showed four to six times its light, the street's beyond
  it, until the camera stood still. The reference that follows whole paths showed the same wall right once still and
  as wrong on arrival, which is how the filter, not the samples, was found out.
- **`g` is 1 unless two cells' means differ by more than 3 of their difference's deviations** and 5 % of the
  brighter: then the difference is the light's own, and the pair is cut loose.
- **A step starts from each cell's mean now plus what the filter added last frame**, not from last frame's f, so a
  change of the light is followed as fast as the means follow it.
- **New cells begin with what cells that know their place give** (256 samples of their own), and with their own mean
  for the rest. A cell with no sample stays out of the pixels' blend until a quarter of a neighbour's worth of such
  cells is beside it. A cell no sample has reached for 16 frames keeps what it held; one that has had none for two
  takes its step one frame in four.
- **A pixel whose four cells hold nothing takes the next level's four**, their parents, before the environment's
  light (`gi_surface_blend`): a surface the frames have just come to has its cells a few frames later than its
  pixels, and the environment's light indoors is many times the room's (white patches wherever the view turned).
- **Nothing counts for more than 16 times the light around a cell** (`NPH_RENDER_GI_OUTLIER`): one sample that met a
  lamp is a thousand times the light, and stood out as a white square. The cell's own mean is kept.

### 2. Every sample is also its cell's parent's (the cell of the next level around the point)

Four cells' samples in one. The eight parents around a cell give it the pooled spread and light (kept beside its
key, refreshed one frame in four), a prior while it is young (fading over its first 64 samples, and by how far the
parents know their light themselves), and, through the last eighth of its level's distances, their light: the next
level's cells begin there with the same light, so no step follows the camera. This replaces ADR-0087's seed.

The parents are **not** a neighbour of a settled cell: counted as one, they pulled every cell halfway to a field
twice as coarse, and a box's contact shadow measured 0.139 of the occlusion out where the cells' own means were
0.076 out.

### 3. The test for a change judges by the largest of three variances, and asks more of a rise where paths are few

The recent variance (ADR-0084), a variance over the mean's own history, and the parents' pooled variance, which
knows of the paths before the cell has caught one. A mean of few large samples is skewed: a rise must be further
out by its deviation over the light around (at most four times). A frame's share of the long variance is held to
what the light's noise could give, so that a change does not pass for noise and blind the test.

### 4. What a ray meets unseen gets a cell of its own, filled by rays that go on from it

Where the hit has no cell and its parent none, the update's ray goes on a bounce further for a sample of what the
hit receives, and adds it to a cell made at the hit (at the parent's level; a new one only in the first half of its
bucket, so that the frame's own cells are not crowded out). The next ray to land there finds a mean. A cell whose
last sample is 8 frames old gets one too. What the second bounce meets unseen is taken to receive what the first
does, or what the ray's own surface does. A hit also looks for its cell's parent before it counts as unseen.

Tried on the way: the light of the ray's own surface for every unseen hit (right on average under the roof, 36 %
out a pixel, and too bright near an opening); the ray going on with nothing stored (a third out from one tenth of a
second to the next: two chances taken in a row). The reflections' rays still take the sky for a hit the cache knows
nothing of.

## Measured

Truth tests (128 × 128 views; error is the mean distance from the truth a pixel, over the truth):

| | Before | Now |
|---|---|---|
| A box's occlusion of the sky | 0.076 | 0.049 |
| A sunlit wall's light on the ground | 0.110 | 0.087 |
| Under a roof, 18 % of the sky open | 5.6 % | 3.7 % |
| Under a walled roof, 2 % open | 36 %, 15 % too bright | 8.9 %, within 2 % |
| The same with surfaces reflecting 0.7 | eleven times the truth | within 8 %, 9 % a pixel |
| A doubled sky followed in 4 and 16 frames | 86 %, 99.9 % | 73 %, 97.5 % |

Bistro's arch walked back and forth at 4 m/s, the same pose at four passes (irradiance view, 2560 × 1440; the
path-traced reference, held still: left wall 0.0082, right 0.012):

| | 494ea99 | Now |
|---|---|---|
| Left wall's light | 0.0128 to 0.0135 | 0.0084 to 0.0087 |
| Left wall's mottling | 53 to 55 % | 12 to 17 % |
| Right wall's light | 0.0176 to 0.0190 | 0.0119 to 0.0125 |
| Right wall's mottling | 44 to 49 % | 9 to 10 % |

**Cost** (2560 × 1440, RTX 3060 Ti, the reference views, one round against ADR-0089's results): 0.36 to 0.73 ms a
view more, 0.53 on average: the filter 0.23 to 0.31 ms, the update 0.04 to 0.22 ms (the rays that go on), the
cells' resolve 0.01 to 0.02 ms, and the frame's resolve about 0.1 ms (the lookup's second try, and more cells to
find). The cache is 120 bytes a slot where it was 56 (30 MB at 2^18 slots). 1 % lows past the gate (6.94 ms,
ADR-0082): the city's rooftop 9.26 ms and park 7.21, Bistro's street turn 7.81, terrace 7.47 and street walk
6.99. Nothing is optimised yet; the budgets wait for the user's word.

## Alternatives considered

- **More rays.** Four times the rays halve the noise at four times the update's cost; the filter takes it to a
  fifth for 0.3 ms.
- **A screen-space denoiser** (Schied et al.'s SVGF): it filters what the pixels show, a frame at a time, and what
  the rays' hits read from the cache stays noisy; the cells' filter serves both.
- **Weights from each cell's own variance**: the dark bias above.
- **A fixed strength**: under a roof with 18 % of the sky open the filter then left the light 11.8 % out where the
  cells' own means were 5.8 % out.
- **The axis a surface faces most as a part of a cell's key**, to keep a floor's light out of the walls that share
  its octant: a street's walls stand at any angle, and one near 45 degrees to two axes broke into its bricks, each
  triangle's cells its own.
- **The parents counting for as long as they hold more samples than the cell**: more mottling on arrival, not less
  (their few hundred samples are as rough at twice the size).
- **A pyramid of all the coarser levels with longer histories**, read by significance: the principled way to both
  fast and smooth, and a larger change; later, if the reach of three cells proves too short.

## Consequences

- Shade is quiet while the camera moves, and holds the light the paths give: the walls under Bistro's arch are
  dark, as the reference has them, where the old means showed them half again as bright and mottled.
- **Slower from dark to bright where paths were few**: a cell that has learned to expect rare paths takes the sky
  appearing for one at first (under a card then cut away, 96 % of the way in 96 frames at the test's size, where
  the old test was there at once). A light that changes under an open sky is followed as before.
- New surfaces show their own first samples for a few frames (noise that settles in a tenth of a second or two),
  not a neighbour's light.
- A surface no frame has seen is lit by what rays found there, a cell at a time, and takes a second or so to
  settle the first time a view's rays reach it.
- `NphRenderView.gi_smooth` (on by default; the scene viewer's I) shows the cells' own means, for comparisons and
  for the tests of them.
- Light still crosses between a floor and a wall that shares its octant where their cells touch, unless their
  means differ by the test above.
- The filter, the rays that go on and the lookup's second try are 0.53 ms a view to win back: the neighbours'
  slots kept between frames instead of found again, cells stepped less often, fewer rays going on, the samples
  kept two levels up so that one lookup serves where four do now.
- Where the light changes steeply and the cells are new (a wall beside a window) the parents' cells show for some
  frames as soft blocks twice the cells' size.

## Tests

- `Gi.the_filter_keeps_the_light`, `..._of_few_paths`, `..._of_many_bounces`: the roofs above, against rays cast on
  the CPU (all bounces followed for the last): the filtered light's mean, its error against the cells' own, and
  the cells' light summed by their samples.
- `Gi.a_surface_entering_a_finer_level_keeps_its_light`: the new level's cells take their parents' light (within
  8 % the frame after the move).
- `Gi.the_cache_holds_the_cells_the_view_sees`, `VoxelGpu.the_radiance_cache_holds_the_voxels_cells`: every pixel's
  cell and its parent are there; the other slots are the hits' cells.
- The tests of ADR-0039, ADR-0084, ADR-0085 and ADR-0088 run through the filter.
