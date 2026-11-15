# 0098. Trimmed cards where their key holds: the rays choose each frame, paged or whole

- **Date:** 2026-11-15
- **Status:** Accepted (the user's word of 2026-11-15 that it answers: "Finish the trim for the
  game", of the offer to make the leaf-card trim work where the game will need it, with paged
  textures and in the engine's runtime loader; built and measured, and accepted the same day:
  "commit it once lavapipe passes", "push the branch and open a PR")

## Context

ADR-0096 gave alpha-tested cards triangles for the rays alone, cut in the cooker to the points
where one material's alpha test, read at its base colour texture's finest level, can pass. It
used them only where both held, and left two places without them, both of them where the game
will run:

- **Paged textures.** A paged texture's samples take the first level from which every level is
  resident (ADR-0065), and so does the rays' alpha test. A coarser level's alpha spreads past
  the polygons cut for level 0: a ray through a polygon's rim would miss a texel that passes
  there. The samples' loader therefore gave a paged scene no ray triangles at all, and the
  game pages.
- **The engine's runtime** (`engine/runtime/src/render_assets.c`) loads meshes, materials and
  textures apart, by GUID, and the scene's entities make instances as they come: when a mesh is
  made, which materials its instances will draw it with is not known. And the renderer held an
  instance to the keys by refusing it: `nph_render_instance_create` refused materials not of
  the mesh's keys, and `nph_render_material_update` refused another key while such an instance
  lived. A runtime that streams cannot live with the first (an entity would not be drawn), and
  an editor that changes a foliage material's cutoff cannot live with the second. The runtime
  handed over no ray triangles for these reasons.

The condition of all this work stands: the picture does not change (the user's words of
2026-11-13: "We can not sacrifice any image quality to get to the gate").

## Decision

1. **Two bottom levels for a trimmed mesh** (`RenderBlasKind`, `rt.c`): the one over its
   triangles for the rays alone, as before, and one over its own triangles, made with the mesh,
   built in the next frame that traces and compacted as every bottom level is. A paged level
   comes and goes from one frame to the next, and a build then would be a frame late or a
   stall: so both are there from the start.
2. **A key's id on a material's record, each frame.** The renderer numbers the alpha keys as
   materials and meshes first name them (`render_ray_key_id`: a map of the 64-bit key to an id
   from 1, kept for the renderer's life, so an id is never another key's). A material's record
   carries its key's id on a frame its rays read the level the key was made of, the base colour
   texture's finest, and 0 on any other (`NphRenderMaterialData.ray_key`, the word that was
   padding; `frame.c` `material_ray_key`): always for a texture resident whole, and for a paged
   one while its level 0 is resident.
3. **The top level chooses.** A trimmed mesh's record holds its rays' table (`NphVisMesh.rays`):
   each submesh's id of the key its ray triangles were made for. An instance's record in the top level names the trimmed bottom level where every trimmed
   submesh's material carries that id this frame, and the bottom level of its own triangles
   otherwise (`rt_instances.comp` on the production path, `rt.c` `ray_keys_hold` on the
   reference's; a material slot with none is the default material, whose id is 0).
4. **A hit reads the same on either.** In the bottom level of a trimmed mesh's own triangles a
   trimmed submesh's geometry is empty (no primitives, which Vulkan builds as nothing), and its
   own triangles are a geometry past the submesh count, in submesh order (`rt.c`
   `mesh_geometries`), whose entry in the submesh table, past the submeshes', is a plain one:
   its material slot and its own first index (`vis_scene.c`). A hit's geometry index names its
   entry on either bottom level, and `vis_ray_triangle`, the alpha test (`rt_mask.glsl`) and the
   hit's surface (`gi_hit.glsl`) are as they were: the shaders that trace cost what they did.
5. **The records follow the ids.** A frame whose material records change an id rebuilds that
   frame's top level (`frame.c` `write_materials`), so the samplers' level and the records'
   choice always agree; a material destroyed while its id was live has the next frame's
   rebuilt, as its instances draw the default material from then on.
6. **No refusals.** Any materials go with any mesh, and any key with any material:
   `nph_render_instance_create` and `nph_render_material_update` take what they are given, and
   the count of a material's trimmed users goes (`RenderMaterial.ray_users`). A caller that
   changes what a material's alpha test reads (the cutoff, the texture, its wrap or filter, the
   factor's alpha) gives 0 or that test's own key (renderer.h).
7. **Loaders hand everything over.** The engine's runtime gives a mesh its ray triangles where it
   has them (and triangles, and takes rays), and an alpha-tested material its key where the
   device took its base colour texture (`render_assets.c`). The samples' loader drops its
   paged-scene rule and its walk of the scene's instances (`cooked_scene.c`).
8. **Counted.** On a frame that rebuilds the top level, the production path counts the records
   that name a mesh's triangles for the rays alone and those that name its own instead
   (`NphRenderFrameStats.vis_rt_trimmed`, `vis_rt_own`).

## Measured

On the RTX 3060 Ti at 2560 × 1440, a process a scene, 240 frames, the window still: main (da009a3)
and this change built alike, with the same benchmark, run in turn, the branch then main, twice
(the interior once: nothing of it is trimmed), on the same packs. Milliseconds, this change less main,
round one and round two.

| View | Frame | reflect_rays | gi_update |
|---|---|---|---|
| Bistro street | −0.032, +0.004 | −0.002, +0.003 | −0.008, +0.001 |
| Bistro street walk | −0.036, −0.001 | −0.003, +0.001 | −0.010, +0.002 |
| Bistro street turn | −0.030, −0.001 | −0.013, +0.003 | −0.001, −0.001 |
| Bistro terrace | −0.024, −0.001 | −0.008, 0.000 | +0.002, +0.002 |
| Bistro hedge | −0.023, −0.015 | −0.001, −0.002 | −0.002, −0.006 |
| City street | −0.005, +0.004 | −0.001, +0.002 | 0.000, +0.003 |
| City street walk | +0.001, +0.003 | +0.001, +0.003 | +0.002, +0.003 |
| City street sprint | +0.007, +0.002 | +0.006, +0.001 | +0.004, +0.003 |
| City street turn | +0.006, +0.002 | −0.001, +0.001 | +0.007, +0.002 |
| City crossing | +0.005, +0.015 | +0.001, +0.003 | 0.000, +0.002 |
| City park | −0.011, 0.000 | 0.000, +0.001 | +0.002, +0.002 |
| City rooftop | +0.007, 0.000 | +0.008, +0.004 | +0.005, −0.001 |

- **No cost that shows.** The frames' differences are of both signs (Bistro's all within 0.036 ms,
  the city's within 0.015), and the 99th percentiles' too (−0.074 to +0.097, the two largest a
  single round's each). The city's `gi_update` is 0.000 to 0.007 ms slower in 13 of 14 (a quarter
  of a percent): its shader, and every other that traces, compiles to the same code as main's
  (the same size, registers and local memory in render_perf's log). The interior's six views lie
  within 0.02 ms. The CPU's frame is as it was (0.020 to 0.025 ms in every view).
- **The bottom levels' memory:** Bistro 60.2 → 64.8 MB (+4.6 MB), the city 91.0 → 99.0 MB
  (+8.0 MB), compacted (render_perf's line). A paged scene on main had no ray triangles at all
  (54.3 and 85.5 MB).
- **Paged, streamed within a budget** (`--texture-budget=1024`, once each; main's samples' loader
  gave a paged scene no ray triangles, this change gives them). The stream held every level the
  pixels asked for ("0 short of what they want"), but in no reference view do they ask for a card
  texture's finest level, so this change's rays took the cards' own triangles throughout, as
  main's did: the city street's `reflect_rays` 0.634 ms on both, the rooftop's 2.138 and 2.146,
  frames within −0.014 to +0.028 ms. The polygons pay in a paged scene only where a card's texture
  has its finest level resident (Consequences).
- **The picture:** as main's, to the runs' own unrest. A view's last frame at a fixed count of
  frames (render_perf's probe): with the radiance cache off, Bistro's street differs from main's
  in 0.008 % of its pixels and the city's rooftop in 0.026 %, where a build repeats itself to about
  0.02 %; with the cache on, the city's street differs from main's in 0.294 % and 0.212 %, where
  two runs of main differ in 0.280 %.
- **A first form of the choice** read the record's binding table offset in the hit's shaders and
  cost the rays 0.02 ms (Alternatives).

## Alternatives considered

- **Keep a card's texture whole** (never paged) wherever a trimmed mesh's material reads it. The
  polygons would hold on every frame, and a paged scene's rays would gain as an unpaged one's
  do, the far trees' too. But that is the texture's memory, all its levels, for as long as the
  scene lives, a choice for the game's streaming budget to make; and it changes what a paged
  scene draws, raster and rays, to the whole texture's picture (the better one). With the choice
  in the renderer it stays correct whichever way that goes: it can be a texture's flag later.
- **The bottom level of the mesh's own triangles made only when an instance first needs it.**
  It saves its memory where nothing needs it (a reference scene's own materials, whole). But a
  paged level goes from one frame to the next and a build then is late, and a rule on the CPU
  must foresee every case where the GPU's choice will want it or the rays go wrong. Made with
  the mesh instead, as plain as it is safe.
- **A second render mesh in the runtime** for instances whose materials the trim does not hold
  for. Every loader would need it, the vertex and index buffers would be twice, and paged
  textures would still get nothing.
- **The record's shader binding table offset, telling a hit which bottom level it is on** (built
  first; ray queries leave the offset to us): the alpha test and the hit's surface read it, and
  every candidate paid for a test that a scene of whole textures never needs. Against main, two
  rounds of each, interleaved, with the window still: Bistro's `reflect_rays` 0.013 to
  0.024 ms slower in every view and the city's rooftop 0.019 and 0.020 ms (1,280 bytes more
  code and 16 bytes more local memory in `reflect_rays.comp`), `gi_update` 0.004 to 0.012 ms.
  The geometries past the submesh count cost the shaders nothing.
- **A flag in the instance's record.** `NphVisInstance.flags` has no bit to spare (the cell takes
  its upper 24); and whether the trim holds is a fact of the instance's materials this frame,
  which the top level's writer reads anyway.
- **The full 64-bit keys on the GPU.** A material's record has one free word; the ids fit it,
  and an id is never another key's.
- **The custom index's top bit for "its own triangles".** Every reader of the custom index (the
  instance's slot) would have to mask it, and a hit's shader would still test it.
- **A trim that holds for every level** (ADR-0096's alternative): it takes little, nothing of
  Bistro.

## Consequences

- **Paged scenes trace their cards' polygons only while the cards' finest level is resident**,
  which is while the pixels ask for it: a card close enough to want it. Streamed within a budget,
  no reference view's pixels ask for it (Measured), and their rays pay what they did before
  ADR-0096. For the polygons to pay in a streamed scene, its rays must read a level that is
  resident whole whatever the pixels ask: the card textures kept whole, or their finest alpha in
  a small texture of its own that never pages (ADR-0096 estimated a foliage atlas at 5.6 MB at
  2048 texels a side). Either is memory, and changes what a paged scene's rays (and, kept whole,
  its pixels) read, to what a scene of whole textures shows; neither is made here.
- **Memory.** A trimmed mesh keeps the bottom level of its own triangles: the bottom levels of Bistro
  60.2 → 64.8 MB (+4.6 MB), of the city 91.0 → 99.0 MB (+8.0 MB), compacted (render_perf's line, the
  same packs); nothing of the interior is trimmed. A scene of whole textures and its own materials never
  traces them; a paged one needs them.
- **Editors and streams need nothing special**: a changed key or texture, a material destroyed
  under its instances, an instance drawn with another material, all trace what is right.
- **The mesh record is 224 bytes** (was 208). A trimmed mesh's submesh table has an entry more
  a trimmed submesh, and its submesh buffer holds the rays' table after its records (4 bytes a
  submesh); the bottom level of its own triangles has an empty geometry a trimmed submesh.

## Tests

- `engine/render/tests/test_ray_trim.c`: an instance whose mesh's ray triangles are one solid
  quarter's parts casts the parts' shadow with the keyed material and the card's own with
  another key, none, or no material (to the pixel, against the untrimmed card's), turns with a
  material update and back, and draws the default's shadow once its material is destroyed; the
  production path counts its record as the mesh's own (`vis_rt_own`). A paged card's rays take
  the parts only while its level 0 is resident: the tail alone, every level, a page of level 0
  evicted. Control: with both top levels made to ignore the keys, both tests fail (27
  expectations) and the others pass.
- `engine/render/tests/test_renderer.c` (`Api.destroy_releases_what_the_caller_left`): a renderer
  destroyed with a trimmed mesh alive frees its keys' ids too (a counting allocator).
- `tests/integration/test_proxy_stream.c`: a cooked scene's cards reach the renderer trimmed
  through the samples' loader, paged or whole; and through the engine's runtime, whose top
  level's record of the cards names their triangles for the rays alone and none their own, with
  and without a texture budget.
