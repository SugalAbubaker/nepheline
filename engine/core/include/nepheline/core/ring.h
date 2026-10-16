// SPDX-License-Identifier: Apache-2.0
#pragma once

// Lock-free ring buffers (CLAUDE.md §8.1 "Ring buffers: SPSC lock-free,
// MPMC bounded (Vyukov)").
//
// NphSpscRing: one producer thread, one consumer thread. Positions are
// monotonically increasing 32-bit counters masked into a power-of-two
// buffer; the producer publishes with a release store of its position and
// the consumer acquires it, so element writes happen-before the matching
// read. Each side keeps a cached copy of the other's position and only
// re-reads the shared line when the ring looks full or empty (Lamport
// queue with the FastForward-style caching of Giacomoni et al., PPoPP 2008).
//
// NphMpmcRing: any number of producers and consumers. Vyukov's bounded
// queue: every cell carries a sequence number that encodes which turn of
// the ring it belongs to; producers and consumers claim positions with a
// compare-exchange and hand cells over with release/acquire on the
// sequence, so no operation ever blocks another. Elements are copied in and
// out by value (elem_size bytes).
//
// Both rings are bounded and never allocate after init: push returns false
// when full, pop returns false when empty. Capacity is a power of two.
//
// An MPMC push can also return false while the ring holds fewer than
// `capacity` elements: the cell it lands on may still belong to a pop one
// lap behind that has claimed the cell but not finished copying out of it
// (a pop preempted midway while later pops finish and their values come
// back). Likewise a pop can return false while an earlier push is still
// writing its cell. Code that knows a free cell or an element must exist
// (a free list holding each of its values once; a pop after a semaphore
// permit) retries rather than treating false as final.
//
// Thread-safety: as described per ring; init and free are single-threaded.

#include <nepheline/core/alloc.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// SPSC
// ---------------------------------------------------------------------------

typedef struct NphSpscRing {
    // Producer line: written by the producer only.
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 tail;
    u32 cached_head;
    // Consumer line: written by the consumer only.
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 head;
    u32 cached_tail;
    // Shared, read-only after init.
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) u8 *buffer;
    u32 capacity;
    u32 mask;
    u32 elem_size;
    NphAllocator *alloc;
} NphSpscRing;

// Allocates a ring of `capacity` (power of two >= 2) elements of
// `elem_size` bytes. INVALID_ARGUMENT or OUT_OF_MEMORY on failure.
NPH_MUST_USE NphResult nph_spsc_ring_init(NphSpscRing *ring, NphAllocator *alloc, u32 elem_size,
                                          u32 capacity);
void nph_spsc_ring_free(NphSpscRing *ring);

// Producer: copies `elem` in. false when full.
bool nph_spsc_ring_push(NphSpscRing *ring, const void *elem);

// Consumer: copies the oldest element to `out`. false when empty.
bool nph_spsc_ring_pop(NphSpscRing *ring, void *out);

// Consumer: address of the oldest element without removing it, or NULL.
// Valid until the next pop.
const void *nph_spsc_ring_peek(NphSpscRing *ring);

// Number of elements, as seen from either thread (may be stale by the time
// it is used, but never larger than the capacity).
u32 nph_spsc_ring_count(const NphSpscRing *ring);

NPH_INLINE u32 nph_spsc_ring_capacity(const NphSpscRing *ring)
{
    return ring->capacity;
}

// ---------------------------------------------------------------------------
// MPMC (Vyukov)
// ---------------------------------------------------------------------------

typedef struct NphMpmcRing {
    u8 *cells; // capacity cells of `stride` bytes: {sequence, padding, element}
    u32 capacity;
    u32 mask;
    u32 elem_size;
    u32 stride;
    u32 elem_offset;
    NphAllocator *alloc;
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 enqueue_pos;
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 dequeue_pos;
} NphMpmcRing;

NPH_MUST_USE NphResult nph_mpmc_ring_init(NphMpmcRing *ring, NphAllocator *alloc, u32 elem_size,
                                          u32 capacity);
void nph_mpmc_ring_free(NphMpmcRing *ring);

// Any thread: copies `elem` in. false when full.
bool nph_mpmc_ring_push(NphMpmcRing *ring, const void *elem);

// Any thread: copies the oldest element to `out`. false when empty.
bool nph_mpmc_ring_pop(NphMpmcRing *ring, void *out);

// Approximate element count; exact only when no other thread is active.
u32 nph_mpmc_ring_count(const NphMpmcRing *ring);

NPH_INLINE u32 nph_mpmc_ring_capacity(const NphMpmcRing *ring)
{
    return ring->capacity;
}

#ifdef __cplusplus
}
#endif
