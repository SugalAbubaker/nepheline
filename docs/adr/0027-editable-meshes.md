# 0027. Editable meshes: a polygon source the cooker cooks, edited in place, reloaded where it is drawn

- **Date:** 2026-10-16
- **Status:** Accepted

## Context

CLAUDE.md §8.13 asks for model creation, "Phase 1: primitives (cube,
sphere, plane, cylinder) and basic mesh editing (vertex/face transform,
extrude), saved as cooked meshes". Complex models come from DCC tools
through glTF. Several facts constrain how the editor makes and changes a
mesh:

- **Cooked meshes cannot be edited.** A cooked mesh (ADR-0016, ADR-0017)
  is triangles with quantised vertices, split wherever an attribute
  differs and reordered for the vertex cache. A cube's face is two
  triangles, and its corners are three vertices each. The faces and the
  shared positions an editor works on are gone.
- **Identity comes from the source's path.** Everything drawn is cooked,
  and the cooker derives each asset's GUID from its source's path in the
  project (ADR-0017, ADR-0026). A mesh the editor makes needs a source in
  the project. Only then do the database, the asset browser, and scenes
  refer to it like any other.
- **Editing must show its result as it changes.** A drag changes the mesh
  every frame. A cook goes through the import thread (ADR-0026), so it is
  at least a frame behind, and its mesh pipeline is too slow to run every
  frame.
- **A re-cook replaces files that are in use.** Cooking an edited mesh
  again replaces a file the loader has mapped, while the scene renderer
  and its asset cache hold the old version by GUID. On Windows the
  replacing rename failed while the file was mapped, so re-importing a
  changed source failed too (ADR-0026, Consequences).
- **The scene history knows only the world.** The editor's undo history
  (docs/editor.md "Undo and redo") records operations on the world. A mesh
  edit changes a file outside it.

## Decision

1. **Editable meshes are `.nmesh` sources.** An editable mesh is a JSON
   polygon mesh (`CookPolyMesh`, `tools/cook/include/cook/polymesh.h`):
   - positions;
   - faces, each a loop of 3 to 256 corners with a UV per corner;
   - a material slot per face, below 8;
   - a smoothing angle.

   The cooker cooks it like any other source, into one mesh asset with
   sub-asset `mesh`. Faces are triangulated in the plane of their Newell
   normal: a fan when convex, ear clipping otherwise (Eberly 2002), and a
   fan when clipping fails. Normals follow the smoothing angle and are
   weighted by corner angles. The mesh pipeline does the rest. The text
   form is canonical, so a mesh diffs in git as scenes do (§8.7). The
   reader checks every value, and it is a fuzz target (§8.8).
2. **Primitives are sources too.** Create writes
   `meshes/<kind>[_N].nmesh` into the project and cooks it. The entity is
   placed once the cook is done, as a dropped file is (ADR-0026). Each
   primitive has its own source, so editing one changes no other.
3. **Mesh editing edits the source and previews it.**
   - The database gives the source of the asset the selected entity
     draws: its key and sub-asset (`editor_project_source_of`). Only a
     `.nmesh` source can be edited.
   - The editor loads it and changes it in memory.
   - It draws a preview with a render mesh and instance of its own. The
     preview is the mesh triangulated by the cooker's own code
     (`cook_polymesh_to_mesh`), quantised as the cooker quantises
     (`nph_render_mesh_data_build`), without the vertex-cache and meshlet
     passes.
   - The scene renderer hides the entity's instance meanwhile, whatever
     its MeshRenderer says (`nph_scene_renderer_set_hidden`).
4. **Mesh edits have a history of their own.** Each change (a gizmo drag,
   a move, an extrusion) pushes a copy of the mesh, at most 64. While
   editing, Ctrl+Z and Ctrl+Y walk that history. The scene history sees
   none of it.
5. **Ending an edit commits it.** A changed mesh is saved to its source
   and cooked on the import thread. What the cook wrote is reloaded. The
   preview stays until the scene renderer gives the entity an instance of
   the new version, so no frame is without the entity. An unchanged mesh
   just hands back.
6. **Cooked files are replaced while in use, and reloaded.** The chain has
   five links, each with its own tests:
   1. *The platform.* `nph_fs_rename` on Windows renames with POSIX
      semantics: `SetFileInformationByHandle(FileRenameInfoEx)` with
      `FILE_RENAME_FLAG_POSIX_SEMANTICS | FILE_RENAME_FLAG_REPLACE_IF_EXISTS`
      (Windows 10 1809 and later, NTFS). A mapped destination is
      replaced as on Linux, and its mappings keep the old file. Where the
      system refuses the flag (another file system, another volume, an
      older system), `MoveFileExW` remains.
   2. *The asset writer.* It writes `<file>.part` and renames it over the
      file. A reader never sees half a file, and a mapped reader keeps
      the old one.
   3. *The loader.* `nph_asset_forget` takes an asset out of the path and
      GUID indices. The next load reads the new file, and holders keep
      the old asset until they release it. A slot that was forgotten
      leaves the indices' newer entries alone when it is freed.
   4. *The runtime.* `nph_scene_renderer_reload` forgets the cached
      version in the render asset cache. It also forgets every material
      that uses the asset as a texture. Each entity drawing any of them
      acquires them again at the next sync, through one pass over every
      chunk.
   5. *The editor.* The cooker reports every asset it writes
      (`CookerDesc.on_cooked`). The editor reloads them all before it
      places anything an import made. Mesh commits and re-imports share
      this path.
7. **The gizmo drives targets.** The gizmo's drag code is shared by
   entities and vertex selections. An `EditorGizmoTarget` gives a
   transform, an identity, and begin, apply, and end callbacks.

## Alternatives considered

- **Edit the cooked mesh.** Faces and shared positions would have to be
  rebuilt from quantised triangles, and that is lossy. A quad would come
  back as two triangles, and a seam as two vertices.
- **glTF as the editable format.** glTF stores triangles, not polygons,
  and its vertices are split at seams as a cooked mesh's are. Writing it
  would need a glTF writer, and cgltf only parses. glTF stays the format
  that DCC tools exchange.
- **A binary editable format.** It reads faster, but sources live in git,
  and the meshes the editor makes are small. The text form costs little
  and diffs.
- **A new cooked file for each version.** Writing new paths avoids
  replacing files that are in use, but old versions pile up and need
  collecting. The database would also have to map GUIDs to changing
  paths. Replacing in place keeps one file for each asset. The loader's
  forget keeps old holders safe.
- **Unload before cooking.** Entities would have nothing to draw for the
  length of the cook, and a cook that failed would leave nothing at all.
  The loader also cannot unload an asset that others still hold.
- **Read cooked files into memory instead of mapping them.** That frees
  files on every system, but it costs memory and load time for every
  asset. It would also take the editor off the runtime's loading path
  (§10.5: assets are mapped, with no parsing step).
- **Show the cooked mesh as it changes.** The editor would cook on every
  change and replace and reload files every frame of a drag, through the
  import thread, which is a frame behind at best.
- **Draw the preview as lines only.** That is cheaper, but it shows
  neither the surface nor its shading. The triangles the cooker will make
  show both.
- **Mesh edits in the scene history.** Undoing an edit after the commit
  would mean saving and cooking again from the history. The scene history
  would also hold copies of meshes beside world operations. A step that
  holds the source before and after can come later if editing across
  sessions calls for it.

## Consequences

- Editing a mesh changes its source, so every entity drawing that mesh
  shows the change after the commit. The other entities lose their
  instance for the frame or two the reload takes.
- A mesh edit cannot be undone once editing has ended, because its source
  is already saved. The scene history keeps working as before.
- The preview draws with the default material, whatever the entity's.
- The old version of a reloaded asset stays in memory until its last
  holder releases it, so both versions are briefly resident.
- Where the rename falls back to `MoveFileExW` (FAT32, a network share,
  Windows before 1809), a mapped cooked file still cannot be replaced, and
  the cook reports it as before. Development on Windows 10 1809 or later
  on NTFS does not hit this.
- Reloading covers what the editor cooks. Files changed outside the editor
  need a file watcher (§8.8 "hot reload in the editor"), which is a later
  platform addition. When one exists, it will call the same reload.
- §8.13's "Phase 2" modeling tools build on `CookPolyMesh`: deletion,
  merging, splitting, and edge selection.
