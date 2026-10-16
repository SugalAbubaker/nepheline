# 0024. Game modules: C shared libraries against a shared engine, reloaded by migrating the world

- **Date:** 2026-10-16
- **Status:** Accepted

## Context

CLAUDE.md §8.12: "Game code is built as a shared library loaded by the
runtime and hot-reloaded in development. State lives in engine-owned
memory, so it survives reloads." §13 left open whether gameplay is C only
or also a scripting language; the user chose **C hot-reload only** at the
start of Phase 9 (2026-10-16), to revisit by a later ADR if a scripting
need appears.

A reload changes code. During development it also changes data layouts:
a component gains a field, a state struct grows, a component goes away.
The editor holds entity handles in its selection and its undo history, and
game state holds entity handles too.

## Decision

1. **The ABI is one exported function.** `nph_game_entry` returns an
   `NphGameApi`: an ABI version, the game's component descriptors, the size,
   alignment, and version of a state block, and optional callbacks
   (`load`, `unload`, `add_systems`, `frame`, `play_begin`, `play_end`).
   Game code reaches the engine through ordinary engine functions and a few
   `nph_game_*` services (state, component ids, step input, actions, cvar
   and command registration).
2. **The engine is a shared library for hosts and games.**
   `nepheline_shared` is built from the engine modules' object files and
   exports the engine's API. On Linux that is default visibility (vendored
   libraries' symbols excluded). With MSVC the export list is a
   module-definition file generated from the static libraries' archive
   indexes: `nph_*`, the `Nph*_component` descriptor functions, and the
   `NPH_LOG_*` categories as DATA (cmake/NphExportDef.cmake).
   `WINDOWS_EXPORT_ALL_SYMBOLS` cannot read the Shipping configuration's
   link-time-code-generation objects, and would also export the C
   runtime's inline functions the objects carry. clang-cl keeps
   `WINDOWS_EXPORT_ALL_SYMBOLS`. Engine data in public headers is marked
   `NPH_ENGINE_DATA` (dllimport where `NPH_ENGINE_SHARED_IMPORT` is
   defined). Hosts that load games (`nph_player`, the editor, the game
   tests) and the games link it, so there is one log, one profiler, one
   window system. Everything else keeps linking the static modules.
3. **Loaded copies.** The engine loads a temporary copy of the module, so
   the build can overwrite the original; a change is swapped in once the
   file has settled.
4. **The engine owns everything that must outlive an image.** Component
   descriptors are copied (the world points at the copies), cvars and
   console commands are registered through the engine and removed before
   unloading (a tuned cvar value carries over to the next build's cvar of
   the same name), state lives in the engine's block, input actions are
   names.
5. **Layout changes migrate the world through the text scene format.**
   When a component's layout hash changes or a component disappears, the
   world is saved as text and loaded into a new world registered with the
   new descriptors; `nph_world_adopt_handles` gives the new world the old
   one's handle pool, so every entity keeps its handle, and the loader
   places each entity at its handle. Fields are matched by name (new
   fields take their defaults; removed fields and components are dropped
   with a warning). The migration is built before anything is torn down:
   if it fails, the running build stays.
6. **Safe unloading.** Before an old image is unloaded, queued log records
   are delivered (`nph_log_drain`: records point at their category's name
   and call site in the image) and the profiler forgets zone names. If any
   component descriptor, cvar, or console command the engine still holds
   points into the image (`nph_dylib_contains`), the image stays loaded,
   with a warning.
7. **C only (§13).** No embedded scripting language: hot-reloaded C
   covers fast iteration without a second language, a binding layer, or a
   second debugger story.

## Alternatives considered

- **An API table passed to the game** (the game calls the engine only
  through function pointers). It is a clean ABI, but every engine function
  a game uses needs an entry and a wrapper, and game code reads unlike
  engine code. It may suit binary-stable SDKs later; it is not needed while
  games build against the engine's source.
- **Games statically linking the engine.** Each game image would carry its
  own copy of the engine's globals (log sinks, profiler state, window
  system), so its logging and profiling would silently go nowhere.
- **The host exporting its symbols** (`-rdynamic` on Linux, an import
  library of the executable on Windows). It works, but differently on
  each platform, and the editor and the player would each have to export
  the whole engine.
- **Refusing reloads that change layouts** (as Unreal Live Coding refuses
  class layout changes). Simpler, but forces a restart for the most common
  gameplay change, adding a field.
- **In-place migration in the world** (rebuilding each archetype's chunks
  for a new component size). Faster for huge worlds, but a third
  serializer of component data, by field name, next to the two formats
  the fuzzers already cover. The text round trip reuses them, and keeping
  handles makes it invisible to everything outside the world.
- **Keeping every old image loaded.** It never unloads code something
  might still call, but memory grows with every reload; with the engine's
  references checked and the logs drained, unloading is safe, and the
  check keeps an image whenever it is not.
- **A scripting language (Lua, a custom language)** (§13). It means a
  second language with its own debugger, profiler, and binding layer, and
  a performance cliff at the boundary. C hot reload gives the iteration
  speed without them.

## Consequences

- Development builds of hosts and games depend on the shared library
  next to them (`bin/<Config>`), and on Linux on its RPATH.
- Game code follows rules the engine cannot fully check (game.h): no
  state in globals that must survive a reload, no threads, every job
  finished when its system returns.
- A migration costs a text save and load of the world: milliseconds for
  editor-sized worlds. A component the new build renames loses its data
  (the name is the key); a renamed field likewise.
- Component ids may change on reload; games look them up in `load`.
- Shipping builds load the game the same way for now; linking it into the
  player statically for whole-program optimisation is Phase 15 work.
