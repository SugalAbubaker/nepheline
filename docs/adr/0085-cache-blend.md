# 0085. The radiance cache's lookup blends the cells around a point

- **Date:** 2026-11-10
- **Status:** Accepted (the user's go-ahead of 2026-11-10: "start step 2")

## Context

The second part of the city's static (ADR-0084's context) is the cache's
lookup. Each frame under temporal anti-aliasing, a pixel looked its light up
at a random point of its cell in the surface's plane (Binder et al.'s jittered
hashing, ADR-0039), so that the cells' edges blended over frames instead of
showing. A pixel then read one of up to four cells each frame, and their
differences became noise for TAA to average. TAA keeps about 16 frames of
history still and 4 moving, and clips it to the neighbourhood's mean and one
deviation, which this noise often defeats: its share of the street's static
was about 6 % (ADR-0084), the same with a 255-sample history, so it comes from
the cells' real differences, not their noise. A per-pixel offset moved along
R2 each frame, which spreads a pixel's points evenly in time for TAA to
average, barely helped (the street's noise 9.3 → 8.9 %).

## Decision

**The lookup blends the four cells around the point** (`gi_surface_blend`):

1. Across the axis the surface's normal faces most, the four cells whose
   centres surround the point (half a cell out along the normal, as the keys
   take it), weighted bilinearly by where it lies between them. Each is
   taken in the layer of cells the surface's plane crosses at its centre, so
   a wall or a floor blends its cells exactly and a slope (whose normal is at
   least 1/sqrt 3 along that axis) near enough.
2. Each cell's weight is also its trust. A cell the cache does not hold (past
   the surface's edge, or a part of it no sample has reached) drops out, its
   weight shared among the others, so an edge does not fade toward the sky.
   The blend's trust is the found cells' weighted trust, and the
   environment's irradiance makes up the rest, as it did for one cell.
3. The many lights' far fields' share is blended too, weighted by each
   cell's light.
4. Every pixel takes it, under temporal anti-aliasing or not; the frame's
   `jitter` flag remains the reflection cells' (ADR-0077).
5. The blend has a CPU twin, `render_gi_blend`, as the keys have.

## Alternatives considered

- **The jitter, spread over time** (a per-pixel offset moved along R2): the
  street's noise 9.3 → 8.9 %. TAA's clipping, not the jitter's distribution,
  is what lets it through.
- **No jitter, the one cell holding the point**: no noise, but each cell's
  light a block with edges.
- **Eight cells, trilinear in three dimensions**: twice the lookups, most of
  them empty for a surface (cells hold surfaces, not volumes).
- **Making the four lookups cheaper** (the city street's resolve, 925 µs with
  the jitter):
  - The first form indexed the turned axes at run time (`x[i]`): the
    compiler put those vectors in local memory, and the resolve took 72
    registers where it had 64: 1,307 µs. Swizzles chosen by the axis
    (`x.yzx`) brought it to 1,142 µs at 64 registers.
  - Where the cost is, by ablation: one lookup with the blend's arithmetic
    952 µs, four keys made and hashed with one lookup 1,079 µs. Making and
    hashing a corner's key (its plane's layer, the turned cell, the 64-bit
    key) costs more than reading its cell.
  - pcg3d (Jarzynski & Olano, JCGT 2020) of the key's two words for the
    table's hash, 32-bit multiplies where MurmurHash3's 64-bit finaliser took
    64-bit ones: 1,129 µs. Kept (`gi_mix` and its CPU twin).
  - Lookups shared across a subgroup whose pixels lie on one plane (each
    invocation looks up one cell, each pixel takes its four by shuffles):
    1,245 µs, the resolve at 72 registers again. Not kept.
  - The blend in helper functions in place of one loop: 1,165 µs, more of the
    resolve spilled. Not kept.
  - Not tried: a pass over the live cells (some 150,000, against 3.7 million
    pixels) storing what a pixel's blend needs, so a pixel looks one cell up.
    Its cost is the cells' and its axes' (a cell may hold walls and floors
    alike); it is for the speed work toward 240 FPS.

## Consequences

- **Noise**, after 600 still frames (`render_perf --accumulate=64`, the
  history of ADR-0084 in both):

  | View | Noise, jitter → blend | Pixels over 10 % | Dark pixels' wander |
  |---|---|---|---|
  | `city_street` | 6.8 → 4.3 % | 16.8 → 11.0 % | 6.6 → 4.0 % |
  | `city_park` | 6.8 → 4.5 % | 19.3 → 12.4 % | 15.2 → 8.7 % |
  | `city_crossing` | 4.5 → 3.1 % | 13.6 → 9.8 % | 9.3 → 6.1 % |
  | `city_rooftop` | 3.7 → 2.6 % | 12.7 → 8.6 % | 6.4 → 4.6 % |

  Against the 128-sample history and the jitter both, the street's noise is
  half (8.8 → 4.3 %), and the park's foliage, whose leaves' cells differ the
  most, lost most of its speckle.
- **Accuracy** against the ray-cast truths (`test_gi.c`): the boxes'
  occlusion's error 0.102 → 0.076, the sunlit wall's bounce 0.142 → 0.110:
  a point's light is no longer its cell's average but its neighbourhood's
  blend.
- **Levels**: the four cells are the point's level's. Where a surface
  crosses into the next level (twice the distance, cells twice the size),
  the blend switches levels, where the jitter had dithered them; the
  street's irradiance view at 2560 x 1440 shows no seam there.
- **The light leak the jitter had**: a jittered point that fell where the
  cache held no cell (past an edge) took the environment's irradiance, the
  open sky's; the blend leaves such cells out. The shade is a little darker
  for it at edges (the street's pixels below 8 of 255: 36.9 → 38.0 %).
- **The cost** (RTX 3060 Ti, two rounds interleaved with #170's build,
  `render_perf`, 240 frames a view), in the resolve, which blends four cells
  a pixel where it read one (more for scenes whose small surfaces leave
  corners missing, which scan their whole bucket):

  | Scenes | Resolve | Frame |
  |---|---|---|
  | The city, 2560 × 1440 (7 views) | +162 to +269 µs (+21 to +28 %) | +0.20 to +0.28 ms |
  | Bistro, 2560 × 1440 (5 views) | +474 to +536 µs (+40 to +42 %) | +0.48 to +0.55 ms |
  | The interior, 2560 × 1440 (6 views) | +272 to +340 µs (+26 to +30 %) | +0.32 to +0.40 ms |
  | Synthetic, 3840 × 2160 | +230 to +359 µs | +0.24 to +0.36 ms |

  Every reference view still clears the 6.94 ms gate (ADR-0082): Bistro's
  slowest, the terrace, 4.06 → 4.60 ms on average. The user chose the blend at
  this cost (2026-11-11: "Keep the blend, open the PR"), its faster form left
  for the speed work. DRAM: each pixel reads four buckets (64 bytes) and four
  cells (16) of a table of at most 5 MB, which neighbouring pixels share, so
  most of it comes from L2; not traced. Budgets re-based: the resolve's
  entries, the transparent pass's (its blended surfaces blend too) and the
  frames' where the blend moved them 1.5 % or more and to within 3 % of their
  budgets (`reference_budgets.txt` 58, `render_budgets.txt` 33).
- **Tests**: `a_pixel_blends_the_cells_around_its_point` (`test_gi.c`): each
  pixel of the irradiance view against the CPU twin's blend from the table's
  own fingerprints and cells: 15,404 pixels, all within a level of 8-bit
  rounding. It fails for the one cell holding the point (the worst pixel 15
  levels off), for the bilinear weights swapped (23), and for a missing cell
  taken as distrust (25, edges brightening toward the sky).
