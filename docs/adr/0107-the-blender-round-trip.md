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
     object collides as its proxies, one hull each, and the proxies are neither drawn nor in the scene. A name that ends
     in `_<NN>` itself is matched whole first.
   - A **mesh** shape is the drawn triangles (every submesh) as one physics mesh: static and kinematic bodies only. A
     **hull** is Quickhull of the positions, to `hull_vertices`; a flat mesh asked for as a hull becomes a mesh, with a
     warning.

   The sub-assets are `collision/mesh/<i>/<kind>` (shared by every node drawing mesh `i` the same way) and
   `collision/node/<i>` (a node's proxies). The cooked scene (type version 3) names each node's collision in an
   optional `COLS` blob (a node and a GUID), so an empty with proxies collides too. Step four (ADR-0102 §1.4) makes
   colliders of them in the runtime.

3. **A second texture-coordinate set.** `TEXCOORD_1` is read, and each texture slot names its set (a texture info's
   `texCoord`, 0 or 1, written to `NphMaterialTextureRef.texcoord`; material type version 4 lets it be 1). Mesh type
   version 9 adds an optional stream, `UV1 ` (`f16x2` a vertex, `NPH_MESH_FLAG_HAS_UV1`), apart from the 16-byte
   attributes, so a mesh without one costs what it does today. The levels of detail carry it through their
   simplification, the voxel bake and the rays' trims sample each slot with its set, and the renderer binds the stream
   per mesh. Every shader that samples a material texture picks the slot's set under a specialization constant, made
   lazily the first time a material reads set 1 (#189's precedent), so frames without such materials run today's code.
   Tangents stay set 0's: a normal map on set 1 is read in set 0's tangent frame, with a warning (glTF exporters keep
   normal maps on the set the tangents were made for).

4. **Glass, and the material extensions the renderer can draw.**
   - **IOR and specular:** `KHR_materials_ior` and the factor of `KHR_materials_specular` give each material its F0,
     `((ior - 1) / (ior + 1))^2` times the factor, in the GPU record's free `emissive.w`. Their defaults give 0.04,
     today's constant: a material without them draws exactly as before.
   - **Tinted glass:** a transmissive material's transmitted light is its base colour (glTF's rule), times
     `KHR_materials_volume`'s attenuation over its thickness where it has one (Beer's law,
     `attenuationColor ^ (thickness / attenuationDistance)`). The blended pass gains a second output (dual-source
     blending), so what lies behind glass is multiplied by that colour, channel by channel, where it was dimmed by one
     scalar. **This changes the image** of transmissive materials whose base colour is not grey, wherever they are drawn:
     it waits for the user's word, and its cost and pictures are measured on the reference scenes first.
   - **`transmission_texture`** is baked into a copy of the base colour texture, texel by texel, as the
     specular-glossiness conversion bakes its textures.
   - **Still ignored, with the warning they give today:** clearcoat, sheen, iridescence, anisotropy, dispersion, diffuse
     transmission, the specular colour, refraction.

5. **Re-import on change.** The editor watches what it has cooked and where it came from:
   - each import records its origin (the path of the file dropped) in the project;
   - the project's import thread polls, about once a second, every source in its database, their companions (a glTF's
     buffers and images), their settings files and the origins (`nph_fs_stat`);
   - a file whose size or time changed is taken once it has stayed the same for 300 ms (its writer has finished);
   - a changed origin is copied over its source as an import is; then the source is cooked, and the cook's reload
     (`nph_scene_renderer_reload`) brings it into the open scene, placed entities and all.
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
  the hour a change of the cooker's output takes there.
- **Collision is in the frame of what collides.** An entity drawn at a scale other than one needs its geometry scaled:
  the physics geometry has no scale (ADR-0042), so step four makes scaled copies of hulls and scales meshes where it
  creates shapes.
- **Tinted glass changes pictures** where coloured transmissive materials are drawn; it waits for the user's word, with
  its pictures and its cost on the reference scenes.
- **A material reading set 1 pays** one more stream read in the resolve and a variant of each shader that samples it;
  the reference scenes read none and pay nothing.
- **Sub-asset keys stay index-based** (`mesh/<i>`, `collision/node/<i>`), as glTF's are: a re-export that reorders
  meshes or nodes renames their assets, as it does today.
