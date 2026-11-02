# 0075. Light only from the cache: occlusion, blended surfaces, and a night's sky

- **Date:** 2026-11-02
- **Status:** Accepted

## Context

After ADR-0074 the interior stood 0.5 to 0.6 ms over the 144 FPS gate, and looking at where the time went showed three things that
were wrong as well as slow.

- **The sky lit a night scene.** The interior's manifest had `sky_intensity: 0.001`, for a dark sky through the atrium. But the sky at
  that scale is still about a third of the display's white in a mirror direction, unoccluded: the floor's pale wash, and blue-grey
  patches where the cache's trust is low, were the sky. And every pixel paid for it: 27 loads for its spherical harmonics, a cube
  sample, a little more for every blended fragment.
- **The ambient occlusion was tied to that sky.** It ran only where the environment's light was on, from when it was the only
  indirect light. A scene lit by the radiance cache and no sky got none, though the cache's light is what it is for.
- **Blended surfaces drew black.** Forward shading has no radiance cache. The palace's dirt is an alpha-blended sheet, and, its
  material leaving `metallicFactor` at glTF's default of 1, a rough metal: with no lights, sun or sky it had no light at all, and a
  stain was a hole of darkness at its alpha. Its 0.37 ms of shading computed that black.

## Decision

1. **The occlusion darkens the cache's light as it does the environment's:** it runs where either is on (`ao.c`). Scenes lit
   by the cache alone gain it.
2. **Blended surfaces take the cache's light,** as the resolve's surfaces do, by a pipeline of their own (`forward_frag.glsl`
   `k_gi`). With many lights in the frame the cell also gives their specular its energy, as if arriving evenly from everywhere: no
   reservoir holds for a blended surface.
3. **A frame whose only light is the cache's shades blended surfaces from it alone** (`k_cache_only`): no lights, no sun, no sky.
   Their colour times what the cell gathered, the specular's share in that colour, plus emission. The full shading's BRDF, and its
   normal map and roughness, have no light to act on: cells keep no direction. Against a truth by ray casting the two draw the same
   (101.0 % each, `Many.a_blended_surface_takes_*`).
4. **Such a frame skips the light grid** in the resolve (`NphRenderManyView.no_grid`): nothing is in it, and a pixel otherwise reads its
   distance bin and its tile's word to find that out.
5. **A manifest may say its sky lights nothing** (`"ibl": false`): a background only. The interior says so, and its exposure goes from
   EV100 4.5 to 4.0, which is what the wash had been standing in for.

## What it does

Native 4K, RTX 3060 Ti, Profile, 240 frames, the editor minimized; frame in ms, with reservoirs on (ADR-0074):

| View | ADR-0074, mean (p99) | Now, mean (p99) |
|---|---|---|
| `floor` | 7.36 (7.43) | 6.91 (6.99) |
| `floor_walk` | 7.34 (7.44) | 6.91 (6.97) |
| `floor_turn` | 6.89 (7.40) | 6.43 (6.98) |
| `gallery` | 7.47 (7.53) | 7.03 (7.10) |
| `among` | 7.38 (7.47) | 6.94 (6.98) |
| `upward` | 6.40 (6.44) | 6.05 (6.11) |

The gate is 6.94 ms: the slowest view is 0.16 ms over it at the 99th percentile, the others 0.03 to 0.05 or under.

The `transparent` pass, 0.41 ms on `floor` before, is 0.29: of that, 0.05 is the pass with every fragment discarded first thing, so
the rest is real shading of real dirt. Early exits for what is hidden or clear (decision 3's fragment returns as soon as it has
served its quad) saved nothing measurable, but cost nothing, and stay. Bistro's blended surfaces take the cache's lookup: its
`transparent` goes from 0.084 to 0.094 ms on the street and 0.143 to 0.160 on the terrace, and its frames by 0.02 to 0.05 ms, within a
round's spread. The city draws nothing blended.

The synthetic scenes at night (`lights_*`, `many_lights_*`) gain the occlusion's 1.3 ms: 2.98 to 4.34 ms on `many_lights_1k`. Their
budgets are re-based. They had simply not been drawing it.

**Bytes.** No new memory. The blended pipelines read the cache's cells; the resolve reads a word less a pixel in such a frame.

## Alternatives

- **Keeping the sky and only skipping the occlusion's tie to it:** the floor keeps its wash, and every pixel its 27 loads.
- **A dark sky by a smaller intensity:** the cost stays, and it is no scene's truth: this is a night with a sky to see, not to be lit by.
- **The full shading alone for blended surfaces:** 0.51 ms on `floor` against 0.29, for the same picture here.
- **Early-out only, without the cache's light:** cheaper by 0.09 ms, and black dirt.

## Consequences

- A blended surface in a frame lit only by the cache has no highlight. A window of glass in a candle-lit hall shows no flame until
  reflections' rays do.
- A frame's `transparent` depends on whether any other light exists: one small ordinary light in a many-light scene puts all its blended
  surfaces on the full shading, about 0.2 ms on the interior's views.
- The occlusion is now part of scenes without an environment, in the tests' scenes too where `gi` is on.
