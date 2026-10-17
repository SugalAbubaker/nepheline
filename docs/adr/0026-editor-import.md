# 0026. Editor projects and import: sources copied in, cooked on an import thread, placed through the history

- **Date:** 2026-10-16
- **Status:** Accepted

## Context

CLAUDE.md §8.13 asks for an "asset browser with drag-and-drop import
(triggers the cooker)", and Phase 9's exit criterion is a scene "built,
saved, reloaded, and played in the editor". Several facts constrain how
imported content gets into a scene and back out of a file:

- The cooker (ADR-0017) derives every asset's GUID from its source's path
  relative to a source root ("<key>#<sub>"), and it refuses sources outside
  that root. A source's location in the project is therefore its identity.
- Scenes reference meshes and materials by GUID. A pack indexes GUIDs, but
  loose cooked files do not. The loader already takes a resolver for
  identities no pack knows (`NphAssetLoaderDesc.resolve`,
  [asset.md](../asset.md)), meant for the editor to answer from the
  cooker's database.
- The editor runs the engine through its shared library (ADR-0024). The
  cooker library links the engine's static modules. Linking both would put
  two copies of core, platform, and jobs in one process: two logs, two
  profilers, and two sets of thread-local job state.
- Cooking takes seconds: BC7 encoding dominates. A frame must never wait
  that long, and the editor must keep drawing.
- `nph_scene_import_cooked` spawns a cooked scene straight into a world,
  but every edit must be undoable (§8.13).
- On Windows a loaded asset's file is memory-mapped, so it cannot be
  replaced while loaded.

## Decision

1. **A project is two directories**, set by the cvars `editor.assets` (the
   sources, the cook's root) and `editor.cooked` (the cooker's output and
   `cook.db.json`). Both default to directories of those names under the
   working directory. The cooked directory is mounted at the VFS root
   after the packs, so freshly cooked files shadow packed ones with the
   same path.
2. **Imports copy sources into the project.** A dropped file outside the
   sources is copied into the folder the asset browser shows, and replaces
   a file of the same name: that is how a re-import works. A `.gltf` or
   `.glb` brings the files it names (`cook_gltf_files`), at the same
   relative paths. A folder comes whole. A file already inside the sources
   is cooked where it is. URIs with a scheme, absolute paths, and paths
   above the document's folder are refused, and so is a folder that holds
   the sources.
3. **Cooking runs on the editor's import thread**, one request at a time,
   at low priority. It goes through `nph::cook_shared`, the cooker's
   sources built over the shared engine. The engine's job system encodes
   textures, and the import thread waits on its counters as a thread
   outside it. Requests and results go through a bounded queue
   (`EDITOR_PROJECT_REQUESTS`); finished results are taken at the sync
   point.
4. **The resolver reads a snapshot of the database**, loaded when the
   editor attaches and swapped in after every request. One mutex guards
   the snapshot and the queue, and it is never held across a copy or a
   cook, so a GUID lookup never waits for a cook. The editor's resolver
   reads its project pointer under a second lock, which detaching takes
   before the project is destroyed.
5. **Placing a cooked scene goes through a scratch world.**
   `nph_scene_import_cooked` spawns it into a world made with the edited
   world's components (`registry_from`), sized from the scene's counts.
   `editor_edit_paste` then copies the subtree into the edited world
   through the history, as one step. References inside the subtree are
   remapped, and others are cleared. The point a scene is placed at (the
   surface under the mouse, else the ground plane, else in front of the
   camera) is worked out at the first sync point after the drop, while the
   camera is still where it was.
6. **The platform layer gains `nph_fs_copy_file`.** The OS copies into
   `<to>.part`, which is then renamed over the destination, so a reader
   never sees half a file. The copy is made writable, so a read-only source
   (a version-control checkout) can be re-imported over it.

## Alternatives considered

- **Cook on the main thread.** Simplest, but a texture freezes the
  interface for seconds, and Windows marks a window that stops pumping
  messages as not responding.
- **Cook as an I/O job** (`nph_job_run_io`). I/O jobs run first in, first
  out on the engine's I/O threads. A cook would hold one for seconds and
  stall asset streaming queued behind it, including the viewport's own
  loads.
- **Run `nph_cook` as a child process.** That isolates crashes, but the
  platform layer has no process API yet, and the editor would still read
  the database afterwards. The cooker's parsers are fuzzed (§8.8), which
  makes in-process cooking acceptable.
- **Reference sources where they are, without copying.** GUIDs would
  depend on where the user's files happened to be, and moving a project
  or sharing it would break every reference. Unity and Godot copy for the
  same reason.
- **Link the static cooker into the editor.** That gives two engines in
  one process (see Context). Building the cooker's sources twice costs
  only compile time.
- **Teach the history to adopt entities spawned outside it.** It would
  have to synthesise spawn and set operations for state it did not
  create. The scratch world plus paste reuses duplicate's tested path, and
  prefabs (§8.7) will want the same operation.
- **A per-GUID map maintained alongside the cook.** That duplicates the
  database. Reloading the saved file after each request costs one JSON
  parse per request and keeps one source of truth.

## Consequences

- Scenes saved by the editor resolve their assets in the editor, and after
  a restart, through the database. To play outside the editor, the cooked
  directory is packed (`nph_cook --pack-only`), and the player mounts the
  pack. Phase 9's exit-criterion test covers the editor half.
- Re-importing a changed source whose assets are loaded failed on Windows
  while their files were mapped. Update: ADR-0027 removed this. Files are
  replaced as POSIX renames replace them, and the editor reloads what it
  re-cooks.
- Imports cannot be cancelled. Closing the editor waits for the one in
  progress.
- The editor's allocator must be thread-safe, because the import thread
  allocates from it. `main.c` uses the system allocator; the tests use a
  debug allocator, which takes a spin lock.
- The asset browser lists the folder it shows when stale, after imports,
  and every 2 seconds, so outside changes appear without a file watcher.
  A file watcher is a later platform addition.
