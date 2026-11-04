# 0078. Local lights' shadows: lamps as many lights, glass and their shells out of the rays

- **Date:** 2026-11-03
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 6 asks for the local lights' shadows, and ADR-0041 put them in Phase 14, to come from the many-light
sampling: "Phase 14's many-light sampling traces a shadow ray toward each light it samples". An ordinary light
(`NphRenderLight`) has none unless its `shadows` is set, which traces a ray a pixel and light in the resolve. Bistro's 96
lamps were ordinary lights without them, and their light went through walls.

At night (`render_perf --no-sun`, the sky off, EV 4) against the truth, the same lamps with a shadow ray each
(`--local-shadows`), the lamps without shadows were too bright by over 16 levels on 16 to 27 % of the pixels, 8.7 to 22.5
levels on average.

Made many lights, Bistro went almost black, and so did the truth: 95 of the 96 lamps sit inside meshes rays could not pass.
Their bulbs, glowing shells around the light, and their lanterns' panes, glass that the glTF marks as letting light through
(`KHR_materials_transmission`) and the cooker took as opaque. Every shadow ray toward a lamp met its own bulb or glass.

## Decision

1. **The cooker keeps glass and lights' shells out of the rays** (docs/cook.md "Rays"): a mesh every material of which lets
   at least half the light through (`KHR_materials_transmission`, read for this alone), and a mesh every material of which
   glows, the box of one of whose instances holds a point or spot light. In rays, glass would stop the light it passes, and a
   shell its own light, which it would also count twice, as the light and as the glow the rays see (as the interior's
   candles' flames, ADR-0073). Bistro has 42 such meshes of glass and 35 shells, around 95 of its lamps; the city and the
   interior none.
2. **Bistro's lamps are many lights** (its manifest's `light_near_range` 0.25 and `many_lights` 128), as the interior's
   candles: their far fields shadowed by the cache's rays, their highlights by the reservoirs' (ADR-0073, ADR-0074).
   Ordinary lights are left as they are, without shadows unless asked, for lights that need none.
3. **The cache counts each cell's share of the many lights' far fields,** in the cell's last word beside its count and age
   (8 bits each now: they are at most 128 and 240), from the update's samples of the far fields summed apart. Rough
   surfaces' specular takes the rest of the cell where reservoirs give the far fields' highlights, and all of it where they
   do not (`NPH_RENDER_RESTIR_OFF`, or `ALL`, whose far fields the cache never holds). This mends what reflections
   (ADR-0077) had broken: in a frame with many lights, rough surfaces took none of the cell, and fetched no sky. With
   Bistro's lamps as many lights, its day went 6.7 levels darker on average for that alone.

## What it does

Night, 64 frames averaged, against the lamps with a shadow ray each. Pixels too bright by over 16 levels, too dark by over
16, and the mean difference (positive: darker):

| View | Lamps without shadows | Lamps as many lights |
|---|---|---|
| Bistro `street` | 16.0 %, 0.0 %, -8.7 levels | **2.6 %, 4.4 %, +1.3 levels** |
| Bistro `terrace` | 19.4 %, 0.0 %, -12.3 levels | **2.0 %, 3.6 %, +1.0 levels** |
| Bistro `hedge` | 26.7 %, 0.0 %, -22.5 levels | **2.5 %, 3.2 %, -0.1 levels** |

What is left is the cache's: its cells are up to a dozen pixels across, so a shadow's edge blurs by one, and a light's far
field changes from frame to frame (a pixel 18 % on average at night, against 3 % for lights the resolve shades itself),
which the temporal resolve smooths. Reservoirs for all of the light (`NPH_RENDER_RESTIR_ALL`) are sharper and steadier on
the street (0.5 % too bright, 5.3 % too dark, a pixel 5.6 % from frame to frame), for the tiers.

By day the lamps are 0.2 % of the sun's light: as many lights instead of ordinary ones, Bistro's street moves 0.21 levels
on average, 0.13 % of its pixels by over 16. The glass out of the rays moves it 2.5 levels darker on average (13 % of the
pixels by over 4, 0.6 % by over 16): the cache's rays saw sunlit panes as white and diffuse, a light the street does not
get from glass, which reflects a twentieth of it as a mirror would.

## What it costs

Frame in ms, mean, against the reflections' build (ADR-0077, #151's head), interleaved, two rounds of 240 frames a scene, a
scene a process, native 4K on the RTX 3060 Ti (2026-11-04); the passes' columns are this build's, or what it adds:

| View | Before | With this | `many turns` + `many tiles` | resolve | `gi update` | `gi resolve` |
|---|---|---|---|---|---|---|
| Bistro `street` | 7.35 | **7.50** (+0.15) | 0.25 + 0.02 | -0.068 | +0.010 | +0.003 |
| Bistro `street_walk` | 7.43 | **7.58** (+0.15) | 0.27 + 0.02 | -0.082 | +0.011 | +0.003 |
| Bistro `street_turn` | 7.60 | **7.98** (+0.37) | 0.34 + 0.02 | +0.059 | +0.008 | +0.003 |
| Bistro `terrace` | 7.92 | **8.13** (+0.21) | 0.25 + 0.02 | +0.027 | +0.004 | +0.003 |
| Bistro `hedge` | 7.40 | **7.71** (+0.32) | 0.23 + 0.02 | +0.077 | +0.009 | +0.003 |
| City `street` | 6.59 | **6.59** (+0.01) | none | +0.001 | +0.001 | +0.000 |
| City `street_walk` | 6.68 | **6.69** (+0.01) | none | +0.004 | +0.001 | +0.000 |
| City `street_sprint` | 6.86 | **6.86** (+0.00) | none | +0.002 | +0.001 | +0.000 |
| City `street_turn` | 6.78 | **6.78** (+0.00) | none | +0.000 | +0.000 | +0.000 |
| City `crossing` | 6.71 | **6.72** (+0.01) | none | +0.003 | +0.001 | +0.000 |
| City `park` | 7.84 | **7.85** (+0.01) | none | +0.002 | +0.001 | +0.000 |
| City `rooftop` | 9.99 | **10.00** (+0.01) | none | +0.002 | +0.000 | +0.000 |
| Interior `floor` | 7.16 | **7.17** (+0.01) | +0.001 | +0.020 | +0.005 | +0.004 |
| Interior `floor_walk` | 7.14 | **7.19** (+0.05) | +0.001 | +0.017 | +0.005 | +0.004 |
| Interior `floor_turn` | 6.68 | **6.70** (+0.01) | +0.000 | +0.010 | +0.003 | +0.003 |
| Interior `gallery` | 7.24 | **7.26** (+0.02) | +0.000 | +0.016 | +0.004 | +0.003 |
| Interior `among` | 7.11 | **7.14** (+0.03) | +0.001 | +0.016 | +0.006 | +0.004 |
| Interior `upward` | 6.22 | **6.24** (+0.02) | +0.000 | +0.012 | +0.004 | +0.003 |

On Bistro the reservoirs' pass, `many turns`, is most of it: 0.23 to 0.34 ms, what it costs the interior's 10,000 candles too,
for it decodes every pixel of the tiles whose turn it is, whatever the lights. The resolve, which no longer loops over the lamps
the tiles listed, is cheaper where many were in view (the street, 0.07 to 0.08 ms) and dearer where few were (the hedge, 0.08),
for the reservoir it reads and shades with. The far sums cost 0.01 ms in `gi update` and 0.003 in `gi resolve`, and the far
share 0.01 to 0.02 ms in the interior's resolve. The city has no many lights, and nothing of it moves beyond the noise.

**DRAM**, as the render graph declares it (whole resources, an upper bound), on Bistro's street: the frame 4,829 MB where it
was 4,383, `many turns` 177 MB read and 66 MB written, `many tiles` 66 MB and 2 MB, the resolve 66 MB and 66 MB more, for
the reservoirs' two buffers. What they touch (ADR-0074): the resolve reads one buffer of reservoirs and writes the other, 133
MB a frame, and `many turns` about 10 MB; the far sums, a word a cell (1 MB), are written by the update and read by the
resolve only for the cells that had samples. Device memory: Bistro's 4.52 GB went to 4.67, the reservoirs (66 MB each at 4K),
their tiles' lists and the far sums.

Against §10.4 it sits on the "Shadows" line, and the "Material classify + fused shading + direct light" line, which shades
with the reservoirs. It is to be won back in Phase 14's last task, the reference scenes brought to 144 FPS (ADR-0067),
starting with the reservoirs' pass, whose cost does not follow the lights.

## Alternatives

- **A shadow ray a pixel and light,** the ordinary lights' `shadows`: the night street's frame went from 6.6 to 24.3 ms,
  where as many lights it takes 6.6.
- **A shadow map a lamp:** 96 lamps, six faces each, of pages in the sun's pool, for light that by day is 0.2 % of it.
- **Testing glass and shells in the rays as they are met** (non-opaque geometry): every ray of every pass would pay for it;
  a mesh's flag costs nothing at run time.
- **Fetching the sky's specular again** for rough surfaces in frames with many lights: the wash ADR-0077 took away.

## Consequences

- **Glass still casts the sun's shadow** in its map, which the raster draws as before; the cache's and the reflections'
  rays pass it. A shop behind its window takes the sky's light through the glass, and not the sun's.
- A mesh of glass and other materials together stays in the rays; a glowing mesh whose box holds a light it does not
  surround goes out of them. The cook log counts both rules' meshes.
- Near fields (0.25 m) cast no shadows (ADR-0073), and the reservoirs' limit on what they show (four times white) darkens
  what is right by a bright lamp.
- `render_perf --no-sun` and `--local-shadows` show a scene's local lights alone, and their truth.
- `Many.a_rough_metal_keeps_the_skys_light_among_many_lights`, `gltf.glass_stays_out_of_the_rays`,
  `gltf.a_lights_shell_stays_out_of_the_rays`; 10 planted faults, each caught, are listed in the PR.
