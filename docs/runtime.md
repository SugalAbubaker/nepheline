# Runtime (`engine/runtime`)

The last module in the order (CLAUDE.md §4): the `NphEngine` context that
owns every subsystem, the frame loop, configuration and the console, input
as the simulation sees it, the bridges from the ECS world to the
renderer, to the physics world, and to the mixer, drawing between
simulation steps, and game modules with hot reload (§8.12). The player
(`nph_player`) and the editor are hosts of an engine. Decisions:
[ADR-0023](adr/0023-runtime.md) (the engine and its frame),
[ADR-0024](adr/0024-game-modules.md) (game modules, hot reload, and the
§13 scripting decision: C only),
[ADR-0050](adr/0050-physics-in-the-engine.md) (physics in the engine and
drawing between steps), and [ADR-0058](adr/0058-audio-in-the-engine.md)
(audio in the engine).

| Header | What |
|---|---|
| `engine.h` | `NphEngine`: creation and destruction, frames, play and pause, scenes, the game module, the view, subsystem accessors |
| `game.h` | the game module ABI (`NphGameApi`, `nph_game_entry`) and the services game code calls |
| `config.h` | the command line: options and cvar assignments |
| `console.h` | commands and cvars behind one command line |
| `clock.h` | the fixed-step clock, and where frames draw between its steps |
| `sim_input.h` | input snapshots for simulation steps |
| `components.h` | MeshRenderer, MeshMaterials, Light, Camera, Environment, RigidBody, Collider, AudioSource, AudioListener, WorldCell |
| `audio.h` | the audio output's modes, one-shots, and the audio statistics |
| `scene_render.h` | the scene renderer (world to renderer, between steps) and the cooked-scene importer |

## The engine

`nph_engine_create` brings the subsystems up in module order and
`nph_engine_destroy` takes them down in reverse (§8.12):

1. cvars, the console and its commands; the user config, `--config`
   files, and command-line assignments, in rising precedence;
2. profiling (`prof.enabled`): core recording, unless the host started it,
   and the collector, tracking the engine's seven allocators;
3. the job system; the VFS with every `--pack` and `--content` mounted; the
   asset loader (with the host's resolver for loose cooked files);
4. input; then by mode the window, the device, the swapchain or offscreen
   target, the render graph (recording passes on jobs), the renderer, the
   GPU profiler, and the scene renderer;
5. the edited world, its transform system, and its schedule; the physics
   bridge; the audio bridge with its mixer, streamer, and device
   (`audio.device`);
6. the game module (`--game`), a scene (`--scene`), play (`play_on_start`),
   and the console script (`--script`).

A failure at any step takes down what came before and returns the error;
`NPH_RESULT_UNSUPPORTED` means the mode needs a window system or a GPU the
machine lacks (hosts fall back or skip).

**Modes.** WINDOWED (window and swapchain), OFFSCREEN (a device and an
image to render into, no window: tests, thumbnails, servers that render),
HEADLESS (no window and no device: the simulation only). `--headless`
selects HEADLESS whatever the host asked for.

**Allocators.** The engine's memory comes from the host's allocator through
seven tracked wrappers: engine, world, render, assets, game, physics, and
audio. The profiler reports them; destruction logs any bytes still live in
one. The host's allocator must be thread-safe: the asset loader's I/O
thread and the audio streamer's free from it too.

**Logging** is the host's to set up (sinks, the logger thread): the player
and the editor do, tests may not. The engine only logs.

## The frame

```
poll events -> input -> sim input
sync point:  wait for the simulation | capture what it moved | swap in a new game build
             gpu frame begin | game frame | host sync hook
             transforms | scene renderer sync (between steps) | physics events read
             clear world events | physics debug lines | debug flip
kick:        the next frame's simulation (a job): each step runs the game's systems,
             the physics system, and the transform system
render:      acquire | renderer passes | host render hook | present | frame end
collect profile, frame limiter
```

The simulation of frame N+1 runs on the job system while this thread
records and submits frame N (§10.5 "Simulation of frame N+1 overlaps GPU
rendering of frame N"). Nothing that runs during rendering reads the
world: the renderer draws what the sync point mirrored, and the host's
render hook draws what it prepared in its sync hook. Debug lines drawn by
systems during the simulation go to the next frame
(`nph_render_debug_flip` closes the list at the sync point). The CPU stays
within one frame of the GPU: `nph_gpu_frame_begin` waits on the frame slot
(§10.6).

With `rt.pipelined` off, the simulation runs at the sync point instead,
before rendering: one thing at a time, for debugging and profiling.

**Steps.** The fixed-step clock (`sim.rate`, 240 by default for the §8.9
target; `sim.max_steps` per frame, beyond which time is dropped and
counted; `sim.time_scale`) decides how many steps a frame takes. Each
step is a run of the schedule (the game's systems, the physics system, and
the transform system) with that step's input (`sim_input.h`): the first
step of a frame gets the edges and the motion gathered since the last
step, so a key tapped between steps is seen once. `rt.lockstep` takes
exactly one step per frame whatever the time: tests, frame-by-frame
debugging. Frames draw the world between its last two steps
("Interpolation" below).

**Play.** A world simulates only while playing. The player plays from the
first frame (`play_on_start`); the editor plays a copy
(`nph_engine_play_begin(e, true)`): the edited world is copied with every
entity handle kept (scene.md "Copies that keep their handles"), the copy
simulates and is shown, and `nph_engine_play_end` discards it. Pause and
single steps work while playing. `nph_engine_wait` waits for the
simulation in flight, after which the world is the caller's until the next
frame; the engine's own world-changing functions wait by themselves.

**The view** is the world's first active Camera (by entity slot), or the
host's (`nph_engine_set_view`, the editor's camera). Either way the
`r.pipeline`, `r.occlusion`, `r.cone_culling`, `r.sw_raster`,
`r.virtual_shadows`, `r.taa`, `r.bloom`, `r.auto_exposure`,
`r.exposure_compensation`, `r.exposure_speed`, `r.motion_blur`, `r.dof`,
`r.ao`, and `r.gi` cvars choose the renderer, its culling, its
rasterisers, how it shadows the sun, whether it resolves temporally, its
bloom, how it exposes, whether the camera's shutter blurs motion, whether
its lens blurs what is out of focus, how far surfaces occlude the
environment's light, and whether the scene's light bounces
(docs/render.md, "The production path", "Shadows", "Temporal
anti-aliasing", "Motion blur", "Depth of field", "Ambient occlusion",
"Global illumination", and "Bloom and exposure"); an automatic exposure
starts from the camera's EV100 and adapts at the pace of the frames' real
time (one simulation step a frame under `rt.lockstep`, so runs repeat),
and the motion blur spans the camera's shutter time over that frame time.
A switch between the world's camera and the host's is a camera cut: the
frame after it starts a new temporal history; a host that moves its camera
discontinuously sets `camera_cut` in that frame's view.

## Configuration

Settings are cvars. The engine's:

| Cvar | Default | |
|---|---|---|
| `rt.max_frames` | 0 | stop after N frames (`--frames=N`) |
| `rt.pipelined` | true | overlap the simulation with rendering |
| `rt.max_fps` | 0 | frame-rate cap (saved) |
| `rt.lockstep` | false | one step per frame |
| `rt.max_entities`, `rt.world_events`, `rt.workers` | 65536, 16384, 0 | latched |
| `sim.rate`, `sim.max_steps`, `sim.time_scale` | 240, 8, 1 | the clock |
| `sim.interpolate` | true | draw the world between its last two steps ("Interpolation") |
| `phys.solver`, `phys.substeps` | 0, 4 | the physics solver (0 the soft step, 1 TGS, 2 XPBD; physics.md "Choosing a solver") and its substeps, from the next play |
| `phys.sleep`, `phys.continuous` | true, true | bodies at rest fall asleep; fast bodies are swept (physics.md), from the next play |
| `phys.debug_draw` | 0 | draw the physics world: 1 shapes, 2 boxes, 4 contacts, 8 centres of mass, 16 sleep, 32 joints |
| `audio.device` | auto | the output: auto, off, system, null, offline ("Audio"; saved, latched) |
| `audio.sample_rate`, `audio.channels` | 0, 0 | the mix's format (0: the device's own; saved, latched) |
| `audio.voices`, `audio.real_voices`, `audio.streams` | 512, 64, 32 | voices at once (latched), voices mixed at once, the rest virtual (saved, latched), streams at once (latched) |
| `audio.stream_seconds` | 1 | the frames a stream's ring holds ahead of its voice |
| `audio.volume`, `audio.volume_effects`, `audio.volume_music`, `audio.volume_dialogue`, `audio.volume_ambience`, `audio.volume_ui` | 1 | the master's and the buses' volumes (saved) |
| `audio.reverb` | true | sources send to the room's reverberation (their next voices) |
| `audio.occlusion_rays` | 72 | occlusion rays a frame, up to nine a source ("Audio"; 0: none) |
| `r.vsync`, `r.width`, `r.height` | true, 1280, 720 | saved, latched |
| `r.max_instances`, `r.max_meshes`, `r.max_materials`, `r.max_textures`, `r.max_lights`, `r.max_cells` | 0 | the renderer's capacities (0: its defaults; latched; "World partition") |
| `r.max_assets` | 0 | meshes, materials and textures the scene renderer caches at once (0: 4096; latched) |
| `r.deferred_destroys` | 0 | GPU objects a frame may destroy before its queue has to grow (0: 4096; latched) |
| `r.lists` | 1 | the production path's lists that can drop work, times their defaults (latched) |
| `r.voxel_pool_mb` | 0 | the pool the cells' proxy levels live in, so that they are made on the jobs (0: none; latched) |
| `r.proxy_budget_mb`, `r.texture_budget_mb` | 0, 0 | the cells' finer levels' memory (0: unlimited), and the textures' streamed levels' (0: textures resident whole; latched) |
| `r.pipeline` | 1 | 0: the forward reference renderer, 1: the GPU-driven production one (saved) |
| `r.occlusion`, `r.cone_culling` | true, true | the production path's culling stages |
| `r.sw_raster` | true | the production path's compute rasteriser for small triangles |
| `r.virtual_shadows` | true | the production path shadows the sun with its virtual shadow map (false: with rays) |
| `r.taa` | true | the production path's temporal anti-aliasing at native resolution |
| `r.bloom` | 0.04 | the bloom's weight in the image, 0 to 1 (saved) |
| `r.auto_exposure` | false | meter each frame and adapt the exposure, from the camera's EV100 (saved) |
| `r.exposure_compensation`, `r.exposure_speed` | 0, 1.5 | the automatic exposure's offset in stops (positive: brighter) and e-foldings per second (saved) |
| `r.motion_blur` | true | motion blur over the camera's shutter time (its `shutter`, 0 by default; saved) |
| `r.dof` | true | depth of field through the camera's lens (its `aperture`, 0 by default: a pinhole; saved) |
| `r.ao` | 0.5 | the production path's ambient occlusion: the radius in metres within which surfaces occlude the environment's light, 0 to 8 (0: none; saved) |
| `r.gi` | true | the production path's global illumination: the diffuse light from the radiance cache, where the device has ray queries (saved) |
| `game.hot_reload`, `game.reload_poll_ms` | on in development, 250 | |
| `prof.enabled`, `prof.history` | on in development, 240 | latched |

Values come, in rising precedence, from the defaults, the user config
(`--user-config`, written back with the SAVED cvars at exit), `--config`
files in order, and `name=value` arguments. Unknown names wait for a
registration (a game's cvars). The console (`console.h`) sets and prints
cvars and runs the engine's commands: `quit`, `play`, `stop`, `pause`,
`step`, `reload`, `scene.load`, `scene.save`, `trace`, `stats`, plus
`help`, `cvars`, `echo`.

## Game modules

A game is a shared library exporting `nph_game_entry`, which returns an
`NphGameApi`: its components, the size of a state block the engine keeps
for it, and callbacks (`load`, `unload`, `add_systems`, `frame`,
`play_begin`, `play_end`). `game.h` lists the rules game code follows.
CMake builds one with `nph_add_game_module`; `samples/game_sandbox` is an
example.

**The shared engine.** A game calls engine functions, and the host that
loads it must run the same engine: one log, one profiler, one window
system. So hosts and games link `nepheline_shared`, the engine built as one
shared library from the modules' object files (the static libraries every
other target links are unchanged). It exports the engine's API: `nph_*`
functions, the `Nph*_component` descriptor functions, and the engine's
log categories (with MSVC through a `.def` file generated from the static
libraries, cmake/NphExportDef.cmake, since Shipping's link-time-code-
generation objects carry no symbol table to export from). Engine data in
public headers (the log categories) is declared `NPH_ENGINE_DATA`, which
imports it on Windows where `NPH_ENGINE_SHARED_IMPORT` is defined, as it
is for every target linking the shared engine.

**Loading.** The engine loads a temporary copy of the module
(`<temp>/nepheline-<pid>/<name>-<n><ext>`), so the build can write the
original while the game runs. It checks the API (`NPH_GAME_ABI_VERSION`,
valid component descriptors, a sane state block, no component named like
the engine's), copies every component descriptor into memory of its own
(the world points at the copies, never into the module), registers the
components, prepares the state block, and calls `load`, then
`add_systems`.

**Hot reload.** With `game.hot_reload` on, the module's file is checked
every `game.reload_poll_ms`; a change is swapped in once the file has
stopped changing for 300 ms (a linker writes in steps). `reload` in the
console, or `nph_engine_reload_game`, checks at once, comparing the
content when the time and size did not change. At the sync point, with
nothing simulating:

1. The new build is opened next to the running one and checked.
2. If any of the running build's components is gone or has a new layout
   (the layout hash: name, size, every field's name, kind, offset), the
   edited world is migrated: saved as text and loaded into a new world
   registered with the new components, with every entity handle kept,
   fields matched by name, and the components the new build dropped
   skipped. A play copy is stopped first. Any failure up to here leaves
   the running build in place, and is logged.
3. The running build's `unload(reloading)`; its cvars and console commands
   are removed (a tuned cvar's value is kept for the next build's cvar of
   the same name).
4. The new world replaces the old one (the host's `world_replaced` hook
   runs), or, when the layouts held, new components are registered and the
   others keep their copies, refreshed with the new build's flags, hints,
   and defaults.
5. The state block is kept when its version, size, and alignment are the
   same, and replaced by a zeroed one otherwise.
6. The new build's `load(reloaded, state_reset, world_migrated)` and
   `add_systems`. A world that was playing goes on playing.
7. The old image is unloaded once its log records are delivered
   (`nph_log_drain`) and the profiler has forgotten its zone names. If
   anything the engine holds still points into it (a component
   descriptor, a cvar, a console command), it stays loaded, with a
   warning, rather than leave a dangling pointer.

What survives a reload: entities and their handles, component values
(matched by field name), the state block, tuned cvars, input actions. What
does not: the module's globals, component ids (looked up again in `load`),
anything pointing into the old image.

## Input

The window's events reach the engine's `NphInput` (platform/input.h) on
the main thread. The simulation never reads it: each step gets an
immutable `NphSimInput` (`sim_input.h`), because steps run on the job
system while the main thread already handles the next frame's events.
Held state is the latest frame's; edges and mouse motion accumulate until
a step consumes them. Game code reads its step's input with
`nph_game_input` and registers actions with `nph_game_action`.

## From the world to the renderer

The scene renderer (`scene_render.h`) makes the renderer show the world.
Once per frame at the sync point it mirrors:

- entities with a MeshRenderer and a WorldTransform as render instances,
  once their mesh and materials have loaded;
- entities with a Light and a WorldTransform as render lights, pointing
  down their -Z axis, casting ray-traced shadows when the Light's
  `shadows` is set (docs/render.md "Shadows"; off by default, also in
  text scenes that predate the field);
- the first Environment as the sky, or the component's defaults (a clear
  day) when the world has none, applied only when the result differs from
  the sky applied last;
- the first active Camera as the view.

Only what changed is touched: chunks whose MeshRenderer, Light, or
WorldTransform column changed since the last sync (the world's change
ticks), and the despawns and removals in the world's event stream. A static
world costs one version check per chunk. An overflowed event stream makes
the next sync check every entity it mirrors.

Assets are cached by GUID: meshes, materials, and the textures materials
name load asynchronously, become render resources when ready, and are
destroyed when the last entity using them goes. An entity's new references
are taken before its old ones are released, and a world switch holds the
old world's assets until the new world has taken its own, so an asset
named before and after is neither unloaded nor reloaded (play-in-editor
starts without a hitch). A mesh that fails to load leaves its entity
undrawn (counted, logged once), a material that fails draws with the
default material, and a MeshRenderer that names no mesh draws nothing and
is no failure. A frame never waits for a load (§10.5). A MeshRenderer holds
eight material slots; a mesh of more also carries a MeshMaterials, with 24 more
(ADR-0072), read whenever the MeshRenderer is: after changing one alone, touch
the MeshRenderer.

Two calls are for a host, such as the editor:
- `nph_scene_renderer_reload` loads an asset again after its cooked file
  was replaced (a re-import, an edited mesh). The cached version is
  forgotten by the cache and the loader (`nph_asset_forget`), along with
  the materials that use it as a texture. Every entity drawing them
  acquires them again at the next sync, which visits every chunk, and is
  without an instance until the new versions are ready.
- `nph_scene_renderer_set_hidden` hides an entity's instance whatever its
  MeshRenderer says, while the host draws it its own way (the editor's
  mesh editing). It holds across mesh and transform changes. Showing the
  instance again applies the transform it missed.

`nph_scene_import_cooked` spawns a cooked scene asset (asset/scene.h)
under a named root: one entity per node with its transform and hierarchy,
a MeshRenderer per mesh instance, a Light per light, and an inactive
Camera per perspective camera. It is all or nothing.

## World partition

A world large enough to need it has cells (ADR-0062), which a cooked scene carries
(asset/scene.h version 2). `nph_scene_import_cooked` makes each a child of the root
with a `WorldCell` of a new id, and the mesh instances in it name that id in
`MeshRenderer.cell`. A scene imported again, a further tile of a world, has cells of
its own, and with `instances_only` no lights or cameras of its own. A root that turns
or scales its scene takes no cells: an axis-aligned box could not follow it.

The scene renderer mirrors a `WorldCell` as it does instances and lights:

- It becomes a render cell once its proxy has loaded: a head of levels apart with its
  coarsest level (the stream brings the rest), or a mesh cooked whole, and its material.
  The instances that name it wait until then, so a cell's first frame holds them all;
  an instance's cell is fixed when it is made, so a game makes the cell first.
- A cell without a proxy, or whose proxy fails, makes no render cell, and its instances
  draw as themselves. The failure is logged.
- A despawned or removed `WorldCell` goes, and its instances are made anew, drawn as
  themselves unless another cell of that id stands. A change to its place or its proxy
  is one going and one coming. What goes is marked, and released in one pass over the
  records, so a whole world going is linear.

`nph_scene_renderer_stream` runs the streams for the frame's view, between
`nph_gpu_frame_begin` and `nph_render_frame`, and the engine calls it. It is nothing
until there is a cell that streams or a texture budget. `NphSceneRenderStats` counts
the render cells, those loading, and what the streams hold.

The renderer's capacities are cvars (`r.max_instances` and the rest, above), since a
game knows its worlds and the renderer is made before they load. A world past them
fails as it always did (FULL on the frame it arrives). `samples/game_sandbox/game.c`
gives the commands for the city and the landscape, with `sandbox.tiles`, the way a game
would lay out one scene over and over.

## Proxy streaming

`proxy_stream.h` streams a world partition's cells' proxies level by level
(ADR-0063 stage 3; [render.md](render.md) "Cell proxies"). A proxy cooked
as its levels apart is a head naming each level's mesh asset, finest
first. The caller makes each cell with its coarsest level alone
(`nph_proxy_level_mesh_create`, the stream's own way of making a level) and
its levels' errors, and adds it to a stream; each frame, between
`nph_gpu_frame_begin` and `nph_render_frame`, `nph_proxy_stream_update`
gives the cells the levels the views want and takes back what its budget
cannot hold. The samples' cooked-scene bridge does so with
`CookedSceneDesc.stream`, and render_perf with `--stream`. In a game, the scene
renderer does ("World partition"). A cell leaves the stream by
`nph_proxy_stream_remove` with the index its `add` gave, its levels going with it.

- **Wanted.** A cell is wanted at the level the views ask for
  (`nph_render_cell_request`: the camera's cut at its box brought a quarter
  nearer, and the sun's shadow map's), a request standing 30 updates once
  the views stop asking, so that a cell flickering in and out of view
  keeps its level. Unasked and past its distance less a quarter, it is
  wanted one level coarser than its cut from the camera (the requests'
  test, on the CPU): a turn shows the far field at most a level coarser,
  briefly. Nearer, its coarsest alone: its instances are drawn.
- **Worth.** A level is worth the error its cell shows without it, seen
  from the camera over the view's threshold; a level at or coarser than
  the one its cell is wanted at (needed) before any finer (spare). A
  cell's finest held level is its least worth keeping and its next finer
  its most worth loading, so one ranking orders both loads and drops, and
  what is held converges on the levels worth most within the budget.
- **Loads.** The levels worth most load first, at most `max_loads` at
  once (64 by default: the levels range from a few kilobytes to a few
  megabytes, and a load takes two or three frames whatever its size, so
  the far field's thousands of small levels need many under way; the
  loader's I/O thread, decompression on the jobs), each within the budget
  with the loads under way. Room is made by dropping levels
  worth enough less (spare ones, or needed ones worth a quarter less, so
  that cells of nearly the same worth do not trade a level back and
  forth), and only when the levels so dropped free enough; a level that
  does not fit is passed over (eight an update) for smaller ones. A
  level's bytes are known once it has loaded; before, they are its
  clusters at the bytes a cluster of its depth has taken so far (its
  voxels' size is the same in every cell), or nothing for a depth not seen
  yet: it loads, and the budget then drops what it must.
- **Installs.** A loaded level still needed becomes a mesh and joins its
  cell's proxy (`nph_render_cell_set_proxy`; the cell's finest held level
  stands for the finer ones), those worth most first, at most
  `max_install_bytes` begun a frame (by their estimates). Given a job
  system (`NphProxyStreamDesc.jobs`) and a renderer with a voxel pool
  ([render.md](render.md) "The voxel pool"), the level is made on a
  loading job: the update reserves its mesh's slot; the job checks its
  asset (the mesh view), describes it, uploads it into the pool and
  releases the asset (its decompressed bytes freed off the main thread);
  a later update finishes the mesh and gives it to the cell. Without, or
  where the pool has no room (logged once), it is made on the main
  thread. Past the budget, the levels worth least go.
- **Failures.** A level that fails to load or to become a mesh leaves its
  cell as it is, logged and counted (`NphProxyStreamStats.failed`); the
  cell is not tried again.

Costs: creation allocates the cells' table, the builds' slots
(`max_loads`) and the picks'; an update allocates nothing of its own and
passes over the cells once (ADR-0063 "Stage 4"): each cell's request read
and its level wanted, its next finer level kept if it is among the
`max_loads` and eight more worth loading most, and its finest held if
among the 64 worth keeping least (bounded heaps, ties to the cell added
first, as a scan's); the loads and drops then take the picks in order,
passing over a pick whose cell the update has already changed. The loads
and the meshes allocate what the loader and the renderer do. On the open
landscape (384 cells, 2,639 levels) an idle update takes 0.006 to 0.03
ms, and with the jobs a frame that installs 0.05 to 0.08 ms on average
and 0.54 ms at most, whatever the levels' bytes (on the main thread, 0.12
ms and 0.54 ms for each megabyte: ADR-0063 "Stage 3c"). Tiled into a
world of the game's size (4,608 cells), an idle update takes 0.17 ms
(about 37 ns a cell) and an installing one 0.4 ms on average (about 2.4
µs a level installed); a pass and a scan for each load and drop, as
before, took 0.63 ms idle. `nph_proxy_stream_stats`
and `nph_proxy_stream_cell_state` report what the stream holds and does,
for tools and tests. NPH_MAIN_THREAD_ONLY.

## Texture streaming

`texture_stream.h` streams paged textures' levels (ADR-0065 stage 3;
[render.md](render.md) "Virtual textures"). A paged texture holds its
levels resident whole: its small levels from creation, and above them the
levels from its first resident one, which its samples clamp to. The caller
adds each paged texture with its levels' texels (the cooked texture's,
which it keeps) and removes it when it goes; each frame, between
`nph_gpu_frame_begin` and `nph_render_frame`, `nph_texture_stream_update`
installs the levels the pixels ask for and evicts what the budget cannot
hold. The samples' cooked-scene bridge does so with
`CookedSceneDesc.texture_budget`, and render_perf with
`--texture-budget=MB`.

- **Wanted.** A texture is wanted at the finest level its pixels asked for
  within `hold_frames` (240: a second at 240 Hz;
  `nph_render_texture_levels_requested`, the frame each level was last
  asked for), so that a texture glimpsed keeps its levels a while.
  Unasked so long, it wants its small levels alone.
- **Installs.** Each update the textures held coarser than wanted install
  their next finer level, a level at a time from the coarsest missing (a
  level is sampled only once whole), the textures furthest from their
  wanted level first, then the most recently asked: a level step each per
  update, so that a turn sharpens every texture it reveals a level before
  any texture two. At most `max_pages` pages an update (32, the renderer's
  moves a frame); a page waits where the renderer's staging or page memory
  is full. A level being installed that its texture stops wanting is
  evicted with what of it arrived.
- **Budget.** The levels held above the small ones, and those being
  installed whole, take at most `budget_bytes`. Before installing, it
  evicts the levels its textures do not want (finer than wanted, or of a
  texture unasked), the least recently asked first, the finest of each
  first. It evicts no wanted level, so that textures that want the same do
  not trade levels back and forth: where the wanted levels fill the
  budget, the rest stay coarser than wanted
  (`NphTextureStreamStats.short_textures`).
- **The page memory.** The renderer's page memory
  (`NphRendererDesc.texture_page_bytes`) must hold the budget beside the
  textures' small levels and the pages evicted within the frames in
  flight. Where it does not, the renderer refuses installs, and the stream
  reads why (`nph_render_texture_page_room`): pages returning, or the
  staging full, mean waiting; a page memory full of pages none of which
  are returning means the budget is too large for it, and the stream
  evicts what no texture wants as it would past the budget, a level an
  update (logged once), so that it never waits on itself.

A texture destroyed while the stream holds it is let go at the next
update; its pages went with it. Its texels come from the caller's levels,
a page's rows within the level's (`texture_stream_page_texels`, by the
format and size the renderer reports in `NphRenderTexturePages`), copied
as the page is installed: at most `max_pages` x 64 KiB an update on the
calling thread. An update asks the renderer for each texture's requests
(a lock each) and sorts those that want more, O(n log n).
`nph_texture_stream_stats` and `nph_texture_stream_texture_state` report
what the stream holds and does, for tools and tests. NPH_MAIN_THREAD_ONLY.

Measured on Bistro (ADR-0065 stage 3): its street wants 866 MB of levels
where its textures whole take 3.2 GiB (device-local 1.97 GB against 4.46),
settles from the small levels in 3.0 s at 32 pages a frame, and at 512 MB
leaves 189 of its 282 textures short with 0.24% more of its pixels changed
by more than 8 levels. The update costs 0.01 to 0.06 ms a frame, and
0.23 ms in frames that copy 32 pages.

The stream reaches the renderer's paging through a table of its calls
(`TextureStreamPaging`, internal): its tests drive the stream over a
stand-in, a page memory and staging and the frames each level was asked
for, so that its decisions run on every device, lavapipe's included
(which pages no textures); the GPU tests over a drawn plane skip there.

## Physics

While the world plays, every entity with a LocalTransform and a RigidBody
or a Collider (`components.h`) has a body in the engine's physics world
(physics.md),
which the physics system (`nph.physics`) steps once per simulation step,
after the game's systems and before the transform system:

| Component | |
|---|---|
| `RigidBody` | the body: static, kinematic, or dynamic; its mass (0: the Collider's), velocities, damping, gravity scale, locks, `bullet` (swept against moving bodies too), `can_sleep` |
| `Collider` | its shape: a sphere, a capsule along Y, or a box, at a centre and rotation in the entity's frame, scaled by the entity's world scale (a box per axis, a sphere by the largest, a capsule's core by Y and its radius by the larger of X and Z); its density, friction, restitution; its filter (category, mask, group); hit events |

A Collider alone is a static body (the level); a RigidBody alone, a point
of 1 kg (or of its `mass`).

**The game moves bodies through their components.** A dynamic body's pose
is written into its entity's LocalTransform and its velocities into its
RigidBody after every step. The game:

- changes velocities, or any other RigidBody field, and the body follows;
- writes a LocalTransform to teleport a dynamic or static body (drawn
  where it lands), or to move a kinematic one there over the step, pushing
  what it meets on the way (it stops the step after, unless moved again):
  a platform, a door;
- writes a Collider to make the shape (and the mass over it) again, and a
  new body type makes a new body.

A dynamic body moves in the world: its parent's motion does not carry it
(its LocalTransform is written relative to wherever the parent is); a
static or kinematic body follows its parent.

**The game uses the physics world** for what components do not say:
`nph_game_physics` returns it while playing (queries, character
controllers, joints, events, bodies of the game's own),
`nph_game_physics_body` an entity's body, and `nph_game_physics_entity` a
body's entity: what a query hit or an event reports (none for a body of
the game's own). The physics system runs alone,
so systems in the stages before and after it may use the world: those that
change bodies declare a write of RigidBody (so that no two run at once),
those that only query declare a read. A character controller's moves and
pushes run in an exclusive system (`samples/game_physics`).

**Play.** The physics world is made when play begins (with the `phys.*`
cvars) and a body for every entity, where it is, before the game's
`play_begin`, which may use them; the bodies of what `play_begin` makes or
moves are made when it returns, placed where it put them, before the first
step. Play end destroys them all. Play-in-editor plays a copy, so the
edited world's components never change. A hot reload keeps the bodies,
and a world migrated for new component layouts keeps every handle, so the
next step only compares.

**Finding changes.** The bridge keeps, per body, the components it was made
from or last wrote back. A step looks at the chunks whose RigidBody,
Collider, or LocalTransform columns were written since its previous run
(change ticks; its own writes carry its own tick) and applies what differs.
The world's event stream says what went and what changed parent; when it
overflowed, the next step checks every body. The write-back goes to the
chunks of the bodies that moved, through a query view stamping each chunk
once, with no per-entity events: a pile of a thousand moving boxes does not
touch the event stream.

**Debug drawing.** `phys.debug_draw` draws the physics world through the
renderer's debug lines at each frame's sync point; the editor shows every
Collider's outline and toggles these (editor.md "Physics").

## Interpolation

With the simulation stepping at `sim.rate` and frames coming at their own
rate, a frame may fall anywhere between two steps; drawn as of the last
step, motion stutters (a 144 Hz display over a 60 Hz simulation shows some
states twice and some once). The engine draws the world between its last
two simulated states instead (Fiedler, "Fix Your Timestep!", 2004;
§8.9):

1. after a frame's batch of steps, before anything else changes the world,
   the scene renderer captures what the simulation moved: where each
   instance, light, and the view's camera was at the previous capture and
   where it is now (`nph_scene_renderer_capture`);
2. every sync until the next capture draws them `fraction` of the way from
   one to the other (`nph_scene_renderer_sync_at`): positions linearly in
   64 bits, turns by normalised linear interpolation, scale and shear
   linearly (ADR-0050).

The fraction is 1 − (1 − α) / N (`nph_fixed_clock_blend`), with α the
clock's leftover fraction of a step and N the steps of the last batch: the
state one step behind the time the clock has reached, which the two states
bracket, so what is drawn moves at the pace of real time at any frame
rate. The cost is one step of delay (4.2 ms at 240 Hz), inside §10.6's
three frames. What changes outside the simulation (a camera rig in the
game's `frame`, an edit) is drawn as it is, and so is anything drawn the
first time.

It is off (everything drawn as it is) while paused, for single steps, in
lockstep (`rt.lockstep`, so runs repeat frame for frame), and with
`sim.interpolate` off. A move that is no motion, a teleport or a respawn,
is drawn where it lands rather than on the way: the game calls
`nph_game_reset_interpolation` (thread-safe; systems call it) for the
entity and its descendants, and the physics bridge does for the bodies it
teleports. A reset camera, or another camera than the last frame's, is a
camera cut (a new temporal history).

## Audio

The engine plays the world's sound through the audio module's mixer
([audio.md](audio.md)), which the audio bridge owns with the streamer and
the output device ([ADR-0058](adr/0058-audio-in-the-engine.md)):

| Component | |
|---|---|
| `AudioSource` | a sound (by GUID: a cooked sound asset, [cook.md](cook.md) "Sounds") on a bus (effects, music, dialogue, ambience, interface), at a volume and pitch, looping or not; `autoplay`; `playing`, which the game writes to start or stop it and the engine writes back when it starts by itself or ends; in 3D or not, with its fall-off (inverse, linear, a four-point curve in dB, or none), size, Doppler, occlusion, reverberation send, and priority |
| `AudioListener` | where 3D sound is heard: the first active one; else the host's view (the editor's camera), else the first active Camera |

**The output** (`audio.device`): `auto` plays to the system's device with
a window and to nothing without one; `system` (falling back to `null` when
the machine has no device), `null` (a thread asking for the mix in real
time, without hardware), `offline` (each frame renders its own duration, a
step's in lockstep, on the engine's thread: tests and capture,
`nph_engine_audio_output`; faster than real time, so before each render it
waits for the streamer to hold half of every playing stream's ring, and a
capture never starves for the machine's speed), or `off`. The mix runs at the device's own rate
and channels unless `audio.sample_rate` and `audio.channels` say otherwise,
so nothing converts at the edge.

**While the world plays**, at each sync point:

- Like a system run, the bridge looks at the chunks whose AudioSources were
  written since its last pass (change ticks) and compares each with its
  voice's record.
  - A source that starts playing gets a voice, and one that stops loses it
    (a short fade).
  - Volume, pitch, the reverberation's send, and occlusion change a playing
    voice at once, and a new sound plays again. The rest (looping, bus, 3D,
    fall-off, size, Doppler, priority), which a mixer voice keeps from its
    start, applies when it next plays.
  - A source seen for the first time with `autoplay` starts by itself: when
    play begins, or when it is spawned or given its AudioSource while
    playing.
  - The event stream says what went: a despawn or a removed AudioSource
    stops its voice.
- After the view is chosen, every 3D voice is placed in the listener's
  frame: its entity's world position less the listener's, in 64 bits,
  rounded to 32 in the listener's axes, with the velocity of its motion
  since the last frame (smoothed over 50 ms; none across a teleport,
  `nph_engine_reset_interpolation`) for Doppler.
- Sources that ask for occlusion sample their centre and four points
  around it, at their size or 30 cm, through the physics world, their own
  body ignored (as Steam Audio's volumetric occlusion samples a sphere
  round a source):
  - a probe from the centre to each point finds those it cannot see, which
    lie in or behind what the source rests against and are not part of it;
  - a sight line from the listener to each of the rest finds what hides
    it, every line ending 5 cm short, so the surface a source rests on (an
    impact's) does not.

  The share hidden becomes the voice's occlusion, followed over 80 ms
  ([audio.md](audio.md) "3D"), except as it starts: its first rays are
  cast before it plays, and it starts as hidden as they say, so a knock
  behind a wall is dark from its attack. At most `audio.occlusion_rays`
  are cast a frame (up to nine a source), the voices starting in it
  first, then round the rest; one that starts past the budget starts open
  and is cast first thing the next frame.

**Sounds** load when first named: the world's when play begins (waited
for, so what plays by itself starts on time), a one-shot's on first use
(`nph_engine_preload_sound` loads it ahead, from the game's `play_begin`).
A sound in memory becomes a clip, a copy, since the audio thread must never
fault on a mapping, and its asset is released. A streamed sound keeps its
file open, checked against the asset the loader mapped, and each of its
voices gets a stream that reads it at DATA's offset
([asset.md](asset.md) "The loader"). The streamer primes the stream
(stream.h `defer_fill`), so the main thread does no I/O, and the voice
starts once its ring holds a quarter second (half the ring, when
`audio.stream_seconds` makes it smaller). Sounds stay loaded until play
ends, which stops every voice, waits for the streams to close, and
releases them all.

**One-shots** (`nph_engine_play_sound`, `nph_game_play_sound`): a sound
played once without an entity of its own, from any thread (the
simulation's systems), queued (256 a frame) and started at the next sync
point, in 3D at a position or following an entity. One whose sound takes
more than a quarter second to load is dropped: late is worse than not at
all.

`samples/game_audio` plays a yard of sound through all of this
([audio.md](audio.md) "The samples"), and `samples/game_physics` sounds its
impacts.

**Buses.** Five buses under the master take the sources, each with a
volume cvar (`audio.volume_effects` and the others, with `audio.volume`
for the master; saved), and every voice also sends to a sixth with the
room's reverberation (`audio.reverb`). `nph_engine_audio_bus` and
`nph_engine_audio_mixer` give a game the rest of the mixer from its
callbacks: effects on the engine's buses, buses and snapshots of its own.

## The player

`nph_player` runs a game module in a window, playing from the first frame:

```
nph_player --game=build/bin/Debug/game_sandbox.dll \
           --pack=build/samples/asset_viewer/Debug/showcase.npk
nph_player --game=... --headless --frames=600 sim.rate=120
```

`player.title_fps=true` puts the frame rate and the frame time in the
window's title, measured twice a second (`r.vsync=false` uncaps them from
the display's refresh). The sandbox sample flies a camera through any cooked
scene: `sandbox.scene=bistro.gltf#scene` with Bistro's pack mounted, lit by
its manifest's values, and the city and the landscape with the renderer's
capacities set and their tiles, cells and streams on (the sample's header gives
the commands). `scene_viewer` (docs/benchmarks.md "Reference scenes") walks
through the same scenes on the benchmark's own path.

It exits with 0, 1 on a failure (validation errors included), 2 for a bad
command line, and 77 without a window system or a device, which CTest
reports as a skip (`sample.game_sandbox`, `sample.game_sandbox_headless`,
and `sample.game_physics`; `sample.game_physics_headless` runs the physics
sample's autopilot and checks its verdict in the log).

## Threading model

The engine belongs to the thread that created it (NPH_MAIN_THREAD_ONLY);
with a window, that thread also initialised the window system. Systems run
on the job system during the simulation; the physics system runs alone in
its stage, on the thread running the schedule, and its step spreads over
the job system. The game's callbacks, the host's hooks, and the console run
on the engine's thread at the sync point, with the world quiescent. The
render hook runs while the next simulation runs: it must not touch the
world. `nph_engine_reset_interpolation` and `nph_engine_play_sound` may be
called from any thread. The mixer is the engine's thread's (its commands);
the device's thread renders it (offline, the engine's own, at the end of
the sync point), and the streamer's thread fills the streams.

## Costs (§1.4)

Creation allocates every subsystem from the host's allocator. A frame
allocates nothing on the heap in steady state: the schedule's scratch
arenas, the render graph's cached transients, the scene renderer's tables,
and the physics bridge's records and lists are reused (the physics world's
arrays keep their size, physics.md). Play begin allocates the physics world
and a record per body. The audio bridge's records and sound table grow
when needed and are kept; a sound allocates its clip or its file when it
loads, and a streamed sound's voice its stream's ring when it starts.
Reloads, migrations, play copies, and scene files
allocate while they run. Steady-state costs: the sync point is a wait for
the simulation, the transform update, and the scene renderer's sync (one
version check per chunk of a static world); kicking the simulation is one
job.

## Performance

`benchmarks/runtime` on the reference CPU (Ryzen 7 5800X, Profile build;
`benchmarks/results/runtime-2026-10-16-win-msvc-profile-ryzen7-5800x.json`):

| Benchmark | Median | |
|---|---|---|
| `engine_frame_headless` | 3.1 µs | a frame of an empty world playing in lockstep: the sync point, the simulation job started and waited for, the profiler's collection |
| `engine_frame_headless_10k` | 5.5 µs | the same with 10,000 entities that do not move |
| `engine_frame_headless_10k_moving` | 46.8 µs | the same with every LocalTransform written each frame |
| `engine_play_copy_10k` | 2.2 ms | play-in-editor on 10,000 entities: the copy with handles kept, the world switch, stopping |
| `sim_input_frame` | 0.29 µs | a frame of input into the accumulator and a step's snapshot (16 actions) |
| `console_set_cvar` | 0.25 µs | a cvar set through the console |

The engine's own frame overhead is a few microseconds, well inside the
§10.5 main-thread budget of 1 ms. A static world adds about 0.24 ns per
entity (one version check per chunk). Play-in-editor starts in about 2 ms
for an editor-sized world.

Physics and interpolation
(`benchmarks/results/runtime-2026-10-24-win-msvc-profile-ryzen7-5800x.json`;
the scene renderer's on an offscreen engine on the RTX 3060 Ti, per
instance):

| Benchmark | Median | |
|---|---|---|
| `engine_frame_headless` | 4.05 µs | the empty frame above, now with the physics system: 3.04 µs without it (an empty step and the bridge's checks; its stage boundary measured free) |
| `engine_frame_physics_1k_resting` | 4.44 µs | 1,000 crates asleep on a floor: a settled scene adds 0.4 µs |
| `engine_frame_physics_1k_spinning` | 120.6 µs | 1,000 bodies turning in place, each moved, written back, and composed every step |
| `physics_step_1k_spinning` | 68.1 µs | the same bodies stepped in a physics world of their own |
| `scene_sync_10k_moving` | 79 ns | the scene renderer bringing a moved instance up to date |
| `scene_interpolate_10k_moving` | 262 ns | the same, captured and drawn between its states |

A frame of moving bodies costs about 48 ns a body more than their step
alone: the write-back's 18 ns (its chunk and row, the LocalTransform and
RigidBody written, the record's copy), the transforms' 4 ns, and the step
itself running slower inside the simulation's job than on its own (70 to
88 µs there, timed inside the bridge). Drawing between steps costs a moving
instance about 180 ns more than drawing it as it is: its decomposition at
the capture and its blend at the sync, on the main thread at the sync point
(Limitations).

Audio (`benchmarks/results/runtime-2026-10-25-win-msvc-profile-ryzen7-5800x.json`;
the mix on the null device's thread, as a game's is on the hardware's):

| Benchmark | Median | |
|---|---|---|
| `engine_frame_audio_idle` | 4.3 µs | the empty frame with audio on and nothing sounding: about 0.2 µs more than without it, run back to back (the listener, the buses' volumes, the mixer's update) |
| `engine_frame_audio_256_moving` | 23.0 µs | 256 spatial sources circling the listener, 64 mixed and the rest virtual: about 70 ns a source a frame (placed in the listener's frame with its velocity, eight sources' occlusion in an empty physics world, and its transform) |
| `engine_frame_audio_64_in_10k` | 12.4 µs | 64 sources standing among 10,000 static boxes, occlusion off |
| `engine_frame_audio_64_in_10k_occluded` | 25.2 µs | the same with the default occlusion budget, 72 rays a frame: 12.8 µs, 0.18 µs a ray |

The bridge stays a small share of §10.5's 1 ms on the main thread; its
largest part, occlusion, is bounded by `audio.occlusion_rays`.

## Testing

`engine/runtime/tests` (`runtime_tests`, statically linked):

- the clock, simulation input, the console, and the command line;
- the scene renderer and the importer on the showcase, cooked at build
  time: instances and lights appear when the assets load, follow
  transforms, hide, go with despawns and removals, fail and default
  safely, keep shared assets across world switches, and recover from an
  overflowed event stream; a host hides an instance (across moves and a
  new mesh) and reloads a mesh, a texture through its material, and a
  material, with nothing left behind; the camera and the sky;
- proxy streaming's decisions (`test_proxy_stream.c`): the requests'
  scale, the level a cell is wanted at, a level's worth, and when one
  pushes another out past the budget. The stream itself runs over a
  cooked world in `tests/integration` (`test_proxy_stream.c`, the cooker's
  grove of four cells with four levels each): its cells stream in the
  levels the views ask for, each once, and draw what the cells given every
  level draw, and the stream gone leaves them their coarsest; within half
  the bytes, of two groves the nearer holds the finer levels, nothing moves
  while the camera stays, and turned round they trade levels, each given
  once; a level that cannot be had (an asset no pack holds, a mesh that is
  not a level) leaves its cell as it is;
- the engine: a headless engine leaks nothing (a debug allocator under
  it), configuration precedence and the user config, scenes in both
  formats, console commands, play, pause and steps, the view override, an
  offscreen engine drawing the showcase, and a windowed engine presenting
  under the validation gate (the test that found the read-only depth
  store; gpu.md).

- physics: a box falls onto a floor and rests where its body does; play
  in the editor leaves the edited world alone; the game moves bodies
  through their components (velocities, teleports, kinematic targets that
  carry a rider, type changes); components coming and going make and
  unmake bodies; children follow their parents unless dynamic; a flood of
  world events is recovered; and runs repeat whatever the threads, under
  every solver (the state hashes of a pile);
- interpolation: the clock's blend advances at the pace of real time; the
  scene renderer draws what the simulation moved between its states
  (instances turning without shrinking, frame-rate changes, snaps,
  despawns), lights and the camera too, with cuts; instances take only the
  material slots they name.

`runtime_game_tests` (against `nepheline_shared`, with three builds of a
test game): loading and simulating; reloads that migrate the world (a new
field with its default, every handle kept), keep or reset the state block,
carry tuned cvars over, and drop a removed component; a broken build that
leaves the running one in place; play-in-editor on a copy with the same
handles, and a reload during play; pipelined and sequential frames
agreeing; unloading and loading again; what `play_begin` makes has its
body, placed and at rest, before the first step.

- audio (`test_audio_bridge.c`, headless engines mixing offline in
  lockstep, sound assets written for the test): a source plays by itself
  and says when it ends; the game starts, retunes, and stops one; a 3D
  source sounds from its side of the listener and falls off with distance,
  and the camera hears when there is no listener; a wall between darkens
  and quietens, a knock that starts behind one is dark from its attack
  (three at once under the least budget too, at most nine rays a frame),
  and the floor a source rests on does not hide it; one-shots from another
  thread, a full queue counted, a
  sound that cannot load given up; a streamed sound primes on the streamer
  and plays without starving, and play's end closes it; offline, a loop
  with a tenth-second ring (smaller than a voice primes) starts at half of
  it and renders five seconds far faster than real time without a frame
  starved; despawned and
  removed sources stop, and one added back plays again; the buses'
  volumes; with audio off, nothing plays.

`samples/game_physics` (`sample.game_physics_headless`): the autopilot
walks the player round the playground (up the stairs, off the landing, a
jump, onto the swinging platform for a ride, more balls thrown than are
kept) and the run fails unless it says it got round; its impacts sound,
mixed offline.

`samples/game_audio` (`sample.game_audio_headless`): the yard plays 12.5 s
mixed offline, and the run fails unless the game, listening to its mix,
says it ran clean: sound throughout, the mix leaning towards the drone's
side, its one-shots played, nothing starved or dropped ([audio.md](audio.md)
"The samples"). The windowed runs of both play to the null device.

## Limitations and future work

- Proxy streaming's update is linear in the cells: a world of thousands
  needs its requests read, and its loads and drops picked, in jobs or
  from heaps (ADR-0063 stage 4). A level made on a job takes a frame
  more to arrive than one made on the main thread (at the landscape's 62
  ms frames, settling took 27 s where 18).
- The scene renderer's sync runs serially on the main thread: 10,000
  moving instances cost 0.79 ms (2.6 ms drawn between steps) of the 1 ms
  main-thread budget. Capturing and blending in parallel jobs, or blending
  on the GPU (every consumer of instance transforms would), comes before
  games move that many.
- Colliders are spheres, capsules, and boxes. Convex hulls, triangle
  meshes, and heightfields need cooked collision assets; compounds through
  the hierarchy, joint components, and sensors are to come (the physics
  API has them all meanwhile).
- The character controller is not a component: a game keeps its
  characters in its state block and moves them in its systems.
- Hot reload unloads the old image only after checking the engine's own
  references; pointers a game keeps in places the engine cannot see (its
  own threads, OS callbacks) are ruled out by game.h's rules, not checked.
- A reload that changes components while a play copy runs stops play
  instead of migrating both worlds.
- Audio: sounds stay loaded until play ends (unloading a region's sounds
  comes with unloading a region: the world partition streams proxies and
  textures today, not whole cells); a playing
  voice keeps its fall-off, size, Doppler, looping, and bus from its start;
  one listener; occlusion is the share of five points hidden, with no
  obstruction from it (a game sets that through the mixer) and no
  propagation round corners;
  HRTF and ambisonics are §8.10's "later".
- On Windows a debugger may hold a game module's PDB; game modules that
  need it link with a unique PDB name per build.
- Shipping builds still load the game as a module; linking a game into
  the player statically (for whole-program optimisation) is Phase 15.
