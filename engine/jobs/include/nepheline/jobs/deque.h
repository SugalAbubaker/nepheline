// SPDX-License-Identifier: Apache-2.0
#pragma once

// Work-stealing deque (CLAUDE.md §8.4 "Work-stealing deques per worker").
//
// The owner pushes and takes at the bottom (LIFO: the job it just spawned
// is the one most likely to be in its cache); thieves steal from the top
// (FIFO: they take the oldest, which for divide-and-conquer work is the
// largest remaining piece). Chase & Lev, "Dynamic Circular Work-Stealing
// Deque", SPAA 2005, with the memory orderings of Lê, Pop, Cohen, and
// Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak Memory
// Models", PPoPP 2013 (figure 1). Where that paper uses a seq_cst fence
// between a relaxed store and a relaxed load, this implementation uses a
// seq_cst store and a seq_cst load instead: the same guarantee, compiled
// to the same `xchg` on x86-64, and visible to ThreadSanitizer, which does
// not model fences (docs/adr/0013-job-system.md §3).
//
// Elements are 32-bit values (job indices into the job pool), read and
// written with relaxed atomic accesses so that a thief reading a slot the
// owner is about to overwrite is a defined, if stale, read that the
// subsequent compare-exchange on `top` rejects.
//
// The buffer is fixed: push returns false when `capacity` elements are in
// the deque (the job system then falls back to its shared injection
// queue). Nothing allocates after init.
//
// Thread-safety: push and take are owner-only (one thread at a time; the
// owning thread may change between calls when a fiber migrates, as long
// as the calls do not overlap). steal is NPH_THREADSAFE. init and free are
// single-threaded.

#include <nepheline/core/alloc.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphJobDeque {
    // Thieves increment `top` with a compare-exchange; the owner only reads
    // it. The owner writes `bottom`; thieves read it. Each on its own line.
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU64 top;
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU64 bottom;
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 *buffer;
    u32 capacity;
    u32 mask;
    NphAllocator *alloc;
} NphJobDeque;

typedef enum NphJobDequeSteal {
    NPH_JOB_DEQUE_STOLEN,  // *out holds the element
    NPH_JOB_DEQUE_EMPTY,   // nothing to steal right now
    NPH_JOB_DEQUE_ABORT,   // lost a race with the owner or another thief; try again
    NPH_JOB_DEQUE_REFUSED, // steal_if only: the predicate declined the oldest element
} NphJobDequeSteal;

// Decides whether a take_if or steal_if may remove `value`.
typedef bool (*NphJobDequePred)(u32 value, void *ctx);

// Allocates a buffer of `capacity` (power of two >= 2) elements from
// `alloc`. NPH_RESULT_INVALID_ARGUMENT or NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_job_deque_init(NphJobDeque *d, NphAllocator *alloc, u32 capacity);

// Releases the buffer. Safe on a zero-filled deque.
void nph_job_deque_free(NphJobDeque *d);

// Owner: appends `value` at the bottom. false when the deque is full.
bool nph_job_deque_push(NphJobDeque *d, u32 value);

// Owner: removes the most recently pushed element. false when empty.
bool nph_job_deque_take(NphJobDeque *d, u32 *out);

// Thief: removes the oldest element.
NphJobDequeSteal nph_job_deque_steal(NphJobDeque *d, u32 *out);

// Owner: as take, but the element that would be taken is shown to `pred`
// first and left in place when it declines. Only the owner writes slots,
// so the element shown is the one take returns, unless a thief steals it
// in between, in which case this returns false like an empty deque.
bool nph_job_deque_take_if(NphJobDeque *d, NphJobDequePred pred, void *ctx, u32 *out);

// Thief: as steal, but the oldest element is shown to `pred` before the
// compare-exchange that claims it. NPH_JOB_DEQUE_REFUSED when the
// predicate declines; the element stays. The value shown may already have
// been taken by someone else (the exchange then fails with
// NPH_JOB_DEQUE_ABORT, or the refusal was for an element that is gone,
// which is harmless: the caller moves on and looks again later).
NphJobDequeSteal nph_job_deque_steal_if(NphJobDeque *d, NphJobDequePred pred, void *ctx, u32 *out);

// Approximate element count (exact only for the owner with no thief
// active). Never larger than the capacity.
u32 nph_job_deque_count(const NphJobDeque *d);

NPH_INLINE u32 nph_job_deque_capacity(const NphJobDeque *d)
{
    return d->capacity;
}

#ifdef __cplusplus
}
#endif
