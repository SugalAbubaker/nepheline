// SPDX-License-Identifier: Apache-2.0
// Chase-Lev work-stealing deque; see the header for the papers and the
// ordering argument. Positions are 64-bit so they never wrap in practice
// (a deque doing a billion operations a second would take centuries);
// comparisons are done in signed arithmetic because `take` moves `bottom`
// one below `top` transiently when the deque is empty.
#include <nepheline/core/assert.h>
#include <nepheline/jobs/deque.h>

#include <string.h>

NphResult nph_job_deque_init(NphJobDeque *d, NphAllocator *alloc, u32 capacity)
{
    NPH_ASSERT(d != NULL && alloc != NULL);
    memset(d, 0, sizeof *d);
    if (capacity < 2U || !nph_is_pow2_u32(capacity)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAtomicU32 *buffer =
        NPH_ALLOC_ARRAY_ZEROED(alloc, NphAtomicU32, capacity); // zeroed: no stale reads
    if (buffer == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    d->buffer = buffer;
    d->capacity = capacity;
    d->mask = capacity - 1U;
    d->alloc = alloc;
    return NPH_RESULT_OK;
}

void nph_job_deque_free(NphJobDeque *d)
{
    NPH_ASSERT(d != NULL);
    if (d->buffer != NULL) {
        NPH_FREE_ARRAY(d->alloc, NphAtomicU32, d->buffer, d->capacity);
    }
    memset(d, 0, sizeof *d);
}

bool nph_job_deque_push(NphJobDeque *d, u32 value)
{
    const u64 b = nph_atomic_load_u64(&d->bottom, NPH_MEMORY_ORDER_RELAXED);
    // A stale (lower) `top` only makes the deque look fuller than it is.
    const u64 t = nph_atomic_load_u64(&d->top, NPH_MEMORY_ORDER_ACQUIRE);
    if (b - t >= (u64)d->capacity) {
        return false;
    }
    nph_atomic_store_u32(&d->buffer[b & d->mask], value, NPH_MEMORY_ORDER_RELAXED);
    // Release: a thief that acquires this `bottom` sees the element, and
    // everything the owner wrote before pushing it.
    nph_atomic_store_u64(&d->bottom, b + 1U, NPH_MEMORY_ORDER_RELEASE);
    return true;
}

bool nph_job_deque_take(NphJobDeque *d, u32 *out)
{
    NPH_ASSERT(out != NULL);
    const u64 b = nph_atomic_load_u64(&d->bottom, NPH_MEMORY_ORDER_RELAXED) - 1U;
    // The store must be globally visible before `top` is read, or the
    // owner and a thief could both take the last element (the
    // store-buffering litmus test); seq_cst on both sides forbids it.
    nph_atomic_store_u64(&d->bottom, b, NPH_MEMORY_ORDER_SEQ_CST);
    u64 t = nph_atomic_load_u64(&d->top, NPH_MEMORY_ORDER_SEQ_CST);
    if ((i64)t > (i64)b) {
        // Empty: restore the canonical empty state.
        nph_atomic_store_u64(&d->bottom, b + 1U, NPH_MEMORY_ORDER_RELAXED);
        return false;
    }
    const u32 value = nph_atomic_load_u32(&d->buffer[b & d->mask], NPH_MEMORY_ORDER_RELAXED);
    if (t == b) {
        // Last element: race the thieves for it.
        const bool won = nph_atomic_compare_exchange_u64(
            &d->top, &t, t + 1U, NPH_MEMORY_ORDER_SEQ_CST, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_store_u64(&d->bottom, b + 1U, NPH_MEMORY_ORDER_RELAXED);
        if (!won) {
            return false;
        }
    }
    *out = value;
    return true;
}

NphJobDequeSteal nph_job_deque_steal(NphJobDeque *d, u32 *out)
{
    NPH_ASSERT(out != NULL);
    u64 t = nph_atomic_load_u64(&d->top, NPH_MEMORY_ORDER_SEQ_CST);
    const u64 b = nph_atomic_load_u64(&d->bottom, NPH_MEMORY_ORDER_SEQ_CST);
    if ((i64)t >= (i64)b) {
        return NPH_JOB_DEQUE_EMPTY;
    }
    // The slot may be overwritten by the owner after it wraps around, but
    // only after `top` has moved past it, which the exchange detects.
    const u32 value = nph_atomic_load_u32(&d->buffer[t & d->mask], NPH_MEMORY_ORDER_RELAXED);
    if (!nph_atomic_compare_exchange_u64(&d->top, &t, t + 1U, NPH_MEMORY_ORDER_SEQ_CST,
                                         NPH_MEMORY_ORDER_RELAXED)) {
        return NPH_JOB_DEQUE_ABORT;
    }
    *out = value;
    return NPH_JOB_DEQUE_STOLEN;
}

bool nph_job_deque_take_if(NphJobDeque *d, NphJobDequePred pred, void *ctx, u32 *out)
{
    NPH_ASSERT(pred != NULL && out != NULL);
    const u64 b = nph_atomic_load_u64(&d->bottom, NPH_MEMORY_ORDER_RELAXED);
    const u64 t = nph_atomic_load_u64(&d->top, NPH_MEMORY_ORDER_ACQUIRE);
    if ((i64)t >= (i64)b) {
        return false;
    }
    // A stale (lower) `top` can make an empty deque look non-empty; the
    // slot then holds an element already taken, the predicate judges a
    // stale value, and take below reports the deque empty.
    const u32 value = nph_atomic_load_u32(&d->buffer[(b - 1U) & d->mask], NPH_MEMORY_ORDER_RELAXED);
    if (!pred(value, ctx)) {
        return false;
    }
    return nph_job_deque_take(d, out);
}

NphJobDequeSteal nph_job_deque_steal_if(NphJobDeque *d, NphJobDequePred pred, void *ctx, u32 *out)
{
    NPH_ASSERT(pred != NULL && out != NULL);
    u64 t = nph_atomic_load_u64(&d->top, NPH_MEMORY_ORDER_SEQ_CST);
    const u64 b = nph_atomic_load_u64(&d->bottom, NPH_MEMORY_ORDER_SEQ_CST);
    if ((i64)t >= (i64)b) {
        return NPH_JOB_DEQUE_EMPTY;
    }
    const u32 value = nph_atomic_load_u32(&d->buffer[t & d->mask], NPH_MEMORY_ORDER_RELAXED);
    if (!pred(value, ctx)) {
        return NPH_JOB_DEQUE_REFUSED;
    }
    if (!nph_atomic_compare_exchange_u64(&d->top, &t, t + 1U, NPH_MEMORY_ORDER_SEQ_CST,
                                         NPH_MEMORY_ORDER_RELAXED)) {
        return NPH_JOB_DEQUE_ABORT;
    }
    *out = value;
    return NPH_JOB_DEQUE_STOLEN;
}

u32 nph_job_deque_count(const NphJobDeque *d)
{
    const u64 t = nph_atomic_load_u64(&d->top, NPH_MEMORY_ORDER_RELAXED);
    const u64 b = nph_atomic_load_u64(&d->bottom, NPH_MEMORY_ORDER_RELAXED);
    if ((i64)b <= (i64)t) {
        return 0U;
    }
    const u64 n = b - t;
    return n > (u64)d->capacity ? d->capacity : (u32)n;
}
