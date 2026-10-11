// SPDX-License-Identifier: Apache-2.0
#pragma once

// Interned string IDs (CLAUDE.md §8.1 "interned string IDs (64-bit hash)").
//
// An NphStrId is the 64-bit nph_hash64 of a string. Two equal strings
// always produce the same ID without any table, so IDs can be computed at
// runtime (component names, asset paths) and compared as integers; they
// are also what cooked assets store. A table (NphStrTable) optionally
// remembers the text behind an ID for diagnostics, the editor, and
// serialisation to text.
//
// Zero is reserved as "no ID": the hash value 0 is remapped to 1, which
// merges two inputs with probability 2^-64. Hash collisions between
// different strings are the known risk of 64-bit IDs (~10^-19 per pair);
// the table detects one when both strings are interned and reports
// NPH_RESULT_CORRUPT, which is treated as a build-breaking content error.
//
// Thread-safety: nph_str_id_* are pure. The table is single-threaded;
// interning happens on load paths, and Phase 3 adds a locked wrapper where
// concurrent loading needs it.

#include <nepheline/core/array.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphStrId {
    u64 value;
} NphStrId;

#define NPH_STR_ID_NULL ((NphStrId){0U})

// The ID of `s`, without interning.
NphStrId nph_str_id(NphStr s);
NphStrId nph_str_id_cstr(const char *s);

NPH_INLINE bool nph_str_id_is_null(NphStrId id)
{
    return id.value == 0U;
}
NPH_INLINE bool nph_str_id_equals(NphStrId a, NphStrId b)
{
    return a.value == b.value;
}

typedef struct NphStrTable {
    NphHashMap map;     // id -> NphStr (view into a chunk)
    NphArrayPtr chunks; // owned storage blocks
    u8 *chunk_cursor;
    u64 chunk_remaining;
    u32 chunk_size;
    NphAllocator *alloc;
} NphStrTable;

// `chunk_size` is the size of the blocks that hold interned text (0 for
// the 64 KiB default). Nothing is allocated until the first intern.
NPH_MUST_USE NphResult nph_str_table_init(NphStrTable *t, NphAllocator *alloc, u32 chunk_size);
void nph_str_table_free(NphStrTable *t);

// Records `s` and returns its ID. Interning the same text again is a cheap
// lookup. NPH_RESULT_CORRUPT if a different text already owns the ID
// (hash collision), NPH_RESULT_OUT_OF_MEMORY on allocation failure.
NPH_MUST_USE NphResult nph_str_table_intern(NphStrTable *t, NphStr s, NphStrId *out_id);

// The text behind `id`, or an empty view with data == NULL when unknown.
// The view stays valid for the table's lifetime.
NphStr nph_str_table_lookup(const NphStrTable *t, NphStrId id);

NPH_INLINE u32 nph_str_table_count(const NphStrTable *t)
{
    return nph_hashmap_len(&t->map);
}

#ifdef __cplusplus
}
#endif
