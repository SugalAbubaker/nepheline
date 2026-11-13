# 0091. The radiance cache while the camera runs: a table that makes room, cells of several sizes, and light that holds between frames

- **Date:** 2026-11-12
- **Status:** Accepted (built and measured; the user's report of 2026-11-11 that it answers: "when I move quickly
  towards the arch in Bistro ... I see the flickering. When you are moving around you need to hold shift and move
  quickly through the scenes to see the flickering. Same with the busses with shade in the big city and also the
  trees. Also, the concentric ring effect around the camera is still present and as you move closer or further away
  from a object you see the model flickering ... I am looking for a completely smooth image as I move around the
  world. The indoor area in bistro after turning on the exposure is also very noisy"; of this build: "The flickering
  is almost completely gone", with what ADR-0092 answers; accepted with ADR-0090 to 0095, 2026-11-13: "commit step 3
  to 8")

## Context

ADR-0090's filter made the indirect light of a still or walking camera smooth. At a run (the viewer's 16 m/s) the
user still saw it flicker, walls near the camera worst. A camera path through Bistro's arch at that speed, drawn
twice at each pose so that the two frames differ only by what one frame's samples did, gave the measure: with the
cache off the two differ by 0.24 to 0.70 of an 8-bit level on average (the anti-aliasing's jitter and the
reflections); with it, by 0.5 to 9 levels, and by up to 15 before ADR-0090. Three causes, in the order they were
found:

- **The table was full.** A cell was 1/96 to 1/48 of its distance from the camera across at any distance:
  millimetres at a wall the camera passed. Each had a ray or two in its life, the camera's next step made new
  ones, and each kept its slot for 240 frames. Running past a wall filled the 2^18 slots within frames, after which
  the frame's own samples found none: 2.3 million of a 3,356-frame path's samples were dropped, up to 27 % of a
  frame's. The wall's pixels then found no cell and took the sky's light, or a cell without samples and took none:
  the wall broke into white and black squares, in every build back to ADR-0039, at any speed that outran the
  cells' age. Nothing counted the dropped samples.
- **A new cell showed its own few samples.** With 65,536 rays a frame a cell facing the camera has 3 to 10 samples
  a frame and one seen at a low angle under 1. ADR-0090 held a cell to be its mean plus what the filter added, so
  that a change of the light was followed at once: each frame's samples moved a young cell's mean by a tenth or
  more and what was shown with it, and its neighbours', as young, did the same. A cell's parents (ADR-0090: the
  next level's cells, which take the same samples) counted only by samples of their own, and at a run they were as
  new as the cell.
- **The filter's own steps are slow where the samples are few.** A cell's samples move what it is held to be by
  their share of what is known, a few percent a frame where light arrives by few paths. Begun with a wrong light,
  a region kept it for a second.

## Decision

- **No cell is finer than level 9** (`NPH_RENDER_GI_LEVEL_MIN`): 4.2 cm, for everything within 4 m. The light the
  cells hold has nothing nearer than the ambient occlusion's radius in it (ADR-0039) and does not vary over less.
  Near surfaces then keep their cells as the camera moves along them, and a cell there has dozens of rays a frame.
  (The reflections' cells keep their own levels.)
- **A full bucket's oldest cell gives its slot up** (`gi_claim_cell_within`): the cell no sample has reached for
  longest, if it had none in the last frame (`NPH_RENDER_GI_EVICT_AGE`) and has none in this. Whoever swaps the
  old cell's word for none has the slot; an invocation that loses the swap looks again, since several of a
  frame's rays land in one new cell and all but the first find it made. Only the frame's own cells take slots
  this way, not the cells the rays make where they land (ADR-0090). An invocation that found the old cell before
  the swap may still add a sample to the new one: one sample of another place, in a cell whose first samples
  count for little.
- **The dropped samples are counted** (`NphVisCounters.gi_dropped`, `NphRenderFrameStats.vis_gi_dropped`;
  `render_perf` prints them with the reflections').
- **Every sample goes to five cells**: its own and those of the next four levels around its point
  (`NPH_RENDER_GI_ABOVE`), twice to sixteen times as wide, with four to 256 times the samples a frame (those from
  the second level up take 512 a frame at most, the first to arrive: a cell's sums hold that many of the
  brightest). Some cell around every point then knows its light within a frame or two of the frame coming to it.
- **A cell's history is 1,024 samples or 64 frames' samples, whichever is more**
  (`NPH_RENDER_GI_HISTORY_FRAMES`). The wide cells take hundreds a frame: a thousand samples' window would be a
  frame or two of them, and each frame's would move their light by as much as a frame's samples are out. A
  change of the light cuts a long history to a quarter of 1,024 at once, as before. The whole count is kept
  beside the cell's moments (24 bits); the cell's word counts as far as its 11 bits go.
- **The filter steps from what each cell was held to be**, not from its mean (`held`, `held_before` in place of
  what the filter added). A cell's samples move what it is held to be by their share of what is known of its
  light, a / (a + b + Σg) a frame. Where the resolve's test finds the light changed (ADR-0084), what the cell is
  held to be goes with its mean for that frame and the six after (`NPH_RENDER_GI_FOLLOW_FRAMES`), while the mean
  comes the rest of the way.
- **A cell with fewer than 256 samples leans on its parents' own means** for what it lacks of them, as one
  neighbour at most (b). Each level above gives its cells' means as far as their samples tell their light (128
  of them, `NPH_RENDER_GI_PARENT_KNOWS`) and leaves the rest to the level above it: in a view's first frame that
  is mostly the third and fourth level, a few frames later the first and second, and then the cell's own samples.
  Their means, not what the filter holds them to be: a mean takes each frame's samples for what they are worth,
  so the regions' light is right as soon as their samples are, where cells that leant on what the filter held
  kept the light they began with (below). The parents are looked up in a cell's first frame and one frame in four
  after, and kept between (`cell_keys` holds two vectors a slot now: 136 bytes a slot in all).
- **The pixels' lookup tries all five levels** where the nearer hold nothing (`gi_surface_blend`, and its twin on
  the CPU), before the environment's light.

The band through which a cell goes over to its parents' light at its level's end (ADR-0090) stays, now a part of
what the frame looks up and not of what the cell is held to be.

## Measured

2560 × 1440, RTX 3060 Ti, driver 610.74, `render_perf`'s probe along recorded paths.

**Two frames at one pose** (8-bit levels, the mean over 32 × 32 blocks; the cache off gives 0.24 to 0.70):

| Path | Before ADR-0090 | ADR-0090 | Now |
|---|---|---|---|
| Bistro, through the arch and back at 16 m/s, 29 poses | 0.39 to 14.9 | 0.46 to 9.0 | 0.21 to 0.58 |
| Bistro's interior, the user's walk at twice its speed, automatic exposure, 19 poses | 0.55 to 3.0 | | 0.26 to 1.04 |
| The city, the user's walk past the bus at four times its speed, 28 poses | 0.21 to 1.41 | | 0.14 to 0.83 |
| The city, along the park at 16 m/s, 18 poses | | | 0.40 to 0.67 |

What is left in the interior is on the glossy surfaces (the reflections' rays and the lamps' highlights), not in
the cache's light.

**Dropped samples**, the arch path's 3,356 frames: 2,345,322 (17,637 at most in a frame) with the level floor
alone; 27,649 (616 at most) with the eviction where an invocation that lost the swap gave up; none where it looks
again. None on the other three paths, nor in any of the 18 reference views.

**Against the truth** (`test_gi.c`, rays cast on the CPU): under a roof that leaves 18 % of the sky open, 0.1790
against 0.1789, 2.8 % a pixel (ADR-0090: 3.7 %); 2 % open, 0.0206 against 0.0206, 7.4 % a pixel (8.9 %); the same
roof reflecting 0.7, within 8 %. Shade seen from 6 to 9 m at a camera 0.6 m up (so low an angle that most cells
have no ray in a frame) is within 3 % of the truth after 48 frames, where leaning on what the filter held left it
13 to 31 % too bright. A view never seen before: its first frame's shade 8 % above the settled view's, a pixel
21 % out (begun with the cells' own means: 52 % out). A doubled sky is followed 69 to 75 % of the way in 4 frames
and 97 % in 16 (the cells' means themselves are 97 % there).

**Cost**, the reference views, one round against ADR-0090's: 0.11 to 0.25 ms a view more, 0.17 on average (Bistro
0.14, the city 0.15, the interior 0.23): the filter 0.47 ms where it was 0.29 on the arch path, the update 0.54
where it was 0.47. With ADR-0090 that is 0.6 to 0.9 ms a view over ADR-0089. The cache is 34 MB at 2^18 slots. 1 %
lows past the gate (6.94 ms, ADR-0082): Bistro's street 6.96, street walk 7.17, street turn 7.84 and terrace 7.62,
the city's park 7.18 and rooftop 9.62. Nothing is optimised yet; the budgets wait for the user's word.

## Alternatives considered

- **A larger table.** Eight times the slots still filled on the arch path (the cells of 240 frames at a run are
  20 million), and the resolve and the filter, which visit every slot, cost 1.6 ms more.
- **Cells freed sooner, by an age that follows how full the table is.** A loop that lags the frames that need the
  room, where a bucket's oldest cell giving way needs none and keeps every cell as long as there is room.
- **The parents' light as the filter holds it, for good or while a cell is new** (two forms tried: as long as the
  cell's samples are fewer than its noise asks, and for its first 32 to 96 steps). Shade beside open ground came
  out 14 to 29 % too bright, and 13 to 31 % from the low camera above: not, as first taken, because the wide cells
  reach over the lit ground (their own means were within 4 % of the truth up to the third level), but because
  what the filter holds follows its samples slowly by design, and the cells kept the light their first frame
  gave them.
- **Each cell blended into its parents' light across its whole level**, as a texture's levels are, against the
  rings. The parents' light then stays in every cell for good: under the two roofs 3 and 15 % too bright.
- **Steps from the mean plus what the filter added (ADR-0090), with the pyramid.** The parents were then quiet but
  each young cell's own mean still moved what it showed: the arch path's worst poses stayed at 8 levels.
- **A fixed share of the samples to the wide cells** (one in four, one in sixteen), against the 512 a frame their
  sums hold. They would have no more samples a frame than the level under them, which is what they are for.

## Consequences

- At a run the cache's light is as steady as with the camera still, and the walls a camera passes keep their
  cells. A surface new to the frame is lit at once, smoothly, a tenth or so out where lit ground lies beside its
  shade, and comes right within frames.
- A cell near the camera is 4 cm across whatever its distance. Creased thin geometry within a few centimetres of
  itself (an awning's rumpled cloth) shows the octants' seams there more plainly than under the noise that hid
  them: facets whose normals fall either side of an axis read a little lighter or darker than their neighbours.
- The cells of a view left behind are the first to go when the table is full: turning back to it at a run finds
  its nearer cells new (and lit from their parents).
- A change of the light reaches what the cells show as fast as it reaches their means on the frames the test
  fires and the six after, and by the filter's steps otherwise: the last few percent of a change take some tenths
  of a second where they took frames.
- 0.17 ms a view to win back with ADR-0090's 0.53: the parents' slots kept between frames instead of found
  again, the levels above the second given a share of the rays' samples where the first two know their light,
  the filter's passes over free slots skipped.
- The interior's lamps: their light beyond 25 cm comes through the cache (ADR-0073), soft and without shadows of
  its own, and within 25 cm from the pixels, unshadowed: a bright ball about each lamp on what is near it. With
  the lamps off the room is nearly black, so the cache does carry their light; shadows and pools of their own
  need the reservoirs' direct light (`NPH_RENDER_RESTIR_ALL`, ADR-0079), which is grainier and costs more. Not
  changed here.

## Tests

`test_gi.c`: `a_new_view_is_lit_from_its_first_frame` (fails with no parents: 52 % a pixel);
`a_moving_cameras_frames_hold_their_light` (1.3 % of the light between two frames at one pose after 24 moving;
4 % where what is held goes with the mean every frame); `a_full_table_makes_room_for_the_frames_cells` (120
frames a metre apart: 24 of 1,966,080 samples dropped; 176,707 without the eviction);
`a_surface_entering_a_finer_level_keeps_its_light` now against the truth's level; the history tests read the
cells' whole counts and allow 64 frames' samples; the level test has the floor; the tests of which cells the
cache holds (`test_gi.c`, `test_vis_voxel.c`) expect the four levels above. `a_cell_follows_a_change_of_its_light`
asks for the new light within 4 % after 16 frames where it asked for 3 %: the cells' means are 97 % there.
