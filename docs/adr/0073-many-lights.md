# 0073. Many lights, near and far

- **Date:** 2026-11-01
- **Status:** Accepted

## Context

The many-light interior reference scene (§10.7, ADR-0067) has 10,000 point lights, one in each of Intel's candles, at a
density of 3.5 a cubic metre: a pixel among them is within 2 m of 191 lights and within 4 m of 965. They are unlimited in
range, as the scene's authors made them, and most of the light a surface gets comes from far: at that density the lights
within half a metre deliver about 4 % of the irradiance. So a window at some range cannot stand in for them, and neither
path the engine has can hold them.

- **The light grid** (`docs/render.md` "Lights") takes a view's 4,096 lights, a bit per light and tile: 10,000 would take
  81 MB of masks written every frame, and unlimited lights go to the list every pixel walks.
- **A ray per light per pixel, or per pixel:** the radiance cache's update traces 57,600 rays in 0.18 ms, 3.2 ns each, so
  a ray a pixel at 4K is 27 ms against a frame of 6.94. MegaLights, Unreal's answer, takes 5.51 ms on a PS5 at 1080p
  (Narkowicz & Costa, SIGGRAPH 2025).

CLAUDE.md §8.6 stage 10 names ReSTIR (Bitterli et al., SIGGRAPH 2020) for many-light sampling, "enabled on the reference
hardware only within the §10 budget". Its per-pixel shadow ray does not fit that budget, which leaves it to the high-end
tiers.

## Decision

1. **A light's reach is split at its near range by a smooth partition of unity,** the window a range already gives
   (Karis 2013): `near(d) = (1 - (d / near_range)^4)^2`, and `far(d) = 1 - near(d)`.
2. **Pixels evaluate the near fields,** with their BRDF and without shadows, in the resolve. They find the lights through
   a hash of the world cell the point is in (twice the near range across) to a list of the lights that reach that cell. A
   pixel away from any light pays one read; two cells that share a table entry only hand a pixel a light more, which its
   distance test rejects. It is built with the set, and nothing about it is per frame or per view.
3. **The radiance cache gathers the far fields.** Each of the update's samples also takes one light from a tree, in
   proportion to what it may deliver there, with a shadow ray to it, and adds `I far(d) cos / d^2 / p` to the cell's
   irradiance. The tree (Conty Estevez & Kulla, HPG 2018; Yuksel, HPG 2019) has four children a node, each weighing its
   power over its squared distance, no nearer than its radius or than the near range. Every light with power has a
   positive probability from anywhere, so the estimate is unbiased however poorly a node's sphere stands for its lights.
4. **They are a kind of light of their own,** `NphRenderManyLight`, in a set the renderer rebuilds whole in the first frame
   after any changes, into new buffers. Ordinary lights are untouched, and a renderer without them
   (`max_many_lights` 0, the default) pays nothing: the shaders are specialised on them, and their pipelines are not made.
   `NphLight.near_range` makes a Light one, and `NphSceneImportDesc.light_near_range` a scene's point lights.
5. **Their far fields' specular** is what the cell gathered, taken as arriving evenly from everywhere. Without it the diffuse
   gives up energy that the specular never gets, and metals go black.
6. **A mesh may stay out of the rays** (`NphRenderMeshDesc.no_rays`, from a glTF mesh's extras, `{"no_rays": true}`). The
   candles' 19.5 million triangles would take over a gigabyte of bottom level, and their flames, shells around the lights,
   would stop every shadow ray and count the lights' energy twice.
7. **The interior's candles are written as one mesh per 4 m cube** (`scripts/interior.py`): the same triangles in the same
   places, because one mesh of 19.5 million triangles takes the cooker more memory than a 16 GB machine has.

## What it does

The tests hold the two parts to what they should be.

- **Near:** 400 lights, sharing table entries, without the cache, against ordinary lights of that range: 0 of 16,384 pixels
  differ.
- **Far:** 64 lights over boxes of no albedo, against the irradiance the test computes by casting a ray from each pixel to
  each light: 97.5 % of pixels within 15 % (the rest at the cells' edges of shadows), mean error 4.07 %, bias -0.09 %.
- A wall in rays leaves the ground behind it at 0.0000 of an unshadowed 0.1019. The same wall out of the rays leaves it at
  0.0976.
- The walk on the GPU against its twin on the CPU: 4,096 of 4,096 the same light at the same probability.

On the RTX 3060 Ti at 3840 by 2160 (`render_perf`'s `many_lights_*`: a field of 16,384 spheres, the lights among them):

| Lights | Frame | `vis_resolve` | `gi_update` | The set | Rebuilt in |
|---|---|---|---|---|---|
| 4,000 ordinary (`lights_4k_production`) | 4.18 ms | 2.02 ms | 0.03 ms | | |
| 1,000 many | 2.95 ms | 0.99 ms | 0.04 ms | 0.15 MB | 0.32 ms |
| 10,000 many | 2.98 ms | 1.01 ms | 0.05 ms | 2.11 MB | 3.35 ms |

Ten times the lights, at a tenth of the power each, cost the frame 0.04 ms. The field's lights are spread over 80 m by 80 m, a
sixth of the interior's density, so a pixel takes fewer near lights there. The interior's own numbers come with its
cook (below).

**DRAM.** A pixel reads one table entry, and adjacent pixels mostly read the same one, so the near lights' traffic is that of
the few lights' records (32 bytes each). A sample of the far fields reads seven nodes of 96 bytes, near the root mostly
the same ones, and a light's record. At 65,536 samples a frame that is at most 46 MB, against the line's 100 MB
(§10.4), and in the cache in practice: the set is 2.11 MB, inside the 4 MB L2. Nothing is written per frame.

## Alternatives

- **ReSTIR DI on the reference hardware:** a shadow ray a pixel, without which its reservoirs carry visibility from nowhere:
  27 ms at the update's rate. It remains for the high-end tiers, where rays are cheaper.
- **ReGIR (Boksansky et al., Ray Tracing Gems II, 2021), a reservoir of lights per world cell, sampled per pixel:** no ray a
  pixel if the cell also holds visibility, but it evaluates a few lights per pixel with a cell's guess at their visibility,
  and is noisy where this is smooth: a far field is smooth, which is what lets a cache hold it.
- **A range on the lights, so the grid could hold them:** it drops 96 % of their light, against "never trade quality".
- **Raising the grid to 10,000 lights:** 81 MB of masks, and at this density a pixel's distance bin spans about 800 lights,
  25 words a pixel, for lights that a pixel could not tell apart.
- **Sampling lights uniformly, or by power alone:** a light across the atrium is taken as often as one beside the point.
  The tree costs seven node reads a sample.
- **Rebuilding the tree per frame, or refitting it,** for lights that move: the tree is cheap but not that cheap on a main
  thread budgeted at 1 ms. Moving lights stay ordinary lights until a game needs otherwise.

## Consequences

- Beyond its near range a many light has no highlight of its own, and takes the cache's few frames to arrive. A flame in a
  mirror is for the reflections' rays.
- Near fields cast no shadows. Within half a metre the contact shadows are what can show them.
- Blended surfaces do not take the near fields, and the reference renderer draws no many lights: golden images of them
  come from the production path, against the truths above.
- Moving one of them costs a rebuild, on the main thread: 3.35 ms for 10,000 lights, with its upload, against the main
  thread's 1 ms (§10.5). Loading a scene does it once. Should a game change them while it plays, the build moves to a job.
- The cache now carries direct light in a scene that has these, so a cell that is new is dark until its first samples: a
  cell takes several a frame at 4K.
