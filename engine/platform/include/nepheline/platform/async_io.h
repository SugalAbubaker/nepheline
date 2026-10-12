// SPDX-License-Identifier: Apache-2.0
#pragma once

// Asynchronous file I/O (CLAUDE.md §8.3 "async (IOCP on Windows, io_uring
// with a thread-pool fallback on Linux)"; docs/adr/0011-async-io.md).
//
// An NphAsyncIo context accepts positional reads and writes and reports
// their completions later, so the asset streamer (Phase 6) keeps many
// requests in flight without a thread per request. Three backends share
// the API:
//   - IOCP (Windows): overlapped ReadFile/WriteFile on a completion port.
//   - io_uring (Linux 5.11+): the submission and completion rings, driven
//     with raw system calls (no liburing).
//   - A thread pool (every platform): workers perform the positional calls
//     from file.h. The default where the native backend is unavailable
//     (older kernels, io_uring disabled by policy) and a reference for
//     testing the others.
//
// Requests carry a 64-bit `user` value back in their completion; buffers
// must stay valid until then. Sizes are limited to 4 GiB per request,
// which is also what the OS primitives take; streaming reads are chunked
// anyway. A read past the end completes with fewer bytes (0 at the end)
// and NPH_RESULT_OK, like nph_file_read_at.
//
// Files used with the IOCP backend must be opened with NPH_FILE_ASYNC;
// the other backends accept any readable or writable file. The context
// never closes files.
//
// Thread-safety: submit and poll are NPH_THREADSAFE. The io_uring backend
// serialises ring access with a mutex (documented here per CLAUDE.md
// §1.4); IOCP and the thread pool are lock-free past the request pool,
// which is an MPMC ring.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/platform/file.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphAsyncIo NphAsyncIo;

typedef enum NphAsyncIoBackend {
    NPH_ASYNC_IO_BACKEND_DEFAULT, // the native backend, or the thread pool when unavailable
    NPH_ASYNC_IO_BACKEND_THREADS,
    NPH_ASYNC_IO_BACKEND_IOCP,     // Windows only
    NPH_ASYNC_IO_BACKEND_IO_URING, // Linux only
} NphAsyncIoBackend;

typedef struct NphAsyncIoConfig {
    NphAsyncIoBackend backend;
    u32 queue_depth;    // requests in flight at once, power of two (0: 256)
    u32 worker_threads; // thread pool only (0: 2)
} NphAsyncIoConfig;

typedef struct NphAsyncIoCompletion {
    u64 user;         // as passed to the request
    NphResult result; // NPH_RESULT_OK or the failure
    u32 bytes;        // transferred
} NphAsyncIoCompletion;

// Creates a context from `alloc` (the request pool and backend state).
// NPH_RESULT_UNSUPPORTED when an explicitly requested native backend is
// unavailable on this machine; DEFAULT never fails for that reason.
NPH_MUST_USE NphResult nph_async_io_create(NphAllocator *alloc, const NphAsyncIoConfig *config,
                                           NphAsyncIo **out);

// Waits for every in-flight request, then releases the context.
void nph_async_io_destroy(NphAsyncIo *io);

NphAsyncIoBackend nph_async_io_backend(const NphAsyncIo *io);
const char *nph_async_io_backend_name(NphAsyncIoBackend backend);

// Queues a read of `size` bytes at `offset` into `buf`. NPH_RESULT_FULL when
// queue_depth requests are in flight; try again after polling.
NPH_MUST_USE NphResult nph_async_io_read(NphAsyncIo *io, NphFile *file, u64 offset, void *buf,
                                         u32 size, u64 user);

// Queues a write of `size` bytes from `buf` at `offset`.
NPH_MUST_USE NphResult nph_async_io_write(NphAsyncIo *io, NphFile *file, u64 offset,
                                          const void *buf, u32 size, u64 user);

// Collects up to `max` completions into `out`, waiting up to `timeout_ns`
// for the first one (0: do not wait). Returns the number collected.
u32 nph_async_io_poll(NphAsyncIo *io, NphAsyncIoCompletion *out, u32 max, u64 timeout_ns);

// Requests submitted but not yet collected.
u32 nph_async_io_pending(const NphAsyncIo *io);

#ifdef __cplusplus
}
#endif
