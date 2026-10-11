// SPDX-License-Identifier: Apache-2.0
#pragma once

// Console variables (CLAUDE.md §8.1 "CVars: console variables registered in
// code and editable at runtime from the editor and config files").
//
//     NPH_CVAR_INT(r_shadow_res, "r.shadow.resolution", 2048, 256, 8192,
//                  "Shadow map resolution", NPH_CVAR_FLAG_SAVED);
//     ...
//     nph_cvar_register(&engine->cvars, &r_shadow_res);     // module init
//     ...
//     const i64 res = nph_cvar_get_int(&r_shadow_res);       // hot path: a load
//
// A cvar is a static object defined where it is used and registered with
// an explicit registry (no global list; CLAUDE.md §2). The registry maps
// names to cvars, applies values from config text and command-line
// assignments (values for names registered later are kept pending), writes
// the SAVED set back out, and notifies change callbacks.
//
// Names are lowercase ASCII letters, digits, '.', and '_' (asserted on
// registration). Integer and float cvars carry a range and are clamped.
// String values are copied into registry-owned storage.
//
// Thread-safety: the registry and all setters are NPH_MAIN_THREAD_ONLY.
// Getters are plain aligned loads, safe from any thread with the caveat
// that another thread may observe a change one read later than the main
// thread did.

#include <nepheline/core/alloc.h>
#include <nepheline/core/array.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/result.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphCvarType {
    NPH_CVAR_TYPE_BOOL,
    NPH_CVAR_TYPE_INT,
    NPH_CVAR_TYPE_FLOAT,
    NPH_CVAR_TYPE_STRING,
} NphCvarType;

typedef enum NphCvarFlags {
    NPH_CVAR_FLAG_NONE = 0,
    NPH_CVAR_FLAG_SAVED = 1 << 0,     // written to the config file
    NPH_CVAR_FLAG_READ_ONLY = 1 << 1, // set only by code (nph_cvar_force_*)
    NPH_CVAR_FLAG_CHEAT = 1 << 2,     // settable only when the registry allows cheats
    NPH_CVAR_FLAG_LATCHED = 1 << 3,   // informational: takes effect after a restart
} NphCvarFlags;

typedef struct NphCvar NphCvar;
typedef void (*NphCvarChangeFn)(NphCvar *cvar, void *user);

struct NphCvar {
    const char *name;
    const char *description;
    NphCvarType type;
    u32 flags;
    // Current value; only the member matching `type` is meaningful.
    b32 value_bool;
    i64 value_int;
    f64 value_float;
    const char *value_string; // registry-owned copy, or the static default
    // Defaults and ranges.
    b32 default_bool;
    i64 default_int;
    f64 default_float;
    const char *default_string;
    i64 min_int;
    i64 max_int;
    f64 min_float;
    f64 max_float;
    // Change notification (optional).
    NphCvarChangeFn on_change;
    void *on_change_user;
    // Registry bookkeeping.
    u32 registered; // 1 once registered; a cvar belongs to one registry
    u32 change_count;
};

// Static definitions.
#define NPH_CVAR_BOOL(ident, name, default_value, description, flags)                              \
    NphCvar ident = {(name),                                                                       \
                     (description),                                                                \
                     NPH_CVAR_TYPE_BOOL,                                                           \
                     (flags),                                                                      \
                     (default_value),                                                              \
                     0,                                                                            \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     (default_value),                                                              \
                     0,                                                                            \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     0,                                                                            \
                     0,                                                                            \
                     0.0,                                                                          \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     NULL,                                                                         \
                     0U,                                                                           \
                     0U}

#define NPH_CVAR_INT(ident, name, default_value, min, max, description, flags)                     \
    NphCvar ident = {(name),                                                                       \
                     (description),                                                                \
                     NPH_CVAR_TYPE_INT,                                                            \
                     (flags),                                                                      \
                     0,                                                                            \
                     (default_value),                                                              \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     0,                                                                            \
                     (default_value),                                                              \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     (min),                                                                        \
                     (max),                                                                        \
                     0.0,                                                                          \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     NULL,                                                                         \
                     0U,                                                                           \
                     0U}

#define NPH_CVAR_FLOAT(ident, name, default_value, min, max, description, flags)                   \
    NphCvar ident = {(name),                                                                       \
                     (description),                                                                \
                     NPH_CVAR_TYPE_FLOAT,                                                          \
                     (flags),                                                                      \
                     0,                                                                            \
                     0,                                                                            \
                     (default_value),                                                              \
                     NULL,                                                                         \
                     0,                                                                            \
                     0,                                                                            \
                     (default_value),                                                              \
                     NULL,                                                                         \
                     0,                                                                            \
                     0,                                                                            \
                     (min),                                                                        \
                     (max),                                                                        \
                     NULL,                                                                         \
                     NULL,                                                                         \
                     0U,                                                                           \
                     0U}

#define NPH_CVAR_STRING(ident, name, default_value, description, flags)                            \
    NphCvar ident = {(name),                                                                       \
                     (description),                                                                \
                     NPH_CVAR_TYPE_STRING,                                                         \
                     (flags),                                                                      \
                     0,                                                                            \
                     0,                                                                            \
                     0.0,                                                                          \
                     (default_value),                                                              \
                     0,                                                                            \
                     0,                                                                            \
                     0.0,                                                                          \
                     (default_value),                                                              \
                     0,                                                                            \
                     0,                                                                            \
                     0.0,                                                                          \
                     0.0,                                                                          \
                     NULL,                                                                         \
                     NULL,                                                                         \
                     0U,                                                                           \
                     0U}

// ---------------------------------------------------------------------------
// Getters: plain loads.
// ---------------------------------------------------------------------------

NPH_INLINE b32 nph_cvar_get_bool(const NphCvar *cvar)
{
    return cvar->value_bool;
}
NPH_INLINE i64 nph_cvar_get_int(const NphCvar *cvar)
{
    return cvar->value_int;
}
NPH_INLINE f64 nph_cvar_get_float(const NphCvar *cvar)
{
    return cvar->value_float;
}
NPH_INLINE NphStr nph_cvar_get_string(const NphCvar *cvar)
{
    return nph_str_from_cstr(cvar->value_string);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

typedef struct NphCvarRegistry {
    NphHashMap by_name; // NphStrId -> NphCvar *
    NphArrayPtr cvars;  // registration order
    NphHashMap pending; // NphStrId -> char * (owned text) for names not yet registered
    NphAllocator *alloc;
    bool cheats_enabled;
    u64 version; // increments on every change; the editor polls it
} NphCvarRegistry;

NPH_MUST_USE NphResult nph_cvar_registry_init(NphCvarRegistry *r, NphAllocator *alloc);

// Frees registry-owned strings. Registered cvars revert to their defaults
// so static cvars never point at freed memory.
void nph_cvar_registry_free(NphCvarRegistry *r);

// Adds `cvar`. NPH_RESULT_ALREADY_EXISTS for a taken name,
// NPH_RESULT_INVALID_ARGUMENT for a malformed name or a default outside
// the range. A pending value from an earlier config applies on
// registration (its failure is logged through the result of the set, but
// registration still succeeds).
NPH_MUST_USE NphResult nph_cvar_register(NphCvarRegistry *r, NphCvar *cvar);

NphCvar *nph_cvar_find(const NphCvarRegistry *r, NphStr name);

NPH_INLINE u32 nph_cvar_registry_count(const NphCvarRegistry *r)
{
    return r->cvars.len;
}
NPH_INLINE NphCvar *nph_cvar_registry_at(const NphCvarRegistry *r, u32 index)
{
    return (NphCvar *)r->cvars.data[index];
}

// Setters: NPH_RESULT_TYPE_MISMATCH for the wrong type,
// NPH_RESULT_INVALID_ARGUMENT for a read-only cvar or a cheat cvar while
// cheats are disabled. Out-of-range numbers are clamped. The change
// callback runs when the stored value actually changed.
NPH_MUST_USE NphResult nph_cvar_set_bool(NphCvarRegistry *r, NphCvar *cvar, b32 value);
NPH_MUST_USE NphResult nph_cvar_set_int(NphCvarRegistry *r, NphCvar *cvar, i64 value);
NPH_MUST_USE NphResult nph_cvar_set_float(NphCvarRegistry *r, NphCvar *cvar, f64 value);
NPH_MUST_USE NphResult nph_cvar_set_string(NphCvarRegistry *r, NphCvar *cvar, NphStr value);

// Parses `text` according to the cvar's type (bool: true/false/yes/no/on/
// off/1/0; numbers: decimal) and sets it. NPH_RESULT_PARSE_ERROR when the
// text does not parse.
NPH_MUST_USE NphResult nph_cvar_set_from_string(NphCvarRegistry *r, NphCvar *cvar, NphStr text);

// Like the setters but ignores READ_ONLY and CHEAT: for engine code that
// owns the cvar.
NPH_MUST_USE NphResult nph_cvar_force_from_string(NphCvarRegistry *r, NphCvar *cvar, NphStr text);

NPH_MUST_USE NphResult nph_cvar_reset(NphCvarRegistry *r, NphCvar *cvar);

// Writes the value as text ("true", "42", "1.5", or the string) into `out`
// (NUL-terminated, truncated to `cap`); returns the full length.
u64 nph_cvar_format(const NphCvar *cvar, char *out, u64 cap);

// Applies "name = value" lines ('#' starts a comment; blank lines are
// ignored; values may be quoted with double quotes). Unknown names are kept
// pending. Returns NPH_RESULT_PARSE_ERROR for a malformed line (reported
// in *out_error_line, 1-based, when non-NULL) after applying the good ones.
NPH_MUST_USE NphResult nph_cvar_apply_config(NphCvarRegistry *r, NphStr text, u32 *out_error_line);

// Applies one "name=value" assignment (command line). Unknown names are
// kept pending. NPH_RESULT_PARSE_ERROR without '='.
NPH_MUST_USE NphResult nph_cvar_apply_assignment(NphCvarRegistry *r, NphStr assignment);

// Appends every SAVED cvar as "name = value" lines, sorted by name, to `out`.
NPH_MUST_USE NphResult nph_cvar_write_config(const NphCvarRegistry *r, NphStrBuilder *out);

NPH_INLINE void nph_cvar_registry_set_cheats(NphCvarRegistry *r, bool enabled)
{
    r->cheats_enabled = enabled;
}

#ifdef __cplusplus
}
#endif
