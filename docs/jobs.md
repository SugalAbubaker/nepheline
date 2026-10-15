# Job system (`engine/jobs`)

The job system (CLAUDE.md §8.4) runs every piece of per-frame CPU work as
a job on one worker thread per core; the main thread schedules and waits
(§2 "Parallel by default", §10.5). Decisions:
[ADR-0013](adr/0013-job-system.md). Depends on `platform` for threads,
fibers, semaphores, and CPU topology, and on `core` for atomics and the
MPMC ring.

| Header | Contents |
|---|---|
| `jobs.h` | the system, counters, `nph_job_run`, `nph_job_run_io`, `nph_job_parallel_for`, waits, statistics |
| `deque.h` | the Chase-Lev work-stealing deque, usable on its own |

The design follows Gyrling, "Parallelizing the Naughty Dog Engine Using
Fibers" (GDC 2015): jobs run on fibers, a job that waits parks its fiber
instead of blocking its thread, and the worker picks up other work on
another fiber. The deque is Chase & Lev, "Dynamic Circular Work-Stealing
Deque" (SPAA 2005) with the memory orderings of Lê, Pop, Cohen, and Zappa
Nardelli, "Correct and Efficient Work-Stealing for Weak Memory Models"
(PPoPP 2013). Counters are futex-style (Franke, Russell, and Kirkwood,
"Fuss, Futexes and Furwocks", OLS 2002). The parallel-for splits like
Intel TBB's and Cilk's (Frigo, Leiserson, and Randall, "The
Implementation of the Cilk-5 Multithreaded Language", PLDI 1998). Victim
selection is random (Blumofe & Leiserson, "Scheduling Multithreaded
Computations by Work Stealing", FOCS 1994). Job slots are handed out
through per-worker magazines (Bonwick & Adams, "Magazines and Vmem",
USENIX 2001).

## Using it

```c
NphJobSystemDesc desc = {0};
desc.alloc = nph_allocator_system();   // everything is allocated here, nothing later
NphJobSystem *js;
NPH_TRY(nph_job_system_create(&desc, &js));

static void cull(void *user) { /* ... */ }

NphJobDecl decls[2] = {{cull, &view0, "cull"}, {cull, &view1, "cull"}};
NphJobCounter done = NPH_JOB_COUNTER_INIT;   // a plain struct; on the stack is fine
NPH_TRY(nph_job_run(js, decls, 2, NPH_JOB_PRIORITY_NORMAL, &done));
nph_job_wait(js, &done);                     // parks this fiber on a worker; blocks elsewhere

nph_job_system_destroy(js);
```

A job is a function, a user pointer, and a static name. `nph_job_run`
copies the declarations, so the array may be on the caller's stack; the
user pointers must outlive the jobs. `nph_job_parallel_for(count, batch,
fn, user, ...)` processes `[0, count)` in pieces of at most `batch`.
`nph_job_run_io` queues onto the I/O threads for work that blocks in the
OS. All of them are callable from any thread: a worker (inside a job), an
I/O thread, the main thread, or a thread the system has never heard of.

Waits are on counters. `nph_job_run` adds the batch size to the counter
and each job subtracts one when it returns; `nph_job_wait` returns at
zero and `nph_job_wait_value(c, n)` at `n` or below, which bounds how
much work is outstanding ("at most one frame of rendering in flight").
It says how many items remain, not which: jobs of consecutive frames can
finish out of order, so a frame that reuses another frame's buffers
waits on that frame's own counter (`samples/jobs_demo` keeps one per
frame slot).
`nph_job_counter_add` and `nph_job_counter_signal` make a counter stand
for work that is not a job, such as an asynchronous read whose
completion callback signals it from the I/O backend's thread.

Two rules for job code:

- **Nothing thread-specific survives a wait.** A fiber resumes on
  whichever worker completed its counter. `nph_job_worker_index()` and
  any thread-local address must be re-read after `nph_job_wait`.
- **Only I/O jobs may block in the OS.** A worker thread that sleeps in
  the kernel is a lost core; file reads, device waits, and anything that
  takes a kernel lock go through `nph_job_run_io`.

## Data structures

**Job records** are one cache line each (64 bytes: function, user
pointer, counter, name, and the range for parallel-for pieces), in a
pool of `job_capacity` slots (16384 by default, at most 2^20). Slots are
handed out through a per-worker magazine of 32 over one shared Vyukov
free ring, refilled and spilled 16 at a time, so the shared ring is
touched once per 16 jobs rather than once per job. A slot is freed
*before* its job runs, so a job can always spawn into the slot it came
from, and `nph_job_run` reserves all of a batch's slots or none
(`NPH_RESULT_FULL`).

**Queue elements** are 32-bit values packing the job index (20 bits) with
the job's *depth* in the spawn tree (12 bits, saturating): the main
thread is at depth 0, a job it spawns at 1, and so on; the pieces of a
parallel-for share their job's depth. The depth is what a wait without a
fiber uses to decide what it may run (below).

**Per worker:** three Chase-Lev deques, one per priority (2048 elements
each by default); the owner pushes and takes at the bottom (LIFO: the
job it just spawned is the one most likely in its cache), thieves steal
from the top (FIFO: the oldest, which for divide-and-conquer work is the
largest remaining piece). The buffer is fixed; a full deque spills to the
shared spill queue. `top` and `bottom` sit on separate cache lines; the
positions are 64-bit so they never wrap.

**Shared, per priority:** an *injection* ring for jobs from threads the
system does not own, and a *spill* ring for jobs a worker could not push.
They are kept apart because the exhaustion path scans the spill ring for
deeper jobs and never needs to look at injected ones, which are all at
depth 1. Plus one *I/O* ring, a *ready* ring of fibers whose wait
completed, a *free* ring of fibers, and a ring of free external-waiter
semaphore slots. All are Vyukov MPMC rings sized so that they can never
stay full (every element they can hold exists exactly once).

**Fibers:** `fiber_count` (128) pool fibers with `fiber_stack_size`
(64 KiB) guard-paged stacks from the platform layer, plus one thread
fiber per worker used only to start and stop. A per-fiber depth slot
records the depth of the job running on it.

**Counters** are one 64-bit word: the value in the low 32 bits and the
number of registered waiters in the high 32. Wait lists are not in the
counter: they are 64 buckets in the job system, selected by Fibonacci
hashing of the counter's address, each a spin lock and an intrusive list
of wait nodes. A wait node lives on the waiter's stack for the duration
of the wait and names the counter, the target, and either the parked
fiber or the semaphore a blocked thread sleeps on.

## Threading model

**Workers.** One thread per worker (`worker_count`, default logical
cores minus one, at most 64), at high priority, pinned one per core from
core 1 up when `worker_count + 1 <= logical cores` so that core 0 stays
with the main thread (`NPH_JOB_PIN_AUTO`). A worker converts its thread
to a fiber, takes a pool fiber, and from then on runs the loop on pool
fibers:

1. stop requested: switch back to the thread fiber and exit;
2. a ready fiber: switch to it, returning this loop fiber to the pool
   (resumed jobs first: they hold work in progress and free a fiber when
   they finish);
3. a job, in strict priority order: own deque, injection ring, spill
   ring at high, then the same at normal, then low; then steal from a
   random other worker, again high to low, retrying a victim four times
   when the exchange aborts;
4. otherwise park.

**Parking and waking.** A worker announces itself idle with a seq_cst
increment of the idle count, checks the stop flag and every queue once
more, and only then waits on the shared semaphore. A producer publishes
its work, issues a seq_cst fence, loads the idle count, and posts the
semaphore min(n, idle) times. The announce-then-check and
publish-then-load pairs are the store-buffering pattern; seq_cst on both
sides means a worker can only sleep while work exists if a post for it
is on its way (ADR-0013 §4).

**Waiting on a worker.** `nph_job_wait_value` on a worker takes a ready
fiber (preferred) or a free one, records the wait node in the worker,
and switches. The fiber switched to publishes the node (`after_switch`):
links it into the counter's bucket and registers the waiter in the
counter word, whose returned value says whether the target was met in
the meantime, in which case the node is unlinked again and the fiber
goes straight to the ready ring. Publishing after the switch is what
makes it impossible for a signaller to resume a fiber that is still
running on its thread.

**Signalling.** One `fetch_sub` on the counter word. If the returned
word shows no registered waiters the signaller is done and never
touches the counter again, so a counter on the waiting job's stack may
go out of scope the moment the wait returns. If it shows waiters, the
signaller locks the bucket, unlinks every node whose counter *address*
matches and whose target the new value meets, unregisters them from the
word while it still holds the lock (nobody has been woken yet, so the
counter is alive), releases the lock, and wakes them: a thread's
semaphore is posted; a fiber is pushed to the ready ring and a worker
woken, except that a signaller running *between* jobs on a worker
switches to the first woken fiber directly, one switch, on the core
whose cache holds the data the job was waiting for.

**Waiting elsewhere.** The main thread, an I/O thread, or any foreign
thread registers the same kind of node with one of `external_waiter_slots`
semaphores (16 by default) and sleeps on it; when the slots are all in
use the thread polls with backoff instead. `waits_external` counts both.

**I/O threads.** `io_thread_count` (default 1) threads outside the fiber
pool pop a FIFO ring, woken by a semaphore with one permit per job, and
retry the pop after a permit because the Vyukov ring reports "empty"
while the oldest cell is still being written. A wait inside an I/O job
is an external wait and blocks the thread.

**Shutdown.** `nph_job_system_destroy` sets the stop flag, posts every
worker and I/O thread, joins them, flushes the magazines, and checks
that every job slot and fiber is back in its pool; outstanding work is
logged at error level and asserted in development builds.

## Fiber exhaustion

Every parked wait holds a fiber. When all of them are parked and a job
on a worker needs to wait, the wait stays on its fiber and does three
things in a loop: checks its counter; takes a ready or free fiber the
moment one appears and parks as usual; and otherwise runs a queued job on
its own stack, but **only a job deeper in the spawn tree than itself**
(the depth in the queue element, read through `nph_job_deque_take_if`
and `nph_job_deque_steal_if`, which show the element to a predicate
before removing it; the spill ring has no peek, so too-shallow elements
popped from it go back to its tail, each examined once per scan). A
deeper job can lie *under* shallower ones in the worker's own deque,
out of reach of both predicates: the worker loop starts NORMAL roots
from the injection queue before LOW jobs from its deque, and resumed
fibers spawn at any depth, so a parked job's children get covered. When
`nph_job_deque_any_if` reports one below the newest element, the wait
moves the shallow newest elements to the spill ring, where every worker
still finds them, until the deeper one surfaces. Issue #18 was the hang
without that: with four workers and 128 fibers, about one run in fifty
ended with every worker an inline waiter at depth 4 or 5 and the jobs
they needed sandwiched between depth-2 and depth-3 jobs in the deques.
Finding nothing, it registers itself on its counter like an external
waiter, with its worker's own semaphore, and sleeps there: the counter
wakes it at once when met (a counter another thread pulses and re-arms
would never be *seen* at zero by a poll), and every 500 µs it withdraws
the registration, looks for a fiber or a deeper job again, and goes back
to sleep. It never spins: workers run at high priority, and spinning
there starved the main thread that was supposed to signal them.

The restriction is the depth-restricted stealing of early Intel TBB and
buys two guarantees:

- **Bounded stack.** Every nested wait is strictly deeper than the one
  below it, so a fiber holds at most one job per level of the spawn
  tree: the same stack a serial run of the same code would need. A byte
  reserve (a quarter of the stack, at least 12 KiB) stays as a backstop:
  reaching it asserts in development builds and logs an error once in
  shipping, where the wait sleeps instead of nesting further. The probe
  reads the frame address rather than a local's address, which under
  AddressSanitizer's use-after-return detection can point into a fake
  stack rather than the fiber's.
- **Liveness.** Take the deepest job in progress anywhere. If it has
  outstanding children they are deeper and queued: in the spill ring,
  which every inline wait scans, or in a deque whose owner is either
  running the worker loop or an inline waiter no deeper than that job,
  and so digs them out. If it has none it is computing, parked on
  something outside the system (an I/O completion, a signal from the
  main thread) that will arrive, or ready, and ready fibers are what
  every waiting worker looks for first. Without
  the restriction a wait could nest a job that waits on the very fiber
  under it; with every worker in that state nothing ran at all, which is
  how the first implementation hung.

The cost is latency: a job nested on a waiter's stack delays the
waiter's resumption even after its counter is met, and a job that can
neither park nor run anything holds its worker until its counter is met
or a fiber frees up, as if the worker count were lower. Exhaustion is
therefore logged once (`raise fiber_count`) and counted: `waits_inline`
is the number of waits that found no fiber, `jobs_nested` the number of
jobs run on a waiter's stack. Size the pool so both stay zero in
shipping; the random-mix stress test drives both deliberately.

## Priorities

Three levels, strict at every decision: a worker never takes a normal
job while a high one is visible to it (own deque, shared rings, or
another worker's deque), and never a low one while a normal one is. High
is for latency-critical work (input, the late-latched camera, submission;
§10.5), normal for the frame, low for streaming decompression and
cooking. There is no aging and no ordering within a level.

## parallel_for

`nph_job_parallel_for(js, count, batch, fn, user, priority, counter)`
queues one range job. When it runs it splits its range in half
repeatedly, spawning the upper half and keeping the lower, until a piece
fits the batch, then calls `fn(user, begin, end)`. A thief steals the
oldest piece, which is the largest remaining one, so the number of job
slots in use is proportional to the worker count and the depth of the
split, not to `count / batch`. The counter is incremented once per
spawned piece; if the pool runs out while splitting, the job that holds
the remainder processes it inline. Pieces share their job's depth.

## Deterministic mode

`deterministic = true` forces one worker. With no thieves and one deque
per priority, the order jobs run in is a function of the order they
were submitted in (LIFO within a worker's own spawns, strict by
priority), so a scheduling-dependent bug can be replayed. I/O threads
still run concurrently. The tests check that two runs of a
spawn-and-wait tree produce the same trace.

## Statistics

`nph_job_system_get_stats` sums per-thread counters that each thread
writes with relaxed atomics: jobs executed, stolen, nested, and on I/O
threads; waits that parked a fiber, found no fiber, or blocked an
external thread; fiber switches; and worker parks. Approximate while the
system is busy; exact after a wait that drained it.

## Configuration

| `NphJobSystemDesc` field | Default | Notes |
|---|---|---|
| `alloc` | required | everything is allocated at create |
| `worker_count` | logical cores − 1 (≥ 1) | ≤ 64 |
| `io_thread_count` | 1 | |
| `fiber_count` | 128 | must exceed `worker_count` |
| `fiber_stack_size` | 64 KiB | rounded by `platform/fiber.h` |
| `job_capacity` | 16384 | power of two in [64, 2^20] |
| `deque_capacity` | 2048 | per worker per priority; overflow spills |
| `external_waiter_slots` | 16 | threads outside the system waiting at once |
| `pinning` | `NPH_JOB_PIN_AUTO` | pin when `worker_count + 1 <= logical cores` |
| `deterministic` | false | forces one worker |

## Memory orders

Every cross-thread handshake is either a read-modify-write of a single
word or a seq_cst store and load pair; there are no fences on paths
ThreadSanitizer must follow (ADR-0013 §4 lists each one). The deque's
`take` uses a seq_cst store of `bottom` and a seq_cst load of `top`
where Lê et al. use a fence; same `xchg` on x86-64. Elements are stored
and loaded with relaxed atomics so a thief's read of a slot the owner is
overwriting is defined and rejected by the subsequent compare-exchange.
TSan runs the whole test suite in CI (`tsan (jobs, scene)`,
[build.md](build.md)), together with the scene module's scheduler tests.

## Benchmarks

`benchmarks/jobs/bench_jobs.c`, Profile configuration on the reference
CPU (Ryzen 7 5800X, 8 cores / 16 threads;
`benchmarks/results/jobs-2026-10-12-win-msvc-profile-ryzen7-5800x.json`):

| Benchmark | Median |
|---|---|
| `counter_signal` (add + signal, no waiters: one RMW each) | 3.9 ns |
| `run_wait_1_main` (one job from the main thread: inject, wake a worker, run, wake the main thread) | 572 ns |
| `run_wait_1_worker` (one child from a job: push, park the fiber, run, switch back) | 417 ns |
| `run_wait_batch_256` (256 trivial jobs from the main thread, per job) | 208 ns |
| `fork_256_worker` (256 children from a job, stolen by the other workers, per job) | 192 ns |
| `fork_join_depth_8` (binary tree of 511 nodes, each waiting on two children, per node) | 89 ns |
| `io_run_wait_1` (one I/O job and back) | 229 ns |
| `parallel_for_1M` (sum of 2^20 integers in batches of 4096, per element, 15 workers) | 0.06 ns |

Scaling, the §12 Phase 4 exit criterion ("scales near-linearly with
core count"): 256 pieces of 100 000 dependent multiply-adds (a fixed
amount of work) on a system of N workers, elapsed per run. The reference
CPU has 8 cores and 16 hardware threads; the 16-worker row shares cores.

| Workers | Elapsed | Speedup | Efficiency |
|---|---|---|---|
| 1 | 10.60 ms | 1.00× | 100 % |
| 2 | 5.31 ms | 2.00× | 100 % |
| 4 | 2.83 ms | 3.74× | 93 % |
| 8 | 1.49 ms | 7.09× | 89 % |
| 16 | 0.82 ms | 12.9× | 81 % of 16 threads; 1.8× over 8 cores from SMT |

The per-operation entries have budgets in `benchmarks/budgets.txt`; the
scaling series has none because its absolute numbers are
machine-specific and its ratios are judged here.

## Testing

`engine/jobs/tests/`: the deque alone (single-threaded semantics, wrap
around, conditional take and steal, an owner against seven thieves
taking every element exactly once, and the last-element race);
counters and external waits; the functional suite (lifecycle, bad
descriptions, batches, nesting, fork-join trees with and without enough
fibers, fiber exhaustion by event waiters staying live, pool exhaustion
reporting `FULL`, strict priorities, parallel-for coverage and inline
completion, I/O threads, a fiber resumed by an I/O job, pipeline
bounding with `wait_value`, main-thread signals resuming parked fibers,
deterministic replay, foreign threads submitting concurrently); and the
stress suite (two million tiny jobs, a wide fork-join far past the fiber
pool, a random mix of spawning, waiting, and signalling, and repeated
create/destroy). Work counts scale with `TEST_SCALE` (1 under TSan, 2 in
Debug, 8 elsewhere). The stress suite runs under ThreadSanitizer in CI.
