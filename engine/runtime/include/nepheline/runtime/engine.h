// SPDX-License-Identifier: Apache-2.0
#pragma once

// The engine (CLAUDE.md §8.12; docs/runtime.md; docs/adr/0023-runtime.md):
// the one context that owns every subsystem (§2 "No global mutable state
// outside an explicit NphEngine context"), brings them up in module order
// and down in reverse, and runs the frame.
//
// The frame (docs/runtime.md "The frame"):
//
//   1. window events become input (the host's event hook sees them first);
//   2. the sync point: the simulation job of the previous frame has
//      finished, so the world is quiescent. The scene renderer captures
//      what the steps moved (for interpolation), a newer game build is
//      swapped in, the game's `frame` and the host's `sync` hook run (the
//      editor applies its edits), the world transforms are updated, the
//      scene renderer mirrors the world, drawing what moves between its
//      last two simulated states, and the world's events are cleared;
//   3. the simulation of the next frame starts as a job: the fixed-step
//      clock decides how many steps, each a run of the schedule (the
//      game's systems, the physics system, and the transform system) with
//      that step's input;
//   4. meanwhile this thread records and submits the frame: the renderer's
//      passes, the host's render hook (the editor's UI), and the present.
//
// So the simulation of frame N+1 overlaps the rendering of frame N, and
// the CPU runs at most one frame ahead of the GPU (§10.5, §10.6): the
// renderer and the render hook never touch the world (they draw what the
// sync point mirrored). With rt.pipelined off, the simulation runs at the
// sync point instead, before rendering (debugging, profiling one
// subsystem at a time).
//
// Modes: WINDOWED (a window and a swapchain), OFFSCREEN (a GPU device and
// no window: the frame renders into an image; tests, thumbnails), and
// HEADLESS (no window and no GPU: the simulation only; tests, servers).
//
// Configuration: the engine's settings are cvars (`rt.*`, `r.*`, `sim.*`,
// `phys.*`, `audio.*`, `game.*`, `prof.*`; `cvars` in the console lists
// them) set, in rising
// precedence, by the defaults, the user config, --config files, and
// command-line assignments (config.h).
//
// Logging is the host's: the engine logs through core/log.h and expects
// the host to have set up sinks and the logger thread (the player and the
// editor do; tests may not).
//
// Thread-safety: NPH_MAIN_THREAD_ONLY: the engine belongs to the thread
// that created it, which must be the one that initialised the window
// system when there is a window.

#include <nepheline/asset/loader.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/gpu/swapchain.h>
#include <nepheline/platform/window.h>
#include <nepheline/profile/profiler.h>
#include <nepheline/render/gpu_profiler.h>
#include <nepheline/render/renderer.h>
#include <nepheline/runtime/audio.h>
#include <nepheline/runtime/clock.h>
#include <nepheline/runtime/config.h>
#include <nepheline/runtime/game.h>
#include <nepheline/runtime/scene_render.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_RUNTIME);

typedef enum NphEngineMode {
    NPH_ENGINE_MODE_WINDOWED = 0,
    NPH_ENGINE_MODE_OFFSCREEN = 1,
    NPH_ENGINE_MODE_HEADLESS = 2,
} NphEngineMode;

// The host's hooks (the editor's, mostly). Every one is optional and runs
// on the engine's thread.
typedef struct NphEngineHooks {
    void *user;
    // A window event, before the engine's input sees it; true consumes it
    // (a UI with keyboard focus).
    bool (*event)(NphEngine *engine, const NphWindowEvent *event, void *user);
    // At the sync point, after the game's `frame` and before the world is
    // mirrored: the world is quiescent, so this is where a host reads it
    // (the world's events are still there) and edits it.
    void (*sync)(NphEngine *engine, void *user);
    // The image the scene is rendered into, instead of the swapchain's
    // (the editor's viewport): returns false to use the default. The image
    // must be a valid render target for nph_render_frame.
    bool (*scene_target)(NphEngine *engine, NphGpuImage *out, void *user);
    // After the scene's passes, before the present: a host adds its own
    // passes (the editor's UI). `backbuffer` is the frame's swapchain image
    // in the graph (null in OFFSCREEN mode), `scene` the scene's target.
    // The simulation of the next frame runs meanwhile: the world must not
    // be touched here.
    void (*render)(NphEngine *engine, NphGpuRenderGraph *graph, NphGpuRgImage backbuffer,
                   NphGpuRgImage scene, void *user);
    // The world was replaced (a reload that changed component layouts,
    // play-in-editor starting or ending): references to `old_world` itself
    // are invalid; entity handles are the same in both.
    void (*world_replaced)(NphEngine *engine, NphWorld *old_world, NphWorld *new_world, void *user);
} NphEngineHooks;

typedef struct NphEngineDesc {
    NphAllocator *alloc;           // NULL: the system allocator
    const NphEngineConfig *config; // the command line; NULL: defaults
    NphEngineMode mode;            // overridden to HEADLESS by config->headless
    const char *app_name;          // window title, device name (NULL: "Nepheline")
    const char *shader_dir;        // compiled shaders (NULL: this build's)
    u32 offscreen_width;           // OFFSCREEN: the target's size (0: 1280 x 720)
    u32 offscreen_height;
    // Simulate as soon as the engine is up (the player). The editor starts
    // paused and plays a copy of the world (nph_engine_play_begin).
    b32 play_on_start;
    // Names the files of asset identities no mounted pack indexes (loose
    // cooked content: the editor answers from the cooker's database);
    // asset/loader.h NphAssetLoaderDesc.resolve. Optional.
    NphAssetResolveFn resolve;
    void *resolve_user;
    NphEngineHooks hooks;
} NphEngineDesc;

typedef struct NphEngineFrameStats {
    u64 frame;       // frames run so far
    f64 frame_ms;    // the last frame's wall time
    f64 sync_ms;     // its sync point (wait for the simulation included)
    f64 sim_wait_ms; // of which waiting for the simulation
    f64 render_ms;   // recording and submitting
    u32 steps;       // simulation steps started by the last frame
    u64 total_steps; // since play began
    u64 dropped_ns;  // simulated time dropped by the step cap
    NphSceneRenderStats scene;
    NphRenderFrameStats render;
} NphEngineFrameStats;

// Brings the engine up: cvars and the console; the user config, --config
// files, and command-line assignments; profiling (prof.enabled); the job
// system; the VFS with --pack and --content mounted; the asset loader; the
// window, input, device, swapchain, renderer, and scene renderer (by
// mode); the world and its schedule; the game module (--game); a scene
// (--scene); the --script. NPH_RESULT_UNSUPPORTED when the mode needs a
// window system or a GPU that is missing; the subsystems' errors. Nothing
// is left behind on failure.
NPH_MUST_USE NphResult nph_engine_create(const NphEngineDesc *desc, NphEngine **out);

// Waits for the simulation and the GPU, unloads the game (play_end and
// unload first), writes --trace and the user config, and shuts everything
// down in reverse order. Safe on NULL.
void nph_engine_destroy(NphEngine *engine);

// Runs frames until the window is closed, nph_engine_quit is called, or
// rt.max_frames frames have run. The first frame failure ends the loop and
// is returned.
NPH_MUST_USE NphResult nph_engine_run(NphEngine *engine);

// Runs one frame (hosts with their own loop, tests).
NPH_MUST_USE NphResult nph_engine_frame(NphEngine *engine);

// Waits for the simulation the last frame started (pipelined). Until the
// next frame, the world is the caller's to read and change, as at the
// sync point. Returns the simulation's failure, if it failed (the engine
// also pauses then). The engine's own functions that touch the world wait
// by themselves.
NPH_MUST_USE NphResult nph_engine_wait(NphEngine *engine);

// Ends nph_engine_run after the current frame.
void nph_engine_quit(NphEngine *engine);
bool nph_engine_should_quit(const NphEngine *engine);

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

// Starts simulating the world (the game's play_begin first). With `copy`,
// the edited world is copied (entity handles kept) and the copy plays,
// while the edited world waits untouched: play-in-editor. Called at the
// sync point (or between frames). NPH_RESULT_INVALID_STATE when already
// playing; the copy's and play_begin's errors.
NPH_MUST_USE NphResult nph_engine_play_begin(NphEngine *engine, bool copy);

// Stops (play_end first); a play copy is discarded and the edited world is
// shown again. Nothing when not playing.
void nph_engine_play_end(NphEngine *engine);

bool nph_engine_is_playing(const NphEngine *engine);

// Pauses or resumes the simulation while playing; `step_once` runs exactly
// one step on the next frame while paused (frame-by-frame debugging).
void nph_engine_set_paused(NphEngine *engine, bool paused);
bool nph_engine_is_paused(const NphEngine *engine);
void nph_engine_step_once(NphEngine *engine);

// ---------------------------------------------------------------------------
// The world and scenes
// ---------------------------------------------------------------------------

// The world being shown and simulated: the play copy during play-in-
// editor, the edited world otherwise.
NphWorld *nph_engine_world(NphEngine *engine);

// The edited world (the one scenes load into and save from).
NphWorld *nph_engine_edit_world(NphEngine *engine);

const NphRuntimeComponents *nph_engine_components(const NphEngine *engine);

// Loads a scene file (text, or binary by its magic) into the edited world,
// in addition to what it holds. NPH_RESULT_INVALID_STATE while playing;
// the file's and the scene loader's errors (the world is unchanged on
// failure).
NPH_MUST_USE NphResult nph_engine_load_scene(NphEngine *engine, const char *path);

// Saves the edited world as a text scene (scene_io.h). NPH_RESULT_IO_ERROR
// and the saver's errors.
NPH_MUST_USE NphResult nph_engine_save_scene(NphEngine *engine, const char *path);

// Removes every entity of the edited world. NPH_RESULT_INVALID_STATE while
// playing.
NPH_MUST_USE NphResult nph_engine_clear_scene(NphEngine *engine);

// ---------------------------------------------------------------------------
// The game module
// ---------------------------------------------------------------------------

// Loads the game module at `path` (the --game option does this at
// creation). A module already loaded is unloaded first. The module's and
// its load callback's errors; NPH_RESULT_VERSION_MISMATCH for another ABI.
NPH_MUST_USE NphResult nph_engine_load_game(NphEngine *engine, const char *path);

// Unloads the game module (play_end and unload first).
void nph_engine_unload_game(NphEngine *engine);

// Swaps in a newer build of the game module now if its file changed (the
// engine also checks every game.reload_poll_ms while game.hot_reload is
// on). Returns NPH_RESULT_OK when it reloaded or nothing changed (see
// `out_reloaded`, may be NULL), and the failure that kept the running
// build otherwise.
NPH_MUST_USE NphResult nph_engine_reload_game(NphEngine *engine, bool *out_reloaded);

// The engine's handle on the game, or NULL without one.
NphGame *nph_engine_game(NphEngine *engine);

// The loaded build's API (NULL without a game) and how many builds have
// been loaded since the first.
const NphGameApi *nph_engine_game_api(const NphEngine *engine);
u32 nph_engine_game_reloads(const NphEngine *engine);

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

// Overrides the view (the editor's camera): from now on frames render
// `view` instead of the world's active Camera. NULL returns to the world's.
// The r.pipeline, r.occlusion, r.cone_culling, r.sw_raster,
// r.virtual_shadows, r.taa, r.bloom, r.auto_exposure,
// r.exposure_compensation, r.exposure_speed, r.ao, and r.gi cvars replace
// the view's `pipeline`, `vis_flags`, `anti_aliasing`, `bloom`, `exposure`,
// `exposure_compensation`, `exposure_speed`, `ao_radius`, and `gi` either way
// (r.motion_blur and r.dof turn its `shutter` and `aperture` off), and the
// engine sets `frame_seconds` (one simulation step per frame under
// rt.lockstep); an automatic exposure starts from the view's `ev100`. The
// first frame after a switch between the override and the world's camera
// is a camera cut; a caller that moves its own camera discontinuously sets
// `camera_cut` in the view it passes for that frame.
void nph_engine_set_view(NphEngine *engine, const NphRenderView *view);

// The view the last frame rendered.
const NphRenderView *nph_engine_view(const NphEngine *engine);

// ---------------------------------------------------------------------------
// Subsystems
// ---------------------------------------------------------------------------

NphEngineMode nph_engine_mode(const NphEngine *engine);
NphAllocator *nph_engine_allocator(NphEngine *engine);
NphCvarRegistry *nph_engine_cvars(NphEngine *engine);
NphConsole *nph_engine_console(NphEngine *engine);
NphJobSystem *nph_engine_jobs(NphEngine *engine);
NphVfs *nph_engine_vfs(NphEngine *engine);
NphAssetLoader *nph_engine_loader(NphEngine *engine);
NphInput *nph_engine_input(NphEngine *engine);
NphSchedule *nph_engine_schedule(NphEngine *engine);
// NULL when profiling is off.
NphProfiler *nph_engine_profiler(NphEngine *engine);
// NULL in HEADLESS mode.
NphWindow *nph_engine_window(NphEngine *engine); // also NULL in OFFSCREEN mode
NphGpuDevice *nph_engine_device(NphEngine *engine);
NphGpuSwapchain *nph_engine_swapchain(NphEngine *engine); // also NULL in OFFSCREEN mode
NphRenderer *nph_engine_renderer(NphEngine *engine);
NphRenderGpuProfiler *nph_engine_gpu_profiler(NphEngine *engine);
NphSceneRenderer *nph_engine_scene_renderer(NphEngine *engine);
// OFFSCREEN: the image frames render into (read it back after
// nph_gpu_device_wait_idle).
NphGpuImage nph_engine_offscreen_target(const NphEngine *engine);

const NphEngineFrameStats *nph_engine_frame_stats(const NphEngine *engine);

// The fixed-step clock (rate from sim.rate).
const NphFixedClock *nph_engine_clock(const NphEngine *engine);

// ---------------------------------------------------------------------------
// Physics (docs/runtime.md "Physics")
// ---------------------------------------------------------------------------

// The physics world while the world plays (NULL otherwise): a body for every
// entity with a RigidBody or a Collider (components.h), and any the game
// makes itself. Use it between steps: from the simulation's systems (the
// physics system, NPH_PHYSICS_SYSTEM_NAME, runs alone), at the sync point,
// or after nph_engine_wait. Made at play_begin with the phys.* cvars.
NphPhysicsWorld *nph_engine_physics(NphEngine *engine);

// The body of an entity's RigidBody or Collider while playing, or a null
// handle (none, not playing, or a body that could not be made).
NphPhysicsBody nph_engine_physics_body(const NphEngine *engine, NphEntity entity);

// The entity whose RigidBody or Collider `body` is, while playing: what a
// query's hit or a contact event names (a shape's body:
// nph_physics_shape_body). The null entity for a body of the game's own, a
// stale or null handle, or when not playing. As nph_engine_physics_body,
// from the simulation's systems or at the sync point.
NphEntity nph_engine_physics_entity(const NphEngine *engine, NphPhysicsBody body);

// The physics world's statistics after the last step (zeros when not
// playing).
const NphPhysicsStats *nph_engine_physics_stats(const NphEngine *engine);

// Draws `entity` and its descendants where they are from the next frame on,
// rather than between where they were and where they are (sim.interpolate):
// for a move that is no motion, a teleport or a respawn; the view is a
// camera cut when its camera is among them. NPH_THREADSAFE: the
// simulation's systems call it too. The engine resets the bodies it
// teleports itself (a new LocalTransform for a dynamic or static body).
void nph_engine_reset_interpolation(NphEngine *engine, NphEntity entity);

// ---------------------------------------------------------------------------
// Audio (docs/runtime.md "Audio"; runtime/audio.h)
// ---------------------------------------------------------------------------

// Plays `sound` once while the world plays, as nph_sound_play_desc_default
// and `desc` say: queued now, started at the next frame's sync point once
// the sound has loaded (a sound that takes a quarter second more is
// dropped: late is worse than not at all). NPH_THREADSAFE: the simulation's
// systems call it. NPH_RESULT_FULL when the queue is (256 a frame);
// NPH_RESULT_OK without audio (nothing to hear).
NPH_MUST_USE NphResult nph_engine_play_sound(NphEngine *engine, NphGuid sound,
                                             const NphSoundPlayDesc *desc);

// Loads `sound` now, waiting for it, so that its one-shots start at once (a
// footstep, a shot): the world's AudioSources' sounds are loaded when play
// begins, but a sound only one-shots name loads on first use. It stays
// loaded until play ends. NPH_MAIN_THREAD_ONLY (the game's play_begin is
// the place). NPH_RESULT_NOT_FOUND when it cannot be loaded or played
// (logged); NPH_RESULT_OK without audio.
NPH_MUST_USE NphResult nph_engine_preload_sound(NphEngine *engine, NphGuid sound);

// The engine's mixer (audio/mixer.h), or NULL with audio.device off: for
// what the components do not say (effects on the engine's buses, buses
// and snapshots of a game's own). NPH_MAIN_THREAD_ONLY, as the mixer is:
// from the game's callbacks or the host's hooks, at the sync point.
NphAudioMixer *nph_engine_audio_mixer(NphEngine *engine);

// The mixer's bus that AudioSources with `bus` play on (the master
// without audio).
NphAudioBus nph_engine_audio_bus(const NphEngine *engine, NphAudioSourceBus bus);

// audio.device offline: the mix the last frame rendered, interleaved in the
// mix's channels, and its frame count (`out_frames`); NULL and 0 otherwise.
const f32 *nph_engine_audio_output(const NphEngine *engine, u32 *out_frames);

void nph_engine_audio_stats(const NphEngine *engine, NphEngineAudioStats *out);

#ifdef __cplusplus
}
#endif
