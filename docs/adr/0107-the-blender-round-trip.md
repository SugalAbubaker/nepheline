# 0107. The Blender round trip: collision cooked from glTF, a second UV set, glass, re-import on change

- **Date:** 2026-11-19
- **Status:** Proposed (Phase 15's third step, ADR-0102 §1.3)

## Context

ADR-0102 puts the Blender round trip third in Phase 15: "cooked collision assets, triangle meshes for the static world and
convex hulls for props, chosen per asset or by a naming convention; glass from `KHR_materials_transmission` and the other
material extensions the renderer can draw; `TEXCOORD_1`; a re-import when a source changes on disk; import settings per
asset; a guide to exporting from Blender".

What there is today:

- **Collision geometry exists, collision assets do not.** The physics module builds convex hulls (Quickhull with
  QuickHull3D's face merging, at most 256 vertices) and triangle meshes (a binned-SAH BVH of 4-wide nodes), each as one
  pointer-free block "meant to be written to a cooked file, and used straight from a memory-mapped one", with validators
  (`nph_physics_hull_validate`, `nph_physics_mesh_validate`; ADR-0042, ADR-0046). Nothing cooks them: there is no
  collision asset type, the glTF cook writes none, and the `Collider` component takes spheres, capsules and boxes only
  (ADR-0050: meshes and hulls "need cooked collision assets").
- **The glTF cook reads no conventions and no settings of its own.** Names are copied for files, logs and scene nodes;
  the only authored hint read is a mesh's `extras.no_rays`. Images and sounds have `<file>.import.json` settings
  (ADR-0105 §3, ADR-0057); a glTF has none, and its images take the cooker's settings.
- **Glass is drawn untinted.** `KHR_materials_transmission`'s factor (from 0.5) makes a material blended and
  transmissive, its colour and alpha scaled by what it keeps (docs/cook.md "Glass"). The blended pass dims what lies
  behind by one scalar (`1 - alpha`), so a green bottle shows its reflections and lets the scene through uncoloured.
  `transmission_texture`, `KHR_materials_ior`, `_specular` and `_volume` are not read; F0 is 0.04 everywhere.
- **There is one texture-coordinate set.** `TEXCOORD_0` alone is read (a second set, or a texture's `texCoord` of 1, is
  warned about and ignored). The cooked attribute record is 16 bytes with one UV; the renderer's vertex format, every
  shader that samples a material texture (about 25 sites), the levels of detail, the voxel bake and the rays' trims
  carry one. `NphMaterialTextureRef.texcoord` exists and is always 0.
- **Nothing watches the sources.** A re-import is a copy over the old file (ADR-0026); ADR-0026 and ADR-0027 left a
  watcher to "a later platform addition" calling the same reload. `nph_fs_stat` gives a file's size and modification
  time; the game module already polls its library every 250 ms.

## Decision

1. **Import settings for a scene: `<file>.import.json`** beside a `.gltf` or `.glb`, `{"scene": 1, ...}`, read strictly
   like the texture and sound settings (another version: `VERSION_MISMATCH`; an unknown key or a value out of range:
   `PARSE_ERROR`; a fuzz target). Its bytes join the scene's content hash, so a change cooks it again. It sets:
   - `collision`: `"none"` (the default: content cooks as it did), `"mesh"` or `"hull"`, for every mesh the scene draws
     (decision 2);
   - `hull_vertices`: 4 to 256 (default 32, ADR-0042's advice for props);
   - `lods`, `voxels`: the mesh pipeline's levels of detail and voxel levels, on or off for this scene;
   - `texture_max_extent`, `texture_uncompressed`, `texture_mips`: for the scene's images, as an image's own settings
     set them for an image cooked alone.

   The editor copies the settings file along with an import, as it does an image's, and edits any source's settings
   (a scene's, an image's, a sound's) in an Import Settings window opened from the asset browser; saving cooks again.

2. **Collision assets, cooked from glTF.** A new asset type, `COLL` (type version 1): `HEAD` (the shape count and the
   bounds), `SHAP` (a record a shape: its kind, hull or mesh, and the index of its block), and the physics module's
   blocks as stored blobs (`HULL`, `TMSH`), aligned for use in place from the mapping. Every shape is in the frame of
   what collides (the node, or the mesh drawn there); a proxy's points are moved into that frame when cooked, so shapes
   carry no pose. The asset module checks the container (counts, kinds, blob sizes and alignment); the physics module's
   validators check the blocks when the runtime makes shapes of them (the asset module may not depend on physics,
   CLAUDE.md §4).

   What collides, and how:
   - **per scene:** its settings' `collision`, for every mesh it draws;
   - **per object:** a custom property `collision` on the object (`"mesh"`, `"hull"`, `"none"`), over the scene's.
     Blender exports an object's custom properties as its node's `extras`, as it does the `no_rays` hint;
   - **by name:** nodes named `UCX_<object>` or `UCX_<object>_<NN>` are convex proxies of `<object>` (Unreal's
     convention, which Blender's exporters and asset stores use; `UBX_`, `USP_` and `UCP_` are taken as hulls too). The
     object collides as its proxies, one hull each, over what its extras or the scene would say.
     - A proxy's name is matched whole first, then without a Blender duplicate's `.NNN`, then without the `_<NN>` count.
     - Proxies are not drawn. They leave the scene's node list, except that one with a child, a camera or a light stays
       as a node that draws nothing. A mesh only proxies drew is not cooked.
     - A proxy that matches no node stays as it was, drawn, with a warning, so nothing vanishes unseen.
   - A **mesh** shape is the drawn triangles (every submesh) as one physics mesh: static and kinematic bodies only. It
     is two-sided where a material of the mesh's is double-sided.
   - A **hull** is Quickhull of the positions, to `hull_vertices`. A flat mesh asked for as a hull becomes a mesh,
     two-sided, with a warning.

   The sub-assets are `collision/mesh/<i>/<kind>` (shared by every node drawing mesh `i` the same way) and
   `collision/node/<i>` (a node's proxies). The cooked scene (type version 3) names each node's collision in an
   optional `COLS` blob (a node and a GUID), so an empty with proxies collides too. Step four (ADR-0102 §1.4) makes
   colliders of them in the runtime.

3. **A second texture-coordinate set.** `TEXCOORD_1` is read where a primitive's material has a texture on it, and
   each texture slot names its set (a texture info's `texCoord`, or `KHR_texture_transform`'s override, 0 or 1, written
   to `NphMaterialTextureRef.texcoord`; material type version 4 lets it be 1). Mesh type version 9 adds an optional
   stream, `UV1 ` (`f16x2` a vertex, `NPH_MESH_FLAG_HAS_UV1`), apart from the 16-byte attributes, so a mesh without one
   costs what it does today. The levels of detail carry it through their simplification and keep its seams, the voxel
   bake samples each slot with its set, and the renderer binds the stream per mesh. The shaders take a slot's set where
   ADR-0105 takes the coordinates' transform, and as it does: the resolve, the radiance cache's update and the
   reflections' rays under their `k_uv` variants, and the forward pair under a `k_uv1` of its own, each variant made the
   first time a frame needs it, so frames without such materials run today's code; the rasters' and the rays' alpha
   tests and the page requests test the material's bit where they run. An alpha key (ADR-0096) is the first set's: a
   material whose base colour reads the second has none, and the rays take its test in full. Tangents stay set 0's: a
   normal map on set 1 is read in set 0's tangent frame, with a warning (glTF exporters keep normal maps on the set the
   tangents were made for). Materials made in the editor (`.nmat`) read the first set.

4. **Glass, and the material extensions the renderer can draw.**
   - **IOR and specular:** `KHR_materials_ior` and the factor of `KHR_materials_specular` give each material its F0,
     `((ior - 1) / (ior + 1))^2` times the factor, in the GPU record's free `emissive.w`. Their defaults give 0.04,
     today's constant: a material without them draws exactly as before. Material type version 5 carries it in an
     optional `OPTC` blob (`NphMaterialOptics`). The voxel far field keeps 0.04: its compact format has no room for
     it without making those clusters wide, and the user approved the pictures with it so (2026-11-20).
   - **Tinted glass:** a transmissive material's transmitted light is its base colour (glTF's rule), times
     `KHR_materials_volume`'s attenuation over its thickness where it has one (Beer's law,
     `attenuationColor ^ (thickness / attenuationDistance)`). The blended pass gains a second output (dual-source
     blending), so what lies behind glass is multiplied by that colour, channel by channel, where it was dimmed by one
     scalar. **This changes the image** of transmissive materials whose base colour is not white, wherever they are
     drawn: the user chose (2026-11-19) to have it built and shown first, its pictures and cost measured on the
     reference scenes, and approved it as shown (2026-11-20). As built: the tint goes in `OPTC` as the factor before the cooker scales it by the share it
     keeps (so a transmission of 1 keeps its tint), and the shader tints further by the vertex colour and the base
     colour texture. Glass with a white factor and no volume or base colour texture stays untinted. The GPU record
     grows to 160 bytes (`transmission`, `NPH_RENDER_MATERIAL_TINTED`), and the device baseline gains `dualSrcBlend`.
     The dual-source blend is in variants alone (`k_tint`), drawn while a tinted material is live: blending every
     blended surface so cost 5 to 8 % of the blended pass where no glass was tinted.
   - **`transmission_texture`** is baked into a copy of the base colour texture, texel by texel, as the
     specular-glossiness conversion bakes its textures. Not built yet: no reference scene has one, and it follows
     in this step.
   - **Still ignored, with the warning they give today:** clearcoat, sheen, iridescence, anisotropy, dispersion, diffuse
     transmission, the specular colour, refraction.

5. **Re-import on change.** The editor watches what it has cooked and where it came from:
   - each import records its origin (the path of the file dropped) in the project;
   - the project's import thread polls, about once a second, every source in its database, their companions (a glTF's
     buffers and images), their settings files and the origins (`nph_fs_stat`);
   - a file whose size or time changed is taken once it has stayed the same for 300 ms (its writer has finished);
   - a changed origin is copied over its source as an import is; then the source is cooked, and the cook's reload
     (`nph_scene_renderer_reload`) brings it into the open scene, placed entities and all;
   - what the watch last saw of every file is kept between sessions with the origins (`watch.json` beside the
     database), so an export made while the editor was closed is taken when it opens, and a change seen but not yet
     taken when it closes still waits.
   So exporting from Blender, into the project or anywhere it was imported from, updates the editor.

6. **A guide to exporting from Blender** (`docs/blender.md`): the glTF exporter's settings (binary `.glb`, +Y up,
   modifiers applied, custom properties and materials included), naming and custom properties for collision, glass
   (Principled BSDF's transmission, IOR and volume), a second UV map, and re-export in place.

The step lands as about six pull requests, stacked (ADR-0102; the user's batching of 2026-11-19): the scene settings
with this record, the collision assets, re-import on change with the settings window, the second UV set, the material
extensions and glass, and the guide. The two that change the renderer go up together, so the GPU runner measures them
in one window.

## Alternatives considered

- **Collision inside the mesh assets:** the physics world loads what the renderer does not, streams differently, and a
  proxy belongs to a node, not to a mesh.
- **Godot's suffixes (`-col`, `-convcol`, `-colonly`):** they change the object's name, which becomes the entity's;
  Unreal's `UCX_` names a separate object, the form Blender add-ons and asset stores use.
- **Convex decomposition (V-HACD) of props:** proxies made in Blender cover concave props now; decomposition is for
  later, if hand-made proxies prove slow to make.
- **Operating-system watchers** (`ReadDirectoryChangesW`, inotify): two platform paths and their edge cases (renames,
  network drives, overflows) for a saving the numbers do not ask for: a stat costs microseconds, and a project's
  sources are thousands of files at most. They can replace the poll behind the same call.
- **The second set inside the 16-byte attribute record:** every mesh would pay four bytes a vertex, and the resolve
  would read them for every pixel.
- **Refraction for glass:** screen-space refraction needs the scene behind it as a texture, a copy of the colour target
  before the transparency pass: bandwidth the frame budget (§10.4) does not have for it. Tint is what the eye misses
  most.

## Consequences

- **New formats:** scene import settings version 1, the collision asset type version 1, scene type version 3 (`COLS`),
  mesh type version 9 (`UV1 `), material type version 4 (`texcoord` 1). Each is versioned and fuzzed, and the older
  versions still read. Mesh type version 9 cooks every mesh again once: the reference scenes' next nightly run takes
  the hour a change of the cooker's output takes there. It changes no picture: Bistro cooked by both cookers is the
  same in every blob, its 551 meshes and 254 materials apart only in their headers' type versions.
- **Collision is in the frame of what collides.** An entity drawn at a scale other than one needs its geometry scaled:
  the physics geometry has no scale (ADR-0042), so step four makes scaled copies of hulls and scales meshes where it
  creates shapes.
- **Tinted glass and F0 change pictures**, measured before and after with 64 frames averaged a view (the base's
  own and the new build's renderer on the base's cook agree to 0.001 to 0.002 % of pixels, noise). In Bistro, the
  street lamps' glass (1.0, 0.9, 0.7) turns what is behind it amber, 0.14 to 0.57 % of a view's pixels; its 15
  white glass materials and the two faint blue ones show no other change. In the landscape, ten bark, twig and
  branch materials of F0 0.0007 to 0.034 lose the sky's sheen in shade, 22 to 36 % of a view's pixels by more
  than two levels, the far field's voxels keeping 0.04. The city and the interior have neither.
- **Their cost**, on the RTX 3060 Ti over the 18 reference views at 1440p against 59ad17c, four rounds in
  alternating order: the frame -0.005 ms on average (-0.018 to +0.005 ms). Bistro's blended pass, drawn with the
  dual-source blend while its lamps are tinted, +0.009 to +0.012 ms a view (5 to 7.5 %); the other scenes' blended
  passes unchanged (-0.3 to -1.7 %). F0 costs nothing measurable there. DRAM: no new passes or buffers; the
  material record is 16 bytes longer (Bistro's 254 materials, 4 KB, through L2).
- **A read F0 cost the blended pass where its surfaces lie many deep**, which the reference views do not show and
  the GPU runner's synthetic wall of glass did: `glass_4k_production`'s blended pass 0.243 to 0.272 ms (+12 %),
  over its budget, `glass_1k_production`'s +10 %. The forward passes therefore read F0 only in variants (`k_f0`,
  and the tint variants) drawn while a live material's F0 is another, and shade with 0.04, which the compiler
  folds, in the others; their `forward_blended` keeps its old form, the tint's work in a function of its own over
  it. Against E and F, four rounds: the wall of glass's blended pass back to 59ad17c's (+0.1 to +0.6 %); over the
  18 reference views the frame -0.007 ms on average (-0.017 to +0.002 ms), the blended pass +0.001 to +0.004 ms a
  view, back where 59ad17c had it.
- **The second set costs the reference scenes nothing measurable.** On the RTX 3060 Ti over the 18 reference views at
  1440p, against 7c87711, four rounds in alternating order: the frame −0.005 ms on average (−0.018 to +0.009 ms), the
  CPU +0.0001 ms. All but three of the pipelines `render_perf` reports (70 in Bistro and the interior) have the base's
  registers, binary size and memory. The three test the set where they run: the masked software raster (82 registers,
  from 78; +0.0004 ms a view), the voxel resolve's ray-traced variants (16 more bytes of spill), and AO with the sun's
  rays, an option that is off by default. Before the forward pair had `k_uv1`, passing the set cost the blended surfaces
  7 % (+0.01 to +0.025 ms a view). Each pass that reads the mesh table declares 0.26 MB more (its 16,384 records are 16
  bytes longer, `NphVisMesh` 240 bytes); Bistro's 551 meshes read 8.8 KB more at most, through L2. A material that reads
  set 1 pays one more stream read where its slots sample it, and on its first frame the variants' compiles.
- **Sub-asset keys stay index-based** (`mesh/<i>`, `collision/node/<i>`), as glTF's are: a re-export that reorders
  meshes or nodes renames their assets, as it does today.
