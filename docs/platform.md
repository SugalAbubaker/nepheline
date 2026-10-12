# Platform module (`engine/platform`)

The OS abstraction (CLAUDE.md §8.3): clocks, threads and blocking
synchronisation, virtual memory, CPU detection, dynamic libraries, process
helpers, and the logger thread. Decisions: [ADR-0009](adr/0009-platform-threads-and-memory.md).
Fibers, files, asynchronous I/O, and the window/input layer are documented
in the sections added with their code.

Depends on `core` and `math` (for the SIMD level). Every public header is
self-contained and includes no OS header; the sources are split per
operating system (`*_win32.c`, `*_posix.c`/`*_linux.c`) and share
`src/platform_internal.h`, which holds the OS includes and the mapping from
`GetLastError()`/`errno` to `NphResult` (`ACCESS_DENIED` and `BUSY` were
added to core for it).

| Header | Contents |
|---|---|
| `time.h` | monotonic clock, wall clock, cycle counter, stopwatch |
| `thread.h` | threads, mutex, condition variable, read-write lock, semaphore, once |
| `vmem.h` | reserve/commit/decommit/release/protect, large pages |
| `cpu.h` | CPUID features, caches, core counts, TSC rate |
| `dylib.h` | shared-library loading and symbol lookup |
| `locked_alloc.h` | a mutex around any allocator |
| `log_thread.h` | the thread that flushes core's log rings |
| `platform.h` | executable path, environment, process id, console, debugger |

## Clocks (`time.h`)

`nph_time_now_ns` is the engine clock: `QueryPerformanceCounter` (100 ns
resolution; the tick-to-nanosecond conversion splits the division so no
intermediate overflows) or `CLOCK_MONOTONIC`. `nph_time_wall_ns` is UTC
since the Unix epoch from `GetSystemTimePreciseAsFileTime` /
`CLOCK_REALTIME`. `nph_time_cycles` is `rdtsc`, for the profiler; the rate
comes from `cpu.h`. `NphStopwatch` wraps the monotonic clock for the
common "how long did this take" pattern.

## Threads and synchronisation (`thread.h`)

- **Threads.** `nph_thread_create` takes an `NphThreadDesc` (function,
  argument, name, stack size) and returns once the new thread has copied
  it, so nothing is allocated and the description may live on the caller's
  stack. The trampoline applies the name (`SetThreadDescription`;
  `pthread_setname_np`, 15 bytes) and, after the function returns, detaches
  the thread's log ring. `nph_thread_current` yields a non-joinable handle
  for the setters. Affinity is a 64-bit mask of logical cores; priority has
  four classes (`LOW`, `NORMAL`, `HIGH`, `TIME_CRITICAL`), of which
  raising above normal may be refused on Linux (`ACCESS_DENIED`).
  `nph_thread_sleep_ns` uses a high-resolution waitable timer on Windows
  10 1803+ (so a 1 ms sleep takes about 1 ms rather than a 15.6 ms tick)
  and `clock_nanosleep(CLOCK_MONOTONIC)` on Linux.
- **Mutex, condition variable, read-write lock** are the OS objects in
  place (SRW lock, `CONDITION_VARIABLE`; the pthread types), sized per
  platform in the header and checked by static assertions in the
  sources. Zero-filled objects are initialised, so `NPH_MUTEX_INIT` and
  friends work for statics. Timed waits are against the monotonic clock.
- **Semaphore.** An atomic count (biased by 2³¹ so that permits and
  waiters are both unsigned arithmetic) in front of a kernel semaphore
  (Preshing 2015). Post and wait avoid the kernel whenever no thread has
  to sleep or be woken; a waiter spins 1024 iterations before sleeping.
  `nph_semaphore_wait_timeout` withdraws its registration on timeout
  unless a post has already paid for it, in which case it consumes the
  kernel permit, so the count stays exact.
- **Once.** `nph_once` runs an initialiser exactly once; racers spin until
  it is done. Used for the cached CPU info and the virtual-memory sizes.

## Virtual memory (`vmem.h`)

Reserve address space (no access, no commit charge: `MEM_RESERVE` /
`PROT_NONE` + `MAP_NORESERVE`), commit pages (`MEM_COMMIT` /
`mprotect(RW)`; zero-filled, lazily backed), decommit (`MEM_DECOMMIT` /
`MADV_DONTNEED` + `PROT_NONE`; contents lost), release, and change
protection. Sizes are in pages; reservations round to the allocation
granularity (64 KiB on Windows). Large pages (2 MiB) are reported as
available only when the process can get them: the Windows implementation
tries to enable `SeLockMemoryPrivilege` once, and Linux checks the
hugetlbfs pool; otherwise `nph_vmem_alloc_large` returns `UNSUPPORTED` and
callers fall back explicitly. `nph_vmem_advise_huge` is the transparent
huge page hint (`MADV_HUGEPAGE`; a no-op on Windows).

Fiber stacks (next section when they land), the job system's arenas, and
streaming buffers are built on this: reserve generously, commit as used.

## CPU detection (`cpu.h`)

`nph_cpu_detect` fills an `NphCpuInfo` from CPUID: vendor, brand, display
family/model/stepping, feature bits (SSE3 through AVX-512F, BMI, LZCNT,
AES, SHA, F16C, FMA, RDTSCP, invariant TSC, RDRAND, MOVBE, hypervisor),
cache line size and L1d/L2/L3 sizes (deterministic cache parameters leaf
on Intel and on AMD with topology extensions; legacy AMD leaves otherwise),
the SIMD level from `nph_simd_detect_level`, and the TSC rate when CPUID
reports it (Intel 0x15/0x16). AVX, FMA, F16C, and AVX2 are reported only
when `XGETBV` confirms the OS saves the YMM state; AVX-512F needs the ZMM
state as well. Core counts come from the OS: the process affinity mask and
`GetLogicalProcessorInformationEx(RelationProcessorCore)` on Windows,
`sched_getaffinity` and the sysfs topology (distinct package/core pairs)
on Linux, falling back to the CPUID threads-per-core estimate.
`nph_cpu_info` caches one detection. `nph_cpu_measure_tsc_hz` measures
the counter against the monotonic clock for CPUs that do not report it
(AMD); it blocks for the sample time and is never called implicitly.

## Dynamic libraries (`dylib.h`)

`LoadLibraryW` / `dlopen(RTLD_NOW | RTLD_LOCAL)`. A missing file is
`NOT_FOUND`; a file that exists but cannot be loaded (wrong architecture,
missing dependency) is `IO_ERROR` with the loader's message logged.
Function and data symbols are looked up separately because C has no
portable cast between the two pointer kinds; the implementations convert
through a union. The runtime's hot reload (Phase 9) adds the copy-and-swap
on top.

## Locked allocator (`locked_alloc.h`)

`NphLockedAllocator` forwards alloc/realloc/free to an inner allocator
under a mutex. It exists so that the single-threaded TLSF heap can be
shared where that is the right trade-off, with the lock visible in the
type name (ADR-0009 §5).

## Logger thread (`log_thread.h`)

`nph_log_thread_start` creates a low-priority thread named `logger` that
calls `nph_log_flush` every interval (default 10 ms) or when kicked;
`nph_log_thread_stop` joins it and performs a final flush on the caller,
so everything logged before the call has reached the sinks when it
returns. The runtime owns one instance (ADR-0005 §4).

## Process helpers (`platform.h`)

Executable path and directory (UTF-8, native separators), environment
variables, process id, OS name, enabling virtual-terminal sequences on the
Windows console (so the console sink can use colour), and debugger
detection. The `NPH_LOG_PLATFORM` category carries the OS messages the
layer reports.

## Benchmarks

Recorded with the fibers and file I/O that complete the module.

## Testing

`engine/platform/tests/`:

- `test_time.c`: monotonicity over 100 000 readings, sub-millisecond
  resolution, wall clock in range and tracking the monotonic clock across
  a sleep, cycle counter, conversions, stopwatch.
- `test_thread.c`: create/join/detach, ids, stack size (6 MiB of frames on
  a 16 MiB stack), affinity and priority on current and created threads,
  names including over-long and UTF-8 ones, sleep accuracy and the absence
  of scheduler-tick quantisation, 128 threads created and joined.
- `test_sync.c`: a contended mutex counter; a bounded queue with one
  producer and four consumers on condition variables; timed waits that
  expire and that are signalled; a read-write lock whose readers overlap
  while every writer sees the invariant hold; semaphore permits, try-wait,
  timeouts, a 20 000-round ping-pong, a worker pool fed by `post_n`, and
  the balance of timed waiters against posts; `nph_once` under eight
  racing threads.
- `test_core_concurrent.c`: the tests Phase 1 deferred: atomic
  read-modify-writes exact under contention, the store-buffering (Dekker)
  litmus test for `seq_cst` (20 000 rounds; fails if a `seq_cst` store is
  not a full barrier), release/acquire message passing, the spin lock, the
  SPSC ring (order and contents over 200 000 items), the MPMC ring (four
  producers, four consumers, every item exactly once and per-producer
  order preserved), the debug allocator from eight threads, and the logger
  with writers racing a flusher while threads attach and detach.
- `test_vmem.c`, `test_cpu.c`, `test_dylib.c` (loads
  `platform_test_dylib`, a shared library built by the test CMake),
  `test_locked_alloc.c` (TLSF through the lock from eight threads, then
  `nph_tlsf_validate`), `test_log_thread.c`, `test_platform.c`.

Assertions are made on the main thread only; workers record into shared
atomics. Work counts scale by `TEST_SCALE` (1 in Debug, where the
sanitizers slow atomics down; 4 elsewhere).
