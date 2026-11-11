# 0089. Smooth surfaces reflect by a ray a pixel, gathered with their neighbours'

- **Date:** 2026-11-11
- **Status:** Accepted (the user, 2026-11-11, after switching between the two in the viewer: "The ray per pixel was
  better for reflections, so we will make that the default. Re-base the reflection budgets"; their report before it:
  "The glass on the building next to the park also has horrible noise", and "everytime I move the model/textures
  around me (especially on buildings) is constantly changing")

## Context

ADR-0077 gave smooth surfaces (a roughness under 0.25) reflection cells: a world-space table keyed by where a
surface is and which way its mirror direction points, filled by rays that each tile of the screen traces one frame
in 32. ADR-0086 made a pixel's cell the same every frame and ADR-0087 refilled the cells a moving camera asks for.
Still views became quiet. Walked through at the viewer's speed (4 m/s), with each frame of a recorded path captured,
the glass was wrong in a way no filling fixes:

- **A cell is a dozen pixels across and a direction's.** What a pane of glass shows is a mosaic of them: the tree
  across the street as blocks of light and dark. The mosaic is of the world and of the mirror direction, and the
  mirror direction of every pixel changes with every step of the camera: the blocks rearrange as it moves. Windows
  cover a city's buildings, and this was much of what the user saw changing on them.
- **The table fills.** Walking through Bistro's arch at 4 m/s dropped 25 million rays in 240 frames for want of a
  slot (ADR-0087 had measured 496,000 at 1.4 m/s in the city and named it as what was left).

The renderer already had the other way, `NPH_RENDER_REFLECTIONS_RAYS`, as the high tier's (ADR-0079): a ray from
every smooth pixel every frame, added straight to the image. It has no cells to see or to fill. Its fault was grain:
one ray is one sample of the surface's lobe, a different one each frame, and wherever the lobe is wider than a pixel
of what it shows (the blurred edge of a reflected tree on glass of roughness 0.2) a pixel's value is that one
sample's. A still view's temporal resolve averaged it; a moving one's did not.

## Decision

**Smooth surfaces take a ray a pixel every frame by default, and each pixel's is gathered with its neighbours' on
the same surface** (`NPH_RENDER_REFLECTIONS_RAYS` is `nph_render_view_default`'s, the reference tier's and
`r.reflections`' default; the cells stay as `NPH_RENDER_REFLECTIONS_CACHE`).

- **The rays** (`reflect_rays.comp`) leave each smooth pixel's light, pre-exposed, in an image of their own with how
  far the ray went and how far the pixel is (`gi_reflect_light_pack`: 9-bit mantissas under a shared exponent, two
  half floats; 8 bytes a pixel), and a zero for every other pixel of the tiles they visit.
- **The gather** (`reflect_gather.comp`, the frame's `reflect` pass) takes, for each smooth pixel, the mean of its
  own ray's light and of those around it, Gaussian weighted, and adds it tinted by the pixel's own surface. The
  pixels around it sampled the same lobe from beside it, so their light is the same reflection moved by their
  distance: more samples, at the price of a blur as wide as they reach. The reach is therefore half the lobe's own
  blur on the screen, `2 a d / ((v + d) pixel)` for `a` the roughness squared, a ray that went `d` and a pixel `v`
  away, and at most 2 pixels.
  - A mirror (`a` near 0) takes its own ray alone and stays sharp to the pixel; so does what touches the surface
    (`d` near 0), which the lobe does not blur either.
  - Only the light of pixels on the pixel's own plane counts (where a neighbour's ray meets the plane against how
    far that neighbour is, within 1 %), so an object's reflection does not cross its outline.
  - Only the light of tiles listed this frame counts: a tile not listed may hold what it left frames ago.
  - The tile's light and a border of 2 texels come into shared memory once: 144 fetches a tile, not 25 a pixel.
- **The lobe's samples** come from interleaved gradient noise moved each frame (Jimenez 2014), whose neighbours
  differ evenly, so that a few together are near the lobe's mean.
- **What a ray meets is textured at the width the ray stands for there** (`hit_radiance`'s `footprint`: the pixel's
  width carried along the ray, or the lobe's where wider), not at the radiance cache's cell's size, which blurred
  what mirrors showed.

## Measured

2560 x 1440, RTX 3060 Ti. Two frames of a view that does not move, without a temporal resolve, on a ground of
roughness 0.22 mirroring a sunlit wall's edge: they differ by 24.7 % of the ground's mean a pixel with each pixel's
own ray alone, and by 4.5 % gathered (`Reflect.a_ray_a_pixel_is_gathered_with_its_neighbours`).

The reference scenes' views, the mean of two rounds against the cells (ms):

| Scene | Frame with cells | A ray a pixel | `reflect rays` | The gather |
|---|---|---|---|---|
| City, six views | 3.98 to 5.10 | 4.52 to 5.70 | 0.64 to 0.93 | 0.11 to 0.20 |
| City, rooftop | 6.08 | 7.92 | 2.15 | 0.15 |
| Bistro | 4.33 to 4.82 | 5.10 to 6.14 | 0.68 to 1.49 | 0.12 to 0.21 |
| Interior | 3.53 to 4.10 | 3.55 to 4.12 | 0.02 to 0.12 | 0.01 to 0.05 |

**Two views are past the gate** (1 % low under 6.94 ms, ADR-0082): the city's rooftop, whose view is a skyline of
glass, at 9.06 ms, and Bistro's street turn at 7.01; Bistro's terrace is at it (6.94). The user chose the picture
first ("we have to focus on getting the lighting, shadows, and level of detail working correctly before worrying
about optimization"). On their word the entries the two rounds' mean moved 1.5 % or more and to within 3 % of their
budgets are re-based, 54 in all (`reflect_rays` 13, `reflect` 9, the frame's `gpu` and `gpu.span` 16 each), from
`benchmarks/results/reference_scenes-2026-11-11-reflect-rays-win-msvc-profile-rtx3060ti.json` and its second round.

## Alternatives considered

- **A larger table, or evicting cells** (ADR-0087's "next"): it would stop the drops, and leave the mosaic.
- **Blending the cells around a pixel**, as ADR-0085 does for irradiance: a reflection is as sharp as its surface is
  smooth, and four cells a dozen pixels across blend to a blur a mirror must not have; the cells would have to
  shrink to a pixel, which is a ray a pixel with a table in the way.
- **Screen-space reflections first, rays where they miss**: most of what the city's glass reflects is behind the
  camera or outside the view, the cases that fall back to rays, and the two sources differ at their seam.
- **Rays from a share of the pixels each frame, the rest carried over by reprojection**: the way to most of the
  cost back, and what the later speed work should take up. It needs the reflection's own motion (the reflected
  point's, not the surface's) to carry a pixel's light to where it is next frame without smearing; not before the
  picture is right.
- **The sun's shadow at what a ray meets from the sun's shadow map**, where the map holds the page, instead of a
  second ray: no change to the picture and a second ray fewer for most hits; left for the speed work with the one
  above.
- **A wider gather, or one that follows each neighbour's ray to weigh it by this pixel's lobe** (Stachowiak,
  "Stochastic Screen-Space Reflections", SIGGRAPH 2015): fewer frames of grain left on the roughest of the smooth
  surfaces, for more fetches a pixel. Two pixels' reach took the grain to a fifth; the temporal resolve has the
  rest.

## Consequences

- Glass and polished surfaces show what is there, to the pixel where they are mirrors, and it does not rearrange as
  the camera moves. The reflection table is not used by default: nothing fills, and nothing is dropped.
- The frame costs 0.5 to 1.9 ms more at 2560 x 1440 where much of the view is smooth, and two reference views are
  past the gate until that is won back. The quality tiers no longer differ in their reflections (ADR-0079's table).
- 8 bytes a pixel more for the rays' light (29.5 MB at 2560 x 1440, 66 MB at 4K), made with the first frame that
  takes rays.
- What a ray meets is still lit by the radiance cache's cells (`gi_hit.glsl`), whose blotches show in mirrors as
  they do on walls: the indirect light's own work.
- Surfaces of roughness 0.25 and up still reflect only what their irradiance cell gathered, as if it arrived
  evenly: a polished floor of roughness 0.3 has no image in it. Raising the limit needs the gather to reach
  further; later.
- Blended surfaces start no ray, as before.

## Tests

- `Reflect.a_mirror_by_a_ray_a_pixel_stays_sharp`: of the mirror's pixels whose rays pass by the wall's top edge,
  none lies between the wall's red and the sky's black (gathered two pixels out regardless, a sixth do).
- `Reflect.a_ray_a_pixel_is_gathered_with_its_neighbours`: two frames differ by under 9 % of the mean a pixel
  (4.5 %; ungathered 24.7 %).
- `Reflect.a_ray_a_pixel_gathers_on_its_own_surface`: a plate over the ground, the ground beside it mirroring the
  wall; the plate's sides are no brighter than its middle (without the plane's test, by a tenth of the contrast).
- Three planted faults, each caught by its test: no gather; the full reach for every pixel; no plane's test.
- `Reflect.a_cut_away_surface_is_not_reflected` (ADR-0088) and the tests of ADR-0077 that ask for a ray a pixel run
  through the gather as well.
