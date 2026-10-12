// SPDX-License-Identifier: Apache-2.0
// The concurrent tests of core's lock-free facilities that Phase 1
// deferred until threads existed (ADR-0005 "Consequences"): atomics,
// the spin lock, the SPSC and MPMC rings, the debug allocator, and the
// logger's per-thread rings under a concurrent flusher.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/log.h>
#include <nepheline/core/ring.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Atomics
// ---------------------------------------------------------------------------

typedef struct Counters {
    NphAtomicU32 add32;
    NphAtomicU64 add64;
    NphAtomicU32 cas32;
    NphAtomicU64 cas64;
    NphAtomicU32 bits;
    u32 iterations;
} Counters;

static void atomic_worker(u32 index, void *shared)
{
    Counters *c = (Counters *)shared;
    for (u32 i = 0; i < c->iterations; ++i) {
        nph_atomic_fetch_add_u32(&c->add32, 1U, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_fetch_add_u64(&c->add64, 3U, NPH_MEMORY_ORDER_RELAXED);
        u32 old32 = nph_atomic_load_u32(&c->cas32, NPH_MEMORY_ORDER_RELAXED);
        while (!nph_atomic_compare_exchange_u32(
            &c->cas32, &old32, old32 + 1U, NPH_MEMORY_ORDER_RELAXED, NPH_MEMORY_ORDER_RELAXED)) {
        }
        u64 old64 = nph_atomic_load_u64(&c->cas64, NPH_MEMORY_ORDER_RELAXED);
        while (!nph_atomic_compare_exchange_u64(
            &c->cas64, &old64, old64 + 1U, NPH_MEMORY_ORDER_RELAXED, NPH_MEMORY_ORDER_RELAXED)) {
        }
        // Each worker toggles its own bit; the others' bits must survive.
        nph_atomic_fetch_or_u32(&c->bits, 1U << index, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_fetch_and_u32(&c->bits, ~(1U << index), NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_fetch_or_u32(&c->bits, 1U << index, NPH_MEMORY_ORDER_RELAXED);
    }
}

NTEST(concurrent_atomic, read_modify_write_is_exact_under_contention)
{
    Counters c;
    memset(&c, 0, sizeof c);
    c.iterations = 50000U * TEST_SCALE;
    TestWorker workers[8];
    test_threads_run(workers, 8U, atomic_worker, &c);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.add32, NPH_MEMORY_ORDER_RELAXED),
                         8U * c.iterations);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&c.add64, NPH_MEMORY_ORDER_RELAXED),
                         (u64)8U * c.iterations * 3U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.cas32, NPH_MEMORY_ORDER_RELAXED),
                         8U * c.iterations);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&c.cas64, NPH_MEMORY_ORDER_RELAXED),
                         (u64)8U * c.iterations);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.bits, NPH_MEMORY_ORDER_RELAXED), 0xFFU);
}

// Store buffering (Dekker): each of two threads stores its flag with
// seq_cst and then loads the other's with seq_cst. Sequential consistency
// forbids both loads returning 0 in the same round. On x86 a plain store
// followed by a load may be reordered, so this fails if seq_cst stores do
// not carry a full barrier (the MSVC implementation uses xchg for that).
typedef struct Dekker {
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 flag[2];
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) u32 seen[2];
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) TestBarrier barrier;
    u32 rounds;
    NphAtomicU32 violations;
} Dekker;

static void dekker_worker(u32 index, void *shared)
{
    Dekker *d = (Dekker *)shared;
    const u32 other = 1U - index;
    for (u32 r = 0; r < d->rounds; ++r) {
        test_barrier_wait(&d->barrier); // flags are 0 here
        nph_atomic_store_u32(&d->flag[index], 1U, NPH_MEMORY_ORDER_SEQ_CST);
        d->seen[index] = nph_atomic_load_u32(&d->flag[other], NPH_MEMORY_ORDER_SEQ_CST);
        test_barrier_wait(&d->barrier);
        if (index == 0U) {
            if (d->seen[0] == 0U && d->seen[1] == 0U) {
                nph_atomic_fetch_add_u32(&d->violations, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            nph_atomic_store_u32(&d->flag[0], 0U, NPH_MEMORY_ORDER_RELAXED);
            nph_atomic_store_u32(&d->flag[1], 0U, NPH_MEMORY_ORDER_RELAXED);
        }
        test_barrier_wait(&d->barrier);
    }
}

NTEST(concurrent_atomic, seq_cst_store_load_is_not_reordered)
{
    Dekker d;
    memset(&d, 0, sizeof d);
    test_barrier_init(&d.barrier, 2U);
    d.rounds = 20000U * TEST_SCALE;
    TestWorker workers[2];
    test_threads_run(workers, 2U, dekker_worker, &d);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&d.violations, NPH_MEMORY_ORDER_RELAXED), 0U);
}

// Message passing: the producer writes a payload then publishes with a
// release store; the consumer acquires the flag and must see the payload.
typedef struct MessagePassing {
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) u64 payload[8];
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 ready;
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 consumed;
    u32 rounds;
    NphAtomicU32 violations;
} MessagePassing;

static void mp_worker(u32 index, void *shared)
{
    MessagePassing *m = (MessagePassing *)shared;
    for (u32 r = 1; r <= m->rounds; ++r) {
        if (index == 0U) {
            for (u32 i = 0; i < 8U; ++i) {
                m->payload[i] = (u64)r * 1000U + i;
            }
            nph_atomic_store_u32(&m->ready, r, NPH_MEMORY_ORDER_RELEASE);
            while (nph_atomic_load_u32(&m->consumed, NPH_MEMORY_ORDER_ACQUIRE) != r) {
                nph_cpu_relax();
            }
        } else {
            while (nph_atomic_load_u32(&m->ready, NPH_MEMORY_ORDER_ACQUIRE) != r) {
                nph_cpu_relax();
            }
            for (u32 i = 0; i < 8U; ++i) {
                if (m->payload[i] != (u64)r * 1000U + i) {
                    nph_atomic_fetch_add_u32(&m->violations, 1U, NPH_MEMORY_ORDER_RELAXED);
                }
            }
            nph_atomic_store_u32(&m->consumed, r, NPH_MEMORY_ORDER_RELEASE);
        }
    }
}

NTEST(concurrent_atomic, release_acquire_publishes_payload)
{
    MessagePassing m;
    memset(&m, 0, sizeof m);
    m.rounds = 50000U * TEST_SCALE;
    TestWorker workers[2];
    test_threads_run(workers, 2U, mp_worker, &m);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&m.violations, NPH_MEMORY_ORDER_RELAXED), 0U);
}

typedef struct SpinShared {
    NphSpinLock lock;
    u64 counter;
    u32 iterations;
} SpinShared;

static void spin_worker(u32 index, void *shared)
{
    NPH_UNUSED(index);
    SpinShared *s = (SpinShared *)shared;
    for (u32 i = 0; i < s->iterations; ++i) {
        if ((i & 7U) == 0U) {
            while (!nph_spin_lock_try_acquire(&s->lock)) {
                nph_cpu_relax();
            }
        } else {
            nph_spin_lock_acquire(&s->lock);
        }
        s->counter += 1U;
        nph_spin_lock_release(&s->lock);
    }
}

NTEST(concurrent_atomic, spin_lock_excludes)
{
    SpinShared s;
    nph_spin_lock_init(&s.lock);
    s.counter = 0U;
    s.iterations = 20000U * TEST_SCALE;
    TestWorker workers[TEST_MAX_WORKERS];
    const u32 n = test_worker_count(8U);
    test_threads_run(workers, n, spin_worker, &s);
    NTEST_ASSERT_EQ_UINT(s.counter, (u64)n * s.iterations);
}

// ---------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------

typedef struct Item {
    u64 producer;
    u64 sequence;
    u64 check;
} Item;

typedef struct SpscShared {
    NphSpscRing ring;
    u32 count;
    NphAtomicU32 violations;
    u64 sum; // consumer-owned
} SpscShared;

static void spsc_worker(u32 index, void *shared)
{
    SpscShared *s = (SpscShared *)shared;
    if (index == 0U) {
        for (u64 i = 1; i <= s->count; ++i) {
            Item item;
            item.producer = 0U;
            item.sequence = i;
            item.check = i * 0x9E3779B97F4A7C15ULL;
            while (!nph_spsc_ring_push(&s->ring, &item)) {
                nph_cpu_relax();
            }
        }
    } else {
        u64 expected = 1U;
        while (expected <= s->count) {
            Item item;
            if (!nph_spsc_ring_pop(&s->ring, &item)) {
                nph_cpu_relax();
                continue;
            }
            if (item.sequence != expected || item.check != expected * 0x9E3779B97F4A7C15ULL) {
                nph_atomic_fetch_add_u32(&s->violations, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            s->sum += item.sequence;
            ++expected;
        }
    }
}

NTEST(concurrent_ring, spsc_preserves_order_and_contents)
{
    SpscShared s;
    memset(&s, 0, sizeof s);
    NTEST_ASSERT_EQ_INT(nph_spsc_ring_init(&s.ring, nph_allocator_system(), sizeof(Item), 64U),
                        NPH_RESULT_OK);
    s.count = 200000U * TEST_SCALE;
    TestWorker workers[2];
    test_threads_run(workers, 2U, spsc_worker, &s);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.violations, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(s.sum, (u64)s.count * (s.count + 1U) / 2U);
    NTEST_ASSERT_EQ_UINT(nph_spsc_ring_count(&s.ring), 0U);
    nph_spsc_ring_free(&s.ring);
}

#define MPMC_PRODUCERS 4U
#define MPMC_CONSUMERS 4U

typedef struct MpmcShared {
    NphMpmcRing ring;
    u32 per_producer;
    NphAtomicU32 violations;
    NphAtomicU64 consumed_sum;
    NphAtomicU32 consumed_count;
    NphAtomicU32 producers_done;
    // Per consumer, the last sequence seen from each producer: a ring that
    // reorders one producer's items would show a non-increasing sequence.
    u64 last_seen[MPMC_CONSUMERS][MPMC_PRODUCERS];
} MpmcShared;

static void mpmc_worker(u32 index, void *shared)
{
    MpmcShared *s = (MpmcShared *)shared;
    if (index < MPMC_PRODUCERS) {
        for (u64 i = 1; i <= s->per_producer; ++i) {
            Item item;
            item.producer = index;
            item.sequence = i;
            item.check = (index + 1U) * i;
            while (!nph_mpmc_ring_push(&s->ring, &item)) {
                nph_cpu_relax();
            }
        }
        nph_atomic_fetch_add_u32(&s->producers_done, 1U, NPH_MEMORY_ORDER_RELEASE);
    } else {
        const u32 c = index - MPMC_PRODUCERS;
        for (;;) {
            Item item;
            if (nph_mpmc_ring_pop(&s->ring, &item)) {
                if (item.producer >= MPMC_PRODUCERS ||
                    item.check != (item.producer + 1U) * item.sequence ||
                    item.sequence <= s->last_seen[c][item.producer]) {
                    nph_atomic_fetch_add_u32(&s->violations, 1U, NPH_MEMORY_ORDER_RELAXED);
                } else {
                    s->last_seen[c][item.producer] = item.sequence;
                }
                nph_atomic_fetch_add_u64(&s->consumed_sum, item.sequence, NPH_MEMORY_ORDER_RELAXED);
                nph_atomic_fetch_add_u32(&s->consumed_count, 1U, NPH_MEMORY_ORDER_RELAXED);
                continue;
            }
            if (nph_atomic_load_u32(&s->producers_done, NPH_MEMORY_ORDER_ACQUIRE) ==
                    MPMC_PRODUCERS &&
                nph_mpmc_ring_count(&s->ring) == 0U) {
                // Re-check once more after the producers' release: a push
                // that completed just before producers_done was counted is
                // visible now.
                if (!nph_mpmc_ring_pop(&s->ring, &item)) {
                    return;
                }
                nph_atomic_fetch_add_u64(&s->consumed_sum, item.sequence, NPH_MEMORY_ORDER_RELAXED);
                nph_atomic_fetch_add_u32(&s->consumed_count, 1U, NPH_MEMORY_ORDER_RELAXED);
                continue;
            }
            nph_cpu_relax();
        }
    }
}

NTEST(concurrent_ring, mpmc_delivers_every_item_exactly_once)
{
    MpmcShared s;
    memset(&s, 0, sizeof s);
    NTEST_ASSERT_EQ_INT(nph_mpmc_ring_init(&s.ring, nph_allocator_system(), sizeof(Item), 128U),
                        NPH_RESULT_OK);
    s.per_producer = 50000U * TEST_SCALE;
    TestWorker workers[MPMC_PRODUCERS + MPMC_CONSUMERS];
    test_threads_run(workers, MPMC_PRODUCERS + MPMC_CONSUMERS, mpmc_worker, &s);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.violations, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.consumed_count, NPH_MEMORY_ORDER_RELAXED),
                         MPMC_PRODUCERS * s.per_producer);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&s.consumed_sum, NPH_MEMORY_ORDER_RELAXED),
                         (u64)MPMC_PRODUCERS * s.per_producer * (s.per_producer + 1U) / 2U);
    NTEST_ASSERT_EQ_UINT(nph_mpmc_ring_count(&s.ring), 0U);
    nph_mpmc_ring_free(&s.ring);
}

// ---------------------------------------------------------------------------
// Debug allocator: its live list is shared and spin-locked.
// ---------------------------------------------------------------------------

typedef struct DebugAllocShared {
    NphAllocator *alloc;
    u32 iterations;
    NphAtomicU32 failures;
} DebugAllocShared;

static void debug_alloc_worker(u32 index, void *shared)
{
    DebugAllocShared *s = (DebugAllocShared *)shared;
    u64 rng = 0xABCDU + index;
    void *slots[16];
    u64 sizes[16];
    memset((void *)slots, 0, sizeof slots);
    memset(sizes, 0, sizeof sizes);
    for (u32 i = 0; i < s->iterations; ++i) {
        const u32 slot = (u32)(test_rng_next(&rng) % 16U);
        if (slots[slot] != NULL) {
            if (nph_debug_allocator_check_block(slots[slot]) != NPH_RESULT_OK) {
                nph_atomic_fetch_add_u32(&s->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
            }
            NPH_FREE(s->alloc, slots[slot], sizes[slot], 16U);
            slots[slot] = NULL;
        } else {
            const u64 size = 1U + test_rng_next(&rng) % 500U;
            void *p = NPH_ALLOC(s->alloc, size, 16U);
            if (p == NULL) {
                nph_atomic_fetch_add_u32(&s->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
                continue;
            }
            memset(p, 0x11, (size_t)size);
            slots[slot] = p;
            sizes[slot] = size;
        }
    }
    for (u32 slot = 0; slot < 16U; ++slot) {
        if (slots[slot] != NULL) {
            NPH_FREE(s->alloc, slots[slot], sizes[slot], 16U);
        }
    }
}

NTEST(concurrent_alloc, debug_allocator_tracks_blocks_from_many_threads)
{
    NphDebugAllocator da;
    nph_debug_allocator_init(&da, nph_allocator_system());
    DebugAllocShared s;
    s.alloc = nph_debug_allocator_allocator(&da);
    s.iterations = 4000U * TEST_SCALE;
    nph_atomic_store_u32(&s.failures, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker workers[TEST_MAX_WORKERS];
    const u32 n = test_worker_count(8U);
    test_threads_run(workers, n, debug_alloc_worker, &s);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
    NphDebugAllocStats stats;
    nph_debug_allocator_get_stats(&da, &stats);
    NTEST_ASSERT_EQ_UINT(stats.live_count, 0U);
    NTEST_ASSERT_EQ_UINT(stats.total_allocations, stats.total_frees);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_check(&da, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_debug_allocator_destroy(&da, NULL, NULL), 0U);
}

// ---------------------------------------------------------------------------
// Logger: many writers, one flusher running at the same time, with
// detach/attach churn as threads come and go.
// ---------------------------------------------------------------------------

NPH_LOG_CATEGORY_DEFINE(LOG_CONCURRENT, "concurrent", NPH_LOG_LEVEL_TRACE);

typedef struct LogShared {
    u32 records_each;
    NphAtomicU32 delivered;
    NphAtomicU32 dropped;
    NphAtomicU32 writers_done;
    NphAtomicU32 out_of_order; // per-thread sequence must only increase
    u32 last_sequence[NPH_LOG_MAX_THREADS + 1U];
} LogShared;

static void counting_sink(const NphLogRecord *record, void *user)
{
    LogShared *s = (LogShared *)user;
    nph_atomic_fetch_add_u32(&s->dropped, record->dropped_before, NPH_MEMORY_ORDER_RELAXED);
    if (strcmp(record->category, "concurrent") != 0) {
        return; // the flusher's own report of drops left behind by an exited thread
    }
    nph_atomic_fetch_add_u32(&s->delivered, 1U, NPH_MEMORY_ORDER_RELAXED);
    if (record->thread_index <= NPH_LOG_MAX_THREADS) {
        if (record->sequence <= s->last_sequence[record->thread_index] &&
            s->last_sequence[record->thread_index] != 0U) {
            nph_atomic_fetch_add_u32(&s->out_of_order, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
        s->last_sequence[record->thread_index] = record->sequence;
    }
}

static void log_writer(u32 index, void *shared)
{
    LogShared *s = (LogShared *)shared;
    for (u32 i = 0; i < s->records_each; ++i) {
        NPH_LOG_INFO(&LOG_CONCURRENT, "writer %u record %u", index, i);
        if ((i & 31U) == 0U) {
            nph_thread_yield();
        }
    }
    nph_atomic_fetch_add_u32(&s->writers_done, 1U, NPH_MEMORY_ORDER_RELEASE);
}

static void log_flusher(u32 index, void *shared)
{
    NPH_UNUSED(index);
    LogShared *s = (LogShared *)shared;
    while (nph_atomic_load_u32(&s->writers_done, NPH_MEMORY_ORDER_ACQUIRE) < 8U) {
        nph_log_flush(0U);
        nph_thread_yield();
    }
    nph_log_flush(0U);
}

NTEST(concurrent_log, writers_and_flusher_account_for_every_record)
{
    NphLogConfig config;
    config.alloc = nph_allocator_system();
    config.ring_capacity = 128U;
    config.max_threads = 64U;
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_OK);
    LogShared s;
    memset(&s, 0, sizeof s);
    s.records_each = 500U * TEST_SCALE;
    NTEST_ASSERT_EQ_INT(nph_log_add_sink(counting_sink, &s, NPH_LOG_LEVEL_TRACE), NPH_RESULT_OK);
    TestWorker flusher;
    TestWorker writers[8];
    test_threads_start(&flusher, 1U, log_flusher, &s);
    for (u32 round = 0; round < 2U; ++round) {
        // Threads exit (detaching their rings) and new ones attach while
        // the flusher is running.
        test_threads_run(writers, 4U, log_writer, &s);
    }
    test_threads_join(&flusher, 1U);
    nph_log_flush(0U);
    const u32 delivered = nph_atomic_load_u32(&s.delivered, NPH_MEMORY_ORDER_RELAXED);
    const u32 dropped = nph_atomic_load_u32(&s.dropped, NPH_MEMORY_ORDER_RELAXED);
    NTEST_LOG("delivered %u, dropped %u", delivered, dropped);
    NTEST_ASSERT_EQ_UINT(delivered + dropped, 8U * s.records_each);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.out_of_order, NPH_MEMORY_ORDER_RELAXED), 0U);
    nph_log_shutdown();
}
