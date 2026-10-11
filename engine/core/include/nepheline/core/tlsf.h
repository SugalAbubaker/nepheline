// SPDX-License-Identifier: Apache-2.0
#pragma once

// TLSF: the engine's general-purpose allocator (CLAUDE.md §8.1).
//
// Two-Level Segregated Fit (Masmano, Ripoll, Crespo & Real, "TLSF: a New
// Dynamic Memory Allocator for Real-Time Systems", ECRTS 2004). Free blocks
// are indexed by a first level (power-of-two size class) and a second level
// that splits each class linearly into 32 sub-classes; two bitmaps make
// finding a fitting block a couple of bit scans. Allocation and free are
// O(1) with no loops over block lists, and immediate coalescing bounds
// fragmentation at the level measured in the paper (under 15 % for the
// workloads studied).
//
// Layout: every block has a 16-byte header {prev_phys, size | flags} directly
// before its payload, so payloads are 16-byte aligned, the smallest block is
// 32 bytes, and sizes are multiples of 16. Free blocks keep their list links
// in the first 16 payload bytes. A pool is one contiguous region ending in a
// sentinel header; up to NPH_TLSF_MAX_POOLS regions may be added.
//
// This is not a thread-safe allocator. The runtime (Phase 3) wraps it with a
// lock or gives each thread its own heap; which one is a measured decision.
//
// With AddressSanitizer, free payloads (past the list links) are poisoned so
// use-after-free of heap memory is reported in Debug builds.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tuning constants (see the paper, §3): FL covers sizes up to 2^40 bytes;
// SL splits each power of two into 32 ranges; sizes below 512 bytes are
// mapped linearly in 16-byte steps.
#define NPH_TLSF_ALIGN_LOG2 4U
#define NPH_TLSF_ALIGN      (1U << NPH_TLSF_ALIGN_LOG2)
#define NPH_TLSF_SL_LOG2    5U
#define NPH_TLSF_SL_COUNT   (1U << NPH_TLSF_SL_LOG2)
#define NPH_TLSF_FL_MAX     40U
#define NPH_TLSF_FL_SHIFT   (NPH_TLSF_SL_LOG2 + NPH_TLSF_ALIGN_LOG2)
#define NPH_TLSF_FL_COUNT   (NPH_TLSF_FL_MAX - NPH_TLSF_FL_SHIFT + 1U)
#define NPH_TLSF_MAX_POOLS  16U

// Largest single allocation; also the largest pool.
#define NPH_TLSF_BLOCK_SIZE_MAX ((u64)1U << NPH_TLSF_FL_MAX)

// Smallest pool that can hold one minimal block: alignment slack, the first
// header, the minimum payload, and the sentinel header.
#define NPH_TLSF_MIN_POOL_SIZE 128U

typedef struct NphTlsfPool {
    u8 *base;              // start of the region as given by the caller
    u64 size;              // size of the region as given by the caller
    NphAllocator *backing; // owner of `base` when added via *_from, else NULL
} NphTlsfPool;

typedef struct NphTlsfStats {
    u64 pool_bytes;      // sum of all pool sizes as given by the caller
    u64 live_bytes;      // payload bytes currently allocated (after rounding)
    u64 free_bytes;      // payload bytes in free blocks
    u64 peak_live_bytes; // high-water mark of live_bytes since init
    u32 live_count;      // allocations outstanding
    u32 free_count;      // free blocks
    u32 pool_count;
} NphTlsfStats;

typedef struct NphTlsf {
    NphAllocator allocator; // first, so &tlsf->allocator == (NphAllocator *)tlsf
    // Free-list heads, one per (fl, sl) class. Empty lists point at the null
    // block so insert/remove never test for NULL.
    void *blocks[NPH_TLSF_FL_COUNT][NPH_TLSF_SL_COUNT];
    u32 sl_bitmap[NPH_TLSF_FL_COUNT]; // bit s set when blocks[fl][s] is non-empty
    u64 fl_bitmap;                    // bit f set when sl_bitmap[f] != 0
    u8 null_block[32];                // sentinel for the doubly-linked free lists
    NphTlsfPool pools[NPH_TLSF_MAX_POOLS];
    u32 pool_count;
    u32 live_count;
    u64 live_bytes;
    u64 peak_live_bytes;
} NphTlsf;

// Initialises the heap with [memory, memory + size) as its first pool. The
// caller keeps ownership of the block. Fails with NPH_RESULT_INVALID_ARGUMENT
// when the region is smaller than NPH_TLSF_MIN_POOL_SIZE or larger than
// NPH_TLSF_BLOCK_SIZE_MAX.
NPH_MUST_USE NphResult nph_tlsf_init(NphTlsf *tlsf, void *memory, u64 size);

// Initialises the heap with a first pool of `size` bytes from `backing`;
// nph_tlsf_destroy returns it.
NPH_MUST_USE NphResult nph_tlsf_init_from(NphTlsf *tlsf, NphAllocator *backing, u64 size);

// Returns every pool that was obtained from a backing allocator and clears
// the heap. Outstanding allocations become invalid. Safe on a zero-
// initialised heap.
void nph_tlsf_destroy(NphTlsf *tlsf);

NPH_INLINE NphAllocator *nph_tlsf_allocator(NphTlsf *tlsf)
{
    return &tlsf->allocator;
}

// Adds another region. Fails with NPH_RESULT_FULL at NPH_TLSF_MAX_POOLS
// pools or NPH_RESULT_INVALID_ARGUMENT for a bad size.
NPH_MUST_USE NphResult nph_tlsf_add_pool(NphTlsf *tlsf, void *memory, u64 size);
NPH_MUST_USE NphResult nph_tlsf_add_pool_from(NphTlsf *tlsf, NphAllocator *backing, u64 size);

// Removes the pool that starts at `memory`. Fails with NPH_RESULT_NOT_FOUND
// if no pool starts there, or NPH_RESULT_INVALID_ARGUMENT if any allocation
// in it is still live. A pool obtained through *_from is returned to its
// backing allocator.
NPH_MUST_USE NphResult nph_tlsf_remove_pool(NphTlsf *tlsf, void *memory);

// Allocates `size` bytes aligned to `align` (power of two). NULL when no
// free block fits or size exceeds NPH_TLSF_BLOCK_SIZE_MAX.
void *nph_tlsf_alloc(NphTlsf *tlsf, u64 size, u32 align);

// Resizes `ptr` to `new_size` bytes, growing in place when the following
// block is free and large enough. Returns NULL (leaving `ptr` valid) when
// that fails and no other block fits. `align` must be satisfied by the result.
void *nph_tlsf_realloc(NphTlsf *tlsf, void *ptr, u64 new_size, u32 align);

// Frees `ptr`, coalescing with free neighbours. NULL is ignored.
void nph_tlsf_free(NphTlsf *tlsf, void *ptr);

// Usable size of the allocation at `ptr` (the request rounded up to 16).
u64 nph_tlsf_usable_size(const void *ptr);

// True if `ptr` lies inside one of the heap's pools.
bool nph_tlsf_owns(const NphTlsf *tlsf, const void *ptr);

void nph_tlsf_get_stats(const NphTlsf *tlsf, NphTlsfStats *out);

// Walks every block of every pool in address order. `payload` is the block's
// payload address and `size` its payload size.
typedef void (*NphTlsfWalkFn)(void *payload, u64 size, bool used, void *user);
void nph_tlsf_walk(const NphTlsf *tlsf, NphTlsfWalkFn fn, void *user);

// Checks every structural invariant: block chains, physical links and flags,
// free-list membership and consistency with the bitmaps. O(heap). Returns
// NPH_RESULT_OK or NPH_RESULT_CORRUPT with a static description in
// *out_reason (may be NULL). Used by tests after every operation and by
// the debug overlay on request.
NphResult nph_tlsf_validate(const NphTlsf *tlsf, const char **out_reason);

#ifdef __cplusplus
}
#endif
