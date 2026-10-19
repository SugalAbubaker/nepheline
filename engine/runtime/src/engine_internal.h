// SPDX-License-Identifier: Apache-2.0
#pragma once

// The engine and the game host, shared by engine.c and game_host.c.

#include <nepheline/core/array.h>
#include <nepheline/platform/dylib.h>
#include <nepheline/platform/input.h>
#include <nepheline/profile/memory.h>
#include <nepheline/runtime/console.h>
#include <nepheline/runtime/engine.h>
#include <nepheline/runtime/sim_input.h>

#include "runtime_internal.h"

#define GAME_PATH_MAX     512U
#define GAME_MAX_COMMANDS 64U
#define GAME_MAX_RETIRED  16U
#define ENGINE_MAX_STEPS  64U // sim.max_steps's ceiling: the step inputs of one frame

// ---------------------------------------------------------------------------
// The game host (game_host.c)
// ---------------------------------------------------------------------------

// The engine's own copy of a game component's descriptor: the world points
// at it, so the module's image can go while the world lives on.
typedef struct GameComponentCopy {
    NphComponentDesc desc;
    u64 layout_hash;
    NphFieldDesc *fields; // desc.fields
    char *text;           // desc.name, then every field's name
    u64 text_bytes;
    u8 *defaults; // desc.defaults (desc.size bytes), or NULL
} GameComponentCopy;

typedef struct GameComponentSet {
    GameComponentCopy *items;
    u32 count;
} GameComponentSet;

// A build that was replaced, kept loaded until it is safe to unload.
typedef struct GameImage {
    NphDylib lib;
    char path[GAME_PATH_MAX]; // its temporary copy, deleted once unloaded
} GameImage;

struct NphGame {
    NphEngine *engine;
    const NphGameApi *api; // NULL while no module is loaded
    NphDylib lib;
    char source[GAME_PATH_MAX]; // the file the build writes
    char loaded[GAME_PATH_MAX]; // the temporary copy that is loaded
    u64 source_modified_ns;
    u64 source_size;
    u64 source_hash; // of the content loaded
    u64 next_poll_ns;
    u64 changed_at_ns; // a change was seen and waits for the file to settle (0: none)
    u32 reloads;
    u32 copies; // temporary copies made (their names)
    // The state block.
    void *state;
    u64 state_size;
    u64 state_align;
    u32 state_version;
    // The components: engine-owned copies and their ids in the world.
    GameComponentSet components;
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    // What the module registered with the engine.
    NphArrayPtr cvars;
    char commands[GAME_MAX_COMMANDS][64];
    u32 command_count;
    // Images that wait to be unloaded.
    GameImage retired[GAME_MAX_RETIRED];
    u32 retired_count;
    // The simulation's view of the game.
    const NphSimInput *input;
    NphSimInput idle_input; // between plays
    u64 step;
    b32 playing;
};

// Loads the module at `path` as the engine's game, replacing the one
// loaded (as a reload when it is the same file). Called with the world
// quiescent.
NPH_MUST_USE NphResult game_load(NphEngine *e, const char *path);
// Unloads the module (its components stay registered: the world still
// holds their data).
void game_unload(NphEngine *e);
// Swaps in a newer build if the file changed and settled (`force`: check
// now rather than at the poll interval).
NPH_MUST_USE NphResult game_poll_reload(NphEngine *e, bool force, bool *out_reloaded);
NPH_MUST_USE NphResult game_add_systems(NphEngine *e, NphSchedule *schedule);
void game_frame(NphEngine *e, f64 dt);
NPH_MUST_USE NphResult game_play_begin(NphEngine *e);
void game_play_end(NphEngine *e);
// Frees what the host holds, unloading every image (engine destruction,
// after the worlds are gone).
void game_free(NphEngine *e);

// ---------------------------------------------------------------------------
// The engine (engine.c)
// ---------------------------------------------------------------------------

typedef struct EngineCvars {
    NphCvar max_frames;            // rt.max_frames
    NphCvar pipelined;             // rt.pipelined
    NphCvar max_fps;               // rt.max_fps
    NphCvar lockstep;              // rt.lockstep: one step per frame, whatever the time
    NphCvar max_entities;          // rt.max_entities
    NphCvar world_events;          // rt.world_events
    NphCvar workers;               // rt.workers
    NphCvar sim_rate;              // sim.rate
    NphCvar sim_max_steps;         // sim.max_steps
    NphCvar sim_time_scale;        // sim.time_scale
    NphCvar vsync;                 // r.vsync
    NphCvar width;                 // r.width
    NphCvar height;                // r.height
    NphCvar hot_reload;            // game.hot_reload
    NphCvar reload_poll_ms;        // game.reload_poll_ms
    NphCvar prof_enabled;          // prof.enabled
    NphCvar prof_history;          // prof.history
    NphCvar pipeline;              // r.pipeline: NphRenderPipeline
    NphCvar occlusion;             // r.occlusion
    NphCvar cone_culling;          // r.cone_culling
    NphCvar sw_raster;             // r.sw_raster
    NphCvar virtual_shadows;       // r.virtual_shadows
    NphCvar taa;                   // r.taa
    NphCvar bloom;                 // r.bloom
    NphCvar auto_exposure;         // r.auto_exposure
    NphCvar exposure_compensation; // r.exposure_compensation
    NphCvar exposure_speed;        // r.exposure_speed
    NphCvar motion_blur;           // r.motion_blur
    NphCvar dof;                   // r.dof
} EngineCvars;

typedef enum EngineAllocTag {
    ENGINE_ALLOC_ENGINE = 0,
    ENGINE_ALLOC_WORLD,
    ENGINE_ALLOC_RENDER,
    ENGINE_ALLOC_ASSETS,
    ENGINE_ALLOC_GAME,
    ENGINE_ALLOC_COUNT,
} EngineAllocTag;

struct NphEngine {
    NphAllocator *backing;
    NphTrackedAllocator tracked[ENGINE_ALLOC_COUNT];
    NphEngineMode mode;
    NphEngineHooks hooks;
    NphEngineConfig config;
    char app_name[64];
    char shader_dir[GAME_PATH_MAX];
    u32 offscreen_width;
    u32 offscreen_height;

    // Settings and the console.
    NphCvarRegistry cvars;
    bool cvars_ready;
    EngineCvars cv;
    NphConsole *console;

    // Profiling.
    bool owns_profiling;   // core profiling was initialised by the engine
    bool profile_attached; // the engine attached this thread
    NphProfiler *profiler;

    // Threads and content.
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;

    // The window, input, and the GPU.
    bool owns_window_system;
    NphWindow *window;
    NphInput *input;
    NphGpuDevice *dev;
    NphGpuSwapchain *swapchain;
    NphGpuImage offscreen;
    NphGpuRenderGraph *graph;
    NphRenderer *renderer;
    NphRenderGpuProfiler *gpu_profiler;
    NphSceneRenderer *scene_renderer;

    // Worlds: the edited one, and the play copy while playing a copy.
    NphWorld *edit_world;
    NphWorld *play_world;
    NphWorld *world; // the active one: play_world when set, else edit_world
    NphRuntimeComponents ids;
    NphTransformSystem *transforms; // of the active world
    NphSchedule *schedule;          // of the active world

    // The simulation.
    NphFixedClock clock;
    NphSimInputAccumulator sim_acc;
    NphSimInput step_inputs[ENGINE_MAX_STEPS];
    u32 sim_steps; // the steps of the job in flight (or the next inline run)
    NphJobCounter sim_counter;
    bool sim_in_flight;
    NphResult sim_result;
    bool playing;
    bool paused;
    bool step_once;

    // The game.
    NphGame game;

    // The frame.
    NphRenderView view;
    NphRenderView view_override;
    bool has_view_override;
    bool view_was_override; // the last frame's view was the override (a switch is a cut)
    bool quit;
    u64 last_frame_ns;
    NphEngineFrameStats stats;
};

NPH_INLINE NphAllocator *engine_alloc(NphEngine *e, EngineAllocTag tag)
{
    return nph_tracked_allocator(&e->tracked[tag]);
}

// Waits for the simulation job in flight, if any, and returns its result
// (the world is quiescent afterwards).
NPH_MUST_USE NphResult engine_sim_wait(NphEngine *e);

// Creates an empty world registered with the runtime's components and the
// components of `set` (may be NULL), whose descriptors must outlive it.
NPH_MUST_USE NphResult engine_world_create(NphEngine *e, const GameComponentSet *set,
                                           NphWorld **out);

// Makes `world` the edited world (`edit`) or the play copy, rebinding the
// transform system, the schedule, and the scene renderer, and telling the
// host. The previous world of that role is returned for the caller to
// destroy (after this call nothing refers to it).
NPH_MUST_USE NphResult engine_world_activate(NphEngine *e, NphWorld *world, bool edit,
                                             NphWorld **out_previous);

// Rebuilds the active world's schedule (the transform system and the
// game's systems).
NPH_MUST_USE NphResult engine_schedule_rebuild(NphEngine *e);

// Copies `src` into a new world with the same entity handles. With `set`,
// a migration: the new world registers the runtime's components and
// `set`, and the copy goes through the text format (fields matched by
// name, components the set dropped skipped). Without, a play copy: the
// same registry, through the binary format.
NPH_MUST_USE NphResult engine_world_copy(NphEngine *e, const NphWorld *src,
                                         const GameComponentSet *set, NphWorld **out);
