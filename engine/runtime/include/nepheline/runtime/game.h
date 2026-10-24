// SPDX-License-Identifier: Apache-2.0
#pragma once

// Game modules (CLAUDE.md §8.12 "Game code is built as a shared library
// loaded by the runtime and hot-reloaded in development. State lives in
// engine-owned memory, so it survives reloads"; §13 "Gameplay scripting:
// C hot-reload only"; docs/runtime.md "Game modules";
// docs/adr/0024-game-modules.md).
//
// A game is a shared library that links the engine as a shared library
// (CMake: nph_add_game_module) and exports one function:
//
//     NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void)
//     {
//         static const NphGameApi api = {
//             .abi_version = NPH_GAME_ABI_VERSION,
//             .name = "sandbox",
//             .state_size = sizeof(SandboxState),
//             .state_align = NPH_ALIGNOF(SandboxState),
//             .state_version = 1,
//             .components = g_components,
//             .component_count = 2,
//             .load = sandbox_load,
//             .add_systems = sandbox_add_systems,
//         };
//         return &api;
//     }
//
// The engine loads a temporary copy of the file, so the build can write
// the original while the game runs. When the original changes, the engine
// loads the new build next to the running one and swaps them at the
// frame's sync point, where nothing simulates:
//
//   1. the running build's unload(reloading = true);
//   2. when the components' layouts changed, the world is rebuilt with the
//      new layouts: saved as text and loaded into a world that keeps every
//      entity's handle, so fields are matched by name (scene.md "Copies
//      that keep their handles");
//   3. the new build's load(reloaded = true), then add_systems;
//   4. the old image is unloaded once nothing the engine holds points into
//      it (its log records and profiler names are drained first).
//
// If the new build cannot be loaded or the world cannot be rebuilt, the
// running build keeps running and the failure is logged.
//
// What survives a reload: the world (entities, handles, values), the state
// block (unless state_version, state_size, or state_align changed: it is
// then a new zeroed block and load says so), cvar values that were tuned,
// and input actions. What does not: the module's own globals (the new image
// starts from its initial values), component ids (look them up in load),
// and anything that points into the old image.
//
// Rules for game code:
//   - Keep state in the state block or the world, never in globals that
//     must outlive a reload.
//   - Register cvars and console commands through nph_game_register_cvar
//     and nph_game_register_command, so they are removed before the image
//     that holds them goes.
//   - Every job a system starts has finished when the system returns.
//   - Game code creates no threads and keeps no OS resources of its own;
//     the engine's services cover them.
//
// Thread-safety: the callbacks run on the main thread at the sync point,
// with the world quiescent. Systems run as the schedule runs them
// (scene/schedule.h), on the job system.

#include <nepheline/core/cvar.h>
#include <nepheline/physics/world.h>
#include <nepheline/runtime/console.h>
#include <nepheline/runtime/sim_input.h>
#include <nepheline/scene/schedule.h>

#ifdef __cplusplus
extern "C" {
#endif

// Raised whenever NphGameApi or a service below changes incompatibly; a
// module built against another value is refused.
#define NPH_GAME_ABI_VERSION 1U

// The exported entry point's name.
#define NPH_GAME_ENTRY_NAME "nph_game_entry"

// The engine's physics system in the schedule: it steps the physics world
// once per step, after the game's systems (they run before it unless they
// ask otherwise) and before the transform system. A system that reacts to
// this step's contacts or poses runs `after` it.
#define NPH_PHYSICS_SYSTEM_NAME "nph.physics"

// Marks the entry point (core/base.h NPH_EXPORT).
#define NPH_GAME_EXPORT NPH_EXPORT

typedef struct NphEngine NphEngine;

// The engine's handle on the loaded game: stable from the first load to
// the engine's destruction, across reloads.
typedef struct NphGame NphGame;

typedef struct NphGameLoadInfo {
    b32 reloaded;       // a newer build replaced the previous one
    b32 state_reset;    // the state block is new and zeroed
    b32 world_migrated; // the world was rebuilt for new component layouts
} NphGameLoadInfo;

typedef struct NphGameApi {
    u32 abi_version;   // NPH_GAME_ABI_VERSION
    u32 state_version; // the state block's layout; a change resets the block on reload
    u64 state_size;    // bytes of state the engine keeps across reloads (0: none)
    u64 state_align;   // a power of two (0: 16)
    const char *name;  // for logs and the editor

    // The game's components, registered with the world in this order. The
    // engine registers its own copies, so the descriptors may live in the
    // module's static data.
    const NphComponentDesc *const *components;
    u32 component_count;

    // Every callback is optional.
    // After the components are registered (ids: nph_game_component).
    NphResult (*load)(NphGame *game, const NphGameLoadInfo *info);
    // Before the module goes (reloading: a newer build follows).
    void (*unload)(NphGame *game, b32 reloading);
    // Adds the game's systems to the fixed-step simulation. The engine's
    // transform system is in the schedule already (scene/transform.h
    // NPH_TRANSFORM_SYSTEM_NAME), and its physics system follows the game's
    // (NPH_PHYSICS_SYSTEM_NAME); systems that need this step's world
    // transforms run after the transform system.
    NphResult (*add_systems)(NphGame *game, NphSchedule *schedule);
    // Once per frame at the sync point, at the frame rate (`dt`: the
    // frame's real duration), whether or not the world simulates: cameras
    // and anything that follows the display rather than the simulation.
    void (*frame)(NphGame *game, f64 dt);
    // The world starts and stops simulating (the player starts playing at
    // once; the editor's play button). play_end also runs before an unload
    // while playing. play_begin runs with a body for every entity that has
    // one (nph_game_physics); the bodies of what it makes, or places, are
    // made when it returns, before the first step.
    NphResult (*play_begin)(NphGame *game);
    void (*play_end)(NphGame *game);
} NphGameApi;

typedef const NphGameApi *(*NphGameEntryFn)(void);

// The entry point a game module defines (declared here so its definition
// has a prototype; the engine itself never defines it).
NPH_GAME_EXPORT const NphGameApi *nph_game_entry(void);

// ---------------------------------------------------------------------------
// Services for game code
// ---------------------------------------------------------------------------

NphEngine *nph_game_engine(const NphGame *game);

// The world the game simulates (the play copy during play-in-editor).
NphWorld *nph_game_world(const NphGame *game);

// The state block (state_size bytes, state_align aligned), or NULL when
// state_size is 0.
void *nph_game_state(const NphGame *game);

// The world id of api->components[index], or NPH_COMPONENT_NONE for an
// index out of range. Valid until the next reload.
NphComponentId nph_game_component(const NphGame *game, u32 index);

// The input of the step being simulated (during a step: from systems), or
// of the last one (between steps). Actions are numbered as the engine's
// input numbers them (nph_game_action).
const NphSimInput *nph_game_input(const NphGame *game);

// The fixed step, in seconds, and the steps simulated since play began.
f64 nph_game_step_seconds(const NphGame *game);
u64 nph_game_step(const NphGame *game);

// The physics world while playing, or NULL (engine.h nph_engine_physics):
// queries, characters, joints, events, bodies of the game's own. Systems
// that change bodies declare a write of RigidBody, so that two of them never
// run at once; systems that only query declare a read.
NphPhysicsWorld *nph_game_physics(const NphGame *game);

// The body of an entity's RigidBody or Collider while playing, or a null
// handle.
NphPhysicsBody nph_game_physics_body(const NphGame *game, NphEntity entity);

// Draws `entity` and its descendants where they are from the next frame,
// not between where they were and where they are: a teleport, a respawn
// (engine.h nph_engine_reset_interpolation). NPH_THREADSAFE.
void nph_game_reset_interpolation(NphGame *game, NphEntity entity);

// The input action named `name`, registered with the engine's input on
// first use (unbound until bindings are added through the engine's input).
// Actions outlive reloads. NPH_RESULT_FULL past NPH_SIM_INPUT_MAX_ACTIONS.
NPH_MUST_USE NphResult nph_game_action(NphGame *game, const char *name, NphActionId *out);

// Registers a cvar of the game with the engine's registry; it is removed
// before the module is unloaded, keeping a tuned value for the next build
// (core/cvar.h nph_cvar_unregister). The registry's errors.
NPH_MUST_USE NphResult nph_game_register_cvar(NphGame *game, NphCvar *cvar);

// Registers a console command of the game; it is removed before the module
// is unloaded. The console's errors.
NPH_MUST_USE NphResult nph_game_register_command(NphGame *game, const char *name, const char *help,
                                                 NphConsoleCommandFn fn, void *user);

#ifdef __cplusplus
}
#endif
