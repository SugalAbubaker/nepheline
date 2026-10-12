// SPDX-License-Identifier: Apache-2.0
// The job system: workers, fibers, counters, and queues (docs/jobs.md,
// docs/adr/0013-job-system.md).
//
// Thread-local hazard (ADR-0010 §4): a fiber may resume on another thread,
// so the worker a piece of code runs on is re-read through the non-inlined
// current_worker() after every fiber switch and never kept in a local
// across one. The functions that complete a switch (after_switch) are
// non-inlined for the same reason.
#include <nepheline/core/assert.h>
#include <nepheline/core/ring.h>
#include <nepheline/jobs/deque.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/fiber.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include <stdio.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_JOBS, "jobs", NPH_LOG_LEVEL_INFO);

// Defaults (jobs.h documents them).
#define JOB_DEFAULT_IO_THREADS 1U
#define JOB_DEFAULT_FIBERS     128U
#define JOB_DEFAULT_STACK_SIZE NPH_KIB(64)
#define JOB_DEFAULT_CAPACITY   16384U
#define JOB_DEFAULT_DEQUE      2048U
#define JOB_DEFAULT_EXT_SLOTS  16U
#define JOB_MIN_CAPACITY       64U
#define JOB_MIN_DEQUE          16U

// Each worker keeps a few free job slots to itself so the shared free
// list is touched once per JOB_CACHE_REFILL allocations rather than once
// per job (the "magazine" of Bonwick & Adams, USENIX 2001).
#define JOB_CACHE_SIZE   32U
#define JOB_CACHE_REFILL 16U

// Vyukov's queue reports "empty" while the oldest cell is being written by
// a concurrent push; a pop that fails is retried this often before the
// pool is declared exhausted.
#define JOB_POP_RETRIES 64U

// A thief that loses the compare-exchange race retries the same victim
// this often before moving on.
#define JOB_STEAL_RETRIES 4U

// Queue elements pack a job index with the job's depth in the spawn tree
// (the main thread is at depth 0, a job it spawns at 1, and so on,
// saturating). The depth is what a wait that has no fiber to park on
// uses to decide which queued jobs it may run on its own stack: only
// deeper ones (see wait_inline). Keeping it in the element means the
// decision reads no job record, which another thread may be reusing.
#define JOB_INDEX_BITS   20U
#define JOB_INDEX_MASK   ((1U << JOB_INDEX_BITS) - 1U)
#define JOB_DEPTH_MAX    ((1U << (32U - JOB_INDEX_BITS)) - 1U)
#define JOB_MAX_CAPACITY (1U << JOB_INDEX_BITS)

// Nested inline waits consume the waiting fiber's stack. The depth rule
// bounds the nesting by the spawn tree's depth, so this is a backstop:
// when less than this much stack is left, a wait polls instead of
// running anything (at least this, or a quarter of the stack size).
#define JOB_INLINE_STACK_RESERVE_MIN NPH_KIB(12)

// Polling backoff for an external waiter that found no semaphore slot.
#define JOB_POLL_SPINS    64U
#define JOB_POLL_YIELDS   256U
#define JOB_POLL_SLEEP_NS (50U * NPH_TIME_NS_PER_US)

// A wait that can neither park nor run anything sleeps on its worker's
// semaphore, registered as a waiter so that its counter wakes it at once,
// and wakes by itself this often to look for a fiber or a deeper job.
#define JOB_HOSTAGE_WAIT_NS (500U * NPH_TIME_NS_PER_US)

#define JOB_KIND_SINGLE 0U
#define JOB_KIND_RANGE  1U

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

// One job slot in the pool. A cache line each, so two workers filling
// adjacent slots never share a line.
typedef struct NphJobRecord {
    // The alignment sits on a scalar member: MSVC applies __declspec(align)
    // on an anonymous union to the union type, which would pad it to 64.
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) void *user;
    union {
        NphJobFn single;
        NphJobRangeFn range;
    } fn;
    NphJobCounter *counter;
    const char *name;
    u32 begin; // range jobs; also the chain link while a batch is being reserved
    u32 end;
    u32 batch;
    u8 kind;
    u8 priority;
} NphJobRecord;

NPH_STATIC_ASSERT(sizeof(NphJobRecord) == NPH_CACHE_LINE_SIZE, "a job record is one cache line");

// A waiter on a counter: a parked fiber or a blocked thread. Lives on the
// waiter's stack for the duration of the wait; linked into the wait
// bucket its counter's address hashes to, under that bucket's lock.
typedef struct NphJobWaitNode {
    struct NphJobWaitNode *next;
    NphFiber *fiber;   // a parked job fiber, or NULL
    NphSemaphore *sem; // a blocked thread's semaphore, or NULL
    NphJobCounter *counter;
    u32 target;
} NphJobWaitNode;

// Wait lists live here rather than in the counters (jobs.h on
// NphJobCounter): a signaller that found waiters registered when it
// decremented walks the bucket comparing counter addresses, and so never
// dereferences a counter that a woken waiter may already have released.
// A bucket is shared by every counter that hashes to it; the walk skips
// the others' nodes.
#define JOB_WAIT_BUCKET_BITS 6U
#define JOB_WAIT_BUCKETS     (1U << JOB_WAIT_BUCKET_BITS)

typedef struct NphJobWaitBucket {
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphSpinLock lock;
    NphJobWaitNode *head;
} NphJobWaitBucket;

// The counter word: value in the low half, registered waiters in the high.
#define JOB_COUNTER_WAITER_ONE (1ULL << 32U)

static NPH_FORCE_INLINE u32 counter_value_of(u64 state)
{
    return (u32)state;
}

static NPH_FORCE_INLINE u32 counter_waiters_of(u64 state)
{
    return (u32)(state >> 32U);
}

typedef struct NphJobWorker {
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphJobSystem *js;
    u32 index;
    u32 rng; // xorshift32 state for victim selection (Marsaglia, 2003)
    NphThread thread;
    NphSemaphore sem;             // a wait without a fiber sleeps here (wait_inline)
    NphFiber thread_fiber;        // the thread's own context: used at start and shutdown only
    NphFiber *current_fiber;      // the pool fiber running on this thread
    NphJobWaitNode *pending_wait; // set before a switch; published by the fiber switched to
    NphFiber *fiber_to_free;      // set before a switch; released by the fiber switched to
    u32 job_cache[JOB_CACHE_SIZE];
    u32 job_cache_count;
    NphJobDeque deques[NPH_JOB_PRIORITY_COUNT];
    // Statistics: written by the owning thread only, with relaxed atomics
    // so the reader in nph_job_system_get_stats is race-free.
    NphAtomicU64 jobs_executed;
    NphAtomicU64 jobs_stolen;
    NphAtomicU64 jobs_nested;
    NphAtomicU64 waits_switched;
    NphAtomicU64 waits_inline;
    NphAtomicU64 fiber_switches;
    NphAtomicU64 parks;
} NphJobWorker;

typedef struct NphJobIoThread {
    NphJobSystem *js;
    NphThread thread;
    u32 index;
} NphJobIoThread;

struct NphJobSystem {
    NphAllocator *alloc;
    u32 worker_count;
    u32 io_thread_count;
    u32 fiber_count;
    u32 job_capacity;
    u32 deque_capacity;
    u32 ext_slot_count;
    u64 fiber_stack_size;
    u64 inline_stack_reserve;
    b32 deterministic;
    b32 pinned;

    NphJobWorker *workers;
    NphJobIoThread *io_threads;
    NphFiber *fibers;
    u32 *fiber_depths; // per fiber: the depth of the job running on it (0 between jobs)
    NphJobRecord *jobs;
    NphSemaphore *ext_sems;
    NphJobWaitBucket *wait_buckets; // JOB_WAIT_BUCKETS of them

    // Progress of creation, for releasing a partially built system.
    u32 fibers_created;
    u32 ext_sems_created;
    u32 worker_sems_created;
    u32 workers_started;
    u32 io_threads_started;
    b32 wake_created;
    b32 io_wake_created;

    NphMpmcRing free_jobs;    // u32 job indices
    NphMpmcRing free_fibers;  // u32 fiber indices
    NphMpmcRing ready_fibers; // u32 fiber indices whose wait completed
    // Queue elements are packed index+depth (job_pack).
    NphMpmcRing inject[NPH_JOB_PRIORITY_COUNT]; // from threads the system does not own: depth 1
    NphMpmcRing spill[NPH_JOB_PRIORITY_COUNT];  // from workers whose deque was full
    NphMpmcRing io_queue;                       // for the I/O threads
    NphMpmcRing free_ext_slots;                 // u32 indices into ext_sems

    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphSemaphore wake; // workers park here
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 idle_workers;
    NPH_ALIGNAS(NPH_CACHE_LINE_SIZE) NphAtomicU32 stop;
    NphSemaphore io_wake; // one permit per queued I/O job
    NphAtomicU32 inline_wait_warned;
    NphAtomicU32 stack_limit_warned;
    NphAtomicU64 jobs_io_executed;
    NphAtomicU64 waits_external;
};

// ---------------------------------------------------------------------------
// Thread-local state. Only reached through non-inlined functions: a fiber
// may continue on another thread after a switch, and a thread-local
// address computed before the switch would belong to the old thread.
// ---------------------------------------------------------------------------

static NPH_THREAD_LOCAL NphJobWorker *t_worker;
static NPH_THREAD_LOCAL b32 t_io_thread;

static NPH_NO_INLINE NphJobWorker *current_worker(void)
{
    return t_worker;
}

static NPH_NO_INLINE void set_current_worker(NphJobWorker *w)
{
    t_worker = w;
}

static NPH_NO_INLINE void set_io_thread(b32 value)
{
    t_io_thread = value;
}

static NPH_NO_INLINE b32 is_io_thread(void)
{
    return t_io_thread;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Owner-only statistics counter: a relaxed load and store, no RMW.
static NPH_FORCE_INLINE void stat_inc(NphAtomicU64 *s)
{
    nph_atomic_store_u64(s, nph_atomic_load_u64(s, NPH_MEMORY_ORDER_RELAXED) + 1U,
                         NPH_MEMORY_ORDER_RELAXED);
}

static NPH_FORCE_INLINE u32 fiber_index(const NphJobSystem *js, const NphFiber *f)
{
    return (u32)(f - js->fibers);
}

static NPH_FORCE_INLINE u32 rng_next(u32 *state)
{
    u32 x = *state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

static NPH_FORCE_INLINE u32 job_pack(u32 index, u32 depth)
{
    return (depth << JOB_INDEX_BITS) | index;
}

static NPH_FORCE_INLINE u32 job_index_of(u32 elem)
{
    return elem & JOB_INDEX_MASK;
}

static NPH_FORCE_INLINE u32 job_depth_of(u32 elem)
{
    return elem >> JOB_INDEX_BITS;
}

// The depth of the job running on the calling thread, or 0 off the
// workers: I/O threads and foreign threads spawn at depth 1.
static u32 current_depth(const NphJobSystem *js)
{
    const NphJobWorker *w = current_worker();
    return w != NULL ? js->fiber_depths[fiber_index(js, w->current_fiber)] : 0U;
}

static u32 child_depth(u32 depth)
{
    return depth < JOB_DEPTH_MAX ? depth + 1U : JOB_DEPTH_MAX;
}

// For take_if and steal_if: only jobs below the waiting one in the spawn tree.
static bool deeper_than(u32 elem, void *ctx)
{
    return job_depth_of(elem) > *(const u32 *)ctx;
}

// Pushes into a ring that cannot stay full (every value it can hold
// exists exactly once), spinning through the transient full state a
// concurrent pop leaves behind.
static void ring_push_spin(NphMpmcRing *ring, u32 value)
{
    while (!nph_mpmc_ring_push(ring, &value)) {
        nph_cpu_relax();
    }
}

static bool ring_pop_retry(NphMpmcRing *ring, u32 *out)
{
    for (u32 i = 0; i < JOB_POP_RETRIES; ++i) {
        if (nph_mpmc_ring_pop(ring, out)) {
            return true;
        }
        nph_cpu_relax();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Job pool
// ---------------------------------------------------------------------------

// Takes a free slot. `w` is the calling thread's worker (NULL elsewhere);
// its cache is only ever touched from its own thread.
static bool alloc_job(NphJobSystem *js, NphJobWorker *w, u32 *out)
{
    if (w == NULL) {
        return ring_pop_retry(&js->free_jobs, out);
    }
    if (w->job_cache_count == 0U) {
        u32 n = 0U;
        while (n < JOB_CACHE_REFILL && ring_pop_retry(&js->free_jobs, &w->job_cache[n])) {
            ++n;
        }
        w->job_cache_count = n;
        if (n == 0U) {
            return false;
        }
    }
    *out = w->job_cache[--w->job_cache_count];
    return true;
}

static void free_job(NphJobSystem *js, NphJobWorker *w, u32 index)
{
    if (w == NULL) {
        ring_push_spin(&js->free_jobs, index);
        return;
    }
    if (w->job_cache_count == JOB_CACHE_SIZE) {
        for (u32 i = 0; i < JOB_CACHE_REFILL; ++i) {
            ring_push_spin(&js->free_jobs, w->job_cache[--w->job_cache_count]);
        }
    }
    w->job_cache[w->job_cache_count++] = index;
}

static void flush_job_cache(NphJobSystem *js, NphJobWorker *w)
{
    while (w->job_cache_count != 0U) {
        ring_push_spin(&js->free_jobs, w->job_cache[--w->job_cache_count]);
    }
}

// ---------------------------------------------------------------------------
// Waking workers
// ---------------------------------------------------------------------------

// Wakes up to `n` parked workers. The caller has just made work visible
// (a release store or an RMW); the seq_cst fence orders that before the
// load of the idle count so that a worker announcing itself idle after a
// final scan and a producer publishing work cannot both miss each other
// (the store-buffering pattern, docs/adr/0013 §4).
static void wake_workers(NphJobSystem *js, u32 n)
{
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_SEQ_CST);
    const u32 idle = nph_atomic_load_u32(&js->idle_workers, NPH_MEMORY_ORDER_SEQ_CST);
    if (idle != 0U) {
        nph_semaphore_post_n(&js->wake, n < idle ? n : idle);
    }
}

// ---------------------------------------------------------------------------
// Queues
// ---------------------------------------------------------------------------

// Queues a packed job element. On a worker it goes to that worker's deque,
// or to the spill queue when the deque is full; elsewhere to the injection
// queue. The two shared queues are kept apart because an inline wait
// scans the spill queue for deeper jobs and never the injection queue,
// whose jobs are all at depth 1. Does not wake anyone; callers batch the
// wake.
static void push_job(NphJobSystem *js, NphJobWorker *w, u32 elem, u32 priority)
{
    if (w == NULL) {
        ring_push_spin(&js->inject[priority], elem);
    } else if (!nph_job_deque_push(&w->deques[priority], elem)) {
        ring_push_spin(&js->spill[priority], elem);
    }
}

// Steals from the other workers, highest priority first. With a
// predicate, a victim whose oldest job the predicate declines is skipped
// like an empty one.
static bool steal_job(NphJobSystem *js, NphJobWorker *w, NphJobDequePred pred, void *ctx, u32 *out)
{
    const u32 n = js->worker_count;
    if (n < 2U) {
        return false;
    }
    // A random starting victim spreads thieves across the workers
    // (Blumofe & Leiserson, "Scheduling Multithreaded Computations by
    // Work Stealing", FOCS 1994). Multiply-shift instead of a division.
    const u32 start = (u32)(((u64)rng_next(&w->rng) * n) >> 32U);
    for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
        for (u32 i = 0; i < n; ++i) {
            u32 v = start + i;
            if (v >= n) {
                v -= n;
            }
            if (v == w->index) {
                continue;
            }
            NphJobDeque *d = &js->workers[v].deques[p];
            for (u32 attempt = 0; attempt < JOB_STEAL_RETRIES; ++attempt) {
                const NphJobDequeSteal r = pred != NULL ? nph_job_deque_steal_if(d, pred, ctx, out)
                                                        : nph_job_deque_steal(d, out);
                if (r == NPH_JOB_DEQUE_STOLEN) {
                    stat_inc(&w->jobs_stolen);
                    return true;
                }
                if (r != NPH_JOB_DEQUE_ABORT) {
                    break;
                }
                nph_cpu_relax();
            }
        }
    }
    return false;
}

// Strict priority order: own deque, then the shared queues at each
// priority, then steal, highest priority first.
static bool find_job(NphJobSystem *js, NphJobWorker *w, u32 *out)
{
    for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
        if (nph_job_deque_take(&w->deques[p], out) || nph_mpmc_ring_pop(&js->inject[p], out) ||
            nph_mpmc_ring_pop(&js->spill[p], out)) {
            return true;
        }
    }
    return steal_job(js, w, NULL, NULL, out);
}

// As find_job, restricted to jobs deeper than `depth` in the spawn tree,
// for a wait that must run them on its own stack. The deques are peeked;
// the spill queue has no peek, so a job popped from it that is too
// shallow goes back to its tail, each element examined once per scan.
static bool find_deeper_job(NphJobSystem *js, NphJobWorker *w, u32 depth, u32 *out)
{
    for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
        if (nph_job_deque_take_if(&w->deques[p], deeper_than, &depth, out)) {
            return true;
        }
        u32 n = nph_mpmc_ring_count(&js->spill[p]);
        while (n != 0U && nph_mpmc_ring_pop(&js->spill[p], out)) {
            --n;
            if (job_depth_of(*out) > depth) {
                return true;
            }
            ring_push_spin(&js->spill[p], *out);
            wake_workers(js, 1U); // it was out of every queue for a moment
        }
    }
    return steal_job(js, w, deeper_than, &depth, out);
}

// A cheap look at every queue, for the last check before parking.
static bool work_available(NphJobSystem *js, const NphJobWorker *w)
{
    if (nph_mpmc_ring_count(&js->ready_fibers) != 0U) {
        return true;
    }
    for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
        if (nph_job_deque_count(&w->deques[p]) != 0U || nph_mpmc_ring_count(&js->inject[p]) != 0U ||
            nph_mpmc_ring_count(&js->spill[p]) != 0U) {
            return true;
        }
    }
    for (u32 v = 0; v < js->worker_count; ++v) {
        if (v == w->index) {
            continue;
        }
        for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
            if (nph_job_deque_count(&js->workers[v].deques[p]) != 0U) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fiber switching
// ---------------------------------------------------------------------------

static void publish_wait(NphJobSystem *js, NphJobWaitNode *node);

// Runs on the destination side of every switch, on the thread that now
// runs the fiber: releases the fiber the switcher left behind and
// publishes the wait the switcher registered. Non-inlined so the
// thread-local worker is the current thread's.
static NPH_NO_INLINE void after_switch(void)
{
    NphJobWorker *w = current_worker();
    NphFiber *f = w->fiber_to_free;
    if (f != NULL) {
        w->fiber_to_free = NULL;
        ring_push_spin(&w->js->free_fibers, fiber_index(w->js, f));
    }
    NphJobWaitNode *node = w->pending_wait;
    if (node != NULL) {
        w->pending_wait = NULL;
        publish_wait(w->js, node);
    }
}

// Switches this worker thread from its current fiber to `to`. With
// `free_current`, the fiber being left holds no job state (it is a loop
// fiber between jobs) and returns to the pool once it is off the thread;
// otherwise the caller has set a pending wait for it. `w` is stale after
// this returns: the fiber may be on another thread.
static void switch_fiber(NphJobWorker *w, NphFiber *to, bool free_current)
{
    NphFiber *from = w->current_fiber;
    NPH_ASSERT(from != to);
    if (free_current) {
        w->fiber_to_free = from;
    }
    w->current_fiber = to;
    stat_inc(&w->fiber_switches);
    nph_fiber_switch(from, to);
    after_switch();
}

static bool take_next_fiber(NphJobSystem *js, u32 *out)
{
    // A resumed fiber first: it holds work in progress and frees a slot.
    return nph_mpmc_ring_pop(&js->ready_fibers, out) || nph_mpmc_ring_pop(&js->free_fibers, out);
}

// ---------------------------------------------------------------------------
// Counters and waiting
// ---------------------------------------------------------------------------

static NphJobWaitBucket *wait_bucket(NphJobSystem *js, const NphJobCounter *c)
{
    // Fibonacci hashing of the address (Knuth, TAOCP vol. 3, §6.4).
    const u64 h = (u64)(uptr)c * 0x9E3779B97F4A7C15ULL;
    return &js->wait_buckets[h >> (64U - JOB_WAIT_BUCKET_BITS)];
}

// Links `node` into its counter's bucket and registers it in the counter
// word, whose returned value says whether the target is already met: if
// so the node is unlinked and unregistered again and this returns false.
// Registration and the signaller's decrement are read-modify-writes of
// the same word, so exactly one of them sees the other: a signaller
// whose decrement came first is reflected in the value returned here,
// and one whose decrement came later sees the waiter and walks the
// bucket, where the node already is (ADR-0013 §4).
static bool register_wait(NphJobSystem *js, NphJobWaitNode *node)
{
    NphJobCounter *c = node->counter;
    NphJobWaitBucket *b = wait_bucket(js, c);
    nph_spin_lock_acquire(&b->lock);
    node->next = b->head;
    b->head = node;
    const u64 old =
        nph_atomic_fetch_add_u64(&c->state, JOB_COUNTER_WAITER_ONE, NPH_MEMORY_ORDER_SEQ_CST);
    if (counter_value_of(old) <= node->target) {
        b->head = node->next; // still at the head: the lock is held
        nph_atomic_fetch_sub_u64(&c->state, JOB_COUNTER_WAITER_ONE, NPH_MEMORY_ORDER_SEQ_CST);
        nph_spin_lock_release(&b->lock);
        return false;
    }
    nph_spin_lock_release(&b->lock);
    return true;
}

static void make_ready(NphJobSystem *js, NphFiber *f)
{
    ring_push_spin(&js->ready_fibers, fiber_index(js, f));
    wake_workers(js, 1U);
}

// The second half of a worker's wait, run by the fiber that replaced the
// waiting one, so the waiter's fiber is off its thread before anyone can
// resume it.
static void publish_wait(NphJobSystem *js, NphJobWaitNode *node)
{
    if (!register_wait(js, node)) {
        make_ready(js, node->fiber);
    }
}

// Withdraws a registered node. Returns false when it is no longer in the
// bucket: a signaller has unlinked and unregistered it and is about to
// wake, or has woken, its owner, who must then take that wake-up.
static bool cancel_wait(NphJobSystem *js, NphJobWaitNode *node)
{
    NphJobCounter *c = node->counter;
    NphJobWaitBucket *b = wait_bucket(js, c);
    nph_spin_lock_acquire(&b->lock);
    for (NphJobWaitNode **link = &b->head; *link != NULL; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            nph_atomic_fetch_sub_u64(&c->state, JOB_COUNTER_WAITER_ONE, NPH_MEMORY_ORDER_SEQ_CST);
            nph_spin_lock_release(&b->lock);
            return true;
        }
    }
    nph_spin_lock_release(&b->lock);
    return false;
}

// Unlinks every waiter on `c` whose target `value` (the result of the
// caller's decrement) meets, and unregisters them from the counter word
// before the lock is released: the counter is still alive here, since no
// waiter has been woken yet, and once the lock is released nothing
// touches it again.
static NphJobWaitNode *take_satisfied_waiters(NphJobSystem *js, NphJobCounter *c, u32 value)
{
    NphJobWaitNode *woken = NULL;
    u32 count = 0U;
    NphJobWaitBucket *b = wait_bucket(js, c);
    nph_spin_lock_acquire(&b->lock);
    NphJobWaitNode **link = &b->head;
    while (*link != NULL) {
        NphJobWaitNode *node = *link;
        if (node->counter == c && value <= node->target) {
            *link = node->next;
            node->next = woken;
            woken = node;
            ++count;
        } else {
            link = &node->next;
        }
    }
    if (count != 0U) {
        nph_atomic_fetch_sub_u64(&c->state, (u64)count * JOB_COUNTER_WAITER_ONE,
                                 NPH_MEMORY_ORDER_SEQ_CST);
    }
    nph_spin_lock_release(&b->lock);
    return woken;
}

// Resumes a list of satisfied waiters. In scheduler context (between jobs
// on a worker) the first parked fiber is switched to directly: one switch,
// and the work continues on the core whose cache holds its data. Anything
// else goes through the ready ring.
static void resume_waiters(NphJobSystem *js, NphJobWaitNode *list, bool scheduler_context)
{
    NphJobWorker *w = scheduler_context ? current_worker() : NULL;
    NphFiber *immediate = NULL;
    NphJobWaitNode *node = list;
    while (node != NULL) {
        // Read the link first: once its owner is woken the node, which
        // lives on the owner's stack, may be gone.
        NphJobWaitNode *next = node->next;
        if (node->sem != NULL) {
            nph_semaphore_post(node->sem);
        } else if (w != NULL && immediate == NULL) {
            immediate = node->fiber;
        } else {
            make_ready(js, node->fiber);
        }
        node = next;
    }
    if (immediate != NULL) {
        switch_fiber(w, immediate, true);
    }
}

// One read-modify-write; the counter is not touched again unless it had
// waiters registered at that instant, in which case they, not the
// signaller, are what keeps it alive until they are woken.
static void signal_counter(NphJobSystem *js, NphJobCounter *c, bool scheduler_context)
{
    const u64 old = nph_atomic_fetch_sub_u64(&c->state, 1U, NPH_MEMORY_ORDER_SEQ_CST);
    NPH_ASSERT_MSG(counter_value_of(old) != 0U, "signalling a job counter that is already zero");
    if (counter_waiters_of(old) == 0U) {
        return;
    }
    NphJobWaitNode *woken = take_satisfied_waiters(js, c, counter_value_of(old) - 1U);
    if (woken != NULL) {
        resume_waiters(js, woken, scheduler_context);
    }
}

void nph_job_counter_init(NphJobCounter *counter)
{
    NPH_ASSERT(counter != NULL);
    nph_atomic_store_u64(&counter->state, 0U, NPH_MEMORY_ORDER_RELEASE);
}

u32 nph_job_counter_value(const NphJobCounter *counter)
{
    NPH_ASSERT(counter != NULL);
    return counter_value_of(nph_atomic_load_u64(&counter->state, NPH_MEMORY_ORDER_ACQUIRE));
}

void nph_job_counter_add(NphJobCounter *counter, u32 n)
{
    NPH_ASSERT(counter != NULL);
    if (n != 0U) {
        const u64 old = nph_atomic_fetch_add_u64(&counter->state, n, NPH_MEMORY_ORDER_SEQ_CST);
        NPH_ASSERT_MSG(counter_value_of(old) <= NPH_U32_MAX - n, "job counter value overflow");
        NPH_UNUSED(old);
    }
}

void nph_job_counter_signal(NphJobSystem *js, NphJobCounter *counter)
{
    NPH_ASSERT(js != NULL && counter != NULL);
    // Never switch away from the caller: it may be inside a job.
    signal_counter(js, counter, false);
}

// ---------------------------------------------------------------------------
// Executing jobs
// ---------------------------------------------------------------------------

// Processes a range job: halves the range, queueing the upper half, until
// the piece fits the batch, then runs the function on the piece it kept.
// The worker runs no user code while splitting, so `w` stays valid.
// The split pieces are the same job at the same depth: they are siblings
// of the piece that spawned them, not children.
static void run_range(NphJobSystem *js, NphJobWorker *w, const NphJobRecord *r, u32 depth)
{
    u32 begin = r->begin;
    u32 end = r->end;
    while (end - begin > r->batch) {
        const u32 mid = begin + (end - begin) / 2U;
        u32 index;
        if (!alloc_job(js, w, &index)) {
            break; // pool exhausted: finish the rest inline
        }
        NphJobRecord *child = &js->jobs[index];
        *child = *r;
        child->begin = mid;
        child->end = end;
        if (r->counter != NULL) {
            nph_job_counter_add(r->counter, 1U);
        }
        push_job(js, w, job_pack(index, depth), r->priority);
        wake_workers(js, 1U);
        end = mid;
    }
    r->fn.range(r->user, begin, end);
}

// Runs one job on a worker. `scheduler_context` is true between jobs in
// the worker loop, where the current fiber holds no state and may be
// handed to a woken waiter; false when helping inline from inside a wait.
static void execute_job(NphJobSystem *js, u32 elem, bool scheduler_context)
{
    NphJobWorker *w = current_worker();
    const u32 index = job_index_of(elem);
    const u32 depth = job_depth_of(elem);
    const NphJobRecord r = js->jobs[index];
    free_job(js, w, index); // before running: the job may queue more
    // The fiber's depth slot is read by spawns and waits inside the job.
    // Saved and restored here, on the fiber's own stack, so nesting and
    // migration both work out: after the job returns, whichever thread
    // this is, the current fiber is still the one the job ran on.
    u32 *depth_slot = &js->fiber_depths[fiber_index(js, w->current_fiber)];
    const u32 outer_depth = *depth_slot;
    *depth_slot = depth;
    if (r.kind == JOB_KIND_RANGE) {
        run_range(js, w, &r, depth);
    } else {
        r.fn.single(r.user);
    }
    *depth_slot = outer_depth;
    // The job may have waited and come back on another thread.
    w = current_worker();
    stat_inc(&w->jobs_executed);
    if (!scheduler_context) {
        stat_inc(&w->jobs_nested);
    }
    if (r.counter != NULL) {
        signal_counter(js, r.counter, scheduler_context);
    }
}

static void execute_io_job(NphJobSystem *js, u32 elem)
{
    const u32 index = job_index_of(elem);
    const NphJobRecord r = js->jobs[index];
    free_job(js, NULL, index);
    if (r.kind == JOB_KIND_RANGE) {
        r.fn.range(r.user, r.begin, r.end);
    } else {
        r.fn.single(r.user);
    }
    nph_atomic_fetch_add_u64(&js->jobs_io_executed, 1U, NPH_MEMORY_ORDER_RELAXED);
    if (r.counter != NULL) {
        signal_counter(js, r.counter, false);
    }
}

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------

// Bytes of the running pool fiber's stack below the current frame.
static NPH_NO_INLINE u64 stack_remaining(const NphFiber *f)
{
    volatile u8 marker = 0U;
    const uptr here = (uptr)&marker;
    const uptr lo = (uptr)f->stack_lo;
    return here > lo ? (u64)(here - lo) : 0U;
}

static void poll_backoff(u32 *spins)
{
    if (++*spins < JOB_POLL_SPINS) {
        nph_cpu_relax();
    } else if (*spins < JOB_POLL_YIELDS) {
        nph_thread_yield();
    } else {
        nph_thread_sleep_ns(JOB_POLL_SLEEP_NS);
    }
}

// No fiber to park on: the wait stays on this fiber and keeps the worker
// useful by running queued jobs on its own stack, moving to a fiber as
// soon as one is ready or free. Jobs run here are not in scheduler
// context (this fiber holds the waiting job's frames).
//
// Only jobs deeper than the waiting one in the spawn tree are run (the
// depth-restricted stealing of early Intel TBB, which bounded stack use
// the same way). Each nested wait is then strictly deeper than the one
// below it on the stack, so the nesting is at most the tree's depth: the
// stack a serial run of the same code would need. It also keeps the
// system live when every fiber is parked: the deepest job in progress
// anywhere is running (its children, if any, would be deeper and
// runnable by it), parked on something outside the system (an I/O
// completion, a signal from the main thread), or ready, and ready fibers
// are picked up here. Without the restriction a wait could nest a job
// that waits on the very fiber it is parked on top of, and with every
// worker in that state nothing would run (docs/jobs.md "Fiber
// exhaustion").
//
// With nothing to run, the wait registers itself like an external
// waiter, on its worker's semaphore, so its counter wakes it at once
// rather than when a poll happens to look (a counter pulsed and re-armed
// by another thread would otherwise never be seen at zero), and sleeps
// with a timeout after which it withdraws the registration and looks for
// a fiber or a job again. Spinning here at the workers' high priority
// starved the main thread of the random-mix stress test.
static void wait_inline(NphJobSystem *js, NphJobCounter *c, u32 target, NphJobWaitNode *node,
                        u32 depth)
{
    if (nph_atomic_exchange_u32(&js->inline_wait_warned, 1U, NPH_MEMORY_ORDER_RELAXED) == 0U) {
        NPH_LOG_WARN(&NPH_LOG_JOBS,
                     "all %u fibers are parked; a wait is running jobs inline (raise fiber_count)",
                     js->fiber_count);
    }
    for (;;) {
        if (nph_job_counter_value(c) <= target) {
            return;
        }
        NphJobWorker *w = current_worker();
        u32 index;
        if (take_next_fiber(js, &index)) {
            node->fiber = w->current_fiber;
            node->sem = NULL;
            w->pending_wait = node;
            stat_inc(&w->waits_switched);
            switch_fiber(w, &js->fibers[index], false);
            return;
        }
        if (stack_remaining(w->current_fiber) > js->inline_stack_reserve) {
            u32 elem;
            if (find_deeper_job(js, w, depth, &elem)) {
                execute_job(js, elem, false);
                continue;
            }
        } else if (nph_atomic_exchange_u32(&js->stack_limit_warned, 1U, NPH_MEMORY_ORDER_RELAXED) ==
                   0U) {
            NPH_LOG_ERROR(&NPH_LOG_JOBS,
                          "nested inline waits reached the fiber stack reserve; the worker is "
                          "sleeping (raise fiber_count or fiber_stack_size)");
        }
        node->fiber = NULL;
        node->sem = &w->sem;
        if (!register_wait(js, node)) {
            return;
        }
        if (nph_semaphore_wait_timeout(&w->sem, JOB_HOSTAGE_WAIT_NS)) {
            return; // posted exactly once, by whoever unlinked the node
        }
        if (!cancel_wait(js, node)) {
            nph_semaphore_wait(&w->sem); // unlinked in the meantime: its post is on the way
            return;
        }
    }
}

// Blocks a thread the system does not schedule (the main thread, an I/O
// thread) until the counter meets the target.
static void wait_external(NphJobSystem *js, NphJobCounter *c, u32 target)
{
    nph_atomic_fetch_add_u64(&js->waits_external, 1U, NPH_MEMORY_ORDER_RELAXED);
    u32 slot;
    if (!ring_pop_retry(&js->free_ext_slots, &slot)) {
        // Every slot is taken: poll. Documented as the fallback for more
        // simultaneous external waiters than the description allowed for.
        u32 spins = 0U;
        while (nph_job_counter_value(c) > target) {
            poll_backoff(&spins);
        }
        return;
    }
    NphJobWaitNode node;
    memset(&node, 0, sizeof node);
    node.sem = &js->ext_sems[slot];
    node.counter = c;
    node.target = target;
    if (register_wait(js, &node)) {
        nph_semaphore_wait(node.sem); // posted exactly once, by whoever unlinks the node
    }
    ring_push_spin(&js->free_ext_slots, slot);
}

void nph_job_wait_value(NphJobSystem *js, NphJobCounter *counter, u32 target)
{
    NPH_ASSERT(js != NULL && counter != NULL);
    if (nph_job_counter_value(counter) <= target) {
        return;
    }
    NphJobWorker *w = current_worker();
    if (w == NULL) {
        wait_external(js, counter, target);
        return;
    }
    NphJobWaitNode node;
    memset(&node, 0, sizeof node);
    node.counter = counter;
    node.target = target;
    u32 index;
    if (!take_next_fiber(js, &index)) {
        stat_inc(&w->waits_inline);
        wait_inline(js, counter, target, &node,
                    js->fiber_depths[fiber_index(js, w->current_fiber)]);
        return;
    }
    node.fiber = w->current_fiber;
    w->pending_wait = &node;
    stat_inc(&w->waits_switched);
    // The fiber switched to publishes `node` once this one is off the
    // thread; whoever meets the counter switches back here. The resumer's
    // fiber has been returned to the pool by after_switch by now.
    switch_fiber(w, &js->fibers[index], false);
}

void nph_job_wait(NphJobSystem *js, NphJobCounter *counter)
{
    nph_job_wait_value(js, counter, 0U);
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

static void park_worker(NphJobSystem *js, NphJobWorker *w)
{
    // Announce first, then look once more: a producer that sees the
    // announcement posts the semaphore; one that does not has published
    // its work before this scan (wake_workers).
    nph_atomic_fetch_add_u32(&js->idle_workers, 1U, NPH_MEMORY_ORDER_SEQ_CST);
    if (nph_atomic_load_u32(&js->stop, NPH_MEMORY_ORDER_SEQ_CST) == 0U && !work_available(js, w)) {
        stat_inc(&w->parks);
        nph_semaphore_wait(&js->wake);
    }
    nph_atomic_fetch_sub_u32(&js->idle_workers, 1U, NPH_MEMORY_ORDER_SEQ_CST);
}

// The loop every pool fiber runs. It never returns: at shutdown the fiber
// hands the thread back to its own context.
static void worker_loop(NphJobSystem *js)
{
    for (;;) {
        NphJobWorker *w = current_worker();
        if (nph_atomic_load_u32(&js->stop, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
            switch_fiber(w, &w->thread_fiber, true);
            continue; // not reached: nothing switches to a pool fiber after stop
        }
        u32 elem;
        if (nph_mpmc_ring_pop(&js->ready_fibers, &elem)) {
            switch_fiber(w, &js->fibers[elem], true);
            continue;
        }
        if (find_job(js, w, &elem)) {
            execute_job(js, elem, true);
            continue;
        }
        park_worker(js, w);
    }
}

static void job_fiber_main(void *user)
{
    after_switch(); // the switch that started this fiber may have left work
    worker_loop((NphJobSystem *)user);
}

static void worker_thread_main(void *user)
{
    NphJobWorker *w = (NphJobWorker *)user;
    NphJobSystem *js = w->js;
    set_current_worker(w);
    nph_fiber_init_thread(&w->thread_fiber);
    u32 index;
    while (!nph_mpmc_ring_pop(&js->free_fibers, &index)) {
        nph_cpu_relax(); // fiber_count > worker_count, so one is always there
    }
    w->current_fiber = &js->fibers[index];
    nph_fiber_switch(&w->thread_fiber, w->current_fiber);
    // Back on the thread's own stack: the system is stopping. The thread
    // fiber is never migrated, so `w` is still this thread's worker.
    after_switch();
    nph_fiber_release_thread(&w->thread_fiber);
    flush_job_cache(js, w);
    set_current_worker(NULL);
}

static void io_thread_main(void *user)
{
    NphJobIoThread *t = (NphJobIoThread *)user;
    NphJobSystem *js = t->js;
    set_io_thread(1);
    for (;;) {
        nph_semaphore_wait(&js->io_wake); // one permit per job, plus one per thread at stop
        u32 elem;
        while (!nph_mpmc_ring_pop(&js->io_queue, &elem)) {
            if (nph_atomic_load_u32(&js->stop, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
                set_io_thread(0);
                return;
            }
            nph_cpu_relax(); // the permit's job is still being pushed
        }
        execute_io_job(js, elem);
    }
}

// ---------------------------------------------------------------------------
// Running jobs
// ---------------------------------------------------------------------------

// Reserves `count` slots, chained through their `begin` fields in
// declaration order, or none at all. Returns the first index.
static bool reserve_jobs(NphJobSystem *js, NphJobWorker *w, u32 count, u32 *out_head)
{
    u32 head = NPH_U32_MAX;
    for (u32 i = 0; i < count; ++i) {
        u32 index;
        if (!alloc_job(js, w, &index)) {
            while (head != NPH_U32_MAX) {
                const u32 next = js->jobs[head].begin;
                free_job(js, w, head);
                head = next;
            }
            return false;
        }
        js->jobs[index].begin = head;
        head = index;
    }
    // Allocated last-first; reverse so the walk follows declaration order.
    u32 prev = NPH_U32_MAX;
    while (head != NPH_U32_MAX) {
        const u32 next = js->jobs[head].begin;
        js->jobs[head].begin = prev;
        prev = head;
        head = next;
    }
    *out_head = prev;
    return true;
}

static NphResult validate_decls(const NphJobDecl *decls, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        if (decls[i].fn == NULL) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    return NPH_RESULT_OK;
}

static void fill_single(NphJobRecord *r, const NphJobDecl *d, NphJobCounter *counter, u32 priority)
{
    r->fn.single = d->fn;
    r->user = d->user;
    r->counter = counter;
    r->name = d->name != NULL ? d->name : "job";
    r->begin = 0U;
    r->end = 0U;
    r->batch = 0U;
    r->kind = JOB_KIND_SINGLE;
    r->priority = (u8)priority;
}

NphResult nph_job_run(NphJobSystem *js, const NphJobDecl *decls, u32 count, NphJobPriority priority,
                      NphJobCounter *counter)
{
    NPH_ASSERT(js != NULL && (decls != NULL || count == 0U));
    if ((u32)priority >= NPH_JOB_PRIORITY_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(validate_decls(decls, count));
    NphJobWorker *w = current_worker();
    u32 index;
    if (!reserve_jobs(js, w, count, &index)) {
        NPH_LOG_WARN(&NPH_LOG_JOBS, "job pool exhausted (%u slots); %u jobs refused",
                     js->job_capacity, count);
        return NPH_RESULT_FULL;
    }
    if (counter != NULL) {
        nph_job_counter_add(counter, count);
    }
    const u32 depth = child_depth(current_depth(js));
    for (u32 i = 0; i < count; ++i) {
        NphJobRecord *r = &js->jobs[index];
        const u32 next = r->begin;
        fill_single(r, &decls[i], counter, (u32)priority);
        push_job(js, w, job_pack(index, depth), (u32)priority);
        index = next;
    }
    wake_workers(js, count);
    return NPH_RESULT_OK;
}

NphResult nph_job_run_io(NphJobSystem *js, const NphJobDecl *decls, u32 count,
                         NphJobCounter *counter)
{
    NPH_ASSERT(js != NULL && (decls != NULL || count == 0U));
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(validate_decls(decls, count));
    NphJobWorker *w = current_worker();
    u32 index;
    if (!reserve_jobs(js, w, count, &index)) {
        NPH_LOG_WARN(&NPH_LOG_JOBS, "job pool exhausted (%u slots); %u I/O jobs refused",
                     js->job_capacity, count);
        return NPH_RESULT_FULL;
    }
    if (counter != NULL) {
        nph_job_counter_add(counter, count);
    }
    const u32 depth = child_depth(current_depth(js));
    for (u32 i = 0; i < count; ++i) {
        NphJobRecord *r = &js->jobs[index];
        const u32 next = r->begin;
        fill_single(r, &decls[i], counter, NPH_JOB_PRIORITY_NORMAL);
        ring_push_spin(&js->io_queue, job_pack(index, depth));
        index = next;
    }
    nph_semaphore_post_n(&js->io_wake, count);
    return NPH_RESULT_OK;
}

NphResult nph_job_parallel_for(NphJobSystem *js, u32 count, u32 batch, NphJobRangeFn fn, void *user,
                               NphJobPriority priority, NphJobCounter *counter)
{
    NPH_ASSERT(js != NULL);
    if (fn == NULL || (u32)priority >= NPH_JOB_PRIORITY_COUNT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    NphJobWorker *w = current_worker();
    u32 index;
    if (!alloc_job(js, w, &index)) {
        NPH_LOG_WARN(&NPH_LOG_JOBS, "job pool exhausted (%u slots); parallel-for refused",
                     js->job_capacity);
        return NPH_RESULT_FULL;
    }
    NphJobRecord *r = &js->jobs[index];
    r->fn.range = fn;
    r->user = user;
    r->counter = counter;
    r->name = "parallel_for";
    r->begin = 0U;
    r->end = count;
    r->batch = batch != 0U ? batch : 1U;
    r->kind = JOB_KIND_RANGE;
    r->priority = (u8)priority;
    if (counter != NULL) {
        nph_job_counter_add(counter, 1U);
    }
    push_job(js, w, job_pack(index, child_depth(current_depth(js))), (u32)priority);
    wake_workers(js, 1U);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

u32 nph_job_worker_index(void)
{
    const NphJobWorker *w = current_worker();
    return w != NULL ? w->index : NPH_JOB_WORKER_NONE;
}

bool nph_job_is_io_thread(void)
{
    return is_io_thread() != 0;
}

u32 nph_job_system_worker_count(const NphJobSystem *js)
{
    NPH_ASSERT(js != NULL);
    return js->worker_count;
}

u32 nph_job_system_io_thread_count(const NphJobSystem *js)
{
    NPH_ASSERT(js != NULL);
    return js->io_thread_count;
}

u32 nph_job_system_fiber_count(const NphJobSystem *js)
{
    NPH_ASSERT(js != NULL);
    return js->fiber_count;
}

void nph_job_system_get_stats(const NphJobSystem *js, NphJobStats *out)
{
    NPH_ASSERT(js != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    for (u32 i = 0; i < js->worker_count; ++i) {
        const NphJobWorker *w = &js->workers[i];
        out->jobs_executed += nph_atomic_load_u64(&w->jobs_executed, NPH_MEMORY_ORDER_RELAXED);
        out->jobs_stolen += nph_atomic_load_u64(&w->jobs_stolen, NPH_MEMORY_ORDER_RELAXED);
        out->jobs_nested += nph_atomic_load_u64(&w->jobs_nested, NPH_MEMORY_ORDER_RELAXED);
        out->waits_switched += nph_atomic_load_u64(&w->waits_switched, NPH_MEMORY_ORDER_RELAXED);
        out->waits_inline += nph_atomic_load_u64(&w->waits_inline, NPH_MEMORY_ORDER_RELAXED);
        out->fiber_switches += nph_atomic_load_u64(&w->fiber_switches, NPH_MEMORY_ORDER_RELAXED);
        out->worker_parks += nph_atomic_load_u64(&w->parks, NPH_MEMORY_ORDER_RELAXED);
    }
    out->jobs_io_executed = nph_atomic_load_u64(&js->jobs_io_executed, NPH_MEMORY_ORDER_RELAXED);
    out->waits_external = nph_atomic_load_u64(&js->waits_external, NPH_MEMORY_ORDER_RELAXED);
}

// Stops the threads and releases everything that exists; usable on a
// partially created system.
static void destroy_internal(NphJobSystem *js)
{
    NphAllocator *alloc = js->alloc;
    nph_atomic_store_u32(&js->stop, 1U, NPH_MEMORY_ORDER_SEQ_CST);
    if (js->wake_created && js->workers_started != 0U) {
        nph_semaphore_post_n(&js->wake, js->workers_started);
    }
    if (js->io_wake_created && js->io_threads_started != 0U) {
        nph_semaphore_post_n(&js->io_wake, js->io_threads_started);
    }
    for (u32 i = 0; i < js->workers_started; ++i) {
        nph_thread_join(&js->workers[i].thread);
    }
    for (u32 i = 0; i < js->io_threads_started; ++i) {
        nph_thread_join(&js->io_threads[i].thread);
    }
    for (u32 i = 0; i < js->fibers_created; ++i) {
        nph_fiber_destroy(&js->fibers[i]);
    }
    if (js->workers != NULL) {
        for (u32 i = 0; i < js->worker_count; ++i) {
            for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
                nph_job_deque_free(&js->workers[i].deques[p]);
            }
        }
    }
    for (u32 i = 0; i < js->ext_sems_created; ++i) {
        nph_semaphore_destroy(&js->ext_sems[i]);
    }
    for (u32 i = 0; i < js->worker_sems_created; ++i) {
        nph_semaphore_destroy(&js->workers[i].sem);
    }
    if (js->wake_created) {
        nph_semaphore_destroy(&js->wake);
    }
    if (js->io_wake_created) {
        nph_semaphore_destroy(&js->io_wake);
    }
    nph_mpmc_ring_free(&js->free_jobs);
    nph_mpmc_ring_free(&js->free_fibers);
    nph_mpmc_ring_free(&js->ready_fibers);
    for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
        nph_mpmc_ring_free(&js->inject[p]);
        nph_mpmc_ring_free(&js->spill[p]);
    }
    nph_mpmc_ring_free(&js->io_queue);
    nph_mpmc_ring_free(&js->free_ext_slots);
    if (js->workers != NULL) {
        NPH_FREE(alloc, js->workers, sizeof(NphJobWorker) * (u64)js->worker_count,
                 NPH_CACHE_LINE_SIZE);
    }
    if (js->io_threads != NULL) {
        NPH_FREE_ARRAY(alloc, NphJobIoThread, js->io_threads, js->io_thread_count);
    }
    if (js->fibers != NULL) {
        NPH_FREE_ARRAY(alloc, NphFiber, js->fibers, js->fiber_count);
    }
    if (js->fiber_depths != NULL) {
        NPH_FREE_ARRAY(alloc, u32, js->fiber_depths, js->fiber_count);
    }
    if (js->jobs != NULL) {
        NPH_FREE(alloc, js->jobs, sizeof(NphJobRecord) * (u64)js->job_capacity,
                 NPH_CACHE_LINE_SIZE);
    }
    if (js->ext_sems != NULL) {
        NPH_FREE_ARRAY(alloc, NphSemaphore, js->ext_sems, js->ext_slot_count);
    }
    if (js->wait_buckets != NULL) {
        NPH_FREE(alloc, js->wait_buckets, sizeof(NphJobWaitBucket) * (u64)JOB_WAIT_BUCKETS,
                 NPH_CACHE_LINE_SIZE);
    }
    NPH_FREE_TYPE(alloc, NphJobSystem, js);
}

static NphResult init_index_ring(NphMpmcRing *ring, NphAllocator *alloc, u32 count, bool fill)
{
    NPH_TRY(
        nph_mpmc_ring_init(ring, alloc, sizeof(u32), nph_next_pow2_u32(count < 2U ? 2U : count)));
    if (fill) {
        for (u32 i = 0; i < count; ++i) {
            const bool pushed = nph_mpmc_ring_push(ring, &i);
            NPH_ASSERT(pushed);
            NPH_UNUSED(pushed);
        }
    }
    return NPH_RESULT_OK;
}

static NphResult create_threads(NphJobSystem *js, const NphJobSystemDesc *desc)
{
    const u32 logical = nph_cpu_logical_count();
    js->pinned = desc->pinning == NPH_JOB_PIN_ALWAYS ||
                 (desc->pinning == NPH_JOB_PIN_AUTO && js->worker_count + 1U <= logical);
    for (u32 i = 0; i < js->io_thread_count; ++i) {
        NphJobIoThread *t = &js->io_threads[i];
        t->js = js;
        t->index = i;
        char name[NPH_THREAD_NAME_MAX];
        snprintf(name, sizeof name, "job-io-%u", i);
        NphThreadDesc td;
        td.fn = io_thread_main;
        td.user = t;
        td.name = name;
        td.stack_size = 0U;
        NPH_TRY(nph_thread_create(&t->thread, &td));
        js->io_threads_started = i + 1U;
    }
    for (u32 i = 0; i < js->worker_count; ++i) {
        NphJobWorker *w = &js->workers[i];
        char name[NPH_THREAD_NAME_MAX];
        snprintf(name, sizeof name, "job-worker-%u", i);
        NphThreadDesc td;
        td.fn = worker_thread_main;
        td.user = w;
        td.name = name;
        td.stack_size = 0U;
        NPH_TRY(nph_thread_create(&w->thread, &td));
        js->workers_started = i + 1U;
        // Workers outrank everything but the audio thread (platform/thread.h).
        // Linux refuses the raise without privileges; that is expected.
        (void)nph_thread_set_priority(&w->thread, NPH_THREAD_PRIORITY_HIGH);
        if (js->pinned && logical != 0U) {
            // Core 0 is left to the main thread; workers take the rest in
            // order, wrapping only when they outnumber the cores.
            const u32 core = (i + 1U) % logical;
            if (core < 64U) {
                (void)nph_thread_set_affinity(&w->thread, 1ULL << core);
            }
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_job_system_create(const NphJobSystemDesc *desc, NphJobSystem **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 logical = nph_cpu_logical_count();
    u32 worker_count = desc->worker_count;
    if (worker_count == 0U) {
        worker_count = logical > 1U ? logical - 1U : 1U;
    }
    if (desc->deterministic) {
        worker_count = 1U;
    }
    const u32 io_thread_count =
        desc->io_thread_count != 0U ? desc->io_thread_count : JOB_DEFAULT_IO_THREADS;
    const u32 fiber_count = desc->fiber_count != 0U ? desc->fiber_count : JOB_DEFAULT_FIBERS;
    const u32 job_capacity = desc->job_capacity != 0U ? desc->job_capacity : JOB_DEFAULT_CAPACITY;
    const u32 deque_capacity =
        desc->deque_capacity != 0U ? desc->deque_capacity : JOB_DEFAULT_DEQUE;
    const u32 ext_slots =
        desc->external_waiter_slots != 0U ? desc->external_waiter_slots : JOB_DEFAULT_EXT_SLOTS;
    if (worker_count > NPH_JOB_MAX_WORKERS || fiber_count <= worker_count ||
        job_capacity < JOB_MIN_CAPACITY || job_capacity > JOB_MAX_CAPACITY ||
        !nph_is_pow2_u32(job_capacity) || deque_capacity < JOB_MIN_DEQUE ||
        !nph_is_pow2_u32(deque_capacity) || io_thread_count > NPH_JOB_MAX_WORKERS ||
        ext_slots > 4096U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }

    NphAllocator *alloc = desc->alloc;
    NphJobSystem *js = NPH_ALLOC_TYPE(alloc, NphJobSystem);
    if (js == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    js->alloc = alloc;
    js->worker_count = worker_count;
    js->io_thread_count = io_thread_count;
    js->fiber_count = fiber_count;
    js->job_capacity = job_capacity;
    js->deque_capacity = deque_capacity;
    js->ext_slot_count = ext_slots;
    js->fiber_stack_size =
        desc->fiber_stack_size != 0U ? desc->fiber_stack_size : JOB_DEFAULT_STACK_SIZE;
    js->inline_stack_reserve = nph_max_u64(JOB_INLINE_STACK_RESERVE_MIN, js->fiber_stack_size / 4U);
    js->deterministic = desc->deterministic;

    NphResult r = NPH_RESULT_OUT_OF_MEMORY;
    js->workers = (NphJobWorker *)NPH_ALLOC_ZEROED(alloc, sizeof(NphJobWorker) * (u64)worker_count,
                                                   NPH_CACHE_LINE_SIZE);
    js->io_threads = NPH_ALLOC_ARRAY_ZEROED(alloc, NphJobIoThread, io_thread_count);
    js->fibers = NPH_ALLOC_ARRAY_ZEROED(alloc, NphFiber, fiber_count);
    js->fiber_depths = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, fiber_count);
    js->jobs = (NphJobRecord *)NPH_ALLOC_ZEROED(alloc, sizeof(NphJobRecord) * (u64)job_capacity,
                                                NPH_CACHE_LINE_SIZE);
    js->ext_sems = NPH_ALLOC_ARRAY_ZEROED(alloc, NphSemaphore, ext_slots);
    js->wait_buckets = (NphJobWaitBucket *)NPH_ALLOC_ZEROED(
        alloc, sizeof(NphJobWaitBucket) * (u64)JOB_WAIT_BUCKETS, NPH_CACHE_LINE_SIZE);
    if (js->workers == NULL || js->io_threads == NULL || js->fibers == NULL ||
        js->fiber_depths == NULL || js->jobs == NULL || js->ext_sems == NULL ||
        js->wait_buckets == NULL) {
        goto fail;
    }
    for (u32 i = 0; i < JOB_WAIT_BUCKETS; ++i) {
        nph_spin_lock_init(&js->wait_buckets[i].lock);
    }

    r = init_index_ring(&js->free_jobs, alloc, job_capacity, true);
    if (r == NPH_RESULT_OK) {
        r = init_index_ring(&js->free_fibers, alloc, fiber_count, false);
    }
    if (r == NPH_RESULT_OK) {
        r = init_index_ring(&js->ready_fibers, alloc, fiber_count, false);
    }
    for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT && r == NPH_RESULT_OK; ++p) {
        r = init_index_ring(&js->inject[p], alloc, job_capacity, false);
        if (r == NPH_RESULT_OK) {
            r = init_index_ring(&js->spill[p], alloc, job_capacity, false);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = init_index_ring(&js->io_queue, alloc, job_capacity, false);
    }
    if (r == NPH_RESULT_OK) {
        r = init_index_ring(&js->free_ext_slots, alloc, ext_slots, true);
    }
    if (r != NPH_RESULT_OK) {
        goto fail;
    }

    for (u32 i = 0; i < worker_count; ++i) {
        NphJobWorker *w = &js->workers[i];
        w->js = js;
        w->index = i;
        w->rng = i * 2654435761U + 1U; // never zero
        for (u32 p = 0; p < NPH_JOB_PRIORITY_COUNT; ++p) {
            r = nph_job_deque_init(&w->deques[p], alloc, deque_capacity);
            if (r != NPH_RESULT_OK) {
                goto fail;
            }
        }
    }

    for (u32 i = 0; i < fiber_count; ++i) {
        NphFiberDesc fd;
        fd.fn = job_fiber_main;
        fd.user = js;
        fd.stack_size = js->fiber_stack_size;
        fd.name = "job-fiber";
        r = nph_fiber_create(&js->fibers[i], &fd);
        if (r != NPH_RESULT_OK) {
            goto fail;
        }
        js->fibers_created = i + 1U;
        ring_push_spin(&js->free_fibers, i);
    }

    r = nph_semaphore_init(&js->wake, 0U);
    if (r != NPH_RESULT_OK) {
        goto fail;
    }
    js->wake_created = 1;
    r = nph_semaphore_init(&js->io_wake, 0U);
    if (r != NPH_RESULT_OK) {
        goto fail;
    }
    js->io_wake_created = 1;
    for (u32 i = 0; i < ext_slots; ++i) {
        r = nph_semaphore_init(&js->ext_sems[i], 0U);
        if (r != NPH_RESULT_OK) {
            goto fail;
        }
        js->ext_sems_created = i + 1U;
    }
    for (u32 i = 0; i < worker_count; ++i) {
        r = nph_semaphore_init(&js->workers[i].sem, 0U);
        if (r != NPH_RESULT_OK) {
            goto fail;
        }
        js->worker_sems_created = i + 1U;
    }

    r = create_threads(js, desc);
    if (r != NPH_RESULT_OK) {
        goto fail;
    }

    NPH_LOG_INFO(
        &NPH_LOG_JOBS,
        "job system: %u workers (%s), %u I/O threads, %u fibers x %llu KiB, %u job slots%s",
        worker_count, js->pinned ? "pinned" : "unpinned", io_thread_count, fiber_count,
        (unsigned long long)(js->fiber_stack_size / 1024U), job_capacity,
        js->deterministic ? ", deterministic" : "");
    *out = js;
    return NPH_RESULT_OK;

fail:
    NPH_LOG_ERROR(&NPH_LOG_JOBS, "job system creation failed: %s", nph_result_string(r));
    destroy_internal(js);
    return r;
}

void nph_job_system_destroy(NphJobSystem *js)
{
    if (js == NULL) {
        return;
    }
    // Stop the threads first: the workers' job caches are flushed on exit,
    // and only then can the pool be checked for outstanding jobs.
    nph_atomic_store_u32(&js->stop, 1U, NPH_MEMORY_ORDER_SEQ_CST);
    if (js->workers_started != 0U) {
        nph_semaphore_post_n(&js->wake, js->workers_started);
    }
    if (js->io_threads_started != 0U) {
        nph_semaphore_post_n(&js->io_wake, js->io_threads_started);
    }
    for (u32 i = 0; i < js->workers_started; ++i) {
        nph_thread_join(&js->workers[i].thread);
    }
    for (u32 i = 0; i < js->io_threads_started; ++i) {
        nph_thread_join(&js->io_threads[i].thread);
    }
    js->workers_started = 0U;
    js->io_threads_started = 0U;

    const u32 free_jobs = nph_mpmc_ring_count(&js->free_jobs);
    const u32 free_fibers = nph_mpmc_ring_count(&js->free_fibers);
    const u32 ready = nph_mpmc_ring_count(&js->ready_fibers);
    if (free_jobs != js->job_capacity || free_fibers != js->fiber_count || ready != 0U) {
        NPH_LOG_ERROR(&NPH_LOG_JOBS,
                      "job system destroyed with work outstanding: %u of %u job slots in use, "
                      "%u of %u fibers in use, %u fibers ready",
                      js->job_capacity - free_jobs, js->job_capacity, js->fiber_count - free_fibers,
                      js->fiber_count, ready);
        NPH_ASSERT_MSG(false, "nph_job_system_destroy: jobs or waits outstanding");
    }
    destroy_internal(js);
}
