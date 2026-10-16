// SPDX-License-Identifier: Apache-2.0
#pragma once

// Memory tracking per allocator and tag (CLAUDE.md §8.11 "Memory tracking
// per allocator and tag").
//
// An NphTrackedAllocator wraps another allocator and counts what passes
// through it: the bytes and allocations alive now, their peaks, and the
// totals since creation. The runtime gives each subsystem its own tracked
// allocator (the world, the renderer, the asset loader, ...) over the
// shared heap, so the editor's memory panel shows where memory goes by tag
// (profiler.h, nph_profiler_track_memory) without any subsystem knowing it
// is measured. The counts are of requested sizes: what the allocator
// itself spends on headers or alignment is the backing allocator's
// business (its own tracked wrapper shows it, if it has one).
//
// The wrapper is as thread-safe as the allocator it wraps: the counters
// are atomic and add nothing else. Costs: two or three atomic additions
// per call, plus a compare-and-swap loop while a new peak is being set.
//
// Thread-safety: NPH_THREADSAFE when the backing allocator is; the
// statistics may be read from any thread at any time (each value is
// atomic, a snapshot of several is not).

#include <nepheline/core/alloc.h>
#include <nepheline/core/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphTrackedAllocator {
    NphAllocator allocator; // first, so &t->allocator is the allocator to hand out
    NphAllocator *backing;
    const char *tag; // static string: the panel's label
    NphAtomicU64 bytes;
    NphAtomicU64 peak_bytes;
    NphAtomicU64 allocations; // alive now
    NphAtomicU64 peak_allocations;
    NphAtomicU64 total_allocations; // since init (reallocations of a live block not counted)
    NphAtomicU64 total_bytes;       // allocated since init (growth by reallocation counted)
} NphTrackedAllocator;

typedef struct NphMemoryStats {
    const char *tag;
    u64 bytes;
    u64 peak_bytes;
    u64 allocations;
    u64 peak_allocations;
    u64 total_allocations;
    u64 total_bytes;
} NphMemoryStats;

// Starts counting allocations made through the wrapper; they are served by
// `backing`. `tag` must outlive the wrapper.
void nph_tracked_allocator_init(NphTrackedAllocator *t, NphAllocator *backing, const char *tag);

NPH_INLINE NphAllocator *nph_tracked_allocator(NphTrackedAllocator *t)
{
    return &t->allocator;
}

void nph_tracked_allocator_stats(const NphTrackedAllocator *t, NphMemoryStats *out);

// Clears the peaks to the current values (the panel's "reset peaks").
void nph_tracked_allocator_reset_peaks(NphTrackedAllocator *t);

#ifdef __cplusplus
}
#endif
