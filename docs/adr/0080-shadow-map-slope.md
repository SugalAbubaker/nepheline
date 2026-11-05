# 0080. The sun's map at the level of its slope on a surface, and a pool twice as large

- **Date:** 2026-11-04
- **Status:** Accepted

## Context

ADR-0076 found, and left: where the sun grazes a wall, the virtual shadow map's texels lie stretched across it, and a
cornice's or a ledge's shadow down the wall steps along its edge, which the sun's rays do not show (Bistro's hedge and
terrace). The contact shadows' march does not mend it: what is wrong is the map's texel, and a march only adds shadow.

A pixel takes the level whose texels match its footprint at its distance (Lefohn, Sengupta & Owens, "Resolution-Matched
Shadow Maps", ACM TOG 2007), as if every surface faced both the camera and the sun. A surface the sun lights at N.L has
a texel stretched across it by 1 / N.L; one the camera sees at N.V, a pixel stretched by 1 / N.V. Where the first is the
larger, the map is coarser on the surface than the image.

## Decision

1. **A level N.L / N.V times finer in area** (`vsm_slope`): the footprint's area scaled by that share, so that a texel's
   area on the surface is a pixel's. A surface seen as aslant as it is lit keeps the distance's level: the ground under a
   low sun, seen from a little above, needs nothing more. Never coarser than the distance's level, and at most one level
   finer: a second (down to 1/16) asked for four times the pages again and brought nothing more (below).
2. **The requests take it from each 2 x 2 block's plane** (`vsm_block_slope`), the four points the depth pyramid
   rebuilds already: where they lie on one plane within a quarter of the block's extent; a block across an edge, or with
   background in it, keeps the distance's level. A pixel requesting on its own takes its block's.
3. **The shading takes it from the surface's geometric normal**, starts at that level and passes to coarser ones where
   the page is not (a block not flat, a blended surface's request, which has no normal); its three tries count from the
   distance's level, as before, so a finer level is only ever tried over them.
4. **The pool holds 4,096 pages by default** (`NphRendererDesc.vsm_pages`, 268 MB, was 2,048 and 134 MB): the reference
   views ask for up to two and a half times the pages (Bistro's street 1,538 where it asked 617, the city's park 1,917),
   and of 2,048 the park would take all but 131, where an overflow raises the resolution bias, every level coarser.

## What it brings

Against the sun's rays, 64 frames averaged, native 4K: pixels too bright by over 16 levels (the light a shadow misses),
by over 48, and too dark by over 16.

| View | Before | One level finer at most | Two (not taken) |
|---|---|---|---|
| Bistro `street` | 2.29 %, 0.69 %, 0.42 % | **1.74 %, 0.51 %, 0.33 %** | 2.33 %, 0.75 %, 0.44 % (the pool overflowed) |
| Bistro `terrace` | 1.39 %, 0.53 %, 0.35 % | **1.09 %, 0.39 %, 0.26 %** | 1.12 %, 0.39 %, 0.34 % |
| Bistro `hedge` | 1.42 %, 0.71 %, 0.38 % | **1.08 %, 0.50 %, 0.32 %** | 1.11 %, 0.51 %, 0.31 % |

The ivy's shadows on the terrace's wall, stepped before, are its leaves'.

## What it costs

Frame in ms, mean, native 4K on the RTX 3060 Ti, Profile, against the quality tiers' build (ADR-0079), interleaved, two
rounds of 240 frames a scene, a scene a process (2026-11-04); the columns, what this adds:

| View | Before | With this | resolve | `vis hzb` (requests) | the map's passes |
|---|---|---|---|---|---|
| Bistro `street` | 7.51 | **7.59** (+0.08) | +0.044 | +0.010 | +0.001 |
| Bistro `street_walk` | 7.62 | **7.72** (+0.10) | +0.046 | +0.011 | +0.024 |
| Bistro `street_turn` | 8.02 | **8.13** (+0.11) | +0.051 | +0.009 | +0.038 |
| Bistro `terrace` | 8.16 | **8.27** (+0.11) | +0.072 | +0.010 | +0.001 |
| Bistro `hedge` | 7.74 | **7.82** (+0.08) | +0.048 | +0.009 | +0.001 |
| City `street` | 6.61 | **6.65** (+0.03) | +0.020 | +0.007 | +0.000 |
| City `street_walk` | 6.70 | **6.76** (+0.06) | +0.021 | +0.007 | +0.030 |
| City `street_sprint` | 6.87 | **6.97** (+0.10) | +0.022 | +0.008 | +0.062 |
| City `street_turn` | 6.80 | **6.88** (+0.07) | +0.029 | +0.003 | +0.044 |
| City `crossing` | 6.75 | **6.81** (+0.06) | +0.037 | +0.002 | +0.001 |
| City `park` | 7.87 | **7.93** (+0.05) | +0.036 | +0.011 | -0.000 |
| City `rooftop` | 10.03 | **10.04** (+0.01) | +0.011 | +0.003 | -0.000 |
| Interior `floor` | 7.18 | **7.18** (+0.00) | +0.004 | +0.001 | +0.000 |
| Interior `floor_walk` | 7.19 | **7.19** (+0.00) | +0.002 | +0.001 | +0.000 |
| Interior `floor_turn` | 6.72 | **6.72** (-0.00) | -0.001 | +0.000 | +0.000 |
| Interior `gallery` | 7.29 | **7.30** (+0.00) | +0.001 | +0.001 | +0.000 |
| Interior `among` | 7.14 | **7.14** (+0.00) | +0.001 | +0.001 | +0.000 |
| Interior `upward` | 6.25 | **6.25** (+0.00) | +0.003 | +0.000 | +0.000 |

The resolve pays most of it, 0.04 to 0.07 ms on Bistro, where the walls the sun grazes read pages a level finer, whose
texels its neighbours share less; the depth pyramid's requests 0.01 ms for the blocks' planes; the map's passes only
where the camera moves and new pages are drawn, 0.02 to 0.06 ms. The interior has no sun, and nothing of it changes.

**With the sun moving every frame**, every page drawn again each frame (ADR-0033), the map draws as many more pages as the
views ask for: `lod_shadows_sun_4k` 6.35 ms where it was 5.24, and `foliage_shadows_sun_4k` 24.93 where it was 20.40. That a day-night cycle redraws every page is ADR-0060's and ADR-0061's open problem, "a day-night cycle needs
its pages kept across small turns of the sun"; this makes it larger by as much as the pages. Where the sun stands still or
the camera walks under it, the synthetic scenes move by 0.01 to 0.06 ms, and no budget of theirs moves.

**DRAM**, as the render graph declares it (whole resources, an upper bound), grows by the pool's 134 MB in each pass that
declares it: Bistro's street 5,367 MB where it was 4,829, `vsm raster` writing 268 MB where it wrote 134, the resolve and
the blended surfaces reading 134 MB more each. What the frame touches is the pages its pixels read, a level finer on the
walls the sun grazes, and the pages drawn where the camera moves: up to twice as many. Device memory: the pool, 134 MB
more.

## Alternatives

- **Two levels finer at most** (N.L / N.V down to 1/16): four times the pages again where the sun grazes hardest (Bistro's
  street overflowed the pool of 2,048 with them), and on the terrace and the hedge the same errors as one level.
- **N.L alone**, without the view's slope: the ground under a low sun, seen as aslant, took a level finer everywhere for
  nothing (twice the pages in the test's view, 77 against 37).
- **A longer march** for the contact shadows (ADR-0076 measured the reach): it adds shadow where the map's is too short,
  not where the map's edge is stepped.
- **Rays for the sun** (ADR-0079's high tier): the truth itself, at 0.4 to 4.4 ms; for GPUs with rays to spare.

## Consequences

- The pool takes 134 MB more device memory, and so does the voxel casters' depths image, the pool's size, where a scene
  has voxel meshes. Bistro 4.67 GB to 4.80, the city 3.23 to 3.37.
- Contact shadows find less to do on surfaces the sun grazes: the tests that need the map to miss a caster now take a bar
  1.5 cm tall, the map's lift at the finer level.
- `Contact.a_wall_the_sun_grazes_takes_a_finer_map` (the distance's level alone missed 13.7 % of the ledge's shadow
  and darkened 29 lit pixels; the slope's, 6.0 % and none), `Contact.the_ground_seen_as_aslant_as_lit_keeps_its_level`
  (37 pages; without the view's slope 77). Of 8 planted faults, 6 are caught (the block's flatness and its background
  through `Vsm.block_requests_are_each_pixels`, which states the rule again rather than taking it from the code); one is
  the same program (without the background's test, a block's points are not finite and its normal's comparison already
  gives the distance's level); and one no test reaches (the shading's tries counted from the finer level: it differs only
  where neither the distance's level nor the next holds the page).
