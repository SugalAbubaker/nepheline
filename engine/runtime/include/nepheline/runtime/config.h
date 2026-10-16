// SPDX-License-Identifier: Apache-2.0
#pragma once

// Engine configuration from the command line (CLAUDE.md §8.12
// "Configuration comes from command line, config files, and cvars";
// docs/runtime.md "Configuration").
//
// Settings are cvars (core/cvar.h): the engine registers its own (window,
// rendering, simulation, jobs, profiling, game), modules and games add
// theirs, and values come, in increasing precedence, from the defaults in
// code, the config files, and the command line. What is not a single
// value (the game module, the scene, content to mount, scripts) is an
// option of this parser.
//
//     nph_player --game=build/bin/nph_game_sandbox.dll --scene=level.scene
//                --pack=build/game.npk sim.rate=120 r.vsync=false --frames=600
//
// Arguments of the form name=value without leading dashes are cvar
// assignments, applied after the config files (unknown names wait until
// something registers them). The others:
//
//   --game=PATH         the game module to load (and hot-reload)
//   --scene=PATH        a scene file (text or binary) to load into the world
//   --pack=PATH         a pack to mount at the VFS root (repeatable)
//   --content=DIR       a directory of cooked files to mount (repeatable)
//   --config=PATH       a cvar config file to apply (repeatable, in order)
//   --user-config=PATH  where the SAVED cvars are read from and written to
//   --script=PATH       console commands run once the engine is up
//   --trace=PATH        a Chrome trace of the last frames, written at exit
//   --frames=N          stop after N frames (sugar for rt.max_frames=N)
//   --headless          no window and no GPU: simulation only
//   --help              print the usage
//
// Thread-safety: pure, NPH_THREADSAFE.

#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_ENGINE_CONFIG_MAX_LIST        8U
#define NPH_ENGINE_CONFIG_MAX_ASSIGNMENTS 64U

typedef struct NphEngineConfig {
    // Pointers into argv: valid while the arguments are.
    const char *game;
    const char *scene;
    const char *user_config;
    const char *script;
    const char *trace;
    const char *packs[NPH_ENGINE_CONFIG_MAX_LIST];
    u32 pack_count;
    const char *contents[NPH_ENGINE_CONFIG_MAX_LIST];
    u32 content_count;
    const char *configs[NPH_ENGINE_CONFIG_MAX_LIST];
    u32 config_count;
    const char *assignments[NPH_ENGINE_CONFIG_MAX_ASSIGNMENTS];
    u32 assignment_count;
    u64 max_frames; // 0: no limit
    b32 has_max_frames;
    b32 headless;
    b32 help;
} NphEngineConfig;

#define NPH_ENGINE_CONFIG_ERROR_MAX 160U

// Parses argv[1 .. argc) into *out (zeroed first). NPH_RESULT_INVALID_ARGUMENT
// for an unknown option, an option without its value, a malformed number,
// or a list or assignment past its capacity, with the reason in `error`
// (may be NULL).
NPH_MUST_USE NphResult nph_engine_config_parse(int argc, char **argv, NphEngineConfig *out,
                                               char *error, u64 error_cap);

// The usage text (static).
const char *nph_engine_config_usage(void);

#ifdef __cplusplus
}
#endif
