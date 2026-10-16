# Editor (`editor/`)

The editor is the engine's last host (CLAUDE.md §8.13). It is a separate
executable, `nph_editor`, linked against the engine's shared library like
the player and the game modules. It draws a docked Dear ImGui interface
over the engine's frame. Decisions: [ADR-0025](adr/0025-editor-ui.md)
covers the library pin, the backends, the host model, and input, and
[ADR-0026](adr/0026-editor-import.md) covers projects and import.

Editor v1 has:
- the shell: the viewport and its camera, play-in-editor, the console, the
  profiler, and the statistics;
- scene editing: the outliner, the reflection-driven inspector, picking,
  translate/rotate/scale gizmos, and undo and redo for every edit;
- scenes: new, open, and save, with a prompt before unsaved changes are
  lost;
- assets: a project of sources and cooked files, drag-and-drop import
  through the cooker, and the asset browser.

Model creation (primitives and basic mesh editing) comes next.

| File | What |
|---|---|
| `src/editor.h` | the editor: creation, the engine's description, attach and detach, scenes, imports, test hooks |
| `src/history.h` | the undo history: reversible changes grouped into steps |
| `src/edit.h` | the edits the panels make, each one undoable step |
| `src/selection.h` | the selection, by handle |
| `src/view.h`, `src/picking.h` | the viewport's projection, and the entity under the mouse |
| `src/gizmo.h` | the transform gizmo's geometry (`gizmo_ui.c` draws and drags it) |
| `src/project.h` | the project: sources, cooked files, the import thread, the resolver |
| `src/camera.h` | the editor camera: orbit, look, pan, dolly, fly, focus |
| `src/editor_log.h` | the editor's log category, the console panel's lines, and the log sink |
| `src/ui_platform.h` | Dear ImGui's platform backend on `platform/window.h` |
| `src/ui_renderer.h` | Dear ImGui's renderer backend on the RHI |
| `src/document.c` | new, open, and save |
| `src/panel_*.c` | the panels: viewport, outliner, inspector, assets, console, profiler, statistics |
| `src/main.c` | `nph_editor` |

## Running

```sh
nph_editor editor.assets=my_game/assets editor.cooked=my_game/cooked
nph_editor --pack=build/win-msvc/samples/asset_viewer/Debug/showcase.npk
nph_editor --game=build/win-msvc/bin/Debug/game_sandbox.dll --scene=level.nscene
```

The engine's options and cvars apply (`--help`, and `cvars` in the
console). The edited world never simulates. **Play** (F5) plays a copy of
it, keeping every entity's handle. **Stop** (F5 again) discards the copy
and shows the edited world again. **Pause** is F6 and **Step** is F7.

The editor adds three cvars:

| Cvar | Default | What |
|---|---|---|
| `editor.layout` | `nph_editor_layout.ini` | the file the window layout is kept in; empty: none (tests) |
| `editor.assets` | `assets` | the project's sources ("Assets" below) |
| `editor.cooked` | `cooked` | the cooker's output and database, mounted at the root of the VFS |

The project's two are read when the editor starts. Neither directory has to
exist: the first import makes them.

| Key | Does |
|---|---|
| Ctrl+N, Ctrl+O | new scene, open scene (asking first about unsaved changes) |
| Ctrl+S, Ctrl+Shift+S | save, save as |
| Ctrl+Z; Ctrl+Y or Ctrl+Shift+Z | undo; redo |
| Ctrl+D, Del, F2 | duplicate, delete, rename the selection |
| F | frame the selection in the viewport |
| W, E, R; X | the gizmo moves, rotates, scales; world or local axes |
| F5, F6, F7 | play or stop, pause, step |

Exit status: 0; 1 on a failure (validation errors included); 2 for a bad
command line; 77 without a window system or a Vulkan device.

## The panels

The default layout puts the outliner on the left, the viewport in the
centre, the inspector and the statistics on the right, and the console,
the assets, and the profiler as tabs below. View > Reset Layout restores
it, and the View menu shows or hides each panel.

### Viewport

The scene is rendered into an image the size of the panel. The toolbar
holds Play or Stop, Pause or Resume, Step, the gizmo's mode, and its axes.
While editing, the camera is the editor's:

| Input | Does |
|---|---|
| left click | select what is under the mouse (Ctrl adds or removes); empty space clears |
| left drag on the gizmo | move, rotate, or scale the selection (Ctrl snaps) |
| right drag | look around; W A S D move, Q E go down and up, Shift is faster, the wheel changes the speed |
| Alt + left drag | orbit the pivot |
| middle drag | pan |
| wheel | move toward or away from the pivot |

**Picking** casts a ray from the eye through the mouse. A mesh is hit by
its triangles, tested in the entity's own space after its bounds; a light
or a camera, which has no mesh, by a small sphere around its origin. The
nearest hit wins.

**Overlays** are the renderer's debug lines: the ground grid (a metre a
cell, under the pivot), a sphere and an arrow for each light and camera,
and the bounds of the selected meshes. They are drawn relative to the
frame's debug origin, which belongs to whoever owns the frame (the
renderer takes one origin per frame).

The view keeps the world's exposure and tonemapper, from its active
Camera, and replaces only the camera. While playing, the view is the
world's own camera. A click on the scene gives the game the input, and
Escape takes it back.

A scene dragged from the asset browser, or files dropped from outside,
land where the mouse is (see "Assets").

### Gizmos

The gizmo sits on the primary selection, sized to stay 90 pixels long:
- **translate**: arrows along the axes, and squares for the planes between
  them;
- **rotate**: a ring around each axis;
- **scale**: boxes on the axes, and one in the middle for all three.

Translate and rotate use world or local axes (X toggles); scale always uses
the entity's own. A drag starts where the mouse ray meets the handle's axis
(the point of the axis nearest to the ray) or its plane, and follows it
from there. Ctrl snaps to 0.5 m, 15 degrees, and steps of 0.1. Positions
are relative to the entity's origin, so they keep their precision anywhere
in a large world (§8.2). A whole drag is one undoable step, and a move
keeps the rotation and the scale bit for bit.

### Outliner

The world's entities as a tree by their hierarchy:
- a click selects (Ctrl adds or removes), and a double-click frames the
  entity;
- dragging an entity onto another makes it a child, and keeps it where it
  is in the world; dropped on the empty space below, it becomes a root;
- F2, or Rename in the context menu, renames in place. The context menu
  also creates children, duplicates, and deletes;
- a filter shows the entities whose name contains it, as a flat list.

A newly selected entity is revealed: the tree opens down to it.

### Inspector

The primary selection's components, each field edited by its kind and its
hints (`scene/component.h`): numbers are dragged within their RANGE,
ANGLE fields are shown in degrees, COLOR vectors use a colour editor,
rotations are edited as Euler angles, text, string ids, and GUIDs are
typed, and an entity reference takes an entity dropped from the outliner.
READONLY fields are greyed and HIDDEN ones not shown. Add Component lists
the registered components the entity lacks; each component's header
removes it.

While editing, a change is one undoable step: a drag, or typing into a
field, merges into one step per widget. While playing, the play copy
changes directly, and those changes go with it when play stops.

### Assets

The asset browser shows the project's sources one folder at a time, with
the path as buttons leading back up. Each file shows its kind (scene,
image, or neither) and whether the cooker has cooked it.
- Double-clicking a folder opens it, a scene places it in front of the
  camera (cooking it first if it has not been), and an image cooks it.
- A scene dragged into the viewport is placed where the mouse is.
- The context menu places, cooks, or cooks again even when current; Cook
  Folder cooks every scene under the folder shown.
- While the import thread works, the panel shows what it is doing and how
  many requests wait.

### Console

- It shows the log, coloured by level, and can be filtered by level and
  by text.
- The command line feeds the engine's console (`runtime/console.h`):
  commands and cvars, Up and Down for the history, and Tab to complete
  the name being typed (one match completes, several extend to the
  common prefix and are listed).
- A sink on the logger thread fills a buffer under a mutex, and the
  editor moves the lines into the panel once per frame. If the editor
  falls behind, a warning line counts the dropped lines.

### Profiler

It shows the profile collector's history (`profile/profiler.h`):
- a frame-time graph;
- a **timeline** of the newest complete frame, with one band per thread
  and one row per zone depth. Hovering a zone shows its name, its
  duration, and whether it was cut at a frame or fiber boundary;
- the **CPU zones** and the render graph's **GPU passes** (time and
  declared traffic) over the history window;
- the tracked allocators' **memory**. Dear ImGui's own memory appears as
  `editor.imgui`.

Pause keeps the frame on screen. Save trace writes the Chrome trace
(chrome://tracing, Perfetto) of the whole history.

### Statistics

- The last frame's times: the frame, the sync point with its wait for the
  simulation, and rendering.
- The steps, the entity count, and the renderer's and the scene mirror's
  counts.
- Every cvar, edited in place through the registry, with its type, range,
  read-only, and cheat rules. Hovering a cvar's name shows its
  description.

## Editing

### Undo and redo

CLAUDE.md §8.13 asks for "a command stack, and every edit is a reversible
command". The edited world changes only through the history
(`history.h`), which applies each change and records what it needs to go
both ways:

| Change | Recorded |
|---|---|
| set | a component's value before and after |
| add, remove | a component and its value |
| spawn | an entity created, with its components |
| delete | an entity taken out, with every component's value and its parent |
| parent | an entity's parent before and after |

Changes are grouped into **steps** (begin, changes, end), each undone or
redone as one. A drag is one step: `editor_history_set_merged` keeps
updating the newest step while the same merge key repeats.

A deleted entity is **suspended** (`scene/world.h` `nph_world_suspend`)
rather than despawned: its handle stays reserved, so undoing the deletion
brings it back under the same handle, and the selection, other entities'
references to it, and older steps keep working. Undoing a spawn suspends
the new entity the same way, ready for a redo. A suspended handle is
released once no step can bring it back: its deletion falls off the
bottom of the history, a new edit discards the undone spawn, or the
history is cleared.

A step that fails to undo or redo (the world was changed outside the
history) clears the history and says so in the log. The history keeps at
most 1000 steps and about 64 MiB of values, dropping the oldest.

The edits the panels make are in `edit.h`: create, delete (a subtree
leaves first, so undo brings each child back under its parent), duplicate
(references inside the copied subtree point at the copies), rename,
reparent (keeping the world transform), add and remove a component, and
paste (a subtree of another world with the same components, how imported
scenes arrive). Each is one step.

### Selection

The selection is a set of entity handles with a primary one, the last
clicked, which the inspector and the gizmo use. Handles survive
play-in-editor (the play copy keeps them) and an undone deletion. Entities
that are no longer alive are pruned each frame.

### Scenes

File > New, Open, and Save work on the edited world:
- **New** clears it and the history.
- **Open** loads the file (text or binary) into a scratch world with the
  edited world's components first. A file that does not load leaves the
  scene as it was and reports why.
- **Save** writes the text format (`scene/scene_io.h`), diffable in git.

The scene is **dirty** when the history's state differs from the one saved
or opened, so undoing back to it reads as clean again. The title shows the
file and an asterisk. New, Open, and closing the window ask before
discarding unsaved changes.

## Assets

### The project

A project is two directories (ADR-0026):
- **The sources** (`editor.assets`): what the asset browser shows and
  where imports copy files. The cooker keys every asset by its source's
  path relative to this directory, and the asset's GUID derives from that
  key (`tools/cook`, [cook.md](cook.md)), so a project can move and its
  scenes still name the same assets.
- **The cooked files** (`editor.cooked`): the cooker's output and its
  database, `cook.db.json`. The editor mounts the directory at the root
  of the engine's VFS, after the packs, so freshly cooked files shadow
  packed ones with the same path.

Scenes reference meshes and materials by GUID. Loose cooked files are not
indexed by GUID the way a pack is, so the engine is created with the
editor's **resolver** (`editor_engine_desc`): it answers the loader's GUID
lookups from a snapshot of the database. A scene saved by the editor
therefore opens again, in this editor or after a restart, with its meshes.
To play the project outside the editor, pack the cooked files
(`nph_cook --pack-only -o my_game/cooked --pack my_game.npk`) and run the
player with the pack.

### Importing

Files dropped on the window, or given to `editor_import`, are imported
into the folder the asset browser shows:
1. A file outside the sources is **copied** in, replacing a file of the
   same name (re-importing an edited file updates it). A `.gltf` or `.glb`
   brings the files it names with it (`cook_gltf_files`), and a folder
   everything under it. A file already inside the sources is used where
   it is.
2. Each scene and image is **cooked**, and the scenes under each folder.
   Images dropped together with a scene come along without being cooked
   alone: the scene cooks the ones it uses, with the usage each material
   slot needs.
3. Scenes dropped on the viewport are **placed** where they fell: on the
   surface under the mouse, else on the ground plane, else in front of the
   camera. Each becomes one undoable step and is selected. Files dropped
   elsewhere are only imported.

The copy and the cook run on the project's **import thread**, one request
at a time, so the editor keeps drawing while textures encode. The thread
cooks through the cooker library built over the engine's shared library
(`nph::cook_shared`), with the engine's job system encoding textures in
parallel. When a request is done, the thread saves the database and swaps
in a new snapshot, and the next sync point places its scenes. Placement
waits for the end of play.

A cooked scene is placed in two steps. `nph_scene_import_cooked` spawns it
into a scratch world with the edited world's components, sized from the
scene's counts. Then `editor_edit_paste` copies it into the edited world as
one step.

Refused or failing imports are logged, and the rest of the request still
runs:
- a folder that holds the sources themselves is not copied into them;
- a document that names a file with a scheme, an absolute path, or one
  above its folder cannot be copied with it (drop the folder holding both
  instead);
- a missing file, or a document that does not cook, fails its source.

## How it works

The editor is built in steps, because the engine needs the editor's hooks
and resolver before the editor can use the engine:

```c
editor_create(&desc, &ed);                // the shell
editor_engine_desc(ed, &engine_desc);     // the hooks and the asset resolver
nph_engine_create(&engine_desc, &engine);
editor_attach(ed, engine);                // Dear ImGui, its backends, the cvars, the project
nph_engine_run(engine);
editor_detach(ed);                        // before the engine goes
nph_engine_destroy(engine);
editor_destroy(ed);
```

Each frame, the engine calls the editor's hooks
([runtime.md](runtime.md) "The frame"):

1. **event**: each window event goes to Dear ImGui and is consumed, unless
   the game has the input. A drop of files starts an import.
2. **sync**, at the sync point, where the world is quiescent:
   1. the log lines are drained;
   2. finished imports are taken, and their scenes placed;
   3. a Dear ImGui frame is built: the dock space, the menus, and the
      panels;
   4. the view is set (the editor's camera while editing, the world's
      while playing);
   5. the frame's draw data is finished.

   Panels may read and edit the world here.
3. **scene_target**: the renderer draws into the viewport's image. The
   viewport panel recreates the image when the panel changes size.
4. **render**: the UI renderer copies the draw data into this frame
   slot's buffers, applies Dear ImGui's texture requests, and adds one
   pass. That pass clears the backbuffer and draws the interface,
   sampling the viewport's image, which the graph orders after the
   scene's passes. The next simulation step runs meanwhile.

**The renderer backend** (`ui_renderer.c`):
- `ImTextureID` is the bindless sampled index plus one, so 0 stays "no
  texture". Bit 32 marks opaque images: the viewport's alpha is ignored.
- The vertex shader pulls `ImDrawVert`s through a buffer device address,
  so the pipeline has no vertex input.
- The font atlas is a texture Dear ImGui manages (1.92's
  `RendererHasTextures`). The backend creates it, uploads only the
  rectangles Dear ImGui marks as changed (`nph_gpu_upload_image_region`),
  and destroys it on request. Alpha8 textures become white RGBA.
- The swapchain and the viewport image are UNORM. The renderer writes
  sRGB-encoded values, and the interface's colours are written as they
  are.

**The platform backend** (`ui_platform.c`):
- It maps `NphKey` to `ImGuiKey`, with the ISO world keys as Oem102.
- It tracks both sides of each modifier from the key events, so releasing
  one side keeps the modifier while the other is held. All of them are
  forgotten when the window loses focus.
- It reads and writes the clipboard through the window system, and takes
  the display size and framebuffer scale from the window.

**Memory.**
- The editor allocates from its allocator, which must be thread-safe: the
  import thread allocates from it too.
- Dear ImGui allocates through a tracked allocator over it, installed
  while the editor is attached. The previous hooks are restored on
  detach, and any blocks left behind are reported.
- Dear ImGui's allocator hooks and current context are process-wide, so
  there is one editor per process.

## Threading model

Everything runs on the engine's thread (`NPH_MAIN_THREAD_ONLY`), except:
- the log sink, which runs on the logger thread and shares a mutex with
  the per-frame drain;
- the project's import thread, which copies and cooks. It shares the
  request queue, the results, its status, and the database snapshot with
  the main thread under one mutex, held for one queue operation, one
  lookup, or one swap, never across a copy or a cook. It waits on the job
  system's counters as a thread outside it;
- the resolver, which the loader calls from whichever thread loads by
  GUID. It reads the editor's project under a lock that detaching takes
  before the project goes.

The UI is built at the sync point and recorded while the next simulation
step runs on the job system. It reads nothing the simulation writes.

## Costs (§1.4)

- **Attach** allocates the Dear ImGui context, the console's 4096 lines
  (1.1 MB), the profiler panel's segment buffer (32768 segments,
  768 KB), and the project: its import thread and the database snapshot
  (a hash map per lookup kind, over the entries).
- **A frame** allocates nothing in the editor's own code, except while the
  asset browser lists a folder (when shown: after an import, on a change
  of folder, and every 2 seconds), which grows its entry array. Dear
  ImGui's buffers grow to their steady size and then stop.
- **Every edit** allocates its recorded values from the history.
- **An import** allocates its paths when queued; the import thread's cook
  allocates as the cooker does, and the snapshot is rebuilt from the
  database file. Placing a scene creates and destroys a scratch world
  sized to the scene.
- **The UI renderer** grows its buffers by doubling, and allocates a
  staging copy for each texture upload.
- **Reading the clipboard** grows a buffer to the text's size.

## Testing

`editor_tests` (62 tests):
- The camera's invariants, including a 64-bit pivot 10,000 km from the
  origin.
- The view and the gizmo's math (projection and rays, axis and plane
  hits, angles, snapping), and picking against a mesh's triangles.
- The history on a plain world: every kind of change both ways, deleted
  and undone entities back under their own handles, subtrees with their
  references, merged drags and the clean state, steps falling off and
  taking their suspended entities with them, aborted steps, refusals, and
  a paste from another world.
- The selection, the log lines and the sink (filled from another thread
  while drained), and the platform backend with a real Dear ImGui context.
- The project without an engine: an import copies a document with the
  files it names and cooks it; the resolver names the cooked files and a
  new project starts from the database; sources already inside are cooked
  in place; a folder comes whole; images alone and beside a scene; the
  refusals; failures in the results; bounded requests; cooks by key; and
  the resolver answering from another thread while cooks swap the
  snapshot.
- The editor on an OFFSCREEN engine under the validation gate, with the
  input fed as window events:
  - The interface and the cooked showcase in the viewport are drawn
    (checked by reading pixels back).
  - Drags and keys move the camera, and the engine renders from it.
  - Play hands the input to the game and Escape takes it back.
  - The console panel receives the log.
  - Scenes are saved and opened (a bad file leaves the scene alone), with
    the dirty state tracked; the shortcuts duplicate, delete, undo, and
    redo; unsaved changes are asked about before New, Open, or closing.
  - A click picks what is under it, its marker drawn where it projects;
    a gizmo drag moves the selection in one step, and Ctrl snaps it.
  - A dropped scene is cooked and placed where it fell, drawn, undone,
    and redone; the browser keeps a folder the import is about to make.
  - **Phase 9's exit criterion** (CLAUDE.md §12): a scene is built from
    an imported scene, a move, a light, and a camera; saved; opened by a
    second editor on the same project (as after a restart), where its
    meshes resolve through the database and it renders as before; and
    played from its camera, with the edited scene back unchanged after
    Stop.
  - A headless engine is refused.

`editor.smoke` (label `sample`) runs `nph_editor` windowed for 60 frames
with no layout file, and is skipped (77) without a window system or a
device.

## Limitations and future work

- Model creation comes next: primitives and basic mesh editing, saved as
  cooked meshes.
- Re-importing a changed source whose assets are loaded fails on Windows
  while the loader has their files mapped (the cooker cannot replace a
  mapped file); unchanged sources are skipped and do not hit this. Asset
  hot reload (§8.8) would release and reload them.
- Imports cannot be cancelled, and closing the editor waits for the one in
  progress.
- Scene paths are typed. There is no native file dialog: the window
  system has none. Content comes in through drops and the asset browser.
- The window system has no cursor shapes, so splitters and text fields
  keep the arrow pointer.
- Dear ImGui draw callbacks are not supported (skipped with a warning).
