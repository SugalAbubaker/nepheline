// SPDX-License-Identifier: Apache-2.0
#pragma once

// Fixed-size pool allocator (CLAUDE.md §8.1 "Allocators").
//
// A pool hands out slots of one size from a contiguous block in O(1), with
// O(1) free. Freed slots form an intrusive singly-linked free list; slots
// that have never been used are taken from a bump index, so initialisation
// does not touch the block (no page faults for a pool that is mostly unused).
//
// With AddressSanitizer, free slots are poisoned except for their link
// word, so a use-after-free of a pooled object is reported.
//
// The direct API (nph_pool_alloc) serves any slot alignment down to the
// pointer size. The NphAllocator interface promises NPH_ALLOC_DEFAULT_ALIGN
// (alloc.h), so a pool that will be used through nph_pool_allocator must be
// initialised with elem_align >= NPH_ALLOC_DEFAULT_ALIGN; the vtable refuses
// (asserts and returns NULL) requests the slots cannot satisfy.
//
// Thread-safety: single-threaded.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphPool {
    NphAllocator allocator; // first, so &pool->allocator == (NphAllocator *)pool
    u8 *base;
    u64 capacity;  // bytes
    u32 slot_size; // bytes per slot, including padding to slot_align
    u32 slot_align;
    u32 slot_count; // slots that fit in the block
    u32 bump;       // slots handed out from the never-used region
    u32 live;       // slots currently allocated
    u32 peak;       // high-water mark of `live`
    void *free_head;
    NphAllocator *backing; // owner of `base` when created with init_from, else NULL
} NphPool;

// Initialises `pool` over caller-owned memory. `elem_size` is the payload
// size (> 0); `elem_align` a power of two. Slots are at least pointer-sized
// (the free list lives in them). Never fails; a block too small for one slot
// yields a pool with zero capacity.
void nph_pool_init(NphPool *pool, void *memory, u64 size, u32 elem_size, u32 elem_align);

// Allocates a block for `count` slots from `backing`. Returns
// NPH_RESULT_INVALID_ARGUMENT for count == 0 or a bad size/align, or
// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_pool_init_from(NphPool *pool, NphAllocator *backing, u32 elem_size,
                                          u32 elem_align, u32 count);

// Releases the backing block if the pool owns one. Safe on a zero-initialised
// pool.
void nph_pool_destroy(NphPool *pool);

NPH_INLINE NphAllocator *nph_pool_allocator(NphPool *pool)
{
    return &pool->allocator;
}

// Returns a slot, or NULL when every slot is in use. Uninitialised.
void *nph_pool_alloc(NphPool *pool);

// As nph_pool_alloc, zero-filled.
void *nph_pool_alloc_zeroed(NphPool *pool);

// Returns `ptr` to the pool. NULL is ignored. Asserts that `ptr` is a slot
// of this pool.
void nph_pool_free(NphPool *pool, void *ptr);

// True if `ptr` is the start of a slot in this pool (live or free).
bool nph_pool_owns(const NphPool *pool, const void *ptr);

NPH_INLINE u32 nph_pool_capacity(const NphPool *pool)
{
    return pool->slot_count;
}
NPH_INLINE u32 nph_pool_live(const NphPool *pool)
{
    return pool->live;
}
NPH_INLINE u32 nph_pool_peak(const NphPool *pool)
{
    return pool->peak;
}

#ifdef __cplusplus
}
#endif
