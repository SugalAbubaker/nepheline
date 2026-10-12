// SPDX-License-Identifier: Apache-2.0
// io_uring backend over raw system calls (docs/adr/0011-async-io.md; Axboe,
// "Efficient IO with io_uring", 2019). No liburing: the rings are mapped
// and driven here, which keeps the engine free of a dependency for ~150
// lines of code.
//
// Ring protocol: the kernel reads submission entries between the head it
// advances and the tail we publish with a release store; it writes
// completion entries between the tail it publishes and the head we
// advance. One mutex serialises our side of both rings.
#include <nepheline/core/assert.h>
#include <nepheline/platform/async_io.h>
#include <nepheline/platform/time.h>

#include "async_io_internal.h"
#include "platform_internal.h"

#include <linux/io_uring.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct UringState {
    int fd;
    u8 *ring;
    u64 ring_size;
    struct io_uring_sqe *sqes;
    u64 sqes_size;
    u32 *sq_head;
    u32 *sq_tail;
    u32 *sq_mask;
    u32 *sq_array;
    u32 *cq_head;
    u32 *cq_tail;
    u32 *cq_mask;
    struct io_uring_cqe *cqes;
    u32 entries;
    NphMutex mutex;
} UringState;

static int uring_setup(u32 entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int uring_enter(int fd, u32 to_submit, u32 min_complete, u32 flags, const void *arg,
                       size_t argsz)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
}

static u32 load_acquire(const u32 *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_release(u32 *p, u32 v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static NphResult uring_init(NphAsyncIo *io)
{
    struct io_uring_params params;
    memset(&params, 0, sizeof params);
    const int fd = uring_setup(io->queue_depth, &params);
    if (fd < 0) {
        // ENOSYS: no io_uring in this kernel; EPERM: disabled by policy
        // (kernel.io_uring_disabled, seccomp). Both mean "use the thread pool".
        return NPH_RESULT_UNSUPPORTED;
    }
    const u32 needed = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_EXT_ARG | IORING_FEAT_NODROP;
    if ((params.features & needed) != needed) {
        close(fd);
        return NPH_RESULT_UNSUPPORTED; // kernel older than 5.11
    }
    UringState *st = NPH_ALLOC_TYPE(io->alloc, UringState);
    if (st == NULL) {
        close(fd);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    st->fd = fd;
    st->entries = params.sq_entries;
    const u64 sq_size = params.sq_off.array + (u64)params.sq_entries * sizeof(u32);
    const u64 cq_size = params.cq_off.cqes + (u64)params.cq_entries * sizeof(struct io_uring_cqe);
    st->ring_size = sq_size > cq_size ? sq_size : cq_size;
    st->ring = mmap(NULL, (size_t)st->ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                    fd, IORING_OFF_SQ_RING);
    if (st->ring == MAP_FAILED) {
        const NphResult r = nph_posix_fail("mmap(io_uring sq ring)", errno);
        close(fd);
        NPH_FREE_TYPE(io->alloc, UringState, st);
        return r;
    }
    st->sqes_size = (u64)params.sq_entries * sizeof(struct io_uring_sqe);
    st->sqes = mmap(NULL, (size_t)st->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                    fd, IORING_OFF_SQES);
    if (st->sqes == MAP_FAILED) {
        const NphResult r = nph_posix_fail("mmap(io_uring sqes)", errno);
        munmap(st->ring, (size_t)st->ring_size);
        close(fd);
        NPH_FREE_TYPE(io->alloc, UringState, st);
        return r;
    }
    st->sq_head = (u32 *)(st->ring + params.sq_off.head);
    st->sq_tail = (u32 *)(st->ring + params.sq_off.tail);
    st->sq_mask = (u32 *)(st->ring + params.sq_off.ring_mask);
    st->sq_array = (u32 *)(st->ring + params.sq_off.array);
    st->cq_head = (u32 *)(st->ring + params.cq_off.head);
    st->cq_tail = (u32 *)(st->ring + params.cq_off.tail);
    st->cq_mask = (u32 *)(st->ring + params.cq_off.ring_mask);
    st->cqes = (struct io_uring_cqe *)(st->ring + params.cq_off.cqes);
    nph_mutex_init(&st->mutex);
    io->backend_state = st;
    return NPH_RESULT_OK;
}

static void uring_shutdown(NphAsyncIo *io)
{
    UringState *st = (UringState *)io->backend_state;
    if (st == NULL) {
        return;
    }
    nph_mutex_destroy(&st->mutex);
    munmap(st->sqes, (size_t)st->sqes_size);
    munmap(st->ring, (size_t)st->ring_size);
    close(st->fd);
    NPH_FREE_TYPE(io->alloc, UringState, st);
    io->backend_state = NULL;
}

static NphResult uring_submit(NphAsyncIo *io, NphAsyncIoRequest *req)
{
    UringState *st = (UringState *)io->backend_state;
    nph_mutex_lock(&st->mutex);
    const u32 tail = *st->sq_tail;
    const u32 head = load_acquire(st->sq_head);
    if (tail - head >= st->entries) {
        nph_mutex_unlock(&st->mutex);
        return NPH_RESULT_FULL;
    }
    const u32 index = tail & *st->sq_mask;
    struct io_uring_sqe *sqe = &st->sqes[index];
    memset(sqe, 0, sizeof *sqe);
    sqe->opcode = req->is_write ? IORING_OP_WRITE : IORING_OP_READ;
    sqe->fd = (int)req->file->native;
    sqe->off = req->offset;
    sqe->addr = (u64)(uptr)req->buf;
    sqe->len = req->size;
    sqe->user_data = req->index;
    st->sq_array[index] = index;
    store_release(st->sq_tail, tail + 1U);
    const int submitted = uring_enter(st->fd, 1U, 0U, 0U, NULL, 0U);
    NphResult r = NPH_RESULT_OK;
    if (submitted < 0) {
        const int error = errno;
        // Nothing was consumed; withdraw the entry.
        store_release(st->sq_tail, tail);
        r = error == EAGAIN || error == EBUSY ? NPH_RESULT_FULL
                                              : nph_posix_fail("io_uring_enter", error);
    }
    nph_mutex_unlock(&st->mutex);
    return r;
}

static u32 uring_poll(NphAsyncIo *io, NphAsyncIoCompletion *out, u32 max, u64 timeout_ns)
{
    UringState *st = (UringState *)io->backend_state;
    nph_mutex_lock(&st->mutex);
    u32 head = *st->cq_head;
    u32 tail = load_acquire(st->cq_tail);
    if (head == tail && timeout_ns > 0U) {
        struct __kernel_timespec ts;
        ts.tv_sec = (i64)(timeout_ns / NPH_TIME_NS_PER_S);
        ts.tv_nsec = (i64)(timeout_ns % NPH_TIME_NS_PER_S);
        struct io_uring_getevents_arg arg;
        memset(&arg, 0, sizeof arg);
        arg.ts = (u64)(uptr)&ts;
        // ETIME on timeout, EINTR on a signal: both just mean "look again".
        (void)uring_enter(st->fd, 0U, 1U, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg,
                          sizeof arg);
        tail = load_acquire(st->cq_tail);
    }
    u32 count = 0U;
    while (head != tail && count < max) {
        const struct io_uring_cqe *cqe = &st->cqes[head & *st->cq_mask];
        NphAsyncIoRequest *req = &io->requests[cqe->user_data];
        NphResult result = NPH_RESULT_OK;
        u32 bytes = 0U;
        if (cqe->res < 0) {
            result = nph_posix_result(-cqe->res);
        } else {
            bytes = (u32)cqe->res;
        }
        ++head;
        nph_async_io_complete(io, req, result, bytes, &out[count]);
        ++count;
    }
    store_release(st->cq_head, head);
    nph_mutex_unlock(&st->mutex);
    return count;
}

const NphAsyncIoBackendOps nph_async_io_uring_ops = {uring_init, uring_shutdown, uring_submit,
                                                     uring_poll};
