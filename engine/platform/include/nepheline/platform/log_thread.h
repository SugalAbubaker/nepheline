// SPDX-License-Identifier: Apache-2.0
#pragma once

// The logger thread (docs/adr/0005-atomics-in-core.md §4): core owns the
// per-thread log rings, the sinks, and nph_log_flush; this thread calls
// flush at a fixed interval so that log calls on every other thread stay a
// ring push and never touch a sink. The runtime starts it right after
// nph_log_init and stops it just before nph_log_shutdown. Tools and tests
// that do not run it call nph_log_flush themselves.
//
// Thread-safety: start and stop are NPH_MAIN_THREAD_ONLY; nph_log_thread_kick
// is NPH_THREADSAFE.

#include <nepheline/core/result.h>
#include <nepheline/platform/thread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphLogThread {
    NphThread thread;
    NphSemaphore wake;
    NphAtomicU32 stop;
    u64 interval_ns;
    b32 running;
} NphLogThread;

// Starts the thread, which flushes every `interval_ms` milliseconds (0: 10).
// Requires nph_log_is_initialized(). NPH_RESULT_OUT_OF_MEMORY if the thread
// cannot be created.
NPH_MUST_USE NphResult nph_log_thread_start(NphLogThread *lt, u32 interval_ms);

// Stops the thread and performs a final flush on the calling thread, so
// everything logged before this call has reached the sinks when it
// returns. Safe on a zero-filled or already stopped object.
void nph_log_thread_stop(NphLogThread *lt);

// Asks for a flush as soon as possible instead of at the next interval,
// for callers that just logged something urgent.
void nph_log_thread_kick(NphLogThread *lt);

#ifdef __cplusplus
}
#endif
