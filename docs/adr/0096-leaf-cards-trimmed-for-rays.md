# 0096. Leaf cards trimmed for the rays: triangles for the rays alone, cut in the cooker

- **Date:** 2026-11-13
- **Status:** Accepted (built and measured; the user's words of 2026-11-13 that it answers: "We can not sacrifice
  any image quality to get to the gate", "try leaf-card trimming in the cooker and measure it", and, of the
  prototype's measurements, "go ahead and build it properly in the cooker"; accepted 2026-11-14: "commit it,
  re-base the budgets and swap the packs")

## Context

The frame's gate is a 99th percentile of 6.94 ms at 2560 × 1440 on the RTX 3060 Ti (ADR-0082). With the image
work of ADR-0090 to ADR-0095 in, sixteen of the eighteen reference views hold it; Bistro's street turn (7.39 ms)
and the city's rooftop (8.72 ms) do not, and the terrace and the park hold it by 0.1 and 0.2 ms. The user's
condition for the speed work is that the picture does not change.

- **Where the rays' time goes.** `reflect_rays` takes 0.7 to 2.2 ms of a frame and `gi_update` 0.55 to 1.19 ms.
  A ray that meets a triangle of an instance with an alpha-tested material is handed it as a candidate wherever
  it meets it, and a shader reads the triangle's corners and its texture to learn whether the ray goes on
  (ADR-0088). Taking every surface for opaque (which changes the picture, so it is a bound and no more) took
  0.46 to 0.66 ms off Bistro's views, 0.73 to 0.86 ms off the city's streets, 1.18 ms off its park and 1.73 ms
  off its rooftop: that is what the candidates the test refuses cost.
- **Whose candidates.** Nearly all are true alpha-tested leaves. Their cards are mostly empty: by the textures'
  alpha, 62 to 75 % of the area of the city's trees' cards is cut away and 51 % of Bistro's foliage, and 50 to
  68 % of a tree's card lies outside the convex hull of what is not.
- **What had been tried, exact, and gave nothing** (measured this day, patches kept out of the tree): opacity
  micromaps (`VK_EXT_opacity_micromap`: 77 to 91 % fewer candidates, and the frame 0.11 to 0.15 ms *slower* in
  Bistro, since this card's driver emulates them in software); a second queue for the ray passes (the passes that
  follow run slower for 3 ms); reshaped shading passes; a faster-trace top level; packed rays.

What is left that changes no picture is geometry: give the rays less empty card to meet.

## Decision

1. **Triangles for the rays alone.** A mesh may carry, for some of its submeshes, triangles the bottom level is
   built of in place of the submesh's own (`NphRenderMeshDesc.ray_submeshes`, `ray_triangles`): each
   alpha-tested triangle cut down to a convex polygon that holds every point of it where its material's alpha
   test can pass, written as a fan. The raster, the shadow map, the levels of detail and the voxels draw the
   mesh's own triangles as before: the raster's alpha test reads filtered levels, and its levels of detail are
   made of level 0, so what it draws is not touched.
2. **A ray triangle is a part of a triangle.** It has no vertices: its record names its parent (a triangle of
   its submesh, by its first index) and its three corners as points of the parent, the weights of the parent's
   second and third corners in 65535ths (16 bytes). A hit's place in the ray triangle gives its place in the
   parent (`vis_ray_triangle`), and the alpha test and the hit's surface read the parent's corners as they would
   have had the ray met the parent. What a ray finds is the same to the rounding of that place.
3. **The bound is the cooked texture's.** The cooker reads each alpha-tested material's base colour texture back
   from its output and decodes its finest level as the GPU does (`cook_bc7_decode`). The rays' test reads that
   level at a point, and a bilinear lookup gives no more than the greatest of the four texels about the point:
   a texel whose alpha can pass, with the greatest vertex alpha of the triangle and the material's factor
   against the cutoff, holds the lookups within half a texel beyond its sides, and a quarter of a texel more is
   kept for the shader's float arithmetic and the corners' rounding. Nothing else can pass.
4. **Four cuts.** For each of 32 directions, how far what can pass reaches in the triangle; the triangle then
   loses up to four half planes, each time the one that takes most, while a cut takes 2 % of its area or more
   (Persson's particle trimmer, 2009, for a card and a ray). A triangle nothing passes in has no polygon. A
   triangle whose test cannot be bounded stays whole.
5. **Neighbours share their edges.** The two triangles of a card share its diagonal, and each one's polygon
   would meet it where its own cuts leave it: T-junctions, between which a traversal may let a ray through. The
   cooker grows the polygons of triangles that share an edge and reach it until they meet it over the same
   stretch, and the renderer places a corner on an edge from the edge's two ends alone, in the order of their
   positions: the two polygons then share the whole stretch at the same floats, and the traversal's own
   guarantee for a shared edge holds.
6. **Where it pays.** A submesh is trimmed where a quarter of its area or more goes. Under that the polygons'
   extra triangles cost what the candidates save (the city's grass, 1.6 million triangles of which a sixth
   would go, gains nothing trimmed).
7. **The alpha key.** The polygons hold for one alpha test. The cooker gives each alpha-tested material with a
   base colour texture a key (`NphMaterialHeader.alpha_key`, material version 2): a hash of that texture's alpha
   as cooked, its size, wrap modes and filter, the factor's alpha and the cutoff. A trimmed submesh carries the
   key it was trimmed for (mesh version 7: `RAYS`, `RAYR`, `RAYT`). The renderer holds an instance to them:
   `nph_render_instance_create` refuses materials whose keys are not the mesh's, and
   `nph_render_material_update` refuses another key while such an instance names the material.
8. **Only where the rays read the finest level.** A paged texture's rays read whichever level is resident. The
   samples' loader (`cooked_scene.c`) gives a material its key, and a mesh its ray triangles, only where the
   scene's textures are whole and every instance of the mesh draws it with the keyed materials; otherwise the
   mesh is made without them and its rays take its own triangles, as before.
9. **On by default** in the cooker (`--no-ray-trim` switches it off), so the reference scenes take it through
   their ordinary cook.

## Measured

On the RTX 3060 Ti at 2560 × 1440, a process a scene, 240 frames, each run started with the desktop still
(docs/benchmarks.md "The desktop shares the GPU too"); the mean of two rounds, the same build on the packs cooked
before (no ray triangles) and after (`benchmarks/results/reference_scenes-2026-11-13-adr-0096-win-msvc-profile-rtx3060ti.json`).
Milliseconds.

| View | Frame | 99th percentile | reflect_rays | gi_update |
|---|---|---|---|---|
| Bistro street | 6.01 → 5.94 | 6.13 → 6.05 | 1.02 → 0.97 | 0.53 → 0.51 |
| Bistro street walk | 6.18 → 6.12 | 6.39 → 6.33 | 1.12 → 1.07 | 0.54 → 0.52 |
| Bistro street turn | 6.30 → 6.24 | 7.38 → 7.35 | 0.87 → 0.84 | 0.64 → 0.61 |
| Bistro terrace | 6.71 → 6.65 | 6.83 → 6.76 | 1.20 → 1.17 | 0.61 → 0.58 |
| Bistro hedge | 5.69 → 5.65 | 5.80 → 5.74 | 0.45 → 0.43 | 0.60 → 0.57 |
| City street | 5.18 → 4.91 | 5.36 → 5.08 | 0.64 → 0.48 | 0.66 → 0.52 |
| City street walk | 5.35 → 5.03 | 5.71 → 5.34 | 0.68 → 0.48 | 0.68 → 0.53 |
| City street sprint | 5.49 → 5.14 | 5.91 → 5.47 | 0.68 → 0.49 | 0.72 → 0.55 |
| City street turn | 5.54 → 5.21 | 6.08 → 5.83 | 0.81 → 0.63 | 0.69 → 0.52 |
| City crossing | 5.64 → 5.27 | 5.86 → 5.45 | 0.93 → 0.68 | 0.61 → 0.46 |
| City park | 6.59 → 6.30 | 6.78 → 6.49 | 0.72 → 0.57 | 1.13 → 0.96 |
| City rooftop | 8.58 → 7.93 | 8.79 → 8.12 | 2.14 → 1.58 | 0.66 → 0.54 |

The interior has no card that pays, and its six views are as they were (each within 0.03 ms).

- **The gate.** Sixteen of eighteen views hold it, as before: Bistro's street turn is 0.41 ms over (its slow
  frames are not the leaves' rays) and the city's rooftop 1.18 ms. The terrace now holds it by 0.18 ms and the
  park by 0.45 ms.
- **What is trimmed.** Bistro: 8 of its 42 alpha-tested submeshes, 137,926 triangles as 332,921 ray triangles of
  53 to 72 % of their area. The city: 25 submeshes of its trees, shrubs and hedge, 109,704 triangles as 279,329
  ray triangles of 32 % (the lindens) to 70 % (the azaleas) of their area.
- **Memory.** The bottom levels: Bistro's 54.3 → 60.2 MB, the city's 85.5 → 91.0 MB. The records, 16 bytes a ray
  triangle beside the submesh tables: 5.3 and 4.5 MB. The build's inputs go once the build is recorded. The
  packs grow by 1.4 and 1.2 MB.
- **DRAM traffic.** Less: a candidate on a trimmed submesh reads 16 bytes more (its record) and there are fewer
  of them, each of which read its triangle's indices, three vertices' attributes, its material and a texel.
- **The cook.** 0.2 s of Bistro's cook and 2.3 s of the city's; `ray_trim_cards` (cook_bench) 797 ns a card
  triangle, most of it sorting the triangles and their edges.
- **The picture.** Not the same bits: a hit's place in its parent is rounded in its last digit. With the frames
  the same (the benchmark's warm-up is a time, so a faster frame shifts the noise's sequence: compared at a
  fixed count of frames), the last frame of four views on the trimmed packs differs from the untrimmed in 0.20 to
  0.81 % of its pixels, almost all by one 8-bit step (0.001 % or fewer by more than four), where two runs on the
  untrimmed packs differ in 0.09 to 0.49 %.
- **Validation.** No validation or synchronisation-validation message in the Development suites or on a view of
  each scene.

## Alternatives considered

- **Opacity micromaps** (`VK_EXT_opacity_micromap`). The exact tool for this, and no gain on this card: its
  driver emulates them. Kept as a setting for cards that have them in hardware (the user's word; the patch is
  kept).
- **The trim at load** (the prototype). It measured the same and reads the textures the scene loads, but every
  load would decode every leaf texture and cut every card again, and a loader that pages has no finest level to
  read. The cooker does it once, from the texture it wrote.
- **A trim that holds for every level of the texture**, for paged textures. A coarse level's alpha spreads, and
  the hull of every level's reach is most of the card: nothing of Bistro is trimmed so, and the rooftop gains
  0.27 ms where it gains 0.67. A paged scene keeps its own triangles instead (Consequences).
- **More cuts, or the grass too.** Eight cuts cut no more than four (a cut must take 2 %). With the bar at 5 %
  of a submesh's area the grass is trimmed to 2.7 million ray triangles and no view gains.
- **A split of each triangle with its empty parts dropped**, where a hull takes little (Bistro's branches): an
  8 × 8 split drops 36 % of Bistro's foliage area, as the hull does, for many times the triangles.
- **A polygon a card and not a triangle**, which would need no shared edges made to agree. A card is two
  triangles only in some meshes; shrubs, hedges and coarser cuts are not cards, and the edge's rule holds for
  any mesh.
- **Float positions in the asset.** Three floats a corner would let a loader build without the parent, and let
  an asset put ray geometry anywhere. A place in the parent is 4 bytes a corner, cannot leave the parent, and is
  checked by two additions.
- **The promise without keys.** The renderer could trust a loader. A trimmed mesh drawn with another material
  lets rays through solid cards with no error; the key costs a word a submesh and a comparison an instance.

## Consequences

- **A scene's cook depends on its textures in its meshes.** The textures' settings are in the meshes' and the
  materials' settings hashes; an image's bytes already were in a scene's. The mesh type is version 7 and the
  material type 2: packs cook again (older meshes and materials still read, without ray triangles or keys).
- **A material that changes its alpha test cannot keep a trimmed mesh.** The renderer refuses the instance or
  the update, and a loader makes such a mesh without its ray triangles. An editor that edits a foliage
  material's cutoff will need to make its meshes again untrimmed; the engine's runtime
  (`engine/runtime/src/render_assets.c`) hands over no ray triangles yet for this reason and the next.
- **Paged textures get nothing yet.** The game will page. The rays' alpha could be read from a level that is
  always resident (a foliage atlas kept whole costs about 5.6 MB at 2048 texels a side), which would make the
  trim hold there too and change what a paged scene's rays read today: a decision for when the game's streaming
  is built.
- **The reference renderer's shadow rays**, which test no alpha, meet a trimmed card's polygons where they met
  the whole card: nearer the truth, and a change to its picture where a scene has trimmed cards (the golden
  scenes have none: their grate is not trimmed).
- **The gate is not reached by this.** The rooftop needs 1.18 ms more and Bistro's turn 0.41 ms, and no other
  exact step is known for either.

## Tests

- `tools/cook/tests/test_ray_trim.c`: the key; the test's reference lookup under each wrap mode; triangles that
  stay whole or have no polygon; a leaf cut from its card; 400 random leaves, triangles, wraps, cutoffs and
  vertex alphas with 4,000 points each, none that passes outside its polygon (without the bilinear reach 96
  fail); cards whose triangles meet their diagonal over the same stretch to the 65535th, by whichever vertices
  and turn; the same cook twice; what is left and what is refused. `test_mesh.c`, `test_cooker.c`: the pipeline
  and a scene's cook write them, keyed, and not when switched off or untextured.
- `engine/asset/tests/test_views.c`: the mesh view takes them and refuses each way they can be wrong; the
  material's key; the fuzz seeds `mesh_rays` and `material_keyed`. `tests/fuzz/fuzz_gltf.c` cooks hostile
  texture coordinates through the trim.
- `engine/render/tests/test_ray_trim.c`: a checker card's shadow traced from its own triangles, from their
  halves and quarters (the same to the pixel: a half holds solid and clear quarters, so a hit's place in its
  parent decides it; with two corners swapped in the shader it fails), from the solid quarters' parts alone (the
  same), and from one quarter's (a quarter's shadow gone: the rays take the parts); what is given refused where
  it is not as described; an instance held to its mesh's keys, and a material to its own while one lives.
- `tests/integration/test_proxy_stream.c`: a cooked scene's cards reach the renderer trimmed through the
  samples' loader, and untrimmed where its textures are paged.
