# 0092. Light off what the rays meet, and an image that holds when the camera stands

- **Date:** 2026-11-12
- **Status:** Accepted (built and measured; the user's report of 2026-11-12 that it answers: "The flickering is
  almost completely gone, the only places it remains is on the soil under the tree in bistro and also the table next
  to the building with the interior. Also, the sewer manhole in on the ground right before the arch. You can stand
  still and see this flickering ... If you stand in the middle of one of the long streets you can see the buildings
  in the distance flickering ... The buses that are in the shade are pitch dark on the shaded side, that tells me
  that the light reflections are not being calculated correctly"; of this build they reported what ADR-0093 answers;
  accepted with ADR-0090 to 0095, 2026-11-13: "commit step 3 to 8")

## Context

After ADR-0091 the cache's light held at a run. What the user still saw, standing still:

- **Bistro: the soil under the tree, a terrace table's metal apron, the manhole and bollards before the arch.**
  Eight frames drawn at one pose, each 8 × 8 block's deviation over its mean: 6.6 %, 9.5 % and 1.9 % at the
  three. With the reflections off they went, with the cache's light on: the reflections' rays (ADR-0089), one
  a smooth pixel a frame, on dark surfaces that show little but what they reflect.
- **The city: the far buildings of a long street.** 1.2 % on a far tower's windows and 1.8 % on a glass wall.
  With the anti-aliasing off, 0.15 and 0.12 %: the history's own doing. Detail finer than a pixel (window
  frames a street away) is found by one frame's jittered samples and missed by the next; the history was
  clipped to a box of one deviation of each frame's 3 × 3 neighbourhood within its extremes, which a frame
  that misses a bar leaves it outside of, and the current frame counted for 1/16.
- **The city: the shaded side of a bus near black.** A reference that follows whole paths from the cache's
  rays gave the cache's light to four places, which cleared the cache and pointed at what both share: what a
  ray's hit sends back. Giving every hit three times its light changed nothing. A hit reflected its base
  colour times 1 - its material's metalness **factor**; a material whose metalness is in a texture has a factor
  of 1 (glTF's default, which the city's generator leaves), and the hit never read the texture. Every surface
  of the city was, to the rays, a metal that reflected nothing: no light bounced at all, its shade was the
  sky's alone, and what its windows reflected was the sky and black shapes.

## Decision

- **A ray's hit reflects by its base colour, a metal's too** (`gi_hit.glsl`). A metal reflects by its base
  colour as a mirror or a lobe does, not evenly; sent on evenly it is the same light in all, where a hit that
  reflected nothing left metal black in what reflects it and took its light out of the bounce. No metalness is
  read, so none can be read wrong.
- **The anti-aliasing holds a still pixel's history longer and clips it less** (`taa_common.glsl`). Where the
  history did not move, the current frame counts for 1/32 (it was 1/16), and the history's box is the
  neighbourhood's mean ± 3 deviations whatever its extremes (`TAA_STILL_GAMMA`). At a pixel a frame of motion
  both are what they were (1/4; one deviation within the extremes), and between the two they blend by the
  motion.
- **A smooth pixel's rays over the frames follow a sequence that fills its lobe evenly** (`reflect_rays.comp`):
  the pixel's own two numbers from interleaved gradient noise, moved each frame by the next step of the R2
  sequence (Roberts 2018), the same step for every pixel, so a frame's pixels keep the noise's even spread among
  themselves, which the gather counts on.
- **A smooth surface rougher than 0.1 takes less and less of what it reflects from its ray**
  (`gi_reflect_ray_share`), and the rest from what its irradiance cell has gathered, as the surfaces past
  roughness 0.25 take all of theirs: none from the ray at 0.25, where it is no longer traced. The wider the lobe
  the more a ray's light is one sample of many, which the few pixels around (the gather reaches two) and the
  history's frames do not average; and the surfaces either side of 0.25 no longer differ by a step. (After the
  user's second look, 2026-11-12: "the man hole and table are still flickering".)
- **The viewer has no keys for automatic exposure, the reflections' cells or the cache's unsmoothed cells** (the
  user: "it only makes it look worse", "it just brings back the noise"), and asks for the many lights' direct
  light from the reservoirs (`NPH_RENDER_RESTIR_ALL`, ADR-0079; the user: "turn the shadow direct on for the
  interiors"). The renderer's default stays `SPECULAR`. (Taken out of the viewer again the same day, the
  user having seen it: "the interior is too grainy", ADR-0094.)

## Measured

2560 × 1440, RTX 3060 Ti, driver 610.74. A still pose, 8 frames, an 8 × 8 block's deviation over its mean:

| Where | Before | Now |
|---|---|---|
| Bistro, the terrace table's apron | 9.5 % | 1.0 % |
| Bistro, the soil under the tree | 6.6 % | 0.6 % |
| Bistro, the bollards before the arch | 1.9 % | 0.4 % |
| The city's crossing, the far tower | 1.16 % | 0.33 % |
| The city's crossing, a glass wall | 1.81 % | 0.56 % |
| The city's crossing, the whole view | 0.75 % | 0.30 % |

Of the far tower's 1.16 %, the looser clip took it to 0.75 and the longer history to 0.43; the rest is the
bounced light, which lowers what the windows' reflections stand out by. The rays' sequence moved the table's
3.2 % to 3.1 %: little. The table's apron is a near-black dielectric of roughness 0.22, all of whose light is
what it reflects: with the reflections off it changes by 0.5 %, with them 2.1 %, and with its ray giving a
fifth of what it reflects 1.0 % (the soil 1.6 to 0.6 %, the bollards 0.7 to 0.4 %).

Two frames at one pose along the paths of ADR-0091: Bistro's arch at 16 m/s 0.16 to 0.31 levels (0.21 to 0.58),
the city's bus walk 0.08 to 0.36 (0.14 to 0.83).

**The bus** (the user's place, the city's own exposure): its shaded green side's irradiance was a third of the
shaded road's and its lit colour 0.004 of white; with the bounce its green and yellow read as the road's shade
does. The shaded shop fronts across the street, black at three times the exposure, show their windows'
reflections of the lit street.

**Cost:** none measurable on the 18 reference views (means within 0.04 ms of ADR-0091's, either way).

## Alternatives considered

- **Reading the metalness texture at the hit.** One more texture read a ray, to reflect less than the surface
  does: a metal's reflection is not lost because it is not even.
- **The reflections' light kept over frames in an image of its own**, each pixel's found again where its
  surface, or what it reflects, was a frame ago. Two histories in a row of 32 frames each halve the variance
  of one, no more; it needs 8 bytes a smooth pixel read and written a frame, and a rule for what a mirror shows
  in motion. Not built.
- **A wider gather for wide lobes**, in two passes. A lobe of roughness 0.22 smears what it shows over a
  hundred pixels and more; the rays' share does for nothing what a gather that wide would do for a pass.
- **A history of 64 frames for a still pixel.** Half the shimmer again; and a shadow that moves over a still
  surface (the first game's cars and people) would trail for a quarter of a second.

## Consequences

- The city has bounced light: shade is lit by the sunlit street and walls, trees by the ground, and what glass
  and metal reflect is lit. Every city view's shade is brighter than it was; the reference views' images
  change, their times do not.
- A change of light at a pixel that does not move (a shadow crossing a still wall) is followed over 32 frames
  where it lies within three deviations of the pixel's neighbourhood, where it was 16 within one.
- In the candle-lit interior (10,000 lights) the reservoirs' direct light is grainy and darker than the cache's
  (two frames at one pose differ by 1.2 to 1.6 levels against 0.5 to 0.7; it was so before this work): the
  viewer shows it because the user asked for the lamps' shadows; making it clean is its own work.
- At the city's and Bistro's own exposures, set for their daylight, Bistro's interior is black but for a
  hand's breadth about each lamp (a 300 cd lamp against an exposure for the sun): without the viewer's key
  nothing adapts it. What the game's camera does indoors is to be decided.

## Tests

`Reflect.a_rough_floors_reflection_holds_between_frames` (a dark floor of roughness 0.22 changes by 0.0007 a
pixel between frames, less than one of roughness 0.1 does, 0.0013; with its ray giving all, 0.0030);
`Gi.a_metal_wall_passes_the_suns_light_on` (a rough metal wall bounces the sun's light as the white wall of
`the_suns_light_bounces_off_a_wall` does); `Taa.a_still_views_fine_detail_holds_between_frames` (bars 0.6 of a
pixel wide on a wall: a pixel wanders 1.5 levels between frames; 2.6 with the weight as it was, 2.8 with the clip
too); `Reflect.what_is_hidden_from_the_sky_does_not_reflect_it` has walls that reflect half the light (walls
that reflect nine tenths pass the sky's light along the room, now that metal sends light on).
