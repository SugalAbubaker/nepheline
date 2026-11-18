# 0105. Materials made in the editor: a `.nmat` source, tiled by the material, edited live

- **Date:** 2026-11-17
- **Status:** Accepted 2026-11-18 (Phase 15's second step, ADR-0102)

## Context

ADR-0102 puts materials second in Phase 15: "a material source that the cooker cooks and the editor makes and edits
(textures, factors, tiling, alpha, double-sidedness), a browser of materials with previews, and a material dropped on
an entity or a face; import settings for textures (colour, normal, linear data)". Its formats are versioned and
fuzzed, and every edit is undoable.

What there is today:

- **Materials come only inside glTF documents** (`cooker_cook_gltf`, sub-assets `material/<i>`), assigned by GUID in
  `MeshRenderer` and `MeshMaterials`. An editable mesh (`.nmesh`) has a material slot per face but names no material:
  its slots draw what the entity names, or the renderer's default.
- **Tiling cannot be expressed.** The cooked header (`NphMaterialHeader`, type version 2) is exactly 224 bytes and has
  no texture transform. glTF's `KHR_texture_transform` is baked into each mesh's UVs at import, which only works
  because a glTF mesh and its material come together.
- **A texture's data kind is chosen by whoever cooks the image**: the glTF importer by material slot, a standalone image
  by the cooker's `--usage` (colour in the editor). Nothing checks that a normal map was not cooked as colour; the
  renderer would sample it as sRGB colour and light wrongly.
- **A reload blinks.** `nph_scene_renderer_reload` forgets the asset and acquires it again, and an entity whose
  material changes has no instance until the new one and its textures settle. `nph_render_material_update` (in place,
  the next frame) exists, but nothing calls it.
- **The scene's history holds world operations only**; mesh editing keeps a history of its own while it is open.
- **One frame a renderer**, and the editor has one renderer: there is no path for previews.

## Decision

1. **A material source, `.nmat`.** It is JSON in one canonical form (core/json.h), like `.nmesh`:

       {
         "nmat": 1,
         "base_color": [r, g, b, a], "metallic": m, "roughness": r,
         "emissive": [r, g, b], "emissive_strength": s,
         "normal_scale": n, "occlusion_strength": o,
         "alpha": "opaque" | "mask" | "blend", "alpha_cutoff": c,
         "double_sided": false, "unlit": false,
         "tiling": [u, v], "rotation": degrees, "offset": [u, v],
         "textures": {"base_color": {"image": "textures/brick.png", "wrap": ["repeat", "repeat"],
                                     "filter": "linear"}, ...}
       }

   Colours are linear, as glTF's factors are. The five slots are glTF's (`base_color`, `metallic_roughness`, `normal`,
   `occlusion`, `emissive`), and a slot names an image by its source key. Reading is strict: another version gives
   `VERSION_MISMATCH`; an unknown key, a value out of its range, or a key that is not a relative path inside the
   project gives `PARSE_ERROR`. The reader is a fuzz target, and the writer's output reads back the same.

   The cooker cooks a `.nmat` into one material asset, sub-asset `material`, its GUID from its key as every source's.
   It also cooks each image the material names as a texture, with the usage its slot needs: colour for base colour
   and emissive, linear for metallic-roughness, normal for normal, and mask for occlusion. That texture is keyed
   under the image's own key (`<image>#<usage>`, as a standalone image's), so materials naming the same image share
   it. An occlusion image that is also the metallic-roughness one shares that texture, as a glTF's does. A material
   naming an image that is missing fails to cook. Its alpha key is 0: no mesh's trimmed ray triangles depend on it,
   so it is safe on any mesh it is put on.

2. **Tiling belongs to the material.** Material type version 3 adds an optional blob, `UVTR`, holding a 2×3 matrix
   that every texture coordinate of the material goes through before it is sampled. The cooker composes it from the
   source's tiling, rotation and offset, in `KHR_texture_transform`'s order (scale, then rotation, then offset). A
   material without the blob, and every version 1 and 2 material, has the identity. The renderer applies it wherever
   the material's textures are sampled: shading, the alpha tests of both rasterizers and of the shadow maps, the
   rays' alpha tests, and the radiance cache's hits. Texture derivatives go through the matrix's linear part. glTF's
   own transforms stay baked into their meshes.

3. **Import settings for textures:** `<image>.import.json`, `{"texture": 1, ...}`, beside the image, as a sound's
   settings file is. Its bytes join the image's content hash, so a change cooks the image again. It sets:
   - the data kind of the image when it is cooked alone: colour, linear, normal, mask, or HDR;
   - for every texture made from the image alone or for a material: the largest extent,
     uncompressed, no mipmaps (a glTF's images keep the cooker's settings);
   - for a normal map, its green channel's convention: up (OpenGL's, and glTF's), or down (DirectX's, flipped when
     cooked).

   In a material, the slot says the data kind, as it does in glTF. A normal map is always cooked as one, and no
   material can sample one as colour.

4. **Materials change in place, without a blink.** When a material is reloaded, the scene renderer keeps its render
   material and every instance drawing it. It loads the new cooked asset and its textures, and updates the render
   material once they are ready (`nph_render_material_update`). Until then the old one draws. glTF re-imports gain the
   same.

   While the editor's material panel changes a value, it also updates the render material itself, so the viewport
   follows the drag. It writes and cooks the source when the change settles.

   **The temporal resolve forgets a pixel's history where its material changed** (the user's choice, 2026-11-18). A
   still pixel's new frame weighs 1/32, and its colour box is wide (render.md "Temporal"), so without this a material
   changed in place would fade in over about 30 frames, half a second at 60 FPS. The renderer marks a material that
   changed on the frame it changed. On that frame, the resolve takes the current colour where a pixel draws that
   material, and its usual history from the next. The pixels of other materials keep theirs. The renderer's half of
   this step builds it, measured with the tiling (§10.7: its cost in milliseconds and bytes).

5. **The material panel** opens a material from the browser, from a `MeshRenderer` slot in the inspector, or as "New
   Material" (a `.nmat` with the defaults: light grey, non-metallic, roughness 0.5). It edits colours in sRGB and
   stores them linear. Images come from the project by drag or by a list. A material has no unsaved state: every
   settled change is written. The panel keeps a history of the material's source while it is open, as mesh editing
   keeps the mesh's, so every edit is undoable there. It is not part of the scene's history, because a material is
   not scene state: it is not saved with the scene, and it does not make the scene dirty.

6. **Materials on entities, faces and meshes.**
   - **On an entity:** a material dropped on an entity, in the viewport or the outliner, sets the slot of the surface
     under the cursor. Dropped on a `MeshRenderer` slot in the inspector, it sets that slot. Both are steps in the
     scene's history.
   - **In the mesh:** an editable mesh names a material per slot (`.nmesh` version 2, `"materials"`). Its cooked mesh
     carries them (an optional blob of slot materials, mesh type version 8), and they draw where the entity's
     `MeshRenderer` names none. A placed mesh therefore shows its materials, and every placement follows a change to
     them.
   - **On a face:** a material dropped on a face in mesh editing gives the face that material's slot, a new one when
     the mesh has none for it (up to 8). It is a step in the mesh's history.

7. **The browser lists materials, with previews:** a lit sphere for each, rendered on the CPU from the material's
   images, decoded and downsampled on a job. Previews are cached under the cooked directory by the content hashes of
   the material and its images.

Steps 1 to 3 are the cooker's and the asset format's, step 4 and the renderer's half of step 2 are the runtime's, and
steps 5 to 7 are the editor's. They land as four pull requests, the source and its cook first.

## Alternatives considered

- **The data kind set on each image**, as Unity's and Unreal's texture settings are: one texture an image, but a
  material can sample a normal map as colour, and every image needs setting up. With the slot deciding, as glTF's
  importer already does, a mismatch cannot happen. An image used in two kinds of slot is cooked twice, which is
  correct.
- **A GUID kept inside each `.nmat`**, so that renaming the file keeps its references: a copy made outside the editor
  would hold the same GUID as its original, and no other source carries one. Renaming any source breaks its
  references today. Fixing that for every kind belongs to a later tool that renames through the editor.
- **Tiling baked into the mesh's UVs**, as glTF's transforms are: a mesh drawn with materials of different tiling would
  need a copy for each, and a material change would cook every mesh it is on.
- **Material edits in the scene's history**: undoing them would rewrite material files from the scene's stack, and
  would mark the scene dirty for a change that saving the scene does not save.
- **Previews from a second renderer:** two `NphRenderer`s on one device were never tried in the tree, and each needs
  its own tables and memory. CPU previews are independent of the renderer, at the cost of matching its lighting only
  roughly.
- **Materials only on entities** for editable meshes: every placement would hold copies of the materials, and changing
  a modeled building's materials would not reach the buildings already placed.

## Consequences

- **New formats:** `.nmat` version 1, material type version 3, `.nmesh` version 2, mesh type version 8, and texture
  import settings version 1. Each is versioned and fuzzed, and the older versions still read.
- **The GPU material record grows** by the transform. A material's update rewrites the whole material table, which
  costs 0.18 ms for Bistro's 254 materials: in the editor, a drag pays it each frame. The game changes no material
  in a frame.
- **Later steps that sample materials must apply the transform:** the city's voxels and proxies sample meshes'
  materials, and the transform will have to reach them when editor-made worlds get proxies.
- **Renaming a `.nmat` breaks its references**, as renaming any source does.
- **A change in place cuts through the temporal history** (decision 4). Until the renderer's half of this step
  lands, it fades in over about 30 frames, half a second at 60 FPS. The pixels of a changed material are resolved
  without their history on that one frame, and so show the frame as it was drawn.
- **Recovery is not needed for materials:** every settled change is written, so autosave (ADR-0104) does not keep
  them.
