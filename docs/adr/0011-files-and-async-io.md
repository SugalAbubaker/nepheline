# 0011. Files, memory mapping, and asynchronous I/O backends

- **Date:** 2026-10-12
- **Status:** Accepted

## Context

CLAUDE.md §8.3 asks for synchronous and asynchronous file I/O ("IOCP on
Windows, io_uring with a thread-pool fallback on Linux") and memory-mapped
files. §8.8 makes asset loads memory-mapped with no parsing step and
streaming "async I/O feeding job-based decompression", and §10.5 says
streaming must never hitch a frame. §1.8 keeps third-party code behind
the engine's interfaces, and §3 lists no I/O library, so io_uring must be
driven without liburing.

## Decision

1. **One file API, UTF-8 paths, 64-bit everything.** `NphFile` holds the OS
   handle in place. Sequential and positional (`*_at`) reads and writes,
   seek/tell/size/truncate/flush, whole-file helpers, and a file system
   layer (stat, exists, remove, rename, directories, listing, temp and
   current directory) map to `CreateFileW`/`ReadFile`/`WriteFile` with
   `OVERLAPPED` offsets on Windows and `open`/`read`/`write`/`pread`/
   `pwrite` on Linux. Reads loop until the request is satisfied or the
   file ends; writes loop until complete. Windows files are opened with
   every share mode so an editor holding a file open never breaks a cook,
   matching POSIX behaviour. `NphDirIterator` carries the OS handle and no
   heap memory (`fstatat` resolves entry types the file system does not
   report).
2. **Expected failures are not errors.** A missing file, an existing
   file, a denied path, or a busy directory come back as results
   (`NOT_FOUND`, `ALREADY_EXISTS`, `ACCESS_DENIED`, `BUSY`) and are logged
   at DEBUG; everything else is logged at ERROR with the OS message.
3. **`NPH_FILE_ASYNC` selects the Windows handle type.** Overlapped handles
   are required by IOCP and change how synchronous calls behave, so the
   flag is explicit; the sequential API on such a handle keeps its own
   position and completes each call through an event whose low bit is set
   (which keeps the completion off the port). Linux ignores the flag.
4. **Memory mapping is page-aligned internally** and byte-addressed
   externally: the caller asks for any `[offset, size)` and gets a pointer
   into an aligned view. Three access modes: read, shared read-write, and
   private copy-on-write (for patching cooked data in place without
   touching the file).
5. **Asynchronous I/O has three backends behind one API** (`async_io.h`):
   a fixed pool of request records (an MPMC ring of free indices makes
   submission lock-free), a 64-bit `user` value returned in each
   completion, `poll` with a timeout. The backends:
   - **IOCP** (Windows): files are bound to the port on first use;
     requests that fail before being queued (including end-of-file) are
     posted to the port by the backend so every request yields exactly one
     completion.
   - **io_uring** (Linux 5.11+, needs `IORING_FEAT_SINGLE_MMAP`,
     `IORING_FEAT_EXT_ARG`, and `IORING_FEAT_NODROP`): the rings are mapped
     and driven with `io_uring_setup`/`io_uring_enter` directly; a mutex
     serialises the engine's side of both rings (stated in the header, per
     §1.4). Timeouts use `IORING_ENTER_EXT_ARG`.
   - **Thread pool** (both): workers run the positional calls and hand
     completions back through an MPMC ring and a semaphore. It is the
     fallback when the native backend is unavailable (older kernels,
     `kernel.io_uring_disabled`, seccomp) and the reference the native
     backends are tested against.
   `DEFAULT` tries the native backend and falls back with an INFO log; an
   explicitly requested backend that is unavailable is `UNSUPPORTED`.
6. **Requests are at most 4 GiB and complete in one piece.** The OS
   primitives take 32-bit lengths; streaming reads are chunked by the
   streamer anyway, and a request that fits a single syscall keeps the
   completion semantics simple (one completion, one byte count).
7. **No request cancellation.** `nph_async_io_destroy` waits for in-flight
   requests and discards their completions. The streamer (Phase 6) keeps
   its own notion of stale requests; cancellation support in the OS APIs
   is uneven (`CancelIoEx` is per handle, `IORING_OP_ASYNC_CANCEL` is best
   effort) and would complicate the completion contract.

## Alternatives considered

- **liburing.** Rejected: a dependency for about 150 lines, and it would
  need an ADR under §3's third-party policy; the raw interface is stable
  and documented.
- **Linux AIO (`io_submit`).** Rejected: works only with `O_DIRECT` for
  true asynchrony and is superseded by io_uring on every kernel the
  engine targets (Ubuntu 22.04+).
- **A per-request callback instead of polling.** Rejected: callbacks
  would run on the backend's threads (or the kernel's) and need their
  own synchronisation; the streamer polls from its I/O thread and
  dispatches jobs from there.
- **Growing the request pool on demand.** Rejected: §1.4; the depth is a
  configuration value and `FULL` tells the submitter to poll first.

## Consequences

- Phase 6's loader memory-maps `.nph` files through `nph_file_map` and
  streams through `nph_async_io_*` from one I/O thread, chunking at a
  size tuned by the benchmarks in `docs/platform.md`.
- The thread-pool backend makes every async test run on every CI
  machine; the native backends run where available (IOCP always on
  Windows; io_uring on the hosted Ubuntu runners, which allow it).
- GPU decompression (§8.8) and direct storage paths are later research
  and would arrive as additional backends behind the same API.
