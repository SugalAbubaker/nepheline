// SPDX-License-Identifier: Apache-2.0
// I/O completion port backend (docs/adr/0011-async-io.md).
#include <nepheline/core/assert.h>
#include <nepheline/platform/async_io.h>
#include <nepheline/platform/time.h>

#include "async_io_internal.h"
#include "platform_internal.h"

#include <string.h>

NPH_STATIC_ASSERT(sizeof(OVERLAPPED) <= 32U, "NphAsyncIoRequest.overlapped too small");

// NphFile.state bit: the handle is bound to a completion port.
#define FILE_STATE_IOCP_BOUND 1U

// At most this many completions per GetQueuedCompletionStatusEx call.
#define IOCP_BATCH 64U

typedef struct IocpState {
    HANDLE port;
    NphSpinLock bind_lock; // serialises first-use binding of a file
} IocpState;

static NphResult iocp_init(NphAsyncIo *io)
{
    IocpState *st = NPH_ALLOC_TYPE(io->alloc, IocpState);
    if (st == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    st->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0U, 0U);
    if (st->port == NULL) {
        const NphResult r = nph_win32_fail("CreateIoCompletionPort", GetLastError());
        NPH_FREE_TYPE(io->alloc, IocpState, st);
        return r;
    }
    nph_spin_lock_init(&st->bind_lock);
    io->backend_state = st;
    return NPH_RESULT_OK;
}

static void iocp_shutdown(NphAsyncIo *io)
{
    IocpState *st = (IocpState *)io->backend_state;
    if (st == NULL) {
        return;
    }
    CloseHandle(st->port);
    NPH_FREE_TYPE(io->alloc, IocpState, st);
    io->backend_state = NULL;
}

static NphResult bind_file(IocpState *st, NphFile *file)
{
    if ((file->state & FILE_STATE_IOCP_BOUND) != 0U) {
        return NPH_RESULT_OK;
    }
    nph_spin_lock_acquire(&st->bind_lock);
    NphResult r = NPH_RESULT_OK;
    if ((file->state & FILE_STATE_IOCP_BOUND) == 0U) {
        if (CreateIoCompletionPort((HANDLE)(uptr)file->native, st->port, 0U, 0U) == NULL) {
            r = nph_win32_fail("CreateIoCompletionPort(file)", GetLastError());
        } else {
            file->state |= FILE_STATE_IOCP_BOUND;
        }
    }
    nph_spin_lock_release(&st->bind_lock);
    return r;
}

static NphResult iocp_submit(NphAsyncIo *io, NphAsyncIoRequest *req)
{
    IocpState *st = (IocpState *)io->backend_state;
    if ((req->file->mode & NPH_FILE_ASYNC) == 0U) {
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "async I/O: the file was not opened with NPH_FILE_ASYNC");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(bind_file(st, req->file));
    OVERLAPPED *ov = (OVERLAPPED *)req->overlapped;
    memset(ov, 0, sizeof *ov);
    ov->Offset = (DWORD)(req->offset & 0xFFFFFFFFU);
    ov->OffsetHigh = (DWORD)(req->offset >> 32U);
    const HANDLE h = (HANDLE)(uptr)req->file->native;
    const BOOL ok = req->is_write ? WriteFile(h, req->buf, req->size, NULL, ov)
                                  : ReadFile(h, req->buf, req->size, NULL, ov);
    if (ok) {
        return NPH_RESULT_OK; // completed synchronously; the port still gets it
    }
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        return NPH_RESULT_OK;
    }
    // Failed before it was queued (reading at or past the end gives
    // ERROR_HANDLE_EOF here): deliver through the port like any other
    // completion so the caller sees one completion per request.
    req->posted = 1;
    req->posted_result = error == ERROR_HANDLE_EOF ? NPH_RESULT_OK : nph_win32_result(error);
    if (!PostQueuedCompletionStatus(st->port, 0U, 0U, ov)) {
        return nph_win32_fail("PostQueuedCompletionStatus", GetLastError());
    }
    return NPH_RESULT_OK;
}

static u32 iocp_poll(NphAsyncIo *io, NphAsyncIoCompletion *out, u32 max, u64 timeout_ns)
{
    IocpState *st = (IocpState *)io->backend_state;
    OVERLAPPED_ENTRY entries[IOCP_BATCH];
    const ULONG want = max > IOCP_BATCH ? IOCP_BATCH : max;
    const u64 ms = (timeout_ns + NPH_TIME_NS_PER_MS - 1U) / NPH_TIME_NS_PER_MS;
    const DWORD timeout_ms = ms >= INFINITE ? INFINITE - 1U : (DWORD)ms;
    ULONG n = 0U;
    if (!GetQueuedCompletionStatusEx(st->port, entries, want, &n, timeout_ms, FALSE)) {
        NPH_ASSERT(GetLastError() == WAIT_TIMEOUT);
        return 0U;
    }
    for (ULONG i = 0; i < n; ++i) {
        OVERLAPPED *ov = entries[i].lpOverlapped;
        NphAsyncIoRequest *req = (NphAsyncIoRequest *)(void *)ov; // overlapped is the first field
        NphResult result = NPH_RESULT_OK;
        u32 bytes = 0U;
        if (req->posted) {
            result = req->posted_result;
        } else {
            DWORD transferred = 0U;
            if (GetOverlappedResult((HANDLE)(uptr)req->file->native, ov, &transferred, FALSE)) {
                bytes = transferred;
            } else {
                const DWORD error = GetLastError();
                if (error == ERROR_HANDLE_EOF) {
                    bytes = transferred; // a short read at the end
                } else {
                    result = nph_win32_result(error);
                }
            }
        }
        nph_async_io_complete(io, req, result, bytes, &out[i]);
    }
    return (u32)n;
}

const NphAsyncIoBackendOps nph_async_io_iocp_ops = {iocp_init, iocp_shutdown, iocp_submit,
                                                    iocp_poll};
