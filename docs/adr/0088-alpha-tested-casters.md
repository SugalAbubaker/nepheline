# 0088. Alpha-tested surfaces cast, and stop rays, as they are drawn

- **Date:** 2026-11-11
- **Status:** Accepted (the user, 2026-11-11, after walking the city in this build: "The trees and their shadows look
  right. re-base the budgets above and commit"; their report after walking the city and Bistro in ADR-0087's build: the
  noise "is the worst on the trees and also on shadows", and "right now we have to focus on getting the lighting,
  shadows, and level of detail working correctly before worrying about optimization")

## Context

The reference scenes' foliage is cards: a quad or a few, with the leaves' outline in the base colour's alpha, cut by
the material's alpha test (glTF's MASK). The visibility buffer has drawn them so since ADR-0071: its masked raster
discards the fragments the test fails. Nothing else did.

- **The sun's virtual shadow map** drew every caster with no fragment stage, so a card cast its whole quad. A tree's
  shadow was one solid shape with straight edges, and the tree shadowed itself as a solid: its own leaves, and
  whatever stood under it, got no sun.
- **Every ray** was traced with `gl_RayFlagsOpaqueEXT` against geometries built opaque (docs/render.md "Shadow rays":
  "every triangle is opaque"). To the radiance cache a canopy was a closed shell: a leaf inside it saw other cards
  in every direction and no sky, and its cell gathered nothing. In the irradiance view the canopies were black while
  the trunks under them were lit. Reflection rays met the same shell, and mirrored trees as blobs.

Walked through with the camera at the viewer's speed (4 m/s; a replay of a recorded path, each frame captured), the
trees were dark masses speckled with the few leaves the sun reached from outside, over shadows that looked like
cut paper.

A second fault came out while the first was tested. The test's card was a flat plane, and no ray met it at all,
opaque or not. A mesh with no extent along an axis has a dequantisation scale of 0 there; the top level's instance
transform folds the scale in (`L diag(scale)`), and with a zero column it cannot be inverted, which a ray's
transformation into the instance's space needs. Every flat mesh was invisible to every ray: a ground plane, a wall
of one quad, a card. The reference scenes hold few (3 small meshes in the city, 9 quads in Bistro, 1 in the
interior), but a game's own floors and walls would have been among them.

## Decision

**An alpha-tested surface casts and stops rays where its alpha test leaves it**, on the production path.

- **Rays** (`shaders/render/rt_mask.glsl`). The top level marks each instance with a MASK material on any slot as
  not opaque (`rt_instances.comp`, an instance flag, read from the materials as the records are written). The bottom
  levels stay as they were, opaque, so the traversal decides every other instance as before. A query hands back each
  triangle of a marked instance its ray meets as a candidate, and `nph_ray_candidate_covers` runs the material's
  alpha test there as the raster runs it: the factor, the vertex colour's alpha and the texture's against the
  cutoff. The cache's rays, the sun's shadow rays at what they find, the many lights' far samples, the reflections'
  rays and the resolves' shadow rays all trace so.
- **The texture is read at its finest level**, a point of it. A ray is one sample. The mean of many is the coverage
  the raster's pixels see; a filtered alpha put to the cutoff is not (a card half covered by leaves would pass
  whole or fail whole).
- **The sun's shadow map** (`vsm_raster_masked.*`). `vsm cull meshlets` writes the material's index, plus one, into
  the draw where the meshlet's material is alpha tested. The page raster became two pipelines over the one list of
  draws: the first as it was, the second with the alpha test in a fragment stage, each passing the other's draws by.
  On the indirect path each has commands of its own over the list, the other's with no indices. The second is drawn
  only while the renderer holds an alpha-tested material.
- **A material that becomes alpha tested, or stops being,** has the top level's records written again and every
  page of the map drawn again.
- **A flat mesh's zero scale becomes 1** in the top level's transform (`rt.c`, `rt_instances.comp`). Its quantised
  coordinate along that axis is 0 for every vertex, so any scale gives the same points.

## Measured

The same recorded and scripted camera paths on ADR-0087's build and on this one, in turn, two rounds (2560 x 1440,
RTX 3060 Ti; the last 240 frames of each path; ms):

| Path | Frame before | Frame now | gi update | reflect rays |
|---|---|---|---|---|
| City, the street by the park (4 m/s) | 4.06, 4.10 | 4.57, 4.60 | 0.22 -> 0.64 | 0.18 -> 0.30 |
| City, the user's recorded walk | 3.77, 3.79 | 4.23, 4.26 | 0.18 -> 0.59 | 0.13 -> 0.23 |
| Bistro, through the arch (4 m/s) | 4.16, 4.19 | 4.63, 4.65 | 0.14 -> 0.42 | 0.27 -> 0.40 |

The resolve and the shadow map's raster are unchanged within the rounds' spread (the map keeps its pages). The cost
is the candidates: with the same shaders and no instance marked, `gi update` takes 0.24 and 0.17 ms on the first and
last path, 0.01 to 0.04 over before.

The reference scenes' own views, against their budgets (`benchmarks/reference_budgets.txt`, each a mean times 1.05):

| Scene | Frame, mean (ms) | Slowest view's 1 % low |
|---|---|---|
| City | 3.62 to 5.61 -> 4.00 to 6.12 | rooftop 6.69 (the gate is 6.94) |
| Bistro | 4.27 to 4.60 -> 4.33 to 4.82 | street turn 5.52 |
| Interior | 3.47 to 4.02 -> 3.53 to 4.10 | gallery 4.50 |

Every view holds the gate. `gi_update` is over its budget in every view of the three scenes (the city's about twice
its base, the park's three times; the interior's by 0.01 to 0.02 ms, the traversal's new code), `reflect_rays` in the
city's and Bistro's, `many_turns` in three of Bistro's, and the frame's own line in the city's seven and two of
Bistro's. On the user's word the entries the two rounds' mean moved 1.5 % or more and to within 3 % of their budgets
are re-based from it, 57 in all (`gi_update` 18, `reflect_rays` 12, `many_turns` 3, the frame's `gpu` and `gpu.span`
12 each: the city's seven views, three of Bistro's, two of the interior's), from
`benchmarks/results/reference_scenes-2026-11-11-alpha-tested-win-msvc-profile-rtx3060ti.json` and its second round.

## Alternatives considered

- **Opacity micromaps** (`VK_EXT_opacity_micromap`): the alpha test baked into the bottom levels as a few bits a
  micro-triangle, so that the traversal settles most candidates itself and a shader sees only those on a leaf's
  edge. It is the way to win the candidates' cost back and is the plan for it; it needs the cooker to bake the maps
  and the bottom levels to carry them, and it changes no image. Not first: the picture was wrong, and this is right
  at a cost the gate holds.
- **One pass, then again past a cut-away hit**: trace as before, test the hit's alpha after, and where it fails
  start a new ray beyond it. No cost to rays that meet opaque things first, but a ray through a canopy starts over
  at every card, each a traversal from the top; candidates within one traversal are cheaper there, and the canopy
  is where the cost is.
- **A filtered alpha, or the material's mean coverage, taken as a probability** for the cache's rays, whose
  footprints are wide: fewer or no texture reads a candidate. It gives the right coverage on average and loses
  where the leaves are within the card, which the sun's shadow at a hit and the reflections need; kept in reserve
  for the cache's rays alone if micromaps do not come first.
- **A list of its own for the map's alpha-tested draws**, as the voxel casters have: no draw passed by twice. It
  takes a counter, a share of the page budget's accounting and arguments of its own; the two rasters together take
  what the one did while pages are kept (0.011 ms a frame walking), so the simpler form stays until a burst of pages
  shows otherwise.
- **Marking geometries, not instances**: a bottom level is shared by instances whose materials may differ, so the
  mark is the instance's. An opaque submesh of a marked instance (a tree's trunk) costs a candidate that is
  accepted after four reads.

## Consequences

- A tree's shadow is its leaves', with the sun between them, in the map and by rays alike, and what stands under a
  tree is in dappled light. Canopies gather the sky's and the bounced light; mirrors show leaves.
- The frame costs about half a millisecond more where foliage is in reach of the rays, and its budgets say so now;
  opacity micromaps are the way to win the candidates' cost back.
- **The reference renderer's rays still take every triangle for opaque.** Its top level is written on the CPU from
  the frame's draws and has no GPU scene to read a candidate's material from. Its shadows of alpha-tested casters
  are therefore solid, and it is not a ground truth for them: the production path's rays (`NPH_RENDER_VIS_RT_SHADOWS`)
  are what the map's are tested against there.
- Leaves are still opaque to light: a leaf lit from behind does not glow, and the shade inside a canopy is darker
  than a real one's. Light through thin surfaces is its own change.
- Flat meshes are traced. Scenes that held one and measured its absence (a ground that let the cache's rays through)
  change.

## Tests

- `Shadows.a_ray_passes_where_an_alpha_tested_caster_is_cut_away`: a flat card with a checker of alpha between the
  sun and a floor; the shadow is the checker, the card opaque all four quarters, alpha tested again two.
- `Vsm.a_caster_casts_what_its_alpha_test_leaves` and `vsm.the_indirect_raster_casts_what_an_alpha_test_leaves`:
  the same through the map, on both rasters.
- `Gi.a_cut_away_surface_hides_no_sky`: a card over the whole ground; opaque it hides the sky, cut away the ground's
  irradiance is within 3 % of the open ground's.
- `Reflect.a_cut_away_surface_is_not_reflected`: the same over a mirror, from cells and from a ray a pixel.
- Nine planted faults, each caught: no mark on the instances; the flat mesh's zero scale; every candidate accepted;
  no meshlet routed to the masked raster; no discard; no indirect commands; pages kept across a material's change;
  marks kept across it; the masked raster never drawn.
