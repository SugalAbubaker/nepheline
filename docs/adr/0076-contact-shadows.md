# 0076. Contact shadows: a march in the occlusion's pass, its answer in a sign

- **Date:** 2026-11-02
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 6 asks for screen-space contact shadows for fine detail, and ADR-0041 placed them in Phase 14.
They are usually there for a shadow map too coarse to reach the contact. Ours is not coarse: the sun's virtual shadow
map matches its texels to the pixels at every distance (ADR-0033). What it misses is what its own offset hides: a
receiver looks up its shadow 1.5 texels off along its normal, so it is lifted above anything lower than that, and the
shadow of what stands a little higher starts late. Against the sun's rays (`--rt-shadows`) that shows as lit gaps
under window bars, along kerbs and beside leaves on Bistro's street, a few pixels wide at any distance.

So the march need only cover the map's error: a few of its texels, which are a few pixels at any distance, not a
fixed length in metres.

## Decision

1. **The march's length is in pixels' widths at the point's distance:** `NphRenderView.contact_shadow`, 24 by default
   (`r.contact_shadow`; 0 for none), as the map's texels are. It is bounded at half the point's distance, which keeps
   it in front of the camera.
2. **It runs in the ambient occlusion's pass** (`ao.comp` `sun_blocked`), which has the pixel's depth, plane and jitter
   in hand, while the frame has both the sun's map and the occlusion. 4 steps, closer together near the pixel, jittered
   as the occlusion's steps are, so the temporal resolve averages them. A first step under a pixel, which would read the
   point's own texel, is left out: the image is the same to the byte without it.
3. **Nothing in a step needs the point's distance.** The length being proportional to the distance, a step's screen
   position and depth follow from the pixel's own and from a few numbers fixed for the frame, with one reciprocal.
4. **What blocks:** a depth in front of the marched point by more than half a pixel's change in the pixel's own plane,
   and by no more than 0.6 of the march's length. A point marched off a plane towards the sun lies on the camera's side of
   it, so only the rounding to a pixel's centre can put the plane in front of it. At a grazing angle that is a long way in
   metres, and exactly half a pixel's gradient in reverse-Z depth, which is affine across a plane. Beyond the thickness,
   it is something else standing in front.
5. **The answer is a bit, carried in signs:** of the distance in the raw occlusion, then of the denoised occlusion, a half
   float that is otherwise never negative. The resolves take it with the value they fetch anyway and multiply it into the
   sun's visibility. No memory, no fetch, no descriptor, and nothing new live in the resolve. It is not filtered: the sun's
   penumbra over these lengths is under a pixel, and a hard edge is what the rays show.

## What it does

Native 4K, RTX 3060 Ti, Profile. Against the sun's rays as the truth, the share of pixels a capture shows brighter than
the rays do by over 16 levels of 255, by over 48, and darker by over 16:

| View | Without | With |
|---|---|---|
| Bistro `street` | 2.232 %, 0.529 %, 0.400 % | **1.987 %, 0.458 %, 0.391 %** |
| Bistro `terrace` | 1.523 %, 0.518 %, 0.242 % | **1.259 %, 0.405 %, 0.296 %** |
| Bistro `hedge` | 1.691 %, 0.798 %, 0.299 % | **1.352 %, 0.594 %, 0.348 %** |

That is 11 % to 20 % less of what is wrongly lit by over 16 levels and 13 % to 26 % by over 48, for up to 0.05 % of the
pixels more that are wrongly dark. A capture's place in the jitter's cycle moves these figures by about 0.04 of a
percentage point from run to run, so the gain is clear of it and the rise in what is wrongly dark only just. Much of what
is left is the map's own on surfaces the sun grazes, which the march does not reach ("Consequences"). Where it shows most,
Bistro's window bars have their shadows, close to the rays', where the map alone left the glass lit.

Cost in the frame, with and without the march:

| View | Without | With | `ao` |
|---|---|---|---|
| Bistro `street` (interleaved, three rounds of 120 frames) | 6.88 ms | 7.02 ms | 0.875 to 1.01 ms |
| Bistro `terrace` (one run of 60 frames) | 7.34 ms | 7.53 ms | about 1.0 ms with it |
| Bistro `hedge` (one run of 60 frames) | 7.01 ms | 7.21 ms | about 1.0 ms with it |
| The city | not measured yet: the run was stopped for lack of memory, with only its side without the march done | | |
| The interior | no sun: no march | | |

The code alone, with no march (a compare and the sign), takes the pass from 0.855 to 0.873 ms: 0.02 ms that the interior
pays for nothing.

**DRAM:** no new memory. The march reads the depths the pass reads already, within a few cache lines of them; the pass's
declared reads stay 44.6 MB. Its share of the §10.4 "Shadows" line takes that line from about 0.40 ms (the map in the
field, render.md) to about 0.55 ms, over by 0.15 ms: won back, if at all, with the rest of the frame in Phase 14's last
task, as ADR-0067's ratchet asks.

## Alternatives

Measured on Bistro's `street`, against the same rays. Too bright and too dark are by over 16 levels:

| March | `ao` | Too bright | Too dark |
|---|---|---|---|
| None | 0.85 ms | 2.232 % | 0.400 % |
| 24 pixels, 1 step | 0.914 ms | 2.164 % | 0.410 % |
| 24 pixels, 2 steps | 0.945 ms | 2.094 % | 0.371 % |
| **24 pixels, 4 steps (chosen)** | 1.009 ms | 1.987 % | 0.391 % |
| 24 pixels, 6 steps | 1.102 ms | 1.957 % | 0.407 % |
| 12 pixels, 4 steps, as thick as 3 lengths | 1.003 ms | 1.991 % | 0.458 % |
| 12 pixels, 3 steps, as thick as 3 lengths | 0.993 ms | 2.019 % | 0.433 % |
| 48 pixels, 8 steps, as thick as 3 lengths | 1.172 ms | 1.719 % | 1.151 % |

These are single captures, each about 0.04 of a point either way, so 12 pixels and 24 are not told apart by the numbers.
24 is chosen by the crops: the shadows under window bars reach further towards the rays' at the same cost. From 1 step to 4
the gain is real; 4 to 6 is within the noise, at 0.09 ms.

- **A longer or thicker march** finds more of what is true and more of what is not: at 48 pixels the wrongly dark
  outweigh the gain. Thin railings in front of a window are what it catches, the glass behind them marched behind the rail.
- **A march that reaches further where the sun grazes the surface**, since the map's texels stretch across it by one over
  the cosine (up to 4 times, 1.885 % too bright and 0.663 % too dark on the `street`; on the `terrace` 1.228 % and 0.506 %, on
  the `hedge` 1.412 % and 0.710 %): false shadows behind those railings again. With its thickness held (2.001 % and 0.386 %
  on the `street`; 1.299 % and 0.306 %, 1.438 % and 0.362 % on the others) it gains nothing clear of the noise, and the stairs
  on those walls stay. Not kept.
- **A pass of its own** would read every depth again and store what the occlusion's pass can carry for free. Not built.
- **The march in the resolve,** after the map's lookup, where only what the map has lit need march: its lanes share
  subgroups, shadowed pixels with lit, and the resolve already runs at its register limit (ADR-0059's lesson on the
  denoise moved into it). Not built.
- **The answer filtered over the denoise's window** would need more than a bit in the same 16: the occlusion's own
  precision, or a sum per tap in the hottest loop of the pass, for a softness the sun does not have at these lengths. Not
  built.

## Consequences

- Where a shadow map is simply wrong at the contact, it is now nearly right. Bistro's window bars have their shadows.
- It is the sun's alone, and it comes with the ambient occlusion: a view without one has none of it, and a surface a
  temporal resolve does not reach (a blended one) neither.
- What the depths do not show cannot block. A low occluder's shadow fades towards its far end, where a step must land
  within its height (`Contact.it_shows_what_the_map_lifts_the_ground_above`: 253 of 429 hidden pixels lit by the map alone,
  128 with the march).
- **Where the sun grazes a wall the map itself is poor,** which the march only smooths near the contact: Bistro's cornices
  throw shadows in stairs, each step a stretched texel, against the rays' clean line. A level of the map chosen by the
  receiver's slope as well as its distance would answer that, at up to four times the pages on such walls. Not done here;
  it is the first thing to try if those stairs matter more than their pages.
- `Contact.*` holds it to rays cast by the test, with the sun on every side; 13 of 16 faults planted in it were caught by
  those tests. The other three change no image: the map's guard and the length's, which save time (the resolve does not
  read the answer without the map, and a march of no length finds nothing), and the bound on the length, kept as a guard:
  a very long march's steps run to the sun's vanishing point, off the image.
