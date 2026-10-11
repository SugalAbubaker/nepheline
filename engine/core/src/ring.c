// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/ring.h>

#include <string.h>

// ---------------------------------------------------------------------------
// SPSC
// ---------------------------------------------------------------------------

static u8 *spsc_slot(const NphSpscRing *ring, u32 pos)
{
    return ring->buffer + (u64)(pos & ring->mask) * ring->elem_size;
}

NphResult nph_spsc_ring_init(NphSpscRing *ring, NphAllocator *alloc, u32 elem_size, u32 capacity)
{
    NPH_ASSERT(ring != NULL && alloc != NULL);
    memset(ring, 0, sizeof *ring);
    if (elem_size == 0U || capacity < 2U || !nph_is_pow2_u32(capacity)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    ring->buffer = (u8 *)NPH_ALLOC(alloc, (u64)elem_size * capacity, NPH_CACHE_LINE_SIZE);
    if (ring->buffer == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    ring->alloc = alloc;
    ring->capacity = capacity;
    ring->mask = capacity - 1U;
    ring->elem_size = elem_size;
    return NPH_RESULT_OK;
}

void nph_spsc_ring_free(NphSpscRing *ring)
{
    if (ring == NULL || ring->alloc == NULL) {
        return;
    }
    NPH_FREE(ring->alloc, ring->buffer, (u64)ring->elem_size * ring->capacity, NPH_CACHE_LINE_SIZE);
    memset(ring, 0, sizeof *ring);
}

bool nph_spsc_ring_push(NphSpscRing *ring, const void *elem)
{
    NPH_ASSERT(ring != NULL && elem != NULL);
    const u32 tail = nph_atomic_load_u32(&ring->tail, NPH_MEMORY_ORDER_RELAXED);
    if (tail - ring->cached_head == ring->capacity) {
        // Looks full: refresh the consumer's position (acquire pairs with
        // the consumer's release, so its reads of the slot are complete).
        ring->cached_head = nph_atomic_load_u32(&ring->head, NPH_MEMORY_ORDER_ACQUIRE);
        if (tail - ring->cached_head == ring->capacity) {
            return false;
        }
    }
    memcpy(spsc_slot(ring, tail), elem, ring->elem_size);
    nph_atomic_store_u32(&ring->tail, tail + 1U, NPH_MEMORY_ORDER_RELEASE);
    return true;
}

bool nph_spsc_ring_pop(NphSpscRing *ring, void *out)
{
    NPH_ASSERT(ring != NULL && out != NULL);
    const u32 head = nph_atomic_load_u32(&ring->head, NPH_MEMORY_ORDER_RELAXED);
    if (head == ring->cached_tail) {
        ring->cached_tail = nph_atomic_load_u32(&ring->tail, NPH_MEMORY_ORDER_ACQUIRE);
        if (head == ring->cached_tail) {
            return false;
        }
    }
    memcpy(out, spsc_slot(ring, head), ring->elem_size);
    nph_atomic_store_u32(&ring->head, head + 1U, NPH_MEMORY_ORDER_RELEASE);
    return true;
}

const void *nph_spsc_ring_peek(NphSpscRing *ring)
{
    NPH_ASSERT(ring != NULL);
    const u32 head = nph_atomic_load_u32(&ring->head, NPH_MEMORY_ORDER_RELAXED);
    if (head == ring->cached_tail) {
        ring->cached_tail = nph_atomic_load_u32(&ring->tail, NPH_MEMORY_ORDER_ACQUIRE);
        if (head == ring->cached_tail) {
            return NULL;
        }
    }
    return spsc_slot(ring, head);
}

u32 nph_spsc_ring_count(const NphSpscRing *ring)
{
    NPH_ASSERT(ring != NULL);
    const u32 tail = nph_atomic_load_u32(&ring->tail, NPH_MEMORY_ORDER_ACQUIRE);
    const u32 head = nph_atomic_load_u32(&ring->head, NPH_MEMORY_ORDER_ACQUIRE);
    const u32 count = tail - head;
    return count > ring->capacity ? ring->capacity : count;
}

// ---------------------------------------------------------------------------
// MPMC
// ---------------------------------------------------------------------------

static NphAtomicU32 *mpmc_sequence(const NphMpmcRing *ring, u32 index)
{
    return (NphAtomicU32 *)(ring->cells + (u64)index * ring->stride);
}

static u8 *mpmc_element(const NphMpmcRing *ring, u32 index)
{
    return ring->cells + (u64)index * ring->stride + ring->elem_offset;
}

NphResult nph_mpmc_ring_init(NphMpmcRing *ring, NphAllocator *alloc, u32 elem_size, u32 capacity)
{
    NPH_ASSERT(ring != NULL && alloc != NULL);
    memset(ring, 0, sizeof *ring);
    if (elem_size == 0U || capacity < 2U || !nph_is_pow2_u32(capacity)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Element after the sequence word, aligned to 8 or 16 by its size.
    const u32 elem_align = elem_size >= 16U ? 16U : 8U;
    ring->elem_offset = elem_align;
    ring->stride = nph_align_up_u32(elem_align + elem_size, elem_align);
    ring->cells = (u8 *)NPH_ALLOC(alloc, (u64)ring->stride * capacity, NPH_CACHE_LINE_SIZE);
    if (ring->cells == NULL) {
        memset(ring, 0, sizeof *ring);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    ring->alloc = alloc;
    ring->capacity = capacity;
    ring->mask = capacity - 1U;
    ring->elem_size = elem_size;
    for (u32 i = 0; i < capacity; ++i) {
        nph_atomic_store_u32(mpmc_sequence(ring, i), i, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_atomic_store_u32(&ring->enqueue_pos, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&ring->dequeue_pos, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_SEQ_CST);
    return NPH_RESULT_OK;
}

void nph_mpmc_ring_free(NphMpmcRing *ring)
{
    if (ring == NULL || ring->alloc == NULL) {
        return;
    }
    NPH_FREE(ring->alloc, ring->cells, (u64)ring->stride * ring->capacity, NPH_CACHE_LINE_SIZE);
    memset(ring, 0, sizeof *ring);
}

bool nph_mpmc_ring_push(NphMpmcRing *ring, const void *elem)
{
    NPH_ASSERT(ring != NULL && elem != NULL);
    u32 pos = nph_atomic_load_u32(&ring->enqueue_pos, NPH_MEMORY_ORDER_RELAXED);
    NphAtomicU32 *seq_atomic;
    for (;;) {
        seq_atomic = mpmc_sequence(ring, pos & ring->mask);
        const u32 seq = nph_atomic_load_u32(seq_atomic, NPH_MEMORY_ORDER_ACQUIRE);
        const i32 dif = (i32)(seq - pos);
        if (dif == 0) {
            // The cell is free for this turn: claim the position.
            if (nph_atomic_compare_exchange_u32(&ring->enqueue_pos, &pos, pos + 1U,
                                                NPH_MEMORY_ORDER_RELAXED,
                                                NPH_MEMORY_ORDER_RELAXED)) {
                break;
            }
            // pos was updated with the current value; retry with it.
        } else if (dif < 0) {
            return false; // the cell still holds the previous turn's element: full
        } else {
            pos = nph_atomic_load_u32(&ring->enqueue_pos, NPH_MEMORY_ORDER_RELAXED);
        }
    }
    memcpy(mpmc_element(ring, pos & ring->mask), elem, ring->elem_size);
    nph_atomic_store_u32(seq_atomic, pos + 1U, NPH_MEMORY_ORDER_RELEASE);
    return true;
}

bool nph_mpmc_ring_pop(NphMpmcRing *ring, void *out)
{
    NPH_ASSERT(ring != NULL && out != NULL);
    u32 pos = nph_atomic_load_u32(&ring->dequeue_pos, NPH_MEMORY_ORDER_RELAXED);
    NphAtomicU32 *seq_atomic;
    for (;;) {
        seq_atomic = mpmc_sequence(ring, pos & ring->mask);
        const u32 seq = nph_atomic_load_u32(seq_atomic, NPH_MEMORY_ORDER_ACQUIRE);
        const i32 dif = (i32)(seq - (pos + 1U));
        if (dif == 0) {
            if (nph_atomic_compare_exchange_u32(&ring->dequeue_pos, &pos, pos + 1U,
                                                NPH_MEMORY_ORDER_RELAXED,
                                                NPH_MEMORY_ORDER_RELAXED)) {
                break;
            }
        } else if (dif < 0) {
            return false; // not yet published: empty
        } else {
            pos = nph_atomic_load_u32(&ring->dequeue_pos, NPH_MEMORY_ORDER_RELAXED);
        }
    }
    memcpy(out, mpmc_element(ring, pos & ring->mask), ring->elem_size);
    // Hand the cell to the producer of the next turn.
    nph_atomic_store_u32(seq_atomic, pos + ring->mask + 1U, NPH_MEMORY_ORDER_RELEASE);
    return true;
}

u32 nph_mpmc_ring_count(const NphMpmcRing *ring)
{
    NPH_ASSERT(ring != NULL);
    const u32 enq = nph_atomic_load_u32(&ring->enqueue_pos, NPH_MEMORY_ORDER_ACQUIRE);
    const u32 deq = nph_atomic_load_u32(&ring->dequeue_pos, NPH_MEMORY_ORDER_ACQUIRE);
    const u32 count = enq - deq;
    return count > ring->capacity ? ring->capacity : count;
}
