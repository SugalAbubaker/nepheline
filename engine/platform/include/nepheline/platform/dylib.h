// SPDX-License-Identifier: Apache-2.0
#pragma once

// Dynamic library loading (CLAUDE.md §8.3 "dynamic library loading (for
// hot-reloading game code)"). LoadLibrary on Windows, dlopen on Linux.
//
// The runtime (Phase 9) builds game code as a shared library, copies it to
// a temporary name, loads the copy, and swaps it for a newer build while
// the engine keeps running. This header is just the loading; the copy, the
// watch, and the state hand-over live there.
//
// Thread-safety: NPH_THREADSAFE for distinct libraries. Opening and closing
// the same library concurrently is a caller bug.

#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphDylib {
    void *handle; // HMODULE or dlopen handle; NULL when not open
} NphDylib;

// A generic function pointer; cast it to the symbol's real type.
typedef void (*NphDylibFn)(void);

// Loads the library at `path` (UTF-8, native or forward slashes; the
// platform's extension must be included). Its initialisers run before
// this returns. NPH_RESULT_NOT_FOUND when the file does not exist,
// NPH_RESULT_IO_ERROR when it exists but cannot be loaded (wrong
// architecture, missing dependency); the OS's message is logged.
NPH_MUST_USE NphResult nph_dylib_open(const char *path, NphDylib *out);

// Unloads the library. Every pointer obtained from it becomes invalid. Safe
// on a zero-filled or already closed handle.
void nph_dylib_close(NphDylib *lib);

NPH_INLINE bool nph_dylib_is_open(const NphDylib *lib)
{
    return lib->handle != NULL;
}

// Looks up an exported function. NPH_RESULT_NOT_FOUND when absent.
NPH_MUST_USE NphResult nph_dylib_function(const NphDylib *lib, const char *name, NphDylibFn *out);

// Looks up an exported data object. NPH_RESULT_NOT_FOUND when absent.
NPH_MUST_USE NphResult nph_dylib_data(const NphDylib *lib, const char *name, void **out);

// True when `address` lies in the image of the open library `lib`: its
// code or its static data. The runtime asks before unloading a game
// module whether anything the engine keeps (a component descriptor, a
// system function) still points into it.
bool nph_dylib_contains(const NphDylib *lib, const void *address);

// The platform's shared-library file extension including the dot: ".dll"
// or ".so".
const char *nph_dylib_extension(void);

#ifdef __cplusplus
}
#endif
