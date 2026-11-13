# 0095. A view in motion as sharp as standing: the history's surface, its filter, and its count

- **Date:** 2026-11-13
- **Status:** Accepted (built and measured; the user's report of 2026-11-13 that it answers, of ADR-0094's build:
  "the vine has not changed at all, it is not the leaves on the vine that is shimmering it is the stem's shadow that
  has the problem. The blur on the building that I mentioned earlier is better but still visible, I want it to be
  completly seemless"; of this build: "The fixes are very small for a sacrifice of a lot of speed ... I want you too
  keep it the way it is and test the speed in both scenes"; accepted with ADR-0090 to 0095, 2026-11-13: "commit step
  3 to 8")

## Context

The replay of the user's walk at the vine (`walk_bistro4.txt`), a frame in motion beside the same pose
standing (`gate1440/rig/soft.py`: the ratio of the two frames' mean absolute Laplacians of luma, a pixel apart
for the finest detail and two apart for the next):

- **A view in motion was softer than the same view standing**, by a fifth to a third of its finest detail and
  a tenth to a sixth of the next, at every speed: sprinting at the wall 0.76 and 0.87 over a window and its
  wall, 0.68 and 0.84 over the vine's stem.
- **Softest where the pixels crept.** With the camera stepping to and fro between two poses (frames two apart
  share a pose, each reached by the same motion), by the step's size: at a seventh to a quarter of a pixel a
  frame the finest detail was 0.68 to 0.69 of standing, at a pixel a frame and over 0.79 to 0.80. This is the
  ring ADR-0094 made fainter and left: a pixel's weight still took until half a pixel a frame to reach the
  moving one's, and a history of 8 to 32 frames, each read at a fractional offset, keeps little.
- **It came back slowly.** A still pixel's frame weighs 1/32, so the soft history a motion leaves was half of
  the picture for 22 frames after the camera stopped: detail a few pixels across came into focus each time
  the view came to rest and went again as it moved. The stem's shadow on the wall is a line a pixel or two
  wide: its shimmer is this, not the sun's rays (two frames at one pose in motion differ along the stems and
  slats, as a sharper frame differs from a softer one, and not along the leaves' shadows).
- **Where the softness comes from**, by taking one thing out at a time on the same replay: the moving pixel's
  clip matters little (the still pixel's box for every pixel: the window's 0.76 to 0.78); the filter
  matters more (a sharper cubic: 0.81, and 0.87 with the wider box, which no longer clips what the filter
  restores); the weight is the rest (1/2 in motion: 0.97, and half again as much change from frame to
  frame).

A history read at a fractional offset through a filter of response H at some detail, and blended with weight
w, settles at w / (1 - (1 - w) H) of that detail. At half a pixel's offset Catmull-Rom's H is 0.88 for detail
4 pixels across and 0.69 for 3: a history of 4 frames keeps 0.74 and 0.52, one of 16 frames 0.36 and 0.18.
Nothing but H near 1 or a short history keeps detail, and a gain over 1 anywhere compounds the same way (a
cubic of tension -1 with a history of 8 frames: the next-to-finest detail at 1.6 of standing).

## Decision

1. **The history's cubic is a little sharper than Catmull-Rom's** (`taa_history_cubic`,
   `TAA_HISTORY_TENSION` -0.7; Keys 1981). Its response is 2 to 3 % over 1 at its widest: with the moving
   pixel's 4 frames, the next-to-finest detail is what it is standing, to within 5 %.
2. **The moving pixel's weight is reached at a quarter of a pixel a frame** (`TAA_MOVING_SPEED`), still by
   the square of the speed: what creeps keeps what the rest keeps, and no longer history is there for the
   sharper cubic's gain to compound in.
3. **A history of the same surface is clipped as a still pixel's is, however it moved**
   (`taa_history_valid`). Each history pixel keeps the distance of the surface it showed, the nearest about
   it, whose motion its history follows (`taa_depth_key`: its reverse-Z depth as 24 stops in 16 bits). A
   pixel's point a frame ago (`vis_previous_point`: where it was, and how deep) is of the history's surface
   if its key lies within those of the four history pixels about it, 1.6 % of its distance either side: a
   surface's depths about a point span the point's, however aslant it lies. Then the box is three deviations
   wide and not held to the frame's extremes, which keeps what the filter restores and the detail a frame's
   samples miss. What a nearer surface has just uncovered finds only that surface's keys, and takes the
   moving pixel's box as before.
4. **A frame weighs no less than its share of the frames its history holds** (`taa_count_weight`). Each
   history pixel counts them (`taa_count_after`: 1 / weight after a blend, up to 32): 4 after a motion, so
   the frames after a stop weigh 1/5, 1/6 and so on, a mean, until the still pixel's 1/32. The same from a
   cut or a first frame.
5. **The state beside each history is R8G8B8A8** (it was R8, the unrest alone): the unrest, the count, and
   the key in two halves.

## Measured

2560 x 1440, RTX 3060 Ti, driver 610.74, Profile; Bistro, the user's walk at the vine.

Stepping to and fro, by the step, a window and its wall some 330 pixels from the view's centre, and the
vine's stem some 76 (the finest detail, the next, and how far two frames at one pose lie apart, of the mean
light; the speeds are from the step and the wall's distance, about):

| Pixels a frame (wall; stem) | Wall: before | after | Stem: before | after |
|---|---|---|---|---|
| 0.14; 0.03 | 0.69, 0.84, 1.4 % | 0.91, 1.02, 1.6 % | 0.78, 0.90, 2.7 % | 0.88, 1.01, 2.7 % |
| 0.28; 0.06 | 0.68, 0.84, 1.8 % | 0.93, 1.04, 2.4 % | 0.73, 0.87, 2.9 % | 0.86, 1.01, 3.0 % |
| 0.55; 0.13 | 0.74, 0.88, 3.4 % | 0.89, 1.05, 2.5 % | 0.70, 0.86, 5.0 % | 0.86, 1.01, 3.2 % |
| 1.1; 0.25 | 0.80, 0.91, 3.5 % | 0.94, 1.02, 2.5 % | 0.70, 0.86, 5.4 % | 0.83, 1.01, 3.2 % |
| 2.2; 0.5 | 0.79, 0.91, 3.5 % | 0.92, 1.03, 2.6 % | 0.70, 0.87, 5.5 % | 0.82, 1.01, 3.2 % |
| 4.4; 1.0 | 0.79, 0.91, 3.6 % | 0.92, 1.03, 2.5 % | 0.70, 0.86, 5.5 % | 0.80, 1.00, 3.2 % |

- The ring is gone: the finest detail is 0.89 to 0.94 of standing at every speed, where it was 0.68 to 0.80,
  and the next 1.02 to 1.05, where it was 0.84 to 0.91. From frame to frame the image changes less than it
  did from half a pixel a frame up, and by up to a third more below (1.8 to 2.4 % of the light at a quarter
  of a pixel a frame), where a history of 16 frames is now one of 4 to 8.
- Sprinting at the wall, the user's recording as it is: 0.85 and 0.97 over the window (0.76, 0.87), 0.76 and
  0.94 over the stem (0.68, 0.84).
- Stopping from that sprint: the finest detail over the window is 0.91 of the settled image 4 frames after
  the stop, 0.94 after 8 and 0.97 after 16; over the stem 0.91, 0.96 and 1.0.
- **What is left.** The finest detail, a pixel or two across, is still a tenth to a fifth softer in motion
  than standing. Bars 3 pixels apart (`Taa.a_view_in_motion_keeps_its_fine_detail`) keep 0.51 to 0.57 of
  their contrast in motion, where they kept 0.17 creeping and 0.43 fast: what 4 frames through a 4-tap
  filter can keep of them is 0.6.
- **Cost**, the 18 reference views against ADR-0094's: the pass 0.035 to 0.04 ms more (0.38 to 0.42 ms; a
  gather more, and the state's bytes), the frame 0.02 to 0.09. The state is 4 bytes a pixel written and read
  where it was 1: 11 MB more each way a frame.

## Alternatives considered

- **A heavier frame in motion** (1/2). As sharp as standing by this measure, and half again as much change
  from frame to frame (3.0 to 4.4 % of the light): the sharpness is the frame's own aliasing.
- **A sharper cubic still** (tension -1), or tension -0.75 with the history's length as it was. The
  next-to-finest detail at 1.2 to 1.6 of standing, most where the pixels crept: ringing.
- **Every history trusted in motion** (the wide box without the keys). What a nearer surface uncovers kept
  its colour for the frames its weight takes to fade wherever the wall behind had some pattern of its own
  (`Taa.what_a_near_thing_uncovers_takes_none_of_it`: 6.3 levels from the truth down a cube's side, 3.6 with
  the keys).
- **The previous frame's depth for the test**, which the frame no longer has by the resolve (the pyramid
  holds this frame's), or kept a frame longer at 4 bytes a pixel.
- **The detail a display-only sharpening would add back**, from the history's own taps. The taps' weights
  for it vanish at a whole pixel's offset, so it would come and go with the offset's phase, frame by frame.
- **A history at twice the resolution each way.** What would keep detail a pixel or two across in motion:
  a read's loss falls on detail finer than the display shows. Four times the history's bytes and the pass's
  pixels: by the pass's cost now, an estimate of 2 ms or more and some 100 MB a frame at 1440p. Not built;
  it is what is left to decide if the tenth to a fifth that remains is still seen.

## Consequences

- A view in motion keeps a shorter history at slow speeds than it did: what creeps (a quarter of a pixel a
  frame and under) changes more from frame to frame, by up to a third, and as much as fast pixels did before.
- A history's first frames, after a cut or a stop, are a mean: a flat colour's random rounding shows a level
  more for those frames (`Post.bloom_keeps_a_uniform_image` now lets the history fill before it compares).
- The state image is a mandatory format's now (R8G8B8A8), 4 bytes a pixel.
- `vis_previous_point` gives the depth a point had; `vis_previous_uv` is the same without it.
- The benchmark's twin of the resolve reads the history through the same cubic (`bench_taa.*`).
- Still open: the door frame's level past the arch (ADR-0094), the city's leaf shadows as a soft shade
  (ADR-0094), the park fence's far level, the cache's cells at creases.

## Tests

`Taa.a_view_in_motion_keeps_its_fine_detail` (fails with Catmull-Rom's tension, 0.49 and 0.43, and with the
weight's old speed, 0.25 creeping); `Taa.a_view_that_stops_is_sharp_again_in_a_few_frames` (fails without the
count: 8 frames after a stop a quarter of the way back, not three quarters);
`Taa.what_a_near_thing_uncovers_takes_none_of_it` (fails with every history trusted). The suite's others hold
as they did: a still view converges nearer its supersampled image in the same frames (0.39 levels, from 0.51),
a panning camera holds 0.60 (0.69). Development: render 193 + 192, runtime 96, cook 122, golden 9, asset 59,
editor 68, integration 18, no validation message; format, layering, the older glslang (130 shaders), gcc and
tidy clean.
