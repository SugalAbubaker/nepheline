// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by async_io.c and the backends. The context owns a fixed pool of
// request records handed out through an MPMC ring of free indices, so
// submission from several threads needs no lock of its own; each backend
// adds its state behind `backend_state`.

#include <nepheline/core/ring.h>
#include <nepheline/platform/async_io.h>
#include <nepheline/platform/thread.h>

typedef struct NphAsyncIoRequest {
    NPH_ALIGNAS(8) u8 overlapped[32]; // OVERLAPPED on Windows; unused elsewhere
    NphFile *file;
    u64 offset;
    void *buf;
    u64 user;
    u32 size;
    u32 index;
    b32 is_write;
    b32 posted; // IOCP: completion posted by the backend itself (immediate failure or EOF)
    NphResult posted_result;
} NphAsyncIoRequest;

typedef struct NphAsyncIoBackendOps {
    // Prepares backend state; NPH_RESULT_UNSUPPORTED when the OS lacks it.
    NphResult (*init)(NphAsyncIo *io);
    void (*shutdown)(NphAsyncIo *io);
    // Queues a filled request; on failure the caller releases it.
    NphResult (*submit)(NphAsyncIo *io, NphAsyncIoRequest *req);
    u32 (*poll)(NphAsyncIo *io, NphAsyncIoCompletion *out, u32 max, u64 timeout_ns);
} NphAsyncIoBackendOps;

struct NphAsyncIo {
    NphAllocator *alloc;
    NphAsyncIoBackend backend;
    const NphAsyncIoBackendOps *ops;
    u32 queue_depth;
    u32 worker_threads;
    NphAsyncIoRequest *requests; // queue_depth records
    NphMpmcRing free_slots;      // u32 indices of unused records
    NphAtomicU32 pending;
    void *backend_state;
};

// Fills a completion for `req` and returns its record to the pool.
void nph_async_io_complete(NphAsyncIo *io, NphAsyncIoRequest *req, NphResult result, u32 bytes,
                           NphAsyncIoCompletion *out);

extern const NphAsyncIoBackendOps nph_async_io_threads_ops;
#if NPH_PLATFORM_WINDOWS
extern const NphAsyncIoBackendOps nph_async_io_iocp_ops;
#else
extern const NphAsyncIoBackendOps nph_async_io_uring_ops;
#endif
