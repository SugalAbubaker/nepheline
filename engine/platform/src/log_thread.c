// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/time.h>

#include <string.h>

#define DEFAULT_INTERVAL_MS 10U

static void log_thread_main(void *user)
{
    NphLogThread *lt = (NphLogThread *)user;
    for (;;) {
        // Flush before checking the flag so a stop request still drains
        // what was logged just before it.
        nph_log_flush(0U);
        if (nph_atomic_load_u32(&lt->stop, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
            return;
        }
        (void)nph_semaphore_wait_timeout(&lt->wake, lt->interval_ns);
    }
}

NphResult nph_log_thread_start(NphLogThread *lt, u32 interval_ms)
{
    NPH_ASSERT(lt != NULL);
    NPH_ASSERT_MSG(nph_log_is_initialized(), "nph_log_init must run before the logger thread");
    NPH_ASSERT(!lt->running);
    memset(lt, 0, sizeof *lt);
    lt->interval_ns = nph_time_ms_to_ns(interval_ms != 0U ? interval_ms : DEFAULT_INTERVAL_MS);
    NPH_TRY(nph_semaphore_init(&lt->wake, 0U));
    nph_atomic_store_u32(&lt->stop, 0U, NPH_MEMORY_ORDER_RELAXED);
    NphThreadDesc desc;
    desc.fn = log_thread_main;
    desc.user = lt;
    desc.name = "logger";
    desc.stack_size = 0U;
    const NphResult r = nph_thread_create(&lt->thread, &desc);
    if (r != NPH_RESULT_OK) {
        nph_semaphore_destroy(&lt->wake);
        return r;
    }
    (void)nph_thread_set_priority(&lt->thread, NPH_THREAD_PRIORITY_LOW);
    lt->running = 1;
    return NPH_RESULT_OK;
}

void nph_log_thread_stop(NphLogThread *lt)
{
    NPH_ASSERT(lt != NULL);
    if (!lt->running) {
        return;
    }
    nph_atomic_store_u32(&lt->stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    nph_semaphore_post(&lt->wake);
    nph_thread_join(&lt->thread);
    nph_semaphore_destroy(&lt->wake);
    lt->running = 0;
    // The thread's last flush ran before it saw the flag; anything logged
    // since then is delivered here, on the caller's thread.
    nph_log_flush(0U);
}

void nph_log_thread_kick(NphLogThread *lt)
{
    NPH_ASSERT(lt != NULL);
    if (lt->running) {
        nph_semaphore_post(&lt->wake);
    }
}
