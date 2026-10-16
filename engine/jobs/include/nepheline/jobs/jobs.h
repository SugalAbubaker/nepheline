// SPDX-License-Identifier: Apache-2.0
#pragma once

// The job system (CLAUDE.md §8.4): every piece of per-frame CPU work is a
// job, and the main thread is a scheduler rather than a worker
// (§2 "Parallel by default", §10.5).
//
// Design (docs/jobs.md, docs/adr/0013-job-system.md), after Gyrling,
// "Parallelizing the Naughty Dog Engine Using Fibers", GDC 2015:
//
//   - One worker thread per core (pinned when the core count allows), each
//     with a work-stealing deque per priority (deque.h). Jobs spawned on a
//     worker go to its own deque; jobs spawned on any other thread go to a
//     shared injection queue per priority. An idle worker runs resumed
//     fibers first, then its own deques, then the injection queues, then
//     steals from the other workers, and parks on a semaphore when there
//     is nothing anywhere.
//   - Jobs run on fibers from a pool. A job that waits on a counter parks
//     its fiber and the worker picks up other work on another fiber; the
//     thread never blocks and the waiting job resumes, on whichever worker
//     completes the counter, with its stack intact. One fiber switch
//     (11 ns, docs/platform.md) each way.
//   - Dependencies are counters: nph_job_run adds the batch size to the
//     counter, each job decrements it when it finishes, and
//     nph_job_wait returns when it reaches zero. Counters are plain
//     structs the caller owns; a job may wait on any number of them, in
//     any order, and the same counter may be reused once it is zero.
//   - Three priorities. High is strictly preferred over normal over low
//     at every scheduling decision; within a priority there is no
//     ordering guarantee between jobs.
//   - Dedicated I/O threads outside the fiber pool run jobs that are
//     allowed to block in the OS (file reads, device waits). Never run
//     such a job on a worker: a blocked worker thread is a lost core.
//
// Jobs are plain records: a function, a user pointer, and a static name.
// The system copies the declaration, so the caller's array may be on the
// stack; the user pointer must stay valid until the job has run.
//
// Costs the caller can see (§1.4): nph_job_system_create allocates
// everything up front from the allocator in the description and nothing
// allocates afterwards; nph_job_run returns NPH_RESULT_FULL instead of
// growing when the job pool is exhausted. Signalling a counter is one
// atomic decrement unless something is waiting on it; registering or
// waking a waiter takes a spin lock on one of the system's wait-list
// buckets. A wait from a thread that is not a worker blocks the thread
// on a semaphore.
//
// Hazard: a fiber may resume on a different worker thread after a wait.
// Code must not cache anything thread-specific (the worker index, a
// thread-local address, a per-worker arena) across nph_job_wait.
//
// Thread-safety: create and destroy are NPH_MAIN_THREAD_ONLY. Every other
// function is NPH_THREADSAFE and may be called from workers, I/O threads,
// and foreign threads alike; the documentation of each says what differs.

#include <nepheline/core/alloc.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/log.h>
#include <nepheline/core/result.h>

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_JOBS);

typedef struct NphJobSystem NphJobSystem;

typedef enum NphJobPriority {
    NPH_JOB_PRIORITY_HIGH = 0,   // latency-critical: input, late-latched camera, submission
    NPH_JOB_PRIORITY_NORMAL = 1, // the frame's work
    NPH_JOB_PRIORITY_LOW = 2,    // background: streaming decompression, cooking
    NPH_JOB_PRIORITY_COUNT = 3,
} NphJobPriority;

typedef void (*NphJobFn)(void *user);

// A range job receives [begin, end) of the parallel-for it belongs to.
typedef void (*NphJobRangeFn)(void *user, u32 begin, u32 end);

typedef struct NphJobDecl {
    NphJobFn fn;      // required
    void *user;       // passed to fn; must outlive the job
    const char *name; // optional static string for the profiler and logs
} NphJobDecl;

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

// A dependency counter: one 64-bit word. Zero-filled is a valid, idle
// counter (NPH_JOB_COUNTER_INIT). The low 32 bits are the value; the high
// 32 bits count the waiters registered on it, which the job system keeps
// in its own tables, not in the counter. A signaller learns from its one
// atomic decrement whether anyone could be waiting and otherwise never
// touches the counter again, so a counter may live on the stack of the
// job that waits on it and go out of scope the moment that wait returns
// (the futex design: Franke, Russell, and Kirkwood, "Fuss, Futexes and
// Furwocks", OLS 2002; docs/adr/0013-job-system.md §4).
typedef struct NphJobCounter {
    NphAtomicU64 state;
} NphJobCounter;

#define NPH_JOB_COUNTER_INIT {NPH_ATOMIC_U64_INIT(0U)}

// Resets to zero with no waiters. Not for a counter something may be
// waiting on.
void nph_job_counter_init(NphJobCounter *counter);

// The current value: the number of jobs (and manual increments) still
// outstanding. A snapshot; only a wait gives a guarantee.
u32 nph_job_counter_value(const NphJobCounter *counter);

// Adds `n` outstanding items, for work that is not a job (an asynchronous
// read whose completion will call nph_job_counter_signal). nph_job_run and
// nph_job_parallel_for do this for their jobs. The value must stay below
// 2^32 (asserted).
void nph_job_counter_add(NphJobCounter *counter, u32 n);

// Marks one item complete: decrements by one (asserted non-zero) and wakes
// the waiters whose target is now met. Callable from any thread,
// including threads the job system does not know about (an asynchronous
// I/O completion). Jobs call this for themselves when they return.
void nph_job_counter_signal(NphJobSystem *js, NphJobCounter *counter);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

#define NPH_JOB_MAX_WORKERS 64U

typedef enum NphJobPinning {
    NPH_JOB_PIN_AUTO =
        0, // pin when worker_count + 1 <= logical cores (one core stays free for the main thread)
    NPH_JOB_PIN_NEVER,
    NPH_JOB_PIN_ALWAYS, // pin even when workers outnumber cores (they share)
} NphJobPinning;

typedef struct NphJobSystemDesc {
    NphAllocator *alloc;  // required; everything is allocated at create
    u32 worker_count;     // 0: logical cores - 1, at least 1; at most NPH_JOB_MAX_WORKERS
    u32 io_thread_count;  // 0: 1
    u32 fiber_count;      // 0: 128; must exceed worker_count
    u64 fiber_stack_size; // 0: 64 KiB; see platform/fiber.h for the rounding
    u32 job_capacity;     // 0: 16384 jobs queued or running at once (power of two, 64 .. 1 << 20)
    u32 deque_capacity;   // 0: 2048 per worker per priority (power of two); overflow spills to the
                          // injection queue
    u32 external_waiter_slots; // 0: 16 threads outside the system that may wait at once
    NphJobPinning pinning;
    // One worker, no stealing: the order in which jobs run is a function
    // of the order in which they were submitted (docs/jobs.md
    // "Deterministic mode"). For reproducing scheduling-dependent bugs;
    // I/O threads still run concurrently.
    b32 deterministic;
    // The last worker's thread sleeps this long (at most 10 s) before it
    // starts, while the others already run jobs: reproduces a thread that
    // an oversubscribed machine schedules late. For tests; 0 in real use.
    u32 debug_last_worker_start_delay_ms;
} NphJobSystemDesc;

// Creates the system, its worker and I/O threads, fibers, and queues.
// Workers start parked. NPH_RESULT_INVALID_ARGUMENT for a NULL allocator
// or an out-of-range description, NPH_RESULT_OUT_OF_MEMORY when an
// allocation, a fiber stack, or a thread cannot be created (everything
// created so far is released).
NPH_MUST_USE NphResult nph_job_system_create(const NphJobSystemDesc *desc, NphJobSystem **out);

// Stops and joins every thread and releases everything. Every counter
// must have reached zero and no job may be queued: outstanding jobs are
// dropped (asserted in development builds, logged in shipping builds).
// Safe on NULL.
void nph_job_system_destroy(NphJobSystem *js);

u32 nph_job_system_worker_count(const NphJobSystem *js);
u32 nph_job_system_io_thread_count(const NphJobSystem *js);
u32 nph_job_system_fiber_count(const NphJobSystem *js);

// The index of the worker the calling thread is, in [0, worker_count), or
// NPH_JOB_WORKER_NONE on any other thread (the main thread, an I/O
// thread). For per-worker scratch memory. Re-read it after every wait: the
// job may have moved.
#define NPH_JOB_WORKER_NONE NPH_U32_MAX
u32 nph_job_worker_index(void);

// True on one of the system's I/O threads.
bool nph_job_is_io_thread(void);

// ---------------------------------------------------------------------------
// Running jobs
// ---------------------------------------------------------------------------

// Queues `count` jobs at `priority`. If `counter` is not NULL it is
// incremented by `count` before any job is queued and each job
// decrements it when it returns. The declarations are copied.
// NPH_RESULT_FULL when fewer than `count` job slots are free (nothing is
// queued); NPH_RESULT_INVALID_ARGUMENT for a NULL fn or a bad priority.
// count == 0 is a no-op. Callable from any thread.
NPH_MUST_USE NphResult nph_job_run(NphJobSystem *js, const NphJobDecl *decls, u32 count,
                                   NphJobPriority priority, NphJobCounter *counter);

// As nph_job_run, on the I/O threads. The jobs may block in the OS. They
// run in FIFO order across the I/O threads and may not wait on counters
// through fibers: a wait inside an I/O job blocks the I/O thread.
NPH_MUST_USE NphResult nph_job_run_io(NphJobSystem *js, const NphJobDecl *decls, u32 count,
                                      NphJobCounter *counter);

// Processes [0, count) with fn in pieces of at most `batch` elements, in
// parallel. One job is queued and it splits its range in half repeatedly,
// spawning the upper half, until a piece fits the batch, so the number of
// job slots in use at once is proportional to the worker count and the
// depth of the split, not to count / batch (Intel TBB's parallel_for;
// Frigo et al., "The Implementation of the Cilk-5 Multithreaded
// Language", PLDI 1998 for why a thief takes the largest piece). If the
// pool runs out while splitting, the remaining range is processed inline
// by the job that holds it. `counter` is incremented once per spawned
// piece; wait on it for completion. count == 0 is a no-op.
NPH_MUST_USE NphResult nph_job_parallel_for(NphJobSystem *js, u32 count, u32 batch,
                                            NphJobRangeFn fn, void *user, NphJobPriority priority,
                                            NphJobCounter *counter);

// ---------------------------------------------------------------------------
// Waiting
// ---------------------------------------------------------------------------

// Returns once the counter is zero. On a worker the calling fiber is
// parked and the worker runs other jobs; the fiber resumes on whichever
// worker completes the counter. On any other thread the thread blocks on
// a semaphore. Returns at once when the counter is already zero.
//
// Fiber exhaustion (logged once; raise fiber_count): when every fiber is
// parked, the wait stays on its fiber and runs queued jobs on its own
// stack until the counter is met or a fiber frees up. Only jobs spawned
// below the waiting one (its children, their children, and so on, in any
// job tree) are run that way, so the stack grows at most as deep as the
// spawn tree, and work never stalls as long as every wait is on jobs
// that will run or on something outside the system that will signal.
// A job that waits in that state holds its worker until then, as if the
// worker count were lower. docs/jobs.md "Fiber exhaustion".
void nph_job_wait(NphJobSystem *js, NphJobCounter *counter);

// As nph_job_wait, returning once the value is at most `target`. For
// bounding a count: "at most one frame of rendering in flight". It does
// not say which items finished (jobs complete in any order), so work that
// reuses a particular frame's resources waits on that frame's counter.
void nph_job_wait_value(NphJobSystem *js, NphJobCounter *counter, u32 target);

// ---------------------------------------------------------------------------
// Statistics (for tests, samples, and the profiler overlay)
// ---------------------------------------------------------------------------

typedef struct NphJobStats {
    u64 jobs_executed;    // on workers
    u64 jobs_stolen;      // of those, taken from another worker's deque
    u64 jobs_nested;      // of those, run on a waiting job's stack (no free fiber)
    u64 jobs_io_executed; // on I/O threads
    u64 waits_switched;   // waits that parked a fiber
    u64 waits_inline;     // waits that found no free fiber and stayed on their own
    u64 waits_external;   // waits that blocked a non-worker thread
    u64 fiber_switches;   // every switch on a worker
    u64 worker_parks;     // times a worker slept on the semaphore
} NphJobStats;

// Sums the per-thread counters. Approximate while the system is busy.
void nph_job_system_get_stats(const NphJobSystem *js, NphJobStats *out);

// Writes the scheduler's state to `out` for diagnosing a hang: what each
// worker is doing and on which fiber, every deque's queued jobs with
// their depths in the spawn tree, the shared queues' counts, and every
// registered wait with its counter's value and target. The reads are
// racy by design: call it when the system is stuck (the stress tests do,
// from a watchdog), not as telemetry. Callable from any thread.
void nph_job_system_dump(const NphJobSystem *js, FILE *out);

#ifdef __cplusplus
}
#endif
