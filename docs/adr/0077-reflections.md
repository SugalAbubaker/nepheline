# 0077. Reflections: what surfaces reflect, from the radiance cache's cells

- **Date:** 2026-11-02
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 10 asks for ray-traced reflections on the reference hardware within the §10 budget, and ADR-0067 for
the 144 FPS gate to be met with them on. A surface's specular light came from the sky's prefiltered cube, as if nothing
hid the sky, occluded only by the screen's ambient occlusion within half a metre.

Three measurements decided the design.

**The budget in rays.** The radiance cache traces 65,536 rays a frame with their hit shading in 0.19 ms, 2.9 ns a ray. So
0.3 ms buys about 100,000 rays, 1.2 % of the pixels at 4K. Bistro's street has 10.5 % of its pixels under a roughness of 0.2
and 22.5 % under 0.3. A ray a smooth pixel does not fit by ten times.

**Screen-space tracing would not answer it.** A window seen from the front reflects what is behind the camera, which the
screen does not hold. And a window is what the city is made of.

**The truth.** Real rays along each pixel's lobe from every pixel, averaged over frames, showed something larger than the
reflections themselves. Against them the sky's specular wrongly brightened 66 % of Bistro's street by over 16 levels, 37
levels on average. The pale wash on its cobbles, awnings and shadowed walls was the sky reflected where no sky could be seen.
In the city's street it was 23 % of the pixels and 12.5 levels; in the interior, which has no sky, next to nothing. Reflecting
a mirror correctly matters on few pixels. Not reflecting a hidden sky matters on most.

## Decision

Everything goes through cells of the radiance cache (ADR-0039), which hold light in the world, whatever the camera does.

1. **Rough surfaces (0.25 and up) take the light their irradiance cell has gathered, over pi,** as if arriving evenly. No ray,
   no memory: it is in the resolve's hand already. Where the cell is trusted, nothing of the sky is fetched.
2. **Smooth surfaces take a reflection cell:** the same grid four times finer, for each direction of an octahedral map whose
   bins are as wide as the surface's lobe, 1.6 degrees for the smoothest. A mirror direction changes slowly across a flat
   surface, so a cell's few rays serve many pixels, and being in the world they hold still as the camera moves.
3. **The reflection cells have a table of their own,** of as many slots, freed after 60 frames unseen. In one table a moving
   camera's new directions crowded out the irradiance's cells, whose samples were dropped, and blotches of sky appeared
   wherever the view was turning. A quarter as many was not enough for Bistro at 4K ("What it does").
4. **The rays come from what smooth pixels left,** with no decode of their own: the resolve leaves a mirror direction, a
   roughness and a specular colour in an image and lists the pixel's tile. Each frame, one listed tile in 32 traces a ray per
   pixel along a sample of its lobe. A dielectric facing the camera, showing a twentieth of what arrives, plays Russian
   roulette for it. Nothing is smooth, nothing runs.
5. **A pass after the resolve looks the cells up and adds what they hold,** taking the energy and the occlusion itself, and
   clears what the pixels left, so the image is empty at no cost to pixels that leave nothing.
6. **What the resolve keeps of this is two words and one store, at its very end, after the colour.** It runs at its register
   limit ("Tried"): this placement is what makes the rest affordable.
7. **Where a cell gives out,** the sky along the direction, no brighter than what the irradiance cell has gathered, or
   with no sky what that cell has gathered. A blended surface, which starts no ray, takes the same in forward shading.

`NPH_RENDER_REFLECTIONS_RAYS` gives smooth surfaces a ray a pixel, every frame, in place of the cells, for GPUs with rays to
spare. A build with the roughness limit raised is how the truth above is made: every pixel's lobe, averaged over frames.

## What it does

Native 4K, RTX 3060 Ti, Profile, the last 64 of 240 frames averaged, the truth made by the final code with the roughness
limit raised (2026-11-03). Against it, the share of pixels brighter by over 16 levels (of 255, in the green channel), and how
much brighter on average:

| View | The sky's specular | With this |
|---|---|---|
| Bistro `street` | 65.6 %, 36.7 levels | **15.5 %, 7.6 levels** |
| The city's `street` | 22.7 %, 12.5 levels | **0.9 %, 1.7 levels** |
| The interior's `floor` | 1.1 %, 0.1 levels | **1.1 %, 0.1 levels** |

The interior has no sky to wash it: what differs there either way is the truth's own noise, as many pixels darker as brighter.
What is left in Bistro is mostly its mid-street cobbles, seen at a grazing angle: their lobes reflect the buildings along the
horizon, and what a cell has gathered leans towards the sky above. A term of low order cannot say so.

**The table's size.** A quarter as many reflection cells as irradiance cells was too few for Bistro at 4K, whose cobbles'
normals scatter their mirror directions over many bins. Rays dropped by full buckets (`vis_reflect_dropped`) over 240
frames: the street standing 746,006 against 9,622 with as many slots as the irradiance's, walking 995,926 against 56,057,
turning 695,653 (8,698 in its worst frame) against 52,959. The city's street dropped 2 and the interior none. Standing, the
image gained little (15.9 % to 15.5 % of pixels); moving, the rays dropped are a new direction's first samples. The larger
table costs 7 MB more, and its slots kept by `gi resolve` take 0.01 to 0.03 ms (the cost table below).

Under an even sky with nothing to hide it the three modes agree within 3 %, smooth or rough, metal or not. A mirror on the
ground shows a red wall in all 7,802 pixels whose rays meet the wall, by the test's own rays, and in 5 of the 1,024 whose rays
do not. In a sealed room a rough metal's light falls from 0.65 to 0.27; metal voxels under an even sky from 0.118 to 0.065,
the needles hiding the sky from each other, in either mode. A camera walking over a mirror leaves the irradiance's cells
exactly as many as without. A blended mirror with no sky shows what is gathered around it (0.090 against 0.004 without),
and under many lights takes their far fields once in every mode (`Reflect.*`, `Many.a_blended_*`).

Frame in ms, mean, against the contact shadows' build (ADR-0076, #150's head), interleaved, two rounds of 240 frames a
scene, a scene a process (2026-11-03); the passes' columns are this build's, or what it adds:

| View | Before | With reflections | `reflect` + `reflect rays` | `gi resolve` | resolve |
|---|---|---|---|---|---|
| Bistro `street` | 7.04 | **7.39** (+0.35) | 0.23 + 0.10 | +0.025 | +0.006 |
| Bistro `street_walk` | 7.10 | **7.48** (+0.38) | 0.24 + 0.11 | +0.028 | +0.008 |
| Bistro `street_turn` | 7.29 | **7.65** (+0.36) | 0.20 + 0.09 | +0.023 | +0.052 |
| Bistro `terrace` | 7.60 | **7.97** (+0.37) | 0.23 + 0.10 | +0.022 | +0.027 |
| Bistro `hedge` | 7.22 | **7.44** (+0.22) | 0.14 + 0.07 | +0.012 | +0.016 |
| City `street` | 6.42 | **6.61** (+0.19) | 0.12 + 0.05 | +0.010 | +0.004 |
| City `street_walk` | 6.50 | **6.70** (+0.20) | 0.13 + 0.05 | +0.010 | +0.008 |
| City `street_sprint` | 6.65 | **6.88** (+0.22) | 0.14 + 0.06 | +0.012 | +0.017 |
| City `street_turn` | 6.47 | **6.80** (+0.33) | 0.20 + 0.07 | +0.010 | +0.031 |
| City `crossing` | 6.51 | **6.75** (+0.24) | 0.16 + 0.06 | +0.010 | +0.012 |
| City `park` | 7.63 | **7.86** (+0.23) | 0.12 + 0.07 | +0.009 | +0.047 |
| City `rooftop` | 9.53 | **10.01** (+0.47) | 0.25 + 0.17 | +0.010 | +0.066 |
| Interior `floor` | 7.04 | **7.14** (+0.10) | 0.01 + 0.03 | +0.005 | +0.060 |
| Interior `floor_walk` | 7.03 | **7.14** (+0.11) | 0.01 + 0.03 | +0.005 | +0.063 |
| Interior `floor_turn` | 6.53 | **6.68** (+0.15) | 0.04 + 0.03 | +0.007 | +0.062 |
| Interior `gallery` | 7.13 | **7.23** (+0.11) | 0.01 + 0.02 | +0.005 | +0.055 |
| Interior `among` | 7.03 | **7.11** (+0.08) | 0.01 + 0.02 | +0.005 | +0.056 |
| Interior `upward` | 6.13 | **6.21** (+0.08) | 0.01 + 0.01 | +0.004 | +0.054 |

The resolve hardly moves where the sky's fetches it saves pay for the store, on Bistro's and the city's streets; with no sky
to save, as in the interior, or a thousand lights to shade (the synthetic `lights_1k`, +0.058 ms), it takes about 0.06 ms.
The table as large as the irradiance's costs 0.01 to 0.03 ms in `gi resolve`, which keeps every slot. Where nothing is
smooth, the three passes still cost about 0.01 ms in all, their dispatches' floor.

**DRAM**, as the render graph declares it (whole resources, an upper bound), on Bistro's street: `reflect rays` 115 MB read
and 72 MB written, `reflect` 90 MB and 100 MB, `gi resolve` 19 MB written where it wrote 9.4, the resolve 67 MB more written
for the image it may store to. What they touch is far less: the image (8 bytes a pixel, 66 MB at 4K) only at smooth pixels,
10 to 15 % of the street's, so 7 to 10 MB stored by the resolve and as much read and cleared by `reflect`, which reads and
writes the HDR image at those pixels too; the rays, a listed tile in 32, read their pixels' depth and what their hits need,
as the cache's rays do. The reflection cells' table takes 9.4 MB of VRAM, the tiles' list 0.5 MB.

Against the §10.4 table this sits on the "Indirect light" line, already over from the ambient occlusion at full resolution
(ADR-0041). It is to be won back with the rest of the frame in Phase 14's last task, as ADR-0067's ratchet asks.

## Tried

- **The lookup in the resolve, where the sky's cube was fetched:** 0.82 ms over the frame, 0.34 ms of it in the resolve when off.
- **At the shader's end, after the lights:** 0.44 ms in the resolve when off.
- **Only the store in the resolve, in the shading's indirect light, or after the lights:** 0.13 ms to 0.27 ms. The statistics
  explain it. The shader sits at 64 registers; with no store at all it spills nothing, and with one anywhere before the colour it
  spills 9 to 11 registers a pixel, whichever pixels it is for. Reading anything from the view after the lights, even a flag,
  does it too: the shader read it earlier, the compiler reuses that, and it has to keep it across the light loop. After the
  colour's store it may not reuse it (a store may alias), and the read is free.
- **Rays by a second decode,** a lattice of tiles through the resolve's own code: 0.06 ms more than rays from what pixels left,
  and it paid wherever nothing is smooth.
- **Reflection cells up to a roughness of 0.4:** blotches on normal-mapped surfaces like the awnings, whose mirror directions
  scatter among more cells than rays can fill. Above 0.25 the irradiance cell is as near the truth at no cost.
- **The sky's specular scaled by the scene's irradiance over the sky's** (rather than replaced by the former): as near the truth
  (9.9 levels against 9.6 on the street), and it fetches the sky.
- **A tile's turn by its place in the list:** that order is the order subgroups ran in, so a tile could wait; after 96 frames 5 %
  had had no turn. By where the tile is, each has one every 32 frames.

## Consequences

- **The reference scenes look different:** darker and with more contrast, the wash gone. That is the truth at these exposures,
  which were set with the wash in place, and a manifest may want a stop more light; the look of the first game's world is to be
  set with reflections on.
- **No mirror-sharp reflections on the reference hardware.** A cell is up to a dozen pixels across and its bins 1.6 degrees wide:
  windows and metals show the right things, softly. `NPH_RENDER_REFLECTIONS_RAYS` is for hardware that can afford to be sharp.
- **Newly seen smooth surfaces take up to 32 frames** to have their cells: until then, the fallback.
- A blended surface starts no ray, so it has no reflection cell: smooth, it takes the fallback in forward shading.
- **What a scene's smooth surfaces need of the table shows in `vis_reflect_dropped`.** `gi_cells` sizes both tables; a world
  of more mirrors than Bistro's street, or a larger target, may need it raised.
- A ray's hit shows its diffuse light and its emission, not its own reflections.
- **Screen-space reflections are not excluded.** They would give sharpness on the frame's floors and cost little where cells give
  the rest, and may yet come as a quality tier.
- `Reflect.*` holds it to rays the test casts; 18 planted faults, each caught, are listed in the PR.
