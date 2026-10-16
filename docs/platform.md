# Platform module (`engine/platform`)

The OS abstraction (CLAUDE.md §8.3): clocks, threads and blocking
synchronisation, fibers, virtual memory, files and asynchronous I/O, CPU
detection, dynamic libraries, process helpers, and the logger thread.
Decisions: [ADR-0009](adr/0009-platform-threads-and-memory.md) (threads,
memory, the TLSF lock), [ADR-0010](adr/0010-fibers.md) (fibers),
[ADR-0011](adr/0011-files-and-async-io.md) (files and async I/O),
[ADR-0012](adr/0012-window-and-input.md) (window and input).

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
| `fiber.h` | fibers: guard-paged stacks, assembly context switch, Win32 fallback |
| `vmem.h` | reserve/commit/decommit/release/protect, large pages |
| `path.h` | pure path string helpers (basename, dirname, extension, join) |
| `file.h` | synchronous files, memory mapping, the file system, directory listing |
| `async_io.h` | asynchronous reads and writes: IOCP, io_uring, thread pool |
| `window.h` | the window system, monitors, raw events, gamepads, Vulkan surface hooks |
| `input.h` | the action layer: per-frame state and named actions with bindings |
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
  kernel permit, so the count stays exact. A wait that came back from
  the kernel ends with an acquire load of the count: the kernel object
  already orders the post before the wake-up, but ThreadSanitizer does
  not intercept every kernel wait (`sem_clockwait`), and the load states
  the ordering in terms it models.
- **Once.** `nph_once` runs an initialiser exactly once; racers spin until
  it is done. Used for the cached CPU info and the virtual-memory sizes.

## Fibers (`fiber.h`)

A fiber is a stack plus the saved registers of the code suspended on it.
`nph_fiber_switch(from, to)` pushes the callee-saved registers and MXCSR
on the current stack (on Windows also xmm6-xmm15 and the three TIB stack
bounds), swaps the stack pointer, and pops the target's state; a fresh
fiber's first switch "returns" into a trampoline that calls its function.
The assembly is `src/fiber_win64.asm` (MASM) and `src/fiber_sysv_x64.S`
(GNU assembler); `src/fiber.c` lays out the initial frame, owns the
stacks (a `vmem.h` reservation with a no-access guard page below the
usable range), and carries the ASan and TSan annotations so the sanitizers
follow the stack switches. `-DNPH_FIBERS_WIN32=ON` substitutes the Win32
fiber API behind the same header (`src/fiber_win32.c`); CI builds one
configuration with it.

Rules (the header spells them out): a thread calls `nph_fiber_init_thread`
before any switch, a fiber function never returns, a fiber is destroyed
only while suspended, and no thread-local address is cached across a
switch because a fiber may resume on another thread. The x87 control word
is not part of a fiber's state (ADR-0010 §8): `FLDCW` alone cost more than
the whole rest of the switch.

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

## Paths, files, and the file system (`path.h`, `file.h`)

`path.h` is pure string handling on UTF-8 paths with either separator:
basename, dirname (keeping roots such as `/` and a drive root), extension
and stem, absolute-path detection, join into a caller buffer, separator
normalisation. Nothing allocates.

`file.h` wraps the OS file API (ADR-0011): `NphFile` holds the handle in
place; sequential and positional reads and writes loop until satisfied or
the file ends (short reads only at the end; writes complete or fail);
seek/tell/size/truncate/flush; `nph_file_read_all` (NUL-terminated for
text) and `nph_file_write_all`; memory mapping of any byte range in read,
shared read-write, or private copy-on-write mode over a page-aligned view;
stat, exists, remove, rename (replacing), copy (by the OS, into a
`.part` file renamed over the destination, so a reader never sees half a
copy), create/remove directories (single, recursive, whole tree), temp
and current directory, and a directory iterator that skips `.` and `..`. Windows opens files with every
share mode and converts paths to UTF-16 at the boundary;
`NPH_FILE_ASYNC` opens an overlapped handle for the completion-port backend
(its sequential calls then keep their own position). Expected outcomes
such as a missing file come back as results and are logged at DEBUG;
real failures are logged at ERROR with the OS message.

## Asynchronous I/O (`async_io.h`)

An `NphAsyncIo` context queues positional reads and writes and reports
completions through `nph_async_io_poll`, each carrying the caller's
64-bit `user` value, a result, and the byte count. Requests come from a
fixed pool (an MPMC ring of free indices, so submission from several
threads is lock-free); `FULL` tells a submitter to poll first. Returning
an index retries while the ring looks full, as it does for a moment when
a pop one lap behind is still copying out ([core.md](core.md) "Rings");
the thread pool's queue and completion rings do the same. Three
backends behind one vtable: **IOCP** (overlapped `ReadFile`/`WriteFile`
on a completion port; files are bound on first use; a request that fails
before being queued, including end-of-file, is posted to the port so every
request yields exactly one completion), **io_uring** (the rings mapped and
driven with raw `io_uring_setup`/`io_uring_enter`; one mutex on the
engine's side of the rings; timeouts through `IORING_ENTER_EXT_ARG`;
needs Linux 5.11+), and a **thread pool** (workers run the positional
calls and hand completions back through an MPMC ring and a semaphore),
which is the fallback when the native backend is unavailable and the
reference the others are tested against. `DEFAULT` picks the native
backend when it works. `nph_async_io_destroy` waits for in-flight requests
and discards their completions; there is no cancellation.

## Window and monitors (`window.h`)

The GLFW wrapper (ADR-0012); `src/window_glfw.c` is the only file in the
engine that includes a GLFW header, and GLFW itself is the pinned
submodule `third_party/glfw` built as part of the tree. The window system
is initialised once on the main thread (`NPH_RESULT_UNSUPPORTED` on a
headless machine); `nph_window_poll_events` runs the OS loop and fills
each window's 1024-event ring (32-byte events, oldest dropped and counted
on overflow), drained with `nph_window_pop_event`. Keys are the engine's
`NphKey` enum mapped from the OS codes; text arrives as code points.
Windows expose size, framebuffer size and content scale (the swapchain
uses the framebuffer), position, title, fullscreen on a monitor with the
windowed placement restored on the way back, show/hide/focus/minimise/
maximise, cursor modes (normal, hidden, disabled for mouse look) with raw
motion, cursor position, and the clipboard. Monitors report name,
position, mode, physical size, and scale. Gamepads are polled snapshots in
the standard layout. Vulkan hooks hand Phase 5 the required instance
extensions and a `VkSurfaceKHR` without any Vulkan header dependency.

Files dropped on a window arrive as one `NPH_WINDOW_EVENT_DROP` per drop,
which is how the editor imports content. Events carry no pointers, so a
drop carries the number of its first path and a count. The window copies
the paths and keeps the newest `NPH_WINDOW_DROP_PATHS` (256);
`nph_window_dropped_path` returns one by number, or NULL once newer drops
have replaced it. No OS lets a program drop files on its own window, so
the test drives the internal entry point the GLFW callback uses
(`src/window_internal.h`).

## Input actions (`input.h`)

`NphInput` turns raw events and gamepad snapshots into per-frame state
(held, pressed this frame, released this frame; cursor and scroll deltas;
typed text) and into named actions. A binding is a key with modifiers
(exact or `NPH_MOD_ANY`), a mouse button, a key pair as an axis, a mouse
axis with a scale, a gamepad button, or a gamepad axis with a dead zone,
on one pad or any; an action sums its bindings (digital ±1, analog
values) clamped to [-1, 1], plus unclamped scaled mouse deltas. The frame
cycle is `begin_frame`, events through `process_event`, gamepads through
`poll_gamepads` (or `set_gamepad`), `end_frame`, then queries. Losing
focus releases everything held. The layer is window-system independent
and tested with synthetic events.

`samples/platform_window` is the Phase 3 exit sample: it opens a window,
binds quit/fullscreen/fire/jump/move/look to keyboard, mouse, and gamepad,
and logs every event and action edge; `--frames=N` bounds it for CI,
where Linux runs it under Xvfb.

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

`benchmarks/platform/bench_platform.c`, Profile, Ryzen 7 5800X, Windows 10,
MSVC 14.44, 2026-10-12, median of 5
(`benchmarks/results/platform-2026-10-12-win-msvc-profile-ryzen7-5800x.json`).

| Benchmark | ns/op | Note |
|---|---|---|
| `fiber_switch` (assembly) | 11 | one switch of a two-fiber ping-pong; 38 before the x87 control word was dropped (ADR-0010 §8) |
| `fiber_switch` (Win32 fiber API, `NPH_FIBERS_WIN32`) | 24 | the OS routine does switch the x87 word |
| `mutex_lock_unlock` | 3.9 | SRW lock, uncontended |
| `spin_lock_acquire_release` | 1.9 | core's spin lock, for comparison |
| `semaphore_post_wait` | 3.8 | the user-space fast path: no kernel transition |
| `semaphore_round_trip` | 47 | hand-off to another thread and back, both sides spinning |
| `thread_create_join` | 50 500 | Windows thread creation; never on a frame path |
| `time_now_ns` | 24 | `QueryPerformanceCounter` plus the conversion |
| `time_cycles` | 7.4 | `rdtsc` |
| `vmem_commit_decommit_64k` | 2 670 | two system calls plus one page fault |
| `file_read_at_4k` | 5 020 | synchronous positional read from the page cache |
| `file_write_at_4k` | 7 590 | synchronous positional write, no flush |
| `async_read_4k_depth_1` | 6 640 | IOCP, one request in flight |
| `async_read_4k_depth_32` | 6 330 | IOCP, 32 in flight, per request |
| `async_read_4k_threads_32` | 3 030 | thread pool (4 workers), 32 in flight, per request |

Reading: the fiber switch is where the job system's budget goes, and 11 ns
means a job can yield and resume a few hundred times per frame at 240 FPS
without registering. The semaphore's fast path makes a post with nobody
waiting as cheap as a mutex; the 47 ns round trip is the floor for waking a
parked worker on another core. The file numbers are Windows page-cache
numbers (an NTFS read through the filter stack costs about 5 us even when
it copies from memory): a cached 4 KiB read through IOCP costs about the
same as the synchronous call plus port overhead because the file system
completes it inline on the submitting thread, while the thread pool
parallelises cached reads across its workers. The native backend earns its
place on real device reads, where requests overlap the device latency
instead of the CPU; Phase 6 measures the streamer against both.

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
- `test_fiber.c`: create/switch/resume/destroy, a 20 000-round ping-pong
  with ordering checks, callee-saved registers and MXCSR surviving switches
  past a fiber that churns both, deep recursion and a 20 KiB frame (the
  stack probe) on a 256 KiB stack, 256 fibers round robin, a fiber
  migrating across three threads, and independent fiber sets on eight
  threads at once. The same file runs against the Win32 backend in CI.
- `test_path.c`: the pure path helpers on both separators, roots, and
  overflow.
- `test_file.c`: a 1 MiB write/read round trip, open modes (missing,
  exclusive, append, truncate, directory as file), positional I/O with
  short reads at the end, seek/tell/truncate, the overlapped-handle
  sequential path, memory mapping in all three modes including an
  unaligned sub-range and write-through, nested directories, listing,
  rename-over, busy and not-found results, tree removal, whole-file
  helpers, and copies (large, replacing, empty, and failing without a
  trace). Each test works in its own temp directory.
- `test_async_io.c`: for every backend available (the thread pool
  everywhere, IOCP on Windows, io_uring on Linux) a 4 MiB file is read with
  65 chunks in flight through a 32-deep queue (so `FULL` is exercised),
  end-of-file and short reads, timeouts, and an asynchronous copy of the
  file; plus the unsupported-backend results, destroy with requests in
  flight, the IOCP non-async-file rejection, and four threads submitting
  concurrently while the main thread polls.
- `test_input.c`: key edges across frames (press, repeat, release, tap
  within a frame), mouse buttons, motion deltas, scroll, typed text and
  its cap, focus loss releasing everything, action registration and
  limits, key bindings with exact and any modifiers, axes combining and
  clamping (key pairs, gamepad sticks with dead zones, mouse deltas),
  gamepad buttons on specific and any pads, the name tables.
- `test_window.c` (skipped without a display; CI provides one): init
  idempotence, a hidden window's sizes and state, cursor modes, the event
  queue after pumping and a programmatic resize, monitors, the clipboard
  round trip, gamepad and Vulkan queries without hardware.
- `test_vmem.c`, `test_cpu.c`, `test_dylib.c` (loads
  `platform_test_dylib`, a shared library built by the test CMake),
  `test_locked_alloc.c` (TLSF through the lock from eight threads, then
  `nph_tlsf_validate`), `test_log_thread.c`, `test_platform.c`.

Assertions are made on the main thread only; workers record into shared
atomics. Work counts scale by `TEST_SCALE` (1 in Debug, where the
sanitizers slow atomics down; 4 elsewhere).
