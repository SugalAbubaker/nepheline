// SPDX-License-Identifier: Apache-2.0
#pragma once

// Debug allocator wrapper (CLAUDE.md §8.1 "Debug wrappers add guard bytes,
// leak tracking with callsites, and use-after-free poisoning").
//
// Wraps any NphAllocator. Every allocation gets a header (callsite, size,
// alignment, list links) and guard bytes on both sides. On free the guards
// are verified, the payload is filled with NPH_DEBUG_ALLOC_POISON_FREED, and
// the block is returned to the inner allocator. The live-allocation list
// answers "what is still allocated, and who allocated it".
//
// Cost: ~96 bytes per allocation plus a spin lock around list updates. For
// Debug and Development builds and for tests; the engine's allocators are
// used directly in Profile and Shipping.
//
// Thread-safety: NPH_THREADSAFE for alloc/realloc/free when the inner
// allocator is thread-safe (the list is protected by a spin lock). The
// report and check functions take the same lock.

#include <nepheline/core/alloc.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_DEBUG_ALLOC_GUARD_SIZE    16U
#define NPH_DEBUG_ALLOC_POISON_FRESH  0xCDU // new allocations are filled with this
#define NPH_DEBUG_ALLOC_POISON_FREED  0xDDU // freed payloads are filled with this
#define NPH_DEBUG_ALLOC_GUARD_PATTERN 0xFDU

typedef struct NphDebugAllocBlock NphDebugAllocBlock; // opaque per-allocation header

typedef struct NphDebugAllocStats {
    u64 live_bytes; // payload bytes currently allocated
    u64 peak_live_bytes;
    u64 total_allocations; // alloc + realloc calls that produced a block
    u64 total_frees;
    u32 live_count;
    u32 peak_live_count;
} NphDebugAllocStats;

typedef struct NphDebugAllocator {
    NphAllocator allocator; // first, so &da->allocator == (NphAllocator *)da
    NphAllocator *inner;
    NphSpinLock lock;
    NphDebugAllocBlock *live_head; // most recent first
    NphDebugAllocStats stats;
} NphDebugAllocator;

// Information about one live allocation, for reports.
typedef struct NphDebugAllocInfo {
    void *ptr;
    u64 size;
    u32 align;
    NphCallsite site;
    u64 sequence; // allocation counter value when the block was created
} NphDebugAllocInfo;

typedef void (*NphDebugAllocReportFn)(const NphDebugAllocInfo *info, void *user);

// Initialises the wrapper over `inner`. Never fails.
void nph_debug_allocator_init(NphDebugAllocator *da, NphAllocator *inner);

// Checks for leaks (reports each through `on_leak` if non-NULL) and returns
// the number of live allocations. Does not free them: a leak is a bug to
// fix, and the inner allocator is about to be destroyed by its owner anyway.
u32 nph_debug_allocator_destroy(NphDebugAllocator *da, NphDebugAllocReportFn on_leak, void *user);

NPH_INLINE NphAllocator *nph_debug_allocator_allocator(NphDebugAllocator *da)
{
    return &da->allocator;
}

// Calls `fn` for every live allocation, most recent first. Returns the count.
u32 nph_debug_allocator_report(NphDebugAllocator *da, NphDebugAllocReportFn fn, void *user);

// Verifies the header and guard bytes of every live allocation. Returns
// NPH_RESULT_OK, or NPH_RESULT_CORRUPT with the first offending block
// described through *out_info (may be NULL).
NphResult nph_debug_allocator_check(NphDebugAllocator *da, NphDebugAllocInfo *out_info);

// Verifies one allocation's guards. NPH_RESULT_CORRUPT on overrun/underrun
// or a bad header (which includes a pointer not from this allocator).
NphResult nph_debug_allocator_check_block(const void *ptr);

void nph_debug_allocator_get_stats(NphDebugAllocator *da, NphDebugAllocStats *out);

#ifdef __cplusplus
}
#endif
