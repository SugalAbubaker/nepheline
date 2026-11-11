# 0086. A smooth pixel's reflection cell is the same every frame

- **Date:** 2026-11-10
- **Status:** Accepted (the user's go-ahead of 2026-11-10: "start step 3")

## Context

The third part of the city's static (ADR-0084, ADR-0085) is in what smooth
surfaces reflect: the shop windows' glass. ADR-0077's reflection cells hold
what a surface reflects by place and by mirror direction, and the `reflect`
pass looks a smooth pixel's cell up each frame. Against a ray a pixel every
frame (`NPH_RENDER_REFLECTIONS_RAYS`, the truth for these surfaces: what the
cells are meant to converge to), three faults showed, each a way in which a
pixel's lookup missed the cell its rays had filled:

1. **A random neighbouring direction.** Under temporal anti-aliasing the
   lookup took a random neighbouring direction bin each frame, so that the
   bins' edges would blend over frames. Across a flat surface a mirror
   direction changes slowly: all of a cell's rays fall in one bin, and the
   neighbouring bin's cell holds no ray of this surface. The pick left its
   bin on 44 % of the frames (a quarter on each axis) and fell back on the
   sky, no brighter than the irradiance cell. The city's windows showed
   blocks of a sky they do not see, flickering.
2. **One jitter offset in eight.** A tile's rays come once in 32 frames, and
   the temporal jitter's sequence is 8 frames long, so a tile always traces
   at the same one of its offsets. Where a reflection cell is smaller than
   what a pixel covers (a surface seen at a grazing angle), the other seven
   offsets' points fall in cells no ray fills.
3. **The cells' faces.** The key took the point on the surface. A floor at
   y = 0 lies on the cells' faces (the grid's powers of two align with it),
   so its points fall either side of them as their rounding goes, each
   frame's jitter rounding them anew. The irradiance's keys take their point
   half a cell out along the normal for this reason (`gi_key`); the
   reflection's did not.

The third and second show most at the tests' 128 pixels, where a reflection
cell is smaller than a pixel: there a mirror under temporal anti-aliasing
showed a quarter of its reflection's brightness and changed by 21 levels from
one frame to the next.

## Decision

1. **The lookup takes the pixel's own direction's cell, every frame.** The
   frame's `jitter` flag goes from `NphRenderGiView`; its word and the old
   padding's become `pad_reflect[2]`, so the levels' vectors stay on 16 bytes
   (a static assertion in `gi.c` keeps them there: 8 bytes fewer, and the
   resolve, which reads the levels in each of its blend's lookups, ran 1 to
   3 % slower).
2. **Both passes key a smooth pixel's cell the same way, whatever the frame's
   jitter** (`reflect_key`, `reflect_common.glsl`):
   - the point where the ray through the pixel's centre, without the jitter,
     meets the surface's plane. Depth (reverse Z, a reciprocal of the
     distance) is affine across a plane on the screen, so the plane comes
     from the depths of the pixel's four neighbours: both sides of an axis
     must change alike (within a tenth) and by no more than a twentieth of
     the depth a pixel. Where they do not (an edge, foliage, whose
     neighbours lie on other leaves at much the same depth), the pixel's own
     point;
   - the facing: that plane's normal, the surface's own, which neither a
     normal map nor a mesh's smoothed normals turn (elsewhere, the shading
     normal);
   - the mirror direction: that ray's, about the shading normal.
3. **The key takes its point half a cell out along the facing**, as the
   irradiance's keys do.

## Alternatives considered

- **The four nearest direction bins blended** (as ADR-0085 blends cells): on
  a flat surface the neighbouring bins hold none of its rays, so the blend
  would fall back as the pick did. It would need the rays to fill them too
  (each sample splatted into its neighbouring bins), and would blur the
  reflection by a bin.
- **The plane through the pixel's point, facing its shading normal**: on
  Bistro's normal-mapped cobbles and the interior's smooth-shaded statues it
  is not the surface's plane, and the key moved with the jitter (the
  gallery's image 6.8 levels from its truth where it had been 3.5).
- **The plane from whichever side changes least**, bounded only by the
  twentieth: leaves on other leaves at much the same depth gave false planes,
  and the park's canopy moved from its truth.
- **One side alone where the other is an edge** (two neighbours a side, a
  side taken when its two steps agree): an edge's pixel took a plane that its
  centre's ray may not meet (the city's crossing 1.4 levels from its truth on
  average, against 2.7).
- **The tiles' turns rotated through the jitter's offsets**: each offset's
  cell is filled one turn in eight, and a pixel reads a different cell each
  frame: noise for the temporal resolve, which is what this removes.

## Consequences

- **Accuracy and noise where reflections are drawn**: the pixels under the
  reflection cells' roughness limit (`render_perf --debug-view=roughness`),
  each build against a truth of its own (`--reflections=2`), 600 still frames,
  the last 64 averaged (`--accumulate=64`), 2560 × 1440, RTX 3060 Ti. The
  error is the mean's luma from the truth's, in levels of 255; the noise the
  last frame's from the mean, as a share of it:

  | View | Smooth pixels | Error, before → after | Noise, before → after |
  |---|---|---|---|
  | `city_street` | 7.9 % | 8.54 → **1.06** | 9.1 → **5.2 %** |
  | `city_crossing` | 10.4 % | 12.45 → **1.01** | 8.7 → **3.0 %** |
  | `city_park` | 4.5 % | 8.03 → **1.69** | 5.7 → **3.4 %** |
  | `city_rooftop` | 7.3 % | 5.88 → **1.68** | 8.0 → **7.5 %** |
  | Bistro `street` | 10.6 % | 7.69 → **6.48** | 3.3 → 3.3 % |
  | Bistro `terrace` | 11.8 % | 8.73 → **6.84** | 4.5 → **4.1 %** |
  | Bistro `hedge` | 7.1 % | 3.60 → 4.24 | 2.2 → 2.2 % |
  | The interior's `floor` | 0.4 % | 8.38 → **0.66** | 6.2 → **5.9 %** |
  | The interior's `gallery` | 0.5 % | 9.04 → **2.29** | 5.2 → 5.3 % |

  The whole image's noise (render_perf's line): the city's street 4.2 →
  3.7 %, crossing 3.1 → 2.2 %, park 4.5 → 4.2 %, rooftop 2.7 → 2.5 %; Bistro's
  terrace 4.8 → 4.5 %; the rest within 0.2 %.
- **The measure's resolution.** The irradiance cache's slow wander gives each
  build, and each mode, a realization of its own: between two builds that
  differ by a padding word, the smooth pixels' error moved by up to 1.2
  levels (Bistro's hedge 2.8 against 4.0). Differences below that, such as
  the hedge's, are not this change's. What is left in Bistro (6 to 7 levels,
  bright) is ADR-0077's: surfaces of roughness 0.2 to 0.25 fade toward what
  their irradiance cell has gathered, over pi, which is brighter than their
  lobe sees where buildings hide the sky.
- **The cost** (two rounds interleaved with #171's build, 240 frames a view):
  `reflect` reads four more depths a smooth pixel and no longer makes a random
  number: the city −7 to +1 µs, Bistro 0 to +7 µs, the interior −1 to +1 µs;
  `reflect rays` +0.4 to +1.5 µs. Every frame is within its noise (the city
  −7 to +7 µs, Bistro −8 to +10, the interior −6 to +3, the 4K synthetic
  scenes −24 to +13); no budget moved. DRAM: the four depths (8 bytes each)
  are the neighbours' own, read by the same tile's invocations, so mostly
  from L1; at most 32 bytes a smooth pixel, 12 MB at 2560 × 1440 were all
  missed.
- **Tests**: `a_mirror_under_temporal_anti_aliasing_shows_its_own_cells`
  (`test_reflections.c`): ADR-0077's mirror and red wall at 128 pixels,
  settled through the temporal resolve, against the same view without it.
  The wall's reflection is 0.690 red with it and 0.676 without, moves 0.50
  levels from a frame to the next, and the frames without the jitter add 44
  cells to 9,981 (before: 0.18 and 0.19, 21 levels). Six planted faults, each
  caught by it alone: the random pick back (0.43 red against 0.68), no plane
  (24 levels a frame, 969 new cells), the jitter undone the wrong way (12.8
  levels), the plane's depth ignored (33 levels), the point on the cells'
  faces (2.1 levels, 282 new cells), and the rays keyed by the jittered point
  while the lookup anchors (0.20 red).
- **Left**: the foliage's static, which is the cache's light on leaves (the
  park's canopy 3.2 levels a pixel from frame to frame, 2.1 with the cache
  off) and the temporal resolve's on fine geometry (1.7 with every effect
  off); and Bistro's bright roughness fade, which is a bias, not noise.
