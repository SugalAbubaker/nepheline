# 0013. Job system: fiber workers, Chase-Lev deques, futex-style counters, and depth-bounded inline waits

- **Date:** 2026-10-12
- **Status:** Accepted

## Context

CLAUDE.md §8.4 fixes the shape of the job system: fiber-based, one
pinned worker per core (Gyrling, "Parallelizing the Naughty Dog Engine
Using Fibers", GDC 2015), per-worker work-stealing deques (Chase & Lev,
SPAA 2005; Lê et al., PPoPP 2013), jobs as small records, dependencies as
atomic counters that a job waits on by yielding its fiber rather than
blocking its thread, three priorities, dedicated I/O threads, and a
deterministic mode. §2 asks for "parallel by default" with the main
thread as a scheduler, §1.4 for no hidden allocation or locking, §10.5
for a 4.17 ms frame in which all CPU work finishes on the job system,
and §12 for stress tests and ThreadSanitizer runs as the exit criteria.

Phase 3 delivered the pieces this is built from: an 11 ns assembly fiber
switch with sanitizer annotations and guard-paged stacks (ADR-0010), a
semaphore with a 3.8 ns fast path, threads with affinity and priority,
and core's atomics and Vyukov MPMC ring.

Three questions had to be settled by design rather than by following the
talk, because the talk does not answer them: how a waiter and a signaller
can never miss each other without a lock on the hot path; how a counter
can live on a job's stack when signallers run on other threads; and what
happens when every fiber is parked and a job still needs to wait, which
the Naughty Dog system treats as a fatal configuration error and this
engine must not.

## Decision

1. **Workers run their scheduling loop on pool fibers.** A worker thread
   converts itself to a fiber only to start and stop; everything else
   happens on fibers from a fixed pool (128 by default, 64 KiB stacks).
   A job that waits parks the fiber it is on and the worker continues on
   a ready fiber (one whose wait completed) or a free one. A fiber
   therefore holds at most the frames of one job and whatever that job
   nested, and resumes with its stack intact on whichever worker
   completes its counter. The loop runs ready fibers before new jobs: a
   resumed job holds work in progress and frees a fiber when it finishes.

2. **The wait is published after the switch, not before.** A waiting
   fiber must not be resumed while it is still running on its thread. The
   waiter records its wait node in the worker, switches away, and the
   fiber switched *to* links the node into the wait list (`after_switch`)
   once the waiter is off the thread. The same mechanism returns a loop
   fiber to the pool after its worker has left it. Both `after_switch`
   and the thread-local accessors are non-inlined and re-read the
   thread-local worker, because the fiber that continues may be on a
   different thread than the one that started the switch (ADR-0010 §4).

3. **Counters are one 64-bit word, futex-style.** The low half is the
   value, the high half the number of waiters registered on it. Wait
   lists do not live in the counter but in 64 buckets in the job system,
   selected by hashing the counter's address, each with a spin lock
   (Franke, Russell, and Kirkwood, "Fuss, Futexes and Furwocks", OLS
   2002). A signal is one `fetch_sub`; if the returned word shows no
   waiters the signaller never touches the counter again, and if it does
   show waiters the signaller walks the bucket comparing counter
   *addresses*, unregisters the waiters it wakes while it still holds the
   lock and before it wakes any of them, and never dereferences the
   counter afterwards. A counter may therefore sit on the stack of the
   job that waits on it and go out of scope the moment the wait returns,
   while a signaller that decremented it a few nanoseconds earlier is
   still running: that signaller either saw no waiters and is done with
   the counter, or saw waiters and is working on the bucket. The first
   version kept the lock and list head in the counter and loaded the
   list head after the decrement; AddressSanitizer caught that as a
   stack-use-after-scope in the random stress test on the first run that
   exercised the polling path heavily.

4. **Every handshake is a single-word read-modify-write or a seq_cst
   pair; no fences, so ThreadSanitizer models all of it.**
   - *Waiter versus signaller:* registering adds a waiter to the counter
     word and reads the value from the same RMW; signalling subtracts one
     and reads the waiter count from the same RMW. The two RMWs on one
     word are totally ordered, so one of them sees the other: either the
     waiter sees the decremented value and does not wait, or the
     signaller sees the waiter and walks the bucket, where the node
     already is because it is linked under the bucket lock before the RMW.
   - *Parking versus publishing work:* a worker announces itself idle
     with a seq_cst increment, then scans every queue once more before
     sleeping; a producer publishes its work, then issues a seq_cst fence
     and loads the idle count. This is the store-buffering litmus test;
     seq_cst on both sides forbids both missing each other, so a worker
     can only sleep while there is work if a producer is about to post
     the semaphore.
   - *Owner versus thief on the deque:* where Lê et al. use a seq_cst
     fence between a relaxed store of `bottom` and a load of `top`, the
     deque uses a seq_cst store and a seq_cst load. Same guarantee, the
     same `xchg` on x86-64, and visible to TSan, which ignores fences.
   - *Fiber hand-off:* the ready and free rings are Vyukov MPMC rings
     whose push and pop are release/acquire; the direct switch to a
     woken fiber happens after the bucket lock that unlinked its node was
     released. TSan's fiber support (`__tsan_switch_to_fiber`) carries
     the happens-before across the switch itself.

5. **A wait that finds no free fiber runs deeper jobs on its own stack
   and nothing else.** Queue elements pack the job index with its depth
   in the spawn tree (the main thread is depth 0; a job spawned from
   depth *d* is at *d* + 1; the pieces of a parallel-for share their
   job's depth). A wait that cannot park stays on its fiber, keeps
   checking for a ready or free fiber, and meanwhile runs queued jobs
   whose depth is strictly greater than its own, peeking the deques
   through `take_if`/`steal_if` and rotating the spill queue once per
   scan. This is the depth-restricted stealing of early Intel TBB, which
   bounded stack use the same way: every nested wait is strictly deeper
   than the one under it, so a fiber's stack holds at most one job per
   level of the spawn tree, the same depth a serial run of the same code
   would reach. It also keeps the system live when every fiber is
   parked: consider the deepest job in progress anywhere. Its children,
   if it has any outstanding, are deeper and queued, so it runs them; if
   it has none it is computing, parked on something outside the system
   (an I/O completion, a signal from the main thread), or ready, and
   ready fibers are picked up by the waiting workers. The previous
   version ran *any* queued job inline, which first overflowed the fiber
   stack (an unbounded chain of unrelated waits) and then, with a byte
   limit added, deadlocked: every worker sat at the limit waiting on
   children that only a worker with stack room could run. A byte reserve
   remains as a backstop (a quarter of the stack, at least 12 KiB): a
   wait that reaches it runs nothing and logs an error once. A wait with
   nothing to run does not poll: it registers on its counter with its
   worker's own semaphore, like an external waiter, so the counter wakes
   it the moment it is met, and wakes by itself every 500 µs to withdraw
   the registration and look for a fiber or a deeper job. The polling
   version missed counters that another thread pulsed and re-armed, and
   spun at the workers' high priority, which starved the main thread of
   the random-mix stress test to the point of a 100-second run. The jobs
   that run on a waiter's stack delay the resumption of the job whose
   stack they share even after its counter is met, so exhaustion is
   logged once at warning level and counted in the statistics; the fix
   is to raise `fiber_count`, and the point of the rule is that the
   engine degrades rather than crashes or hangs while it is wrong.

6. **Priorities are strict and small.** Three levels, each with its own
   deque per worker and injection and spill queues. A worker looks at
   every source at high before any at normal, and steals in the same
   order. No aging: latency-critical work (input, the late-latched
   camera, submission) must never queue behind streaming, and the frame
   itself is the normal level. Within a level there is no ordering
   guarantee.

7. **Three shared queues per priority, kept apart by who fills them.**
   Jobs from threads the system does not own go to the injection queue;
   jobs a worker could not push because its deque was full go to the
   spill queue; the deques hold everything else. The split exists for
   rule 5: injected jobs are all at depth 1 and never runnable by a
   waiting job, so the scan for deeper work never has to rotate through
   them.

8. **Dedicated I/O threads outside the fiber pool** run jobs that may
   block in the OS, in FIFO order, woken by a semaphore with one permit
   per job. A wait inside an I/O job blocks the I/O thread (an external
   wait); the header says not to do it.

9. **Threads the system does not own wait on a semaphore** from a fixed
   pool of slots (16 by default) through the same wait-node mechanism;
   more simultaneous external waiters than slots fall back to polling.

10. **Jobs are scheduled from the loop by whoever has the fewest
    switches to make.** A signaller running between jobs on a worker
    (scheduler context) switches straight to the first fiber it woke,
    returning its own empty loop fiber to the pool: one switch, and the
    resumed job continues on the core whose cache holds the data it was
    waiting for. Any other waker (a job that signalled inside its body,
    the main thread, an I/O thread) pushes the fiber to the ready ring
    and wakes a worker.

11. **Nothing allocates after creation** (§1.4). Jobs, fibers, deques,
    rings, wait buckets, and external-waiter semaphores are sized by
    the description and allocated by `nph_job_system_create`;
    `nph_job_run` returns `NPH_RESULT_FULL` rather than growing. Job
    slots are handed out through per-worker magazines over one shared
    free ring (Bonwick & Adams, USENIX 2001) so the ring is touched once
    per 16 jobs. A job slot is freed before the job runs, so a job may
    always spawn into the slot it came from.

12. **Deterministic mode is one worker.** With a single worker and no
    thieves, the order in which jobs run is a function of the order in
    which they were submitted, which reproduces scheduling-dependent
    bugs. I/O threads still run concurrently.

13. **Pinning leaves core 0 to the main thread** when the workers fit
    (`NPH_JOB_PIN_AUTO`: pin when `worker_count + 1 <= logical cores`),
    and the default worker count is logical cores minus one. Workers run
    at high thread priority; Linux may refuse the raise without
    privileges, which is accepted.

## Alternatives considered

- **A thread pool without fibers** (jobs as callbacks, dependencies as
  continuations). Every "wait for these jobs, then continue" becomes a
  new job, which inverts straightforward fork-join code and makes the
  physics and renderer phases harder to write than they need to be. The
  switch is 11 ns; the fiber approach was specified for that reason.
- **Fiber pool growth on exhaustion.** Live and simple, but it is
  allocation on the hot path that the caller cannot see, and the memory
  is proportional to the number of simultaneously waiting jobs, which a
  stress test drives into the thousands. Rule 5 keeps the pool fixed.
  Growth could be added later as an explicit option.
- **Blocking the worker thread when no fiber is free.** Deadlocks a pure
  fork-join tree deeper than the pool: every worker blocks on children
  that no one is left to run. Rule 5 runs those children instead.
- **Running any queued job inline** (the first implementation).
  Unbounded stack use, see rule 5.
- **Larger or lazily committed fiber stacks** instead of a rule: makes
  the overflow rarer, not impossible, and lazy commit needs guard-page
  growth handling on Windows. Not pursued for Phase 4; the rule bounds
  the depth regardless of stack size.
- **A lock in every counter** taken on every signal. Correct and simple,
  but three atomics per signal instead of one. The futex design keeps
  one.
- **Counters allocated from a pool with handles** (the Naughty Dog
  approach). Avoids the stack-lifetime question by making the user free
  the counter later, at the cost of an allocation per counter and a
  handle lookup per signal. The futex design lets counters be plain
  structs on the stack, which is what every fork-join call site wants.
- **Depth tracked in the job record** rather than the queue element.
  The record is freed before the job runs and may be reused by another
  thread while a waiter is still looking at a queue, so the depth read
  would race. Packing it in the element costs 12 bits of index range
  (job capacity at most 2^20) and reads nothing shared.
- **TBB's unrestricted arena stealing** for the wait path. TBB documents
  that it cannot bound stack depth and that nested waits can deadlock;
  the restriction is the price of the guarantee.

## Consequences

- Every fork-join call site is three lines: a counter on the stack,
  `nph_job_run`, `nph_job_wait`. The counter needs no cleanup.
- Fiber exhaustion is a configuration warning with a bounded, live
  fallback, not a crash: the frame pipeline in Phase 5 and later can
  size the pool from the statistics (`waits_inline`, `jobs_nested`)
  rather than guess.
- A job must not cache anything thread-specific across a wait (the
  worker index, a thread-local address). The header says so; the
  `nph_job_worker_index` doc says to re-read it.
- The job capacity is capped at 2^20 by the packed element.
- The spill queue is scanned by waiting jobs with pop-and-re-push, so a
  worker whose deque overflows constantly (more than 2048 jobs queued at
  one priority) will see those jobs reordered under exhaustion. Deque
  capacity is a description field for that case.
- TSan runs in CI on the jobs tests only (ADR-0003 policy: TSan excludes
  ASan). Audio and streaming join when they exist (CLAUDE.md §9).
- The frame pipeline (simulate N+1 while rendering N) is expressed with
  `nph_job_wait_value(counter, 1)` and shown in `samples/jobs_demo`; the
  runtime module (Phase 9) will own the real one.
