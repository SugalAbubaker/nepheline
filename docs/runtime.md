# Runtime (`engine/runtime`)

The last module in the order (CLAUDE.md §4): the `NphEngine` context that
owns every subsystem, the frame loop, configuration and the console, input
as the simulation sees it, the bridge from the ECS world to the renderer,
and game modules with hot reload (§8.12). The player (`nph_player`) and the
editor are hosts of an engine. Decisions:
[ADR-0023](adr/0023-runtime.md) (the engine and its frame) and
[ADR-0024](adr/0024-game-modules.md) (game modules, hot reload, and the
§13 scripting decision: C only).

| Header | What |
|---|---|
| `engine.h` | `NphEngine`: creation and destruction, frames, play and pause, scenes, the game module, the view, subsystem accessors |
| `game.h` | the game module ABI (`NphGameApi`, `nph_game_entry`) and the services game code calls |
| `config.h` | the command line: options and cvar assignments |
| `console.h` | commands and cvars behind one command line |
| `clock.h` | the fixed-step clock |
| `sim_input.h` | input snapshots for simulation steps |
| `components.h` | MeshRenderer, Light, Camera, Environment |
| `scene_render.h` | the scene renderer (world to renderer) and the cooked-scene importer |

## The engine

`nph_engine_create` brings the subsystems up in module order and
`nph_engine_destroy` takes them down in reverse (§8.12):

1. cvars, the console and its commands; the user config, `--config`
   files, and command-line assignments, in rising precedence;
2. profiling (`prof.enabled`): core recording, unless the host started it,
   and the collector, tracking the engine's five allocators;
3. the job system; the VFS with every `--pack` and `--content` mounted; the
   asset loader (with the host's resolver for loose cooked files);
4. input; then by mode the window, the device, the swapchain or offscreen
   target, the render graph (recording passes on jobs), the renderer, the
   GPU profiler, and the scene renderer;
5. the edited world, its transform system, and its schedule;
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
five tracked wrappers: engine, world, render, assets, game. The profiler
reports them; destruction logs any bytes still live in one.

**Logging** is the host's to set up (sinks, the logger thread): the player
and the editor do, tests may not. The engine only logs.

## The frame

```
poll events -> input -> sim input
sync point:  wait for the simulation | swap in a new game build
             gpu frame begin | game frame | host sync hook
             transforms | scene renderer sync | clear world events | debug flip
kick:        the next frame's simulation (a job)
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
step is a run of the schedule (the game's systems and the transform
system) with that step's input (`sim_input.h`): the first step of a frame
gets the edges and the motion gathered since the last step, so a key
tapped between steps is seen once. `rt.lockstep` takes exactly one step
per frame whatever the time: tests, frame-by-frame debugging. Rendering
does not interpolate between steps yet; Phase 11 adds interpolation for
rates that differ from the frame rate (§8.9).

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
`r.exposure_compensation`, `r.exposure_speed`, and `r.motion_blur` cvars
choose the renderer, its culling, its rasterisers, how it shadows the sun,
whether it resolves temporally, its bloom, how it exposes, and whether the
camera's shutter blurs motion (docs/render.md, "The production path",
"Shadows", "Temporal anti-aliasing", "Motion blur", and "Bloom and
exposure"); an automatic exposure starts from the camera's EV100 and
adapts at the pace of the frames' real time (one simulation step a frame
under `rt.lockstep`, so runs repeat), and the motion blur spans the
camera's shutter time over that frame time. A switch between the world's camera
and the host's is a camera cut: the frame after it starts a new temporal
history; a host that moves its camera discontinuously sets `camera_cut`
in that frame's view.

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
| `r.vsync`, `r.width`, `r.height` | true, 1280, 720 | saved, latched |
| `r.pipeline` | 1 | 0: the forward reference renderer, 1: the GPU-driven production one (saved) |
| `r.occlusion`, `r.cone_culling` | true, true | the production path's culling stages |
| `r.sw_raster` | true | the production path's compute rasteriser for small triangles |
| `r.virtual_shadows` | true | the production path shadows the sun with its virtual shadow map (false: with rays) |
| `r.taa` | true | the production path's temporal anti-aliasing at native resolution |
| `r.bloom` | 0.04 | the bloom's weight in the image, 0 to 1 (saved) |
| `r.auto_exposure` | false | meter each frame and adapt the exposure, from the camera's EV100 (saved) |
| `r.exposure_compensation`, `r.exposure_speed` | 0, 1.5 | the automatic exposure's offset in stops (positive: brighter) and e-foldings per second (saved) |
| `r.motion_blur` | true | motion blur over the camera's shutter time (its `shutter`, 0 by default; saved) |
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
is no failure. A frame never waits for a load (§10.5).

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

## The player

`nph_player` runs a game module in a window, playing from the first frame:

```
nph_player --game=build/bin/Debug/game_sandbox.dll \
           --pack=build/samples/asset_viewer/Debug/showcase.npk
nph_player --game=... --headless --frames=600 sim.rate=120
```

It exits with 0, 1 on a failure (validation errors included), 2 for a bad
command line, and 77 without a window system or a device, which CTest
reports as a skip (`sample.game_sandbox`, `sample.game_sandbox_headless`).

## Threading model

The engine belongs to the thread that created it (NPH_MAIN_THREAD_ONLY);
with a window, that thread also initialised the window system. Systems run
on the job system during the simulation; the game's callbacks, the host's
hooks, and the console run on the engine's thread at the sync point, with
the world quiescent. The render hook runs while the next simulation runs:
it must not touch the world.

## Costs (§1.4)

Creation allocates every subsystem from the host's allocator. A frame
allocates nothing on the heap in steady state: the schedule's scratch
arenas, the render graph's cached transients, and the scene renderer's
tables are reused. Reloads, migrations, play copies, and scene files
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
- the engine: a headless engine leaks nothing (a debug allocator under
  it), configuration precedence and the user config, scenes in both
  formats, console commands, play, pause and steps, the view override, an
  offscreen engine drawing the showcase, and a windowed engine presenting
  under the validation gate (the test that found the read-only depth
  store; gpu.md).

`runtime_game_tests` (against `nepheline_shared`, with three builds of a
test game): loading and simulating; reloads that migrate the world (a new
field with its default, every handle kept), keep or reset the state block,
carry tuned cvars over, and drop a removed component; a broken build that
leaves the running one in place; play-in-editor on a copy with the same
handles, and a reload during play; pipelined and sequential frames
agreeing; unloading and loading again.

## Limitations and future work

- Rendering does not interpolate between simulation steps (Phase 11, with
  physics).
- Hot reload unloads the old image only after checking the engine's own
  references; pointers a game keeps in places the engine cannot see (its
  own threads, OS callbacks) are ruled out by game.h's rules, not checked.
- A reload that changes components while a play copy runs stops play
  instead of migrating both worlds.
- On Windows a debugger may hold a game module's PDB; game modules that
  need it link with a unique PDB name per build.
- Shipping builds still load the game as a module; linking a game into
  the player statically (for whole-program optimisation) is Phase 15.
