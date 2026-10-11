// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sparse set (CLAUDE.md §8.1): O(1) insert, remove, and membership for
// small integer keys, with the members packed densely for iteration.
//
// `sparse[key]` holds the position of `key` in `dense`, and `dense[pos]`
// holds the key, so membership is one lookup and one comparison. Removal
// swaps the last dense entry into the hole. An optional value array runs
// parallel to `dense`, which is the storage pattern the ECS uses for
// components keyed by entity index (Briggs & Torczon, "An Efficient
// Representation for Sparse Sets", 1993).
//
// The sparse array grows to the largest key inserted (rounded up to a power
// of two), so keys should be dense small integers such as entity indices.
// Dense positions change on removal; iterate by position only while the
// set is not modified.
//
// Thread-safety: none.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphSparseSet {
    u32 *sparse; // key -> dense position, or NPH_U32_MAX
    u32 *dense;  // position -> key
    u8 *values;  // position -> value (elem_size bytes), NULL when elem_size == 0
    u32 len;
    u32 dense_cap;
    u32 sparse_cap;
    u32 elem_size;
    NphAllocator *alloc;
} NphSparseSet;

// Initialises an empty set whose members carry `elem_size`-byte values
// (0 for a plain set). Nothing is allocated until the first insert.
void nph_sparse_set_init(NphSparseSet *s, NphAllocator *alloc, u32 elem_size);
void nph_sparse_set_free(NphSparseSet *s);

// Pre-sizes for keys up to `max_key` and `count` members.
NPH_MUST_USE NphResult nph_sparse_set_reserve(NphSparseSet *s, u32 max_key, u32 count);

// Adds `key` with a copy of `value` (ignored when elem_size == 0; NULL
// leaves the value uninitialised). NPH_RESULT_ALREADY_EXISTS if present.
NPH_MUST_USE NphResult nph_sparse_set_insert(NphSparseSet *s, u32 key, const void *value);

// Adds or overwrites.
NPH_MUST_USE NphResult nph_sparse_set_set(NphSparseSet *s, u32 key, const void *value);

bool nph_sparse_set_contains(const NphSparseSet *s, u32 key);

// Address of the value for `key`, or NULL. For a set without values, the
// address of the dense key entry (non-NULL).
void *nph_sparse_set_get(const NphSparseSet *s, u32 key);

// Dense position of `key`, or NPH_U32_MAX.
u32 nph_sparse_set_position(const NphSparseSet *s, u32 key);

// Removes `key`; true if it was present. The last member moves into its
// dense position.
bool nph_sparse_set_remove(NphSparseSet *s, u32 key);

void nph_sparse_set_clear(NphSparseSet *s);

NPH_INLINE u32 nph_sparse_set_len(const NphSparseSet *s)
{
    return s->len;
}

// The packed keys (len entries) and values (len * elem_size bytes).
NPH_INLINE const u32 *nph_sparse_set_keys(const NphSparseSet *s)
{
    return s->dense;
}
NPH_INLINE void *nph_sparse_set_values(const NphSparseSet *s)
{
    return s->values;
}
NPH_INLINE void *nph_sparse_set_value_at(const NphSparseSet *s, u32 position)
{
    return s->values + (u64)position * s->elem_size;
}

#ifdef __cplusplus
}
#endif
