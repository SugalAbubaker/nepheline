// SPDX-License-Identifier: Apache-2.0
#pragma once

// A range allocator: TLSF over offsets (CLAUDE.md §8.1 "Allocators"), for
// space the CPU never touches. The managed space is [0, size) in the
// caller's units (bytes, elements, pages); what the allocator hands out is
// an offset into it, and its bookkeeping lives out of band in a node array
// sized at creation, so nothing is ever written into the space itself.
// The renderer uses it to sub-allocate GPU buffers: the meshlet-index pool
// of the indirect-draw path and the per-instance material tables
// (docs/render.md), and the streaming pools of Phase 13 will too.
//
// The algorithm is Two-Level Segregated Fit (Masmano, Ripoll, Crespo &
// Real, "TLSF: a New Dynamic Memory Allocator for Real-Time Systems",
// ECRTS 2004) with the out-of-band nodes and 8-bit floating-point size
// classes of Sebastian Aaltonen's OffsetAllocator (2023): a size class is
// 5 exponent bits and 3 mantissa bits, 256 classes under a two-level
// bitmap, so a class never spans more than 12.5 % of its sizes.
//
// - Allocation rounds the request up to its class and takes the first
//   free range in the first non-empty class at or above it (two bit
//   scans), returning the rest of that range as a new free range. Any
//   range in a class at or above the request's rounded class is large
//   enough, so nothing is ever searched linearly.
// - Freeing merges the range with free physical neighbours at once
//   (immediate coalescing), so two free ranges are never adjacent.
// - Both are O(1). A free range is filed under its size rounded down, an
//   allocation searches from its size rounded up: the price of O(1) is
//   that a request may be refused while a free range of exactly its size
//   sits in a lower class's top end (at most 12.5 % of the request).
//
// Ranges are identified by NphRange: the offset and the node that tracks
// it; freeing takes the pair, so a double free or a forged range is
// detected (asserted in development builds, ignored in shipping).
//
// Costs (§1.4): init allocates the node array and its free stack
// (2 * max_allocations + 1 nodes of 28 bytes) from the given allocator;
// alloc and free never allocate.
//
// Thread-safety: none.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_RANGE_NONE NPH_U32_MAX

// Size classes: 3 mantissa bits, 5 exponent bits.
#define NPH_RANGE_MANTISSA_BITS 3U
#define NPH_RANGE_CLASS_COUNT   256U

// An allocated range: `offset` into the managed space and the `node` that
// tracks it. A failed allocation leaves both NPH_RANGE_NONE.
typedef struct NphRange {
    u32 offset;
    u32 node;
} NphRange;

#define NPH_RANGE_NULL ((NphRange){NPH_RANGE_NONE, NPH_RANGE_NONE})

NPH_INLINE bool nph_range_is_null(NphRange r)
{
    return r.node == NPH_RANGE_NONE;
}

typedef struct NphRangeNode NphRangeNode;

typedef struct NphRangeAllocator {
    NphAllocator *alloc;
    NphRangeNode *nodes;
    u32 *spare; // stack of unused node indices
    u32 spare_count;
    u32 node_count; // 2 * max_allocations + 1
    u32 size;       // the managed space: [0, size)
    u32 max_allocations;
    u32 free_total; // units not allocated
    u32 live;       // allocations outstanding
    u32 head;       // the node at offset 0
    u32 used_top;   // bit e: some class of exponent e is non-empty
    u8 used_classes[NPH_RANGE_CLASS_COUNT >> NPH_RANGE_MANTISSA_BITS]; // bit m: class e<<3|m
    u32 class_heads[NPH_RANGE_CLASS_COUNT];                            // first free node per class
} NphRangeAllocator;

// Prepares an allocator over [0, size) for at most `max_allocations` live
// ranges. NPH_RESULT_INVALID_ARGUMENT for a zero size or count, or more
// than 2^30 allocations; NPH_RESULT_OUT_OF_MEMORY when the nodes cannot be
// allocated. `*ra` is zero-filled on failure.
NPH_MUST_USE NphResult nph_range_alloc_init(NphRangeAllocator *ra, NphAllocator *alloc, u32 size,
                                            u32 max_allocations);

// Releases the nodes. Ranges still outstanding are simply forgotten (the
// space is not the allocator's to clean). Safe on a zero-filled allocator.
void nph_range_alloc_destroy(NphRangeAllocator *ra);

// Forgets every allocation: the whole space is one free range again.
void nph_range_alloc_reset(NphRangeAllocator *ra);

// Allocates `count` units (> 0). NPH_RESULT_FULL when no free range is
// large enough in a class that guarantees it (see above) or every node is
// in use; NPH_RESULT_INVALID_ARGUMENT for a zero count. `*out` is
// NPH_RANGE_NULL on failure.
NPH_MUST_USE NphResult nph_range_alloc(NphRangeAllocator *ra, u32 count, NphRange *out);

// Frees a range from nph_range_alloc and merges it with free neighbours.
// A null range is ignored; a range that is not live (freed twice, or never
// issued) is asserted in development builds and ignored otherwise.
void nph_range_free(NphRangeAllocator *ra, NphRange range);

// The units of a live range (0 for a null or dead one).
u32 nph_range_size(const NphRangeAllocator *ra, NphRange range);

// Units not allocated, in however many pieces.
NPH_INLINE u32 nph_range_alloc_free_total(const NphRangeAllocator *ra)
{
    return ra->free_total;
}

// The largest single free range. Walks the free list of the highest
// non-empty class (O(ranges in that class)); for statistics and tests.
u32 nph_range_alloc_largest_free(const NphRangeAllocator *ra);

// Checks every invariant: the physical list tiles [0, size) exactly, no
// two free ranges touch, each free range sits in the class of its size,
// the bitmaps match the class lists, and the counts agree. On failure
// returns NPH_RESULT_CORRUPT and points `*reason` (may be NULL) at a
// static description. O(nodes); for tests.
NPH_MUST_USE NphResult nph_range_alloc_validate(const NphRangeAllocator *ra, const char **reason);

// The size class of a request (rounded up) and of a free range (rounded
// down), and the smallest size in a class (classes from 240 up start at
// 2^32 or beyond, hence u64): exposed for tests.
u32 nph_range_class_round_up(u32 count);
u32 nph_range_class_round_down(u32 count);
u64 nph_range_class_size(u32 size_class);

#ifdef __cplusplus
}
#endif
