// SPDX-License-Identifier: Apache-2.0
#pragma once

// Generational handles and the pool that issues them (CLAUDE.md §2 "Handles
// over pointers", §8.1 "Generational handle pool").
//
// A handle is a 32-bit value packing a slot index and a generation. The
// pool keeps the current generation of every slot; releasing a slot bumps
// it, so a handle kept past release no longer matches and is detected as
// stale by nph_handle_pool_is_valid. Zero is never a valid handle
// (generations start at 1), so a zero-initialised struct holds "no handle".
//
// The split between index and generation bits is chosen per pool: 20 index
// bits give a million slots with 12 generation bits (4096 releases before a
// slot's generation wraps and an ancient handle could match again). Freed
// slots wait in a FIFO and are reused only after `min_free_before_reuse`
// other releases, which pushes that wrap far out in practice.
//
// The pool issues indices only; callers keep their own arrays indexed by
// nph_handle_pool_index (structure-of-arrays, §2).
//
// Thread-safety: none.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphHandle {
    u32 value;
} NphHandle;

#define NPH_HANDLE_NULL ((NphHandle){0U})

NPH_INLINE bool nph_handle_is_null(NphHandle h)
{
    return h.value == 0U;
}

NPH_INLINE bool nph_handle_equals(NphHandle a, NphHandle b)
{
    return a.value == b.value;
}

// At least 4 generation bits are kept so stale detection always has some
// teeth; 28 index bits is 268 million slots.
#define NPH_HANDLE_POOL_MIN_INDEX_BITS 1U
#define NPH_HANDLE_POOL_MAX_INDEX_BITS 28U

typedef struct NphHandlePool {
    u32 *generations; // current generation per slot; 0 marks a never-used slot
    u32 *free_queue;  // ring of released slot indices
    u32 capacity;
    u32 index_bits;
    u32 index_mask;
    u32 generation_max; // largest generation value before wrapping to 1
    u32 bump;           // first never-used slot
    u32 free_head;      // oldest entry in free_queue
    u32 free_count;
    u32 live;
    u32 min_free_before_reuse;
    NphAllocator *alloc;
} NphHandlePool;

// Allocates a pool for `capacity` slots (<= 2^index_bits) with the given
// index width. `min_free_before_reuse` delays reuse of released slots
// (0 reuses immediately). NPH_RESULT_INVALID_ARGUMENT for a bad width or
// capacity, NPH_RESULT_OUT_OF_MEMORY on failure.
NPH_MUST_USE NphResult nph_handle_pool_init(NphHandlePool *pool, NphAllocator *alloc, u32 capacity,
                                            u32 index_bits, u32 min_free_before_reuse);

void nph_handle_pool_free(NphHandlePool *pool);

// Issues a handle. NPH_RESULT_FULL when every slot is live.
NPH_MUST_USE NphResult nph_handle_pool_alloc(NphHandlePool *pool, NphHandle *out);

// Releases `h`. Returns false (and does nothing) for a stale or null handle.
bool nph_handle_pool_release(NphHandlePool *pool, NphHandle h);

// True if `h` was issued by this pool and has not been released since.
bool nph_handle_pool_is_valid(const NphHandlePool *pool, NphHandle h);

// Makes `dst` a copy of `src`: the same live, released, and never-used
// slots with the same generations and the same reuse order, so a handle
// valid (or stale) in `src` is valid (or stale) in `dst`, and both issue
// the same handles from here on. A world copied for hot reload or
// play-in-editor keeps the original's entity handles this way.
// NPH_RESULT_INVALID_ARGUMENT when the capacities or index widths differ.
NPH_MUST_USE NphResult nph_handle_pool_copy(NphHandlePool *dst, const NphHandlePool *src);

// The slot index packed in `h`. Does not check validity.
NPH_INLINE u32 nph_handle_pool_index(const NphHandlePool *pool, NphHandle h)
{
    return h.value & pool->index_mask;
}

// The generation packed in `h`. Does not check validity.
NPH_INLINE u32 nph_handle_pool_generation(const NphHandlePool *pool, NphHandle h)
{
    return h.value >> pool->index_bits;
}

NPH_INLINE u32 nph_handle_pool_live(const NphHandlePool *pool)
{
    return pool->live;
}

NPH_INLINE u32 nph_handle_pool_capacity(const NphHandlePool *pool)
{
    return pool->capacity;
}

#ifdef __cplusplus
}
#endif
