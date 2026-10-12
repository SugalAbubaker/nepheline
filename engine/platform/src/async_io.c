// SPDX-License-Identifier: Apache-2.0
// Asynchronous I/O: the context, the request pool, and the thread-pool
// backend (docs/adr/0011-async-io.md).
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/async_io.h>
#include <nepheline/platform/time.h>

#include "async_io_internal.h"
#include "platform_internal.h"

#include <string.h>

#define DEFAULT_QUEUE_DEPTH    256U
#define DEFAULT_WORKER_THREADS 2U
#define MAX_WORKER_THREADS     16U

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

void nph_async_io_complete(NphAsyncIo *io, NphAsyncIoRequest *req, NphResult result, u32 bytes,
                           NphAsyncIoCompletion *out)
{
    out->user = req->user;
    out->result = result;
    out->bytes = bytes;
    const u32 index = req->index;
    NPH_VERIFY(nph_mpmc_ring_push(&io->free_slots, &index));
    nph_atomic_fetch_sub_u32(&io->pending, 1U, NPH_MEMORY_ORDER_ACQ_REL);
}

static NphAsyncIoRequest *acquire_request(NphAsyncIo *io)
{
    u32 index = 0U;
    if (!nph_mpmc_ring_pop(&io->free_slots, &index)) {
        return NULL;
    }
    NphAsyncIoRequest *req = &io->requests[index];
    memset(req, 0, sizeof *req);
    req->index = index;
    return req;
}

static void release_request(NphAsyncIo *io, NphAsyncIoRequest *req)
{
    const u32 index = req->index;
    NPH_VERIFY(nph_mpmc_ring_push(&io->free_slots, &index));
}

static const NphAsyncIoBackendOps *native_ops(void)
{
#if NPH_PLATFORM_WINDOWS
    return &nph_async_io_iocp_ops;
#else
    return &nph_async_io_uring_ops;
#endif
}

NphResult nph_async_io_create(NphAllocator *alloc, const NphAsyncIoConfig *config, NphAsyncIo **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    *out = NULL;
    NphAsyncIoConfig cfg;
    if (config != NULL) {
        cfg = *config;
    } else {
        memset(&cfg, 0, sizeof cfg);
    }
    if (cfg.queue_depth == 0U) {
        cfg.queue_depth = DEFAULT_QUEUE_DEPTH;
    }
    if (!nph_is_pow2_u32(cfg.queue_depth) || cfg.queue_depth < 2U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (cfg.worker_threads == 0U) {
        cfg.worker_threads = DEFAULT_WORKER_THREADS;
    }
    if (cfg.worker_threads > MAX_WORKER_THREADS) {
        cfg.worker_threads = MAX_WORKER_THREADS;
    }

    NphAsyncIo *io = NPH_ALLOC_TYPE(alloc, NphAsyncIo);
    if (io == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    io->alloc = alloc;
    io->queue_depth = cfg.queue_depth;
    io->worker_threads = cfg.worker_threads;
    io->requests = NPH_ALLOC_ARRAY_ZEROED(alloc, NphAsyncIoRequest, cfg.queue_depth);
    if (io->requests == NULL) {
        NPH_FREE_TYPE(alloc, NphAsyncIo, io);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphResult r = nph_mpmc_ring_init(&io->free_slots, alloc, sizeof(u32), cfg.queue_depth);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(alloc, NphAsyncIoRequest, io->requests, cfg.queue_depth);
        NPH_FREE_TYPE(alloc, NphAsyncIo, io);
        return r;
    }
    for (u32 i = 0; i < cfg.queue_depth; ++i) {
        NPH_VERIFY(nph_mpmc_ring_push(&io->free_slots, &i));
    }
    nph_atomic_store_u32(&io->pending, 0U, NPH_MEMORY_ORDER_RELAXED);

    // Backend selection: DEFAULT tries the native one and falls back.
    const bool wants_native = cfg.backend == NPH_ASYNC_IO_BACKEND_DEFAULT ||
                              cfg.backend == NPH_ASYNC_IO_BACKEND_IOCP ||
                              cfg.backend == NPH_ASYNC_IO_BACKEND_IO_URING;
    r = NPH_RESULT_UNSUPPORTED;
    if (wants_native) {
#if NPH_PLATFORM_WINDOWS
        const bool available = cfg.backend != NPH_ASYNC_IO_BACKEND_IO_URING;
        const NphAsyncIoBackend native = NPH_ASYNC_IO_BACKEND_IOCP;
#else
        const bool available = cfg.backend != NPH_ASYNC_IO_BACKEND_IOCP;
        const NphAsyncIoBackend native = NPH_ASYNC_IO_BACKEND_IO_URING;
#endif
        if (available) {
            io->ops = native_ops();
            io->backend = native;
            r = io->ops->init(io);
        }
        if (r != NPH_RESULT_OK && cfg.backend != NPH_ASYNC_IO_BACKEND_DEFAULT) {
            nph_mpmc_ring_free(&io->free_slots);
            NPH_FREE_ARRAY(alloc, NphAsyncIoRequest, io->requests, cfg.queue_depth);
            NPH_FREE_TYPE(alloc, NphAsyncIo, io);
            return r;
        }
        if (r != NPH_RESULT_OK) {
            NPH_LOG_INFO(&NPH_LOG_PLATFORM,
                         "async I/O: native backend unavailable (%s), "
                         "using the thread pool",
                         nph_result_string(r));
        }
    }
    if (r != NPH_RESULT_OK) {
        io->ops = &nph_async_io_threads_ops;
        io->backend = NPH_ASYNC_IO_BACKEND_THREADS;
        r = io->ops->init(io);
        if (r != NPH_RESULT_OK) {
            nph_mpmc_ring_free(&io->free_slots);
            NPH_FREE_ARRAY(alloc, NphAsyncIoRequest, io->requests, cfg.queue_depth);
            NPH_FREE_TYPE(alloc, NphAsyncIo, io);
            return r;
        }
    }
    *out = io;
    return NPH_RESULT_OK;
}

void nph_async_io_destroy(NphAsyncIo *io)
{
    if (io == NULL) {
        return;
    }
    // Drain: completions of requests nobody will collect are discarded.
    NphAsyncIoCompletion scratch[16];
    while (nph_atomic_load_u32(&io->pending, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
        (void)io->ops->poll(io, scratch, (u32)NPH_ARRAY_COUNT(scratch), 10U * NPH_TIME_NS_PER_MS);
    }
    io->ops->shutdown(io);
    nph_mpmc_ring_free(&io->free_slots);
    NPH_FREE_ARRAY(io->alloc, NphAsyncIoRequest, io->requests, io->queue_depth);
    NPH_FREE_TYPE(io->alloc, NphAsyncIo, io);
}

NphAsyncIoBackend nph_async_io_backend(const NphAsyncIo *io)
{
    NPH_ASSERT(io != NULL);
    return io->backend;
}

const char *nph_async_io_backend_name(NphAsyncIoBackend backend)
{
    switch (backend) {
    case NPH_ASYNC_IO_BACKEND_DEFAULT:
        return "default";
    case NPH_ASYNC_IO_BACKEND_THREADS:
        return "threads";
    case NPH_ASYNC_IO_BACKEND_IOCP:
        return "iocp";
    case NPH_ASYNC_IO_BACKEND_IO_URING:
        return "io_uring";
    default:
        return "?";
    }
}

static NphResult submit(NphAsyncIo *io, NphFile *file, u64 offset, void *buf, u32 size, u64 user,
                        bool is_write)
{
    NPH_ASSERT(io != NULL && file != NULL && file->open && (buf != NULL || size == 0U));
    NphAsyncIoRequest *req = acquire_request(io);
    if (req == NULL) {
        return NPH_RESULT_FULL;
    }
    req->file = file;
    req->offset = offset;
    req->buf = buf;
    req->size = size;
    req->user = user;
    req->is_write = is_write;
    // Counted before the backend sees it: a completion may land on another
    // thread before submit returns.
    nph_atomic_fetch_add_u32(&io->pending, 1U, NPH_MEMORY_ORDER_ACQ_REL);
    const NphResult r = io->ops->submit(io, req);
    if (r != NPH_RESULT_OK) {
        nph_atomic_fetch_sub_u32(&io->pending, 1U, NPH_MEMORY_ORDER_ACQ_REL);
        release_request(io, req);
    }
    return r;
}

NphResult nph_async_io_read(NphAsyncIo *io, NphFile *file, u64 offset, void *buf, u32 size,
                            u64 user)
{
    return submit(io, file, offset, buf, size, user, false);
}

NphResult nph_async_io_write(NphAsyncIo *io, NphFile *file, u64 offset, const void *buf, u32 size,
                             u64 user)
{
    // The buffer is only read; the OS primitives take a non-const pointer.
    union {
        const void *read_only;
        void *writable;
    } u;
    u.read_only = buf;
    return submit(io, file, offset, u.writable, size, user, true);
}

u32 nph_async_io_poll(NphAsyncIo *io, NphAsyncIoCompletion *out, u32 max, u64 timeout_ns)
{
    NPH_ASSERT(io != NULL && out != NULL);
    if (max == 0U) {
        return 0U;
    }
    return io->ops->poll(io, out, max, timeout_ns);
}

u32 nph_async_io_pending(const NphAsyncIo *io)
{
    NPH_ASSERT(io != NULL);
    return nph_atomic_load_u32(&io->pending, NPH_MEMORY_ORDER_ACQUIRE);
}

// ---------------------------------------------------------------------------
// Thread-pool backend: workers perform the positional calls from file.h.
// ---------------------------------------------------------------------------

typedef struct ThreadsDone {
    u32 index;
    u32 bytes;
    NphResult result;
} ThreadsDone;

typedef struct ThreadsState {
    NphThread threads[MAX_WORKER_THREADS];
    u32 thread_count;
    NphMpmcRing queue; // u32 request indices
    NphSemaphore work; // one permit per queued request (plus wake-ups at shutdown)
    NphMpmcRing done;  // ThreadsDone
    NphSemaphore done_sem;
    NphAtomicU32 stop;
    NphAsyncIo *io;
} ThreadsState;

static void threads_worker(void *user)
{
    ThreadsState *st = (ThreadsState *)user;
    for (;;) {
        nph_semaphore_wait(&st->work);
        // A permit means an element was (or is being) pushed. The MPMC ring
        // reports "empty" while a producer that claimed an earlier cell is
        // still writing it, so retry rather than drop the permit's element.
        // The shutdown permits carry no element and end the loop.
        u32 index = 0U;
        while (!nph_mpmc_ring_pop(&st->queue, &index)) {
            if (nph_atomic_load_u32(&st->stop, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
                return;
            }
            nph_cpu_relax();
        }
        NphAsyncIoRequest *req = &st->io->requests[index];
        u64 done = 0U;
        const NphResult r =
            req->is_write ? nph_file_write_at(req->file, req->offset, req->buf, req->size, &done)
                          : nph_file_read_at(req->file, req->offset, req->buf, req->size, &done);
        ThreadsDone record;
        record.index = index;
        record.bytes = (u32)done;
        record.result = r;
        // The ring holds queue_depth entries and at most that many requests
        // exist, so this never fails.
        NPH_VERIFY(nph_mpmc_ring_push(&st->done, &record));
        nph_semaphore_post(&st->done_sem);
    }
}

static NphResult threads_init(NphAsyncIo *io)
{
    ThreadsState *st = NPH_ALLOC_TYPE(io->alloc, ThreadsState);
    if (st == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    st->io = io;
    NphResult r = nph_mpmc_ring_init(&st->queue, io->alloc, sizeof(u32), io->queue_depth);
    if (r == NPH_RESULT_OK) {
        r = nph_mpmc_ring_init(&st->done, io->alloc, sizeof(ThreadsDone), io->queue_depth);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_semaphore_init(&st->work, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_semaphore_init(&st->done_sem, 0U);
    }
    if (r != NPH_RESULT_OK) {
        nph_mpmc_ring_free(&st->queue);
        nph_mpmc_ring_free(&st->done);
        NPH_FREE_TYPE(io->alloc, ThreadsState, st);
        return r;
    }
    nph_atomic_store_u32(&st->stop, 0U, NPH_MEMORY_ORDER_RELAXED);
    for (u32 i = 0; i < io->worker_threads; ++i) {
        NphThreadDesc desc;
        desc.fn = threads_worker;
        desc.user = st;
        desc.name = "async-io";
        desc.stack_size = 0U;
        r = nph_thread_create(&st->threads[i], &desc);
        if (r != NPH_RESULT_OK) {
            break;
        }
        st->thread_count = i + 1U;
    }
    if (r != NPH_RESULT_OK) {
        io->backend_state = st;
        nph_async_io_threads_ops.shutdown(io);
        return r;
    }
    io->backend_state = st;
    return NPH_RESULT_OK;
}

static void threads_shutdown(NphAsyncIo *io)
{
    ThreadsState *st = (ThreadsState *)io->backend_state;
    if (st == NULL) {
        return;
    }
    nph_atomic_store_u32(&st->stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    if (st->thread_count > 0U) {
        nph_semaphore_post_n(&st->work, st->thread_count);
    }
    for (u32 i = 0; i < st->thread_count; ++i) {
        nph_thread_join(&st->threads[i]);
    }
    nph_semaphore_destroy(&st->done_sem);
    nph_semaphore_destroy(&st->work);
    nph_mpmc_ring_free(&st->done);
    nph_mpmc_ring_free(&st->queue);
    NPH_FREE_TYPE(io->alloc, ThreadsState, st);
    io->backend_state = NULL;
}

static NphResult threads_submit(NphAsyncIo *io, NphAsyncIoRequest *req)
{
    ThreadsState *st = (ThreadsState *)io->backend_state;
    const u32 index = req->index;
    if (!nph_mpmc_ring_push(&st->queue, &index)) {
        return NPH_RESULT_FULL;
    }
    nph_semaphore_post(&st->work);
    return NPH_RESULT_OK;
}

static u32 threads_poll(NphAsyncIo *io, NphAsyncIoCompletion *out, u32 max, u64 timeout_ns)
{
    ThreadsState *st = (ThreadsState *)io->backend_state;
    u32 count = 0U;
    // One permit per completion: wait for the first, take the rest only
    // when their permits are already there.
    if (!nph_semaphore_wait_timeout(&st->done_sem, timeout_ns)) {
        return 0U;
    }
    for (;;) {
        ThreadsDone record;
        // As in the worker: the permit guarantees the element, the ring may
        // need a moment to show it while another worker finishes its push.
        while (!nph_mpmc_ring_pop(&st->done, &record)) {
            nph_cpu_relax();
        }
        nph_async_io_complete(io, &io->requests[record.index], record.result, record.bytes,
                              &out[count]);
        ++count;
        if (count >= max || !nph_semaphore_try_wait(&st->done_sem)) {
            break;
        }
    }
    return count;
}

const NphAsyncIoBackendOps nph_async_io_threads_ops = {threads_init, threads_shutdown,
                                                       threads_submit, threads_poll};
