# 0083. AgX is the default tonemapper

- **Date:** 2026-11-10
- **Status:** Accepted (the user's decision of 2026-11-10)

## Context

Walking the city at native 2560 × 1440 (ADR-0082), the user reported its
shade as pitch dark: building sides out of the sun, the wall of the dark
building left of the park seen from its shadow, the gaps between leaves;
and a constant static noise on most surfaces, worse on dark ones.

**How dark, and why.** In the city's street view, 60.3 % of the pixels came
out below 8 of 255 (sRGB luma of a 64-frame mean), and 23.7 % of the park's.
The light there is low but not zero. The radiance cache's indirect light on
the shaded facade across the street is a pre-exposed 0.0040, about 5.5 stops
under mid-grey at the scene's EV 13. The ACES fit the renderer used
(Stephen Hill's, BakingLab 2016) maps it to 0.0001, 0.3 of 255, where a
clamp would show 12.9: the fit's numerator, v (v + 0.0245786) − 0.000090537,
is negative below v ≈ 0.0033, and the curve is clamped there, while values
up to about 0.05 stay within its lowest 31 levels.

**What it was not** (2026-11-10, each measured on the city's street and park
views):

- The radiance cache's rays treating alpha-tested leaves as opaque: built
  and measured, the rays' alpha test moved the near-black share from
  60.3 % to 59.6 % (park 23.7 % to 23.4 %) at 0.23 to 0.60 ms a frame, and
  was shelved (`feature/render-alpha-rays`). The city's leaf cards are
  mostly opaque at their finest level.
- Too few rays: four times the cache's rays changed nothing.
- The ambient occlusion and the contact shadows: off, nothing changed.
- The fit's clip alone: a toe that keeps the curve above 0.01 and joins it
  smoothly (a power of the value, its slope matched) removed the clip and
  changed nothing visible (the street's near-black share 60.1 % either way,
  its dark pixels 6.7 to 6.8 of 255). ACES renders light that far under
  mid-grey near black by design, not by the fit's error.

**AgX at the same exposure**, the curve the renderer already had
(Sobotka's, with Blender's matrices and Wrensch's polynomial), keeps that
light: the street's near-black share 37.5 % and its dark pixels 17.0 of 255,
the park's 9.7 %, the crossing's 10.1 % (previews of the same frames through
each curve, against ACES's 60.3 %, 23.7 % and 23.0 %). The user chose it from
those previews.

## Decision

1. **AgX is the default.** `nph_render_view_default` tonemaps with AgX, the
   runtime's new camera components do (and a camera's tonemapper past the
   last is taken as AgX), and `pbr_viewer` starts with it. The ACES fit stays,
   as it was, for a view or a camera that asks for it.
2. **The exposure stays** as each scene sets it.
3. **The golden images keep pinning ACES**, as they did: they test the
   renderer's arithmetic, which is unchanged.
4. **The passes that tonemap are re-based** on AgX's cost (below), in
   `benchmarks/reference_budgets.txt` and `benchmarks/render_budgets.txt`.

## Alternatives considered

- **Fix the ACES fit's toe.** No visible change (above).
- **Local exposure**, lifting the shade against the sunlit areas while ACES
  keeps its contrast: the larger change, left for later.
- **Keep ACES.**

## Consequences

- The shade reads: in the engine, the street's near-black share 60.3 % →
  36.7 % and its dark pixels 6.7 → 17.2 of 255; the park's 23.7 % → 9.1 %;
  the crossing's 23.0 % (the previews') → 9.8 %.
- AgX's look: less contrast and less saturation in bright saturated
  colours than ACES's.
- The noise in the shade shows more: the park's dark pixels vary from frame
  to frame by 14.9 % of their level, against 9.0 % under ACES, so the
  radiance cache's noise (the cells' per-pixel jitter, the reflection cells)
  is the next thing to fix.
- **The cost.** AgX's curve is longer arithmetic than the fit's (a
  logarithm, a sixth-order polynomial and a power a channel, where the fit
  takes a ratio of two quadratics), in the pass that ends in the display
  colour: TAA's resolve, or the motion blur where a camera's shutter is
  open, passes bound by the instructions they issue. On the
  RTX 3060 Ti, two interleaved rounds a build (`render_perf`, 240 frames a
  view, ACES f5f9fef against AgX on it):
  - at 2560 × 1440, the reference scenes' 18 views: TAA +16.2 to +18.5 µs
    (+4.7 to +5.2 %; the city street's 0.358 → 0.377 ms), the frame's mean
    +13 to +27 µs, the worst 1%-low 5.46 ms (the rooftop), against the
    6.94 ms gate;
  - at 3840 × 2160, the synthetic scenes: TAA +44 to +50 µs (+5.4 to
    +6.1 %) in the twelve whose TAA tonemaps, the motion blur +29 to +49 µs
    (+5.9 to +12.6 %) in the three whose shutter is open, whose TAA then
    moved less than 1 %; the frames' means +46 to +90 µs, the rest of it
    within the rounds' spread (the ACES build's two rounds of a scene differ
    by up to 0.11 ms).

  The runtime and the viewers present to UNORM swapchains, as `render_perf`
  draws into a UNORM target, so the shader applies the sRGB curve after
  either tonemapper and these are what a player's frame pays. Folding
  AgX's 2.2 power into that sRGB curve, one power a channel where there
  are two, measured the same (TAA +17.5 to +19.5 µs in the 18 views, two
  interleaved rounds), so it was not kept: the transcendentals are not
  where the cost is.
- The budgets of the passes that tonemap are re-based on AgX's means, as
  `scripts/render_budgets.py` computes them: `reference_budgets.txt`'s 18
  `gpu.taa` entries (they came to 100 to 101 % of their ACES budgets), and
  `render_budgets.txt`'s 12 `gpu.taa` entries of the scenes whose TAA
  tonemaps (96 to 97 %) and the motion scenes' 3 `gpu.motion_blur` (99.9 to
  106.6 %; the grids' `gpu.taa` stay at their 50 µs floor). No other entry
  moved past its noise.
- Scenes that saved a camera's tonemapper keep it.
- `Engine.an_offscreen_engine_draws_the_world`, which compares the two
  pipelines through the engine's own view, pins that view to the ACES fit,
  as the golden images do. Under AgX its share of channels within 3 levels
  fell from 99.36 % to 98.54 % (the worst 42 → 56 levels), at the same
  pixels as under ACES: the torus's inner rim and silhouette and the floor
  beneath it, where the production path draws what the reference renderer
  does not and the fit had rendered near black. Turning the radiance
  cache or the ambient occlusion off moves that share by less than 0.5 %
  and leaves the worst pixel as it is.
