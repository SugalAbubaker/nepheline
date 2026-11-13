# 0094. Glass seen through, blended surfaces that cast nothing, and the sun's shadows traced where a view asks

- **Date:** 2026-11-12
- **Status:** Accepted (built and measured; the user's reports of 2026-11-12 that it answers, each with a recorded
  walk: "The shadows on the building in Bistro still has a shadow that apears when going up close to it. I don't
  know why that shadow is even appearing since the window blind has its own shadow behind it. That also led me to
  finding a shadow of a vine that flickers ... the interior is too grainy, remove the shadow direct in the interior.
  Is it possible for you to make the glass window in that Bistro scene transpart so we can see how the light flows
  through it?"; and, of the first build of this: "The correct shadow is more important than speed, once we get the
  shadows correct we can then work on getting it faster ... it did not fully fix the vine's shadows, they still
  flicker when moving towards and away from the vines and also while moving the mouse back and forth. The shadow is
  not a soild shape that is representative of the vine, it has a wave effect when the camera is moved. I also found
  another shadow that pop's in and out that is ontop of a doorway after passing the arch. Also ... a building that
  has a slight bluring effect that moves over it when I move back and forth"; and of the second: "Not only are the
  vine's showdows still wrong, now I'm getting many shadows that are popping in and out of frame on different area's
  on the map. Ontop of the issue not being resolved, the engine is now slower"; and, with both to look at in both
  scenes: "make the shadow map the default and keep rays as an option", "Decided" below; accepted with ADR-0090 to
  0095, 2026-11-13: "commit step 3 to 8")

## Context

The replay of the user's walk (`walk_bistro3.txt`), with the sun's rays (`--rt-shadows`) beside the map:

- **The shadow by the shuttered window was a decal's.** A dirt decal, a blended sheet a hair before the wall,
  was a caster like any mesh: in the map it shadowed the wall under it wherever a texel was finer than the hair
  and the receiver's offset shorter, from near and not from far (a parallelogram that came with the level); in
  the rays it shadowed the wall always.
- **The "blinds" were the glass.** Bistro's glass is `KHR_materials_transmission` on a white base, and the cook
  drew such a material as the extension's fallback has it: opaque. Every window was a white wall that the sun
  lit, that shadowed what stood behind it, and that nothing was seen through; the rays alone passed the meshes
  that were all glass (ADR-0078).
- **A vine's shadow was no shape of the vine's.** On one wall the sun grazes at 3 degrees: a texel of the map
  lies stretched across a surface by 1 / N.L, and a level finer (ADR-0080) brings it to no less than twice a
  pixel, there ten: a stem's shadow down it was a row of steps with light between them, which each level of the
  map steps elsewhere, ADR-0093's blend or not. On the wall the user walked to next the sun stands higher, and
  the vine's shadow was still streaks: what stands a few centimetres off a surface is under the receiver's
  offset in the map, and its shadow was the contact march's to find, 24 pixels of it and jittered, a shimmer
  wherever the camera moved or turned. Against the sun's rays neither was the vine's shape.
- **A building softened in a ring as the camera walked towards it.** The temporal resolve's weight grew evenly
  with a pixel's speed, 1/32 standing to 1/4 at a pixel a frame; every frame that reads the history at a
  fractional offset softens it, and a pixel creeping a quarter of a pixel a frame kept 12 frames of such reads.
  About the point the view heads for the pixels stand still and are sharp, far from it they move fast and keep
  4 frames; between, a ring of the softest, which moved over the building as the walk turned.
- **The shadow over the doorway past the arch is the door frame's level of detail** (found, not fixed: below).

## Decision

1. **A blended surface casts no shadow and stops no ray.** The map's cull passes its meshlets by
   (`vsm_cull.comp`); the top level marks an instance with a MASK or a BLEND material as not opaque, and a
   candidate of a blended material does not stop its ray (`rt_instances.comp`, `rt_mask.glsl`). Glass and
   decals: light passes them as the eye does. (The forward path's own shadow rays, forced opaque, still stop
   at them.)
2. **Glass is a blended material that keeps what it does not let through** (`gltf.c` `convert_material`,
   `GLTF_GLASS_TRANSMISSION`): a material that lets half the light through or more is cooked BLEND and
   `NPH_MATERIAL_FLAG_TRANSMISSIVE`, its base colour and alpha scaled by 1 - the share it lets through.
   Blended surfaces composite with their colour multiplied by alpha in the shader (the pipeline's source
   factor is one), so that glass (`NphRenderMaterialDesc.transmissive`) adds what it shades whole, its
   reflection with it, and covers what is behind it by its body's share and what its surface turns back
   (Schlick's 4 % head on): `forward_frag.glsl` `forward_blended`. `COOK_MATERIAL_VERSION` 1 recooks
   materials alone.
3. **A view may ask for the sun's shadows by rays, where its frame has the rays' scene**
   (`NphRenderView.sun_rays`, `r.sun_rays`, off by default: "Decided" below; `ao_pass.glsl` `AO_RAYS`,
   `ao_rt.comp`; `RenderAo.frame_grazed`): the radiance cache's, or a light's. The occlusion's pass, which
   marched each pixel towards the sun, traces a ray for every pixel the sun faces instead, to the sun's
   centre. The denoise gathers the rays of the 5 x 5 pixels about a pixel on its plane (`ao_denoise.comp`
   `k_sun`) into the sun's light there: an edge two pixels soft and the same every frame. It goes beside the
   occlusion (the image is now R8G8: the occlusion, and the sun's light in 254 steps, 255 for none), and where
   it is there the resolve does not read the map. No resolve gains a ray query. The map and its march stay
   for frames without the rays' scene, for blended surfaces and for the cache's far cells; its pages are
   still asked for and drawn. Two things the rays need to be right (the user's "shadows that are popping in
   and out"):
   - **A pixel that shows a stand-in finds its mesh first.** The rays' scene holds each mesh's level 0; a
     pixel may show a coarser level, voxels, or a cell's proxy, a surface within the view's error of the mesh
     and not on it. The meshlet cull marks such clusters (`NPH_VIS_CLUSTER_COARSE`: the group that made the
     cluster has an error, or it is a voxel cluster or a proxy's), the pass reads the mark through the
     visibility buffer, and a marked pixel's own ray finds the mesh within the error either side of what is
     shown; the sun's ray starts where it does. Pixels that show the mesh itself, most of a view near the
     camera, trace the one ray.
   - **A ray starts off its surface by a share of the point's distance from the camera** (`AO_RAY_SLACK`,
     2^-14, a tenth of a pixel's width; at least an eighth of a millimetre). The rays' own offset
     (`nph_offset_ray`) counts units in the last place of the point's coordinates in their scene's space,
     about an anchor the camera may have left by 16 m; a point that comes from a depth 50 m from the camera
     is not that exact.
4. **The viewer no longer asks for the lamps' direct light from the reservoirs** (ADR-0092's bullet; the
   user: "too grainy").
5. **The temporal resolve's weight grows with the square of a pixel's speed** (`taa_current_weight`), from the
   still pixel's 1/32 to the moving one's 1/4 at half a pixel a frame: what a fractional read softens grows with
   the square of its offset and a history of 1 / weight frames is read that often, so a slow pixel's history
   is no softer than a fast one's.

## Measured

2560 x 1440, RTX 3060 Ti, driver 610.74, Profile; the replays of the user's recordings.

- The decal's parallelogram is gone at the user's poses, near and far.
- Bistro's windows show the curtains and rooms behind them, its doors the interior; from inside, the street.
  At an indoor exposure the room is lit as before and its glassware is glass (the scene's own exposure, set
  for daylight, leaves the interior some 6 stops under: what the camera does indoors is still to decide).
- The vine's shadow, at a moment of the user's mouse sweep, beside the map's and beside a ray a pixel to the
  sun's centre (`--rt-shadows`): the rays' shape, its edges soft where the map's was streaks
  (`n6_vine_cmp.png`). It does not depend on the camera.
- `Contact.a_wall_the_sun_grazes_hard_takes_the_suns_rays`: of 3,881 pixels a ledge hides on a wall grazed
  at 4.6 degrees, none lit (the map alone: 159).
- The building: its sharpness (mean absolute Laplacian over mean light) in a region of the replay rose by
  about 3 % while the camera walked and is as it was standing; the ring is what went, which a region's mean
  does not show.

- **A level's surface and its mesh.** On the user's walks, each frame beside the same frame with every mesh
  at level 0 (`--lod-error=0`), by the 8 x 8 blocks much darker in one: with the ray from the level's surface,
  patches of shadow on the awnings that came and went with the levels; with the pixel's own ray first, one
  block at most in any of 67 frames of one walk, and what differs in the other is geometry (a cable's
  width). `Contact.a_coarser_level_takes_no_shadow_of_its_own_mesh`: a cloth in folds drawn at 39 of its
  18,432 triangles, 11 of 7,923 lit pixels much darker than at its own level; without the pixel's ray,
  4,334.
- **Ground that shadowed itself.** In the city, walking, a patch of sunlit road 30 to 50 m off, frame by
  frame without the temporal resolve: its light went up and down by up to 8 % from one frame to the next,
  whole triangles of the road dark in one frame and lit in the next (the anchor, by the walk's poses, 15.7 m
  from the camera and 1.7 m over the road; standing at the anchor, or with the scene rebuilt about the
  camera every frame, the patch was steady). With the slack it stays within 1.5 % over the same frames, as
  with the scene rebuilt every frame, and the frames beside the map's show the same shadows on the road.
  Which arithmetic loses the place was not found: no scene made for a test shadowed itself this way
  (`Contact.ground_level_with_the_rays_anchor_does_not_shadow_itself` holds the rays to their place once
  the camera has left the anchor, and passes without the slack too).
- **The vine**, twelve frames in a row at three places of the user's walk: the leaves' shapes, the same in
  every frame, walking and turning.

**Cost**, the 18 reference views, a run each, against ADR-0093's:

| Scene | Frame | Of which the sun's rays (`ao`) | Their gathering (`ao denoise`) |
|---|---|---|---|
| Bistro (5 views) | +2.7 to +3.4 ms (mean +2.9) | +2.8 to +3.5 | +0.18 |
| City (7) | +1.3 to +4.0 (mean +2.8) | +1.3 to +3.7 | +0.19 |
| Interior (6) | -0.01 to +0.05 | none (no sun) | none |

Bistro's `street` is 8.8 ms and the city's 7.7 (its `rooftop`, all stand-ins, 12.5): the gate's 6.94 is not met
by any view with a sun, and the rays are what to win back (below). Against the build before the stand-ins'
second ray, the pass is 0.4 to 0.65 ms more in Bistro, and in the city within 0.6 either way but for
`rooftop`'s 2.7; with the second ray for every pixel, as first built, it was 1.9 to 2.3 more in Bistro. The
resolve is 0.01 to 0.17 ms faster for the map's lookups it no longer makes. Glass costs the
forward pass what its smooth pixels' reflection rays cost before (+0.12 to +0.20, -0.19 to -0.27). Traced only
where the sun grazes a flat surface (N.L under 0.125), as first built, the rays cost Bistro 0.3 to 0.8 ms.

## Decided: the map is the default, the rays an option (2026-11-13)

The user, of the build with the rays everywhere: "The fixes are very small for a sacrifice of a lot of
speed"; then, having asked why rays cost so much more and whether they could reach the gate without losing
anything (not by a cheaper ray: by far fewer, reused from frame to frame or left out where the answer is
sure, neither built nor promised), and having looked at both in the viewer in Bistro and in the city: "make
the shadow map the default and keep rays as an option".

Both ways from one build, 2560 x 1440, two runs each (the mean of the means, the worse 1 %-low), in ms:

| View | Rays | Map | |
|---|---|---|---|
| Bistro street | 8.78, 9.52 | 6.11, 6.89 | -2.6 |
| Bistro street, walking | 8.91, 9.74 | 6.28, 6.97 | -2.8 |
| Bistro street, turning | 9.77, 11.69 | 6.42, 8.00 | -3.7 |
| Bistro terrace | 10.15, 10.96 | 6.84, 7.58 | -3.4 |
| Bistro hedge | 8.77, 9.60 | 5.77, 6.44 | -3.2 |
| City street | 7.67, 8.42 | 5.24, 5.91 | -2.5 |
| City street, walking | 7.68, 8.52 | 5.41, 6.33 | -2.2 |
| City street, sprinting | 7.68, 8.53 | 5.55, 6.34 | -2.2 |
| City street, turning | 8.71, 10.58 | 5.59, 6.53 | -4.1 |
| City crossing | 6.92, 7.72 | 5.67, 6.45 | -1.3 |
| City park | 10.16, 11.07 | 6.65, 7.52 | -3.6 |
| City rooftop | 12.49, 13.79 | 8.65, 9.56 | -4.2 |

With the map the sun's shadows cost some 0.3 to 0.4 ms a frame in Bistro (its passes 0.12 to 0.20, drawn
only where the map changed; its lookups in the resolve 0.15; the march), with the rays 3.0 to 3.6: about
three million rays a frame, each from scratch, at about a nanosecond. Against the gate's 6.94 ms the map
leaves seven of the twelve views within, one 0.03 over, and the rays none.

Standing, the two pictures differ on Bistro's vine wall, where the rays draw the leaves' shadows and the
map faint streaks, and little elsewhere in Bistro; in the city the map's leaf shadows on pavements and walls
are each leaf's and the rays' a soft shade (the gathering of 5 x 5 pixels), and nothing was found that the
rays show better. In motion the map's faults of the Context stand as they were with it: a thin caster's
shadow on a grazed wall changes as the camera moves.

So `NphRenderView.sun_rays` is 0 from `nph_render_view_default`, `r.sun_rays` false, and `render_perf` and
`scene_viewer` take `--sun-rays`. Everything else of this record stands with either: blended surfaces cast
nothing, glass is seen through, the temporal resolve's weight. The rays' two corrections (a stand-in's own
ray first, the slack off the surface) are theirs alone.

## Alternatives considered

- **The ray in the resolve** (its `*_rt` variant in every frame with the cache's rays). Built first: +0.4 to
  +1.1 ms on every view with a sun, most of it the variant's own (the resolve's registers), traced or not.
- **Rays only where the sun grazes a flat surface** (N.L under 0.125, the depths about the pixel on its
  plane). Built second, 0.3 to 0.8 ms: the pixels beside a stem or a leaf kept the map's answer, a rim that
  moved with the camera, and the wall the user looked at is not grazed. The user: "it did not fully fix the
  vine's shadows".
- **A point of the sun's disc a pixel, by its place in a 5 x 5 lattice** moved each frame, the 25 gathered
  into a penumbra as wide as the sun makes it. Built third: where a shadow is thin the lattice shows in its
  edge, a zigzag that crawls with the camera (the user, of that build: "the vine's showdows still wrong").
- **A ray to the sun's centre, ungathered.** Every edge a pixel's step, and a gap of light a pixel wide a row
  of dashes that crawls with the camera.
- **A point of the disc drawn at random a pixel.** The mean of 25 is a tenth off in a penumbra's middle, each
  frame differently: grain in every soft shadow in motion.
- **The ray lifted off a level's surface by the view's error.** It clears the folds, and moves every
  shadow's edge by the lift over N.L: on the grazed wall of the test, 179 of 3,881 hidden pixels lit.
- **The pixel's own ray for every pixel.** 1.9 to 2.3 ms in Bistro, most of it for pixels that show the
  mesh itself.
- **A ray only where the map's own answers about the pixel disagree** (near a shadow's edge). A stem thinner
  than a stretched texel is missed by the map altogether at one level and found at the next: the map does not
  know where it is wrong.
- **Decals and glass flagged in the source** (`no_rays`, a `no_shadow` beside it). It is the material that
  says light passes, in every scene, and a mesh of glass and frame together has both.
- **Dropping the glass** (the user's fallback). The room would be lit the same, without its windows.

## Consequences

- Materials recook (seconds); Bistro's pack is recooked. Its glass meshes with other materials too (a window
  and its frame) pass rays now where they stopped them.
- A blended surface that should shadow (smoked glass, a thick tint) does not: there is no partial shadow.
- Where a view asks for them, the sun's rays cost 1.3 to 4.2 ms where the map costs its lookup. To win back,
  should they become the default again, in the order of what they
  promise: the rays in a pass of their own, in rows of pixels and not the occlusion's 16 x 16 groups with their
  shared depths; no ray where a coarse level of the map and its neighbours say all lit or all dark by a
  margin; the map's pages not asked for where rays answer (0.2 to 0.4 ms of its passes); a ray for two pixels
  in four with the denoise filling in.
- Every shadow's edge is 2 pixels soft (the 5 x 5 gathering), whatever cast it and from how far: a caster
  close to its surface is softer than the sun makes it, and one far off sharper. Leaves' shadows on a
  pavement, whose light and dark are a few pixels across, are a soft shade where the map drew each leaf
  (the city's trees, the picture the user had called right): what the sun does to shadows cast from metres
  up, and not what was there. A penumbra by the caster's distance is what would be right for both.
- The other rays (the radiance cache's, the reflections', the lights') still leave their surfaces by
  `nph_offset_ray` alone. Theirs start from points the resolve finds on the triangle itself, and what they
  bring is filtered; none was seen to fail, and none was measured for it.
- Frames without the rays' scene (no cache, no light that casts by rays) keep the map everywhere.
- **Found and not fixed: the shadow over the doorway past the arch** is the stone door frame's level of
  detail. From about 17 m its stepped moulding is drawn as a simplified level, one sloped face for the steps;
  with every mesh at level 0 (`--lod-error=0`) the band never appears, standing or walking, and it shows in
  the cache's light alone (`--debug-view=irradiance`). Not yet shown, and the likely cause: the radiance cache
  finds a surface's cells by its geometric normal's octant, and the sloped face looks down where the steps'
  faces looked out, so it takes darker cells, whatever the level's error and shading normals. If so, either
  the levels must keep what the cache keys on, or the cache must key on what the levels keep.
- Still open from ADR-0093: the park fence's far level, the cache's cells at creases, patterns at the pixels'
  pitch in motion; and what exposure does indoors. The image in motion is still softer than standing, evenly.
## Tests

`gltf.glass_is_a_blended_material_that_keeps_what_it_does_not_let_through`;
`Vsm.a_blended_surface_casts_no_shadow` (the map and the rays; 298 and 264 pixels darker with the rule taken
out); `Vis.glass_shows_what_is_behind_it_and_adds_its_own_light_whole` (reference and production);
`Contact.a_wall_the_sun_grazes_hard_takes_the_suns_rays`;
`Contact.a_coarser_level_takes_no_shadow_of_its_own_mesh` (fails with the pixel's ray taken out);
`Contact.ground_level_with_the_rays_anchor_does_not_shadow_itself` (the rays' place away from the anchor; it
passes without the slack too, which the city's replay measures); `Ao.the_denoise_matches_its_cpu_twin` holds
the gathering of the rays to a C twin at every pixel. Development: render 191 + 191, runtime 96, cook 122,
golden 9, asset 59, editor 68, integration 18, no validation message.
