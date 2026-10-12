# 0009. Platform layer: threads, synchronisation, virtual memory, and the TLSF locking decision

- **Date:** 2026-10-12
- **Status:** Accepted

## Context

CLAUDE.md §8.3 asks the platform module for threads, mutexes, condition
variables, semaphores, affinity and names, virtual memory with large
pages, a high-resolution timer, CPU detection, and dynamic library loading.
§1.4 forbids hidden locking and allocation, §2 asks that every subsystem
receive its allocator explicitly, and core's TLSF heap (ADR-0004,
`core/tlsf.h`) left open whether the runtime wraps it with a lock or gives
each thread its own heap. ADR-0005 placed atomics in core and promised that
platform would supply the logger thread and the concurrent tests that
Phase 1 could not run.

The job system (Phase 4) is the main consumer: it parks and wakes worker
threads thousands of times per second, so the cost of a wake-up with no
contention decides whether the primitives can sit on the frame path.

## Decision

1. **OS primitives, wrapped in place.** `NphMutex`, `NphCondVar`, and
   `NphRwLock` are fixed-size structs holding the native object (SRW lock
   and `CONDITION_VARIABLE` on Windows; `pthread_mutex_t`, `pthread_cond_t`,
   `pthread_rwlock_t` on Linux). The sizes are stated per platform in the
   public header and checked with static assertions in the implementation.
   A zero-filled object is initialised on both platforms, so statics can
   use `NPH_MUTEX_INIT`. Nothing allocates. Timed waits use the monotonic
   clock (`pthread_cond_clockwait`, `sem_clockwait`, glibc 2.30+), so a
   wall-clock change never cuts a wait short.
2. **The semaphore has a user-space fast path.** `NphSemaphore` keeps an
   atomic count in front of the kernel semaphore (Preshing, "Semaphores
   are Surprisingly Versatile", 2015). A post with no waiter and a wait
   with a permit available are one atomic read-modify-write; waiters spin
   briefly (1024 iterations) before sleeping in the kernel. The count is
   unsigned and biased by 2³¹ so that both "permits" and "waiters" are
   defined arithmetic. The timed wait withdraws its registration on
   timeout unless a post already paid for it, in which case it consumes
   the kernel permit, so no permit is ever lost or duplicated.
3. **Threads hand over their start record synchronously.**
   `nph_thread_create` returns only after the new thread has copied the
   function, argument, and name, so the description can live on the
   caller's stack and no allocation is needed. The trampoline names the
   thread and, when the function returns, detaches the thread's log ring
   (`nph_log_thread_detach`) so a thread that logged never leaks its ring.
4. **Priorities are best effort on Linux.** Windows maps the four classes
   to `THREAD_PRIORITY_*`. Linux uses per-thread nice values (10, 0, −5,
   −15); raising needs `CAP_SYS_NICE` or an `RLIMIT_NICE` grant, and a
   refusal is reported as `NPH_RESULT_ACCESS_DENIED` without logging an
   error. `SCHED_FIFO` for the audio thread is left to the runtime's
   configuration in Phase 12.
5. **TLSF stays single-threaded; the lock is a named wrapper.**
   `NphLockedAllocator` puts a mutex around any allocator. The type name at
   the call site is the visibility §1.4 demands: a subsystem given a locked
   allocator knows each call costs ~20 ns uncontended and can decide
   whether that suits its traffic. The job system gives each worker its own
   heap for per-frame work (Phase 4); the locked wrapper serves cold,
   shared allocations (asset database, editor). The alternative of a
   lock inside TLSF was rejected because it would tax every single-threaded
   use and hide the cost.
6. **Virtual memory is page-granular and explicit.** Reserve, commit,
   decommit, release, and protect map directly to `VirtualAlloc`/
   `VirtualFree`/`VirtualProtect` and `mmap`/`mprotect`/`madvise`. Large
   pages are offered only when the process can actually get them
   (`SeLockMemoryPrivilege` enabled on Windows; a non-empty hugetlbfs pool
   on Linux); otherwise `nph_vmem_large_page_size()` is 0 and
   `nph_vmem_alloc_large` reports `NPH_RESULT_UNSUPPORTED`, so callers
   fall back deliberately. Transparent huge pages are a separate hint.
7. **CPU detection reads CPUID directly** for vendor, model, features,
   and cache geometry (deterministic cache parameters leaf on Intel and on
   AMD with topology extensions; the legacy AMD leaves otherwise), takes
   the SIMD level from math (ADR-0008 §9), and asks the OS for the core
   counts (`GetLogicalProcessorInformationEx`; the sysfs topology on
   Linux), falling back to the CPUID threads-per-core estimate. The TSC
   rate is reported from CPUID when the CPU states it (Intel leaves 0x15 /
   0x16) and measured on request otherwise, never implicitly: a
   measurement blocks for its sample time.
8. **One source file per OS per area** (`thread_win32.c`,
   `thread_posix.c`, …) selected in CMake, with a private
   `platform_internal.h` for the OS headers and the error mapping
   (`nph_win32_result`, `nph_posix_result`). Public headers never include
   an OS header. Two result codes were added to core for the OS layer:
   `NPH_RESULT_ACCESS_DENIED` and `NPH_RESULT_BUSY`.
9. **The logger reports drops left behind by an exited thread.** The
   concurrent tests exposed that records dropped after a thread's last
   delivered record, followed by the thread exiting, were lost without
   trace. The flusher now emits a synthetic `core` WARN record carrying the
   count when it retires such a ring.

## Alternatives considered

- **Own futex-based mutex and condition variable.** Rejected for now: the
  OS primitives are already futex-based on Linux and SRW locks are the
  fastest user-mode lock Windows offers; the engine's hot paths are
  lock-free by design, so a custom lock would be optimising a path that
  must stay cold. Revisit if the job system measures otherwise.
- **`NphThread` allocating its start record from an allocator.** Rejected:
  it would make thread creation take an allocator for a 64-byte record that
  a handshake avoids.
- **Lock inside `NphTlsf` behind a flag.** Rejected (decision 5).
- **Processor groups beyond 64 cores on Windows.** Deferred: the affinity
  mask covers one group, which is every machine the engine targets; the
  logical count still reports the whole machine.

## Consequences

- Phase 4 can build the fiber scheduler on `NphSemaphore` for parking
  workers and on `nph_thread_set_affinity` for pinning, with the uncontended
  costs recorded in `docs/platform.md`.
- Core's deferred concurrent tests now exist in
  `engine/platform/tests/test_core_concurrent.c`: atomics under contention,
  the store-buffering (Dekker) litmus test for `seq_cst`, release/acquire
  message passing, the spin lock, both rings, the debug allocator, and the
  logger with writers racing a flusher.
- The runtime (Phase 9) starts `NphLogThread` right after `nph_log_init`.
- Fibers, files, async I/O, and the window layer are covered by their own
  records (ADR-0010 onwards).
