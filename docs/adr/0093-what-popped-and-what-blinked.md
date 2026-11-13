# 0093. What popped with distance and what blinked standing still: a sheet's outline, a level's hand-over, a pixel's unrest

- **Date:** 2026-11-12
- **Status:** Accepted (built and measured; the user's report of 2026-11-12 that it answers, with two recorded
  walks: "In Bistro ... the man hole and table are still flickering. Also, there are shadows that pop in and out of
  view as I move closer or further away. In the big-city scene ... The flicker of the far buildings still persist
  and I also see that some banners on the light posts pop in and out of view if I move closer or further away from
  them", and of the city's third street "part of the fence on the left side of the camera's view by the park is also
  flickering"; of this build: "The fixes you introduced are good ... The big city scene doesn't have flickering
  buildings and fences when standing still anymore and the banner that was popping in and out is fixed"; accepted
  with ADR-0090 to 0095, 2026-11-13: "commit step 3 to 8")

## Context

The user recorded both walks (`scene_viewer --record`), and the replays (`render_perf`'s probe, the recorded poses
a frame each) showed four separate things.

- **A banner gone at 30 m, its rods left.** The city's `PoleBanner` hangs two banners from a post. One is two
  quads back to back on four corners, each face its own vertices. No edge of it is open (every edge has its two
  faces' triangles), so the simplifier gave its outline no perpendicular plane (Garland & Heckbert's, which it
  gives open edges), and the faces' one plane does not see a corner go along an edge: two corners were of the
  seam kind, collapsed along the sheet's edges at no error, and the sheet was gone at level 1, whose error (1.4 cm)
  the view passes at 30 m. The other banner, three quads on its corners, was tangled, never collapsed, and
  stayed. A mesh that may voxelize counts no border's move either (ADR-0069), which would have caught it.
- **Far fronts and a park fence blinking under a still camera.** At the user's three street stops a 32 x 32
  patch of far windows or of the fence changed by 11, 21 and 43 % of its light over 8 frames. The geometry was
  the same every frame (the base colour view: 0.00 %). A pattern at the pixels' own pitch (a fence's bars a
  pixel apart, a far front's window frames) puts every sample of a pixel's 3 x 3 neighbourhood on a bar in one
  frame and between two in the next, as the jitter has it: the neighbourhood is flat either way, its box a
  point however many deviations wide (ADR-0092's three), and the history clipped to it followed each frame.
- **Shadows stepping on Bistro's fronts.** The sun grazes the fronts the user walked to and from. A shadow
  map's texel lies stretched down such a wall (ADR-0080), a shadow's edge on it steps by the texel, and the
  level a pixel reads changes with its distance: at each boundary the texel doubled at once, and the edges of
  the shadows of window heads and cornices jumped by a hand's breadth.
- **The manhole and the table**, dark glossy surfaces: ADR-0092's ray share by roughness, which the user had
  not yet seen built.

## Decision

1. **An edge whose other triangles all fold back on a triangle keeps its place as an open edge does**
   (`cluster_simplify.c` `build_quadrics`, `FOLD_COSINE`): each triangle on it adds the plane through the edge
   square to itself, weighted as an open edge's. Folded back: on the same side of the edge within 10 degrees,
   whichever way it faces (a sheet's other face, a face given twice, a blade's). A triangle whose corners are
   the edge's alone is no surface and carries none on. `COOK_MESH_VERSION` 10.
2. **A still pixel keeps its unrest, and its history's box is as much wider** (`taa_common.glsl` `taa_unrest`,
   `taa_clip_unrest`; an R8 image beside each history, `RenderTaa.unrest`): how far the luma of its frames'
   filtered samples has lain from its history's, as a share of the larger, followed over about 8 frames; the
   clip's box grows each way by 3 times that share of its light, so from a third up the history is not clipped
   and holds the frames' mean. Only where the history moved less than 1/16 of a pixel (`TAA_UNREST_SPEED`): a
   moving surface's pixels write none, so what it uncovers is clipped as ever and nothing trails it, and a slow
   pan clips as before (at 1/3 of a pixel a frame an unrest kept trailed the edges).
3. **A shadow map's level blends with the next over the last quarter of its range** (`vsm_common.glsl`
   `VSM_BLEND_BAND`, `vsm_level_light`): within a quarter of a level below its boundary the shading also reads
   the next coarser level's page and mixes the two, wholly the coarser at the boundary, which the pixels past
   it read alone. The requests ask for that level's pages in the band (`vsm_request_coarser`, a little wider
   than the shading's: `VSM_BLEND_Q`), for a 2 x 2 block as one request by the rule of its own level's
   (`vsm_block.glsl`); where the page is not there yet the shading keeps its own level's light.

## Measured

2560 x 1440, RTX 3060 Ti, driver 610.74, Profile; the replays of the user's recordings.

| What | Before | Now |
|---|---|---|
| The banner at the user's post, 31 m | one of two drawn | both, at every distance of the walk |
| City street stop 1, the hottest 32 x 32 patch over 8 still frames | 11.1 % | 1.8 % |
| City street stop 2 | 21.4 % | 2.0 % |
| City street stop 3 (the fence; the far fronts 25 %) | 42.6 % | 2.6 % |
| Bistro, the user's two manhole poses, the hottest patch | (ADR-0092's) | 1.4 % and 2.0 % |
| Bistro, walking over the manhole, two frames at one pose (against 494ea99) | 0.55 to 0.87 % | 0.12 to 0.17 % |

The city recooked in 174 s from the last cook's cache: six meshes' levels changed (the four `PoleBanner`s, a
building and a lamppost; others with such edges kept theirs). The banner mesh's group that holds its sheets is
now made at an error of 37 cm (the collapse it still makes to reach its target, at its true cost) where it
claimed 1.4 cm, so its level 0 draws to the cells' proxies; its voxels are 4 where they were 74.

**Cost.** The 18 reference views, a run each: a mean of -0.01 ms against ADR-0092's (Bistro -0.14 to -0.23 ms
but for `street_turn`, +0.42; the city -0.11 to +0.18; the interior within 0.05): none that the runs resolve.
The unrest is a byte a pixel written and read once a frame (3.7 MB at 1440p, 8.3 MB at 4K, 16.6 MB of traffic
there). The band's pixels read a second page, and their views ask for the next level's pages, a quarter as many.
On a wall the sun grazes the blend gives back a little of ADR-0080's level: of what a ledge hides, 6.6 % lit
where it was 6.0 (`Contact.a_wall_the_sun_grazes_takes_a_finer_map`; a band of half a level left 10.1 %, the
distance's level alone 13.7).

## Alternatives considered

- **The sheet's corners never to collapse** (tangled, as the banner of three quads is). The same image for this
  mesh, and its group's error stays 1.4 cm; but a sheet's outline with more corners than four then never
  simplifies, where along a straight edge a corner may go at no cost, which the planes allow.
- **A level's error for meshes that may voxelize**, so that a group stops short of a collapse far dearer than
  its others (ADR-0069's limit, which they do not take). It would keep the banner's group at its old error, and
  changes every such mesh's levels, the city's whole balance of triangles and voxels. Its own work.
- **A softer clip for still pixels** (the history pulled a share of the way to the box each frame). What a
  moving object uncovers then trails for as many frames, ten pixels a frame behind a car.
- **The clip's box from two frames' neighbourhoods.** Two of the jitter's 16 phases; a patch can read the same
  for two frames running.
- **A wider, smoother shadow filter** (4 x 4 texels). The steps of a grazed wall's shadows would soften; a
  page's border has no texels beyond it, so the filter needs its neighbours' pages or a seam every 128 texels.
  Not built: what the user saw was the jump, which the blend takes.
- **The band straddling the boundary**, so that no pixel reads coarser than before: the finer level's pages
  over a quarter level more, four times the pages for a quarter of the pixels.

## Consequences

- Meshes recook (version 10); the city's pack differs by 8 kB.
- A light that changes on a still pixel whose samples have been apart from its history (a fence, a grating, a
  noisy reflection) is followed over the history's 32 frames rather than at once; on a steady pixel as before.
- Still to do, seen in the same replays: the shadows' steps down a grazed wall themselves (ADR-0080's); a
  pattern at the pixels' pitch still crawls while the camera moves (the unrest is a still pixel's); the park
  fence's far level has holes (bars collapsed to slivers); and the radiance cache's cells at the creases of
  Bistro's fronts (dark speckled bands under ledges and about window frames, whose width follows the cells'
  level).

## Tests

`ClusterLod.a_sheet_of_two_faces_keeps_its_outline` (cuts within a quarter of the sheet's short side draw both
faces whole); `Taa.a_still_grating_at_the_pixels_pitch_holds_between_frames` (a pixel wanders by 1.5 levels; 71
with the box as it was); `Taa.a_moving_object_leaves_no_trail_on_a_grating`;
`Vsm.a_shadow_holds_where_its_level_hands_over` (two views 0.8 % apart across a boundary: no pixel differs by
40 levels; 40 pixels unblended); `Vsm.block_requests_are_each_pixels` holds the band's requests to each pixel's.
Development: render 189 + 188, runtime 96, cook 121, golden 9, no validation message.
