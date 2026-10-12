// SPDX-License-Identifier: Apache-2.0
#pragma once

// A mutex around any allocator (docs/adr/0009-platform.md §5).
//
// The engine's allocators are single-threaded by design: an arena belongs
// to one thread or job, and the TLSF heap (core/tlsf.h) documents that the
// runtime either wraps it or gives each thread its own. This is the wrapper.
// Its name makes the lock visible at the call site (CLAUDE.md §1.4): a
// subsystem that receives an NphLockedAllocator knows every allocation
// takes a mutex (~20 ns uncontended) and can decide whether that is
// acceptable for its traffic.
//
// Thread-safety: NPH_THREADSAFE for alloc/realloc/free. Init and destroy
// are single-threaded.

#include <nepheline/core/alloc.h>
#include <nepheline/platform/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphLockedAllocator {
    NphAllocator allocator; // first, so &la->allocator == (NphAllocator *)la
    NphAllocator *inner;
    NphMutex mutex;
} NphLockedAllocator;

// Wraps `inner`, which must not be used directly while the wrapper is in
// use. Never fails.
void nph_locked_allocator_init(NphLockedAllocator *la, NphAllocator *inner);

// Releases the mutex. Does not touch the inner allocator or its blocks.
void nph_locked_allocator_destroy(NphLockedAllocator *la);

NPH_INLINE NphAllocator *nph_locked_allocator_allocator(NphLockedAllocator *la)
{
    return &la->allocator;
}

#ifdef __cplusplus
}
#endif
