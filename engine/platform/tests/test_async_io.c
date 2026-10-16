// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/async_io.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include "async_io_internal.h"
#include "test_platform_support.h"

#include <stdio.h>
#include <string.h>

#define CHUNK       NPH_KIB(64)
#define CHUNK_COUNT 64U
#define FILE_SIZE   (CHUNK * CHUNK_COUNT + 777U) // a partial last chunk

static u8 g_source[FILE_SIZE];
static u8 g_target[FILE_SIZE];

typedef struct AsyncFixture {
    char dir[1024];
    char path[1024];
    NphAllocator *alloc;
} AsyncFixture;

static u32 g_counter;

NTEST_SETUP(AsyncFixture)
{
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    snprintf(f->dir, sizeof f->dir, "%s/nph_async_test_%llu_%u", tmp,
             (unsigned long long)nph_platform_process_id(), g_counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_path_join(f->path, sizeof f->path, nph_str_from_cstr(f->dir),
                                      NPH_STR("data.bin"), NULL),
                        NPH_RESULT_OK);
    for (u64 i = 0; i < FILE_SIZE; ++i) {
        g_source[i] = (u8)((i * 2654435761ULL) >> 7U);
    }
    NTEST_ASSERT_EQ_INT(nph_file_write_all(f->path, g_source, FILE_SIZE), NPH_RESULT_OK);
    f->alloc = nph_allocator_system();
}

NTEST_TEARDOWN(AsyncFixture)
{
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(f->dir), NPH_RESULT_OK);
}

// Reads the whole file through `io` with every chunk in flight at once,
// then checks the contents. Returns the backend that served it.
static void read_everything(AsyncFixture *f, NphAsyncIo *io, NphFile *file)
{
    memset(g_target, 0, sizeof g_target);
    u32 submitted = 0U;
    u32 completed = 0U;
    u32 seen[CHUNK_COUNT + 1U];
    memset(seen, 0, sizeof seen);
    const u32 total = CHUNK_COUNT + 1U;
    while (completed < total) {
        while (submitted < total) {
            const u64 offset = (u64)submitted * CHUNK;
            const NphResult r =
                nph_async_io_read(io, file, offset, g_target + offset, (u32)CHUNK, submitted);
            if (r == NPH_RESULT_FULL) {
                break;
            }
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
            ++submitted;
        }
        NphAsyncIoCompletion done[16];
        const u32 n = nph_async_io_poll(io, done, (u32)NPH_ARRAY_COUNT(done), NPH_TIME_NS_PER_S);
        NTEST_ASSERT_GT_UINT(n, 0U); // never a full second without progress
        for (u32 i = 0; i < n; ++i) {
            NTEST_ASSERT_EQ_INT(done[i].result, NPH_RESULT_OK);
            NTEST_ASSERT_LT_UINT(done[i].user, total);
            const u32 expected = done[i].user == CHUNK_COUNT ? 777U : (u32)CHUNK;
            NTEST_ASSERT_EQ_UINT(done[i].bytes, expected);
            seen[done[i].user] += 1U;
            ++completed;
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_async_io_pending(io), 0U);
    for (u32 i = 0; i < total; ++i) {
        NTEST_ASSERT_EQ_UINT(seen[i], 1U); // each chunk exactly once
    }
    NTEST_ASSERT_EQ_MEM(g_target, g_source, FILE_SIZE);
    NPH_UNUSED(f);
}

static void exercise_backend(AsyncFixture *f, NphAsyncIoBackend backend)
{
    NphAsyncIoConfig config;
    config.backend = backend;
    config.queue_depth = 32U; // smaller than the chunk count: FULL is exercised
    config.worker_threads = 3U;
    NphAsyncIo *io = NULL;
    const NphResult cr = nph_async_io_create(f->alloc, &config, &io);
    if (cr == NPH_RESULT_UNSUPPORTED) {
        NTEST_SKIP("backend %s unavailable here", nph_async_io_backend_name(backend));
    }
    NTEST_ASSERT_EQ_INT(cr, NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(io);
    NTEST_LOG("backend %s", nph_async_io_backend_name(nph_async_io_backend(io)));
    if (backend != NPH_ASYNC_IO_BACKEND_DEFAULT) {
        NTEST_ASSERT_EQ_INT(nph_async_io_backend(io), backend);
    }

    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(f->path, NPH_FILE_READ | NPH_FILE_ASYNC, &file),
                        NPH_RESULT_OK);
    const NphStopwatch sw = nph_stopwatch_start();
    read_everything(f, io, &file);
    NTEST_LOG("%.2f ms for %u chunks", nph_stopwatch_elapsed_ms(&sw), CHUNK_COUNT + 1U);

    // A read past the end completes with no bytes; one crossing it is short.
    u8 scratch[1024];
    NTEST_ASSERT_EQ_INT(
        nph_async_io_read(io, &file, FILE_SIZE + 4096U, scratch, sizeof scratch, 1U),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, FILE_SIZE - 100U, scratch, sizeof scratch, 2U),
                        NPH_RESULT_OK);
    NphAsyncIoCompletion done[4];
    u32 got = 0U;
    while (got < 2U) {
        const u32 n = nph_async_io_poll(io, done + got, 2U - got, NPH_TIME_NS_PER_S);
        NTEST_ASSERT_GT_UINT(n, 0U);
        got += n;
    }
    for (u32 i = 0; i < 2U; ++i) {
        NTEST_ASSERT_EQ_INT(done[i].result, NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(done[i].bytes, done[i].user == 1U ? 0U : 100U);
    }
    // Nothing pending: a poll with a timeout returns 0 after waiting.
    const NphStopwatch idle = nph_stopwatch_start();
    NTEST_ASSERT_EQ_UINT(nph_async_io_poll(io, done, 4U, 20U * NPH_TIME_NS_PER_MS), 0U);
    NTEST_ASSERT_GE_UINT(nph_stopwatch_elapsed_ns(&idle), 5U * NPH_TIME_NS_PER_MS);
    NTEST_ASSERT_EQ_UINT(nph_async_io_poll(io, done, 4U, 0U), 0U);
    nph_file_close(&file);

    // Writes: build a copy of the file asynchronously, then compare.
    char copy[1024];
    NTEST_ASSERT_EQ_INT(
        nph_path_join(copy, sizeof copy, nph_str_from_cstr(f->dir), NPH_STR("copy.bin"), NULL),
        NPH_RESULT_OK);
    NphFile out;
    NTEST_ASSERT_EQ_INT(
        nph_file_open(copy, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_TRUNCATE | NPH_FILE_ASYNC,
                      &out),
        NPH_RESULT_OK);
    u32 submitted = 0U;
    u32 completed = 0U;
    const u32 total = CHUNK_COUNT + 1U;
    while (completed < total) {
        while (submitted < total) {
            const u64 offset = (u64)submitted * CHUNK;
            const u32 size = submitted == CHUNK_COUNT ? 777U : (u32)CHUNK;
            const NphResult r = nph_async_io_write(io, &out, offset, g_source + offset, size, 0U);
            if (r == NPH_RESULT_FULL) {
                break;
            }
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
            ++submitted;
        }
        NphAsyncIoCompletion batch[8];
        const u32 n = nph_async_io_poll(io, batch, 8U, NPH_TIME_NS_PER_S);
        NTEST_ASSERT_GT_UINT(n, 0U);
        for (u32 i = 0; i < n; ++i) {
            NTEST_ASSERT_EQ_INT(batch[i].result, NPH_RESULT_OK);
            NTEST_ASSERT_GT_UINT(batch[i].bytes, 0U);
            ++completed;
        }
    }
    nph_file_close(&out);
    void *data = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(copy, f->alloc, &data, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(size, FILE_SIZE);
    NTEST_ASSERT_EQ_MEM(data, g_source, FILE_SIZE);
    nph_file_free_all(f->alloc, data, size);

    nph_async_io_destroy(io);
}

NTEST_F(AsyncFixture, default_backend)
{
    exercise_backend(f, NPH_ASYNC_IO_BACKEND_DEFAULT);
}

NTEST_F(AsyncFixture, thread_pool_backend)
{
    exercise_backend(f, NPH_ASYNC_IO_BACKEND_THREADS);
}

NTEST_F(AsyncFixture, iocp_backend)
{
    exercise_backend(f, NPH_ASYNC_IO_BACKEND_IOCP);
}

NTEST_F(AsyncFixture, io_uring_backend)
{
    exercise_backend(f, NPH_ASYNC_IO_BACKEND_IO_URING);
}

NTEST_F(AsyncFixture, foreign_native_backend_is_unsupported)
{
#if NPH_PLATFORM_WINDOWS
    const NphAsyncIoBackend foreign = NPH_ASYNC_IO_BACKEND_IO_URING;
#else
    const NphAsyncIoBackend foreign = NPH_ASYNC_IO_BACKEND_IOCP;
#endif
    NphAsyncIoConfig config;
    config.backend = foreign;
    config.queue_depth = 0U;
    config.worker_threads = 0U;
    NphAsyncIo *io = NULL;
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, &config, &io), NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_NULL(io);
    config.backend = NPH_ASYNC_IO_BACKEND_THREADS;
    config.queue_depth = 3U; // not a power of two
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, &config, &io), NPH_RESULT_INVALID_ARGUMENT);
}

NTEST_F(AsyncFixture, destroy_with_requests_in_flight_drains_them)
{
    NphAsyncIo *io = NULL;
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, NULL, &io), NPH_RESULT_OK);
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(f->path, NPH_FILE_READ | NPH_FILE_ASYNC, &file),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < 16U; ++i) {
        NTEST_ASSERT_EQ_INT(
            nph_async_io_read(io, &file, (u64)i * CHUNK, g_target + (u64)i * CHUNK, (u32)CHUNK, i),
            NPH_RESULT_OK);
    }
    NTEST_ASSERT_GT_UINT(nph_async_io_pending(io), 0U);
    nph_async_io_destroy(io); // waits for the 16 reads, discards their completions
    nph_file_close(&file);
    NTEST_ASSERT_EQ_MEM(g_target, g_source, 16U * CHUNK); // they did complete
}

#if NPH_PLATFORM_WINDOWS
NTEST_F(AsyncFixture, iocp_rejects_files_opened_without_async)
{
    NphAsyncIoConfig config;
    config.backend = NPH_ASYNC_IO_BACKEND_IOCP;
    config.queue_depth = 0U;
    config.worker_threads = 0U;
    NphAsyncIo *io = NULL;
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, &config, &io), NPH_RESULT_OK);
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(f->path, NPH_FILE_READ, &file), NPH_RESULT_OK);
    u8 scratch[64];
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, 0U, scratch, sizeof scratch, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(nph_async_io_pending(io), 0U);
    nph_file_close(&file);
    nph_async_io_destroy(io);
}
#endif

// Several threads submit reads of the same file at once while the main
// thread polls.
typedef struct Submitters {
    NphAsyncIo *io;
    NphFile *file;
    u32 per_thread;
    NphAtomicU32 failures;
} Submitters;

static void submitter(u32 index, void *shared)
{
    Submitters *s = (Submitters *)shared;
    for (u32 i = 0; i < s->per_thread; ++i) {
        const u32 chunk = (index * 7U + i) % CHUNK_COUNT;
        u8 *dst = g_target + (u64)chunk * CHUNK;
        NphResult r;
        do {
            r = nph_async_io_read(s->io, s->file, (u64)chunk * CHUNK, dst, (u32)CHUNK,
                                  ((u64)index << 32U) | i);
            if (r == NPH_RESULT_FULL) {
                nph_thread_yield();
            }
        } while (r == NPH_RESULT_FULL);
        if (r != NPH_RESULT_OK) {
            nph_atomic_fetch_add_u32(&s->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
    }
}

NTEST_F(AsyncFixture, concurrent_submission)
{
    NphAsyncIo *io = NULL;
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, NULL, &io), NPH_RESULT_OK);
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(f->path, NPH_FILE_READ | NPH_FILE_ASYNC, &file),
                        NPH_RESULT_OK);
    Submitters s;
    s.io = io;
    s.file = &file;
    s.per_thread = 50U * TEST_SCALE;
    nph_atomic_store_u32(&s.failures, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker workers[4];
    test_threads_start(workers, 4U, submitter, &s);
    const u32 expected = 4U * s.per_thread;
    u32 completed = 0U;
    NphAsyncIoCompletion done[32];
    const NphStopwatch sw = nph_stopwatch_start();
    while (completed < expected && nph_stopwatch_elapsed_ns(&sw) < 30U * NPH_TIME_NS_PER_S) {
        const u32 n = nph_async_io_poll(io, done, 32U, 10U * NPH_TIME_NS_PER_MS);
        for (u32 i = 0; i < n; ++i) {
            NTEST_ASSERT_EQ_INT(done[i].result, NPH_RESULT_OK);
            NTEST_ASSERT_EQ_UINT(done[i].bytes, (u32)CHUNK);
        }
        completed += n;
    }
    test_threads_join(workers, 4U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(completed, expected);
    NTEST_ASSERT_EQ_UINT(nph_async_io_pending(io), 0U);
    NTEST_ASSERT_EQ_MEM(g_target, g_source, CHUNK * CHUNK_COUNT);
    nph_file_close(&file);
    nph_async_io_destroy(io);
}

// Many small reads, each into its own place, from more threads than the
// queue is deep: request slots go back to the pool while other threads are
// taking them, so a push into the pool's ring can meet a pop one lap
// behind that has not finished (core/ring.h). That once failed a check
// which took the ring for full.
#define SMALL_READ       64U
#define SMALL_SUBMITTERS 8U
#define SMALL_PER_THREAD (2000U * TEST_SCALE)

static u8 g_small[SMALL_SUBMITTERS * SMALL_PER_THREAD * SMALL_READ];

static u64 small_offset(u32 k)
{
    return ((u64)k * 4099U) % (FILE_SIZE - SMALL_READ);
}

static void small_submitter(u32 index, void *shared)
{
    Submitters *s = (Submitters *)shared;
    for (u32 i = 0; i < s->per_thread; ++i) {
        const u32 k = index * s->per_thread + i;
        NphResult r;
        do {
            r = nph_async_io_read(s->io, s->file, small_offset(k), g_small + (u64)k * SMALL_READ,
                                  SMALL_READ, k);
            if (r == NPH_RESULT_FULL) {
                nph_thread_yield();
            }
        } while (r == NPH_RESULT_FULL);
        if (r != NPH_RESULT_OK) {
            nph_atomic_fetch_add_u32(&s->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
    }
}

static void contend_on_a_shallow_queue(AsyncFixture *f, NphAsyncIoBackend backend)
{
    NphAsyncIoConfig config;
    memset(&config, 0, sizeof config);
    config.backend = backend;
    config.queue_depth = 2U;
    config.worker_threads = 2U;
    NphAsyncIo *io = NULL;
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, &config, &io), NPH_RESULT_OK);
    NTEST_LOG("backend %s", nph_async_io_backend_name(nph_async_io_backend(io)));
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(f->path, NPH_FILE_READ | NPH_FILE_ASYNC, &file),
                        NPH_RESULT_OK);
    memset(g_small, 0, sizeof g_small);
    Submitters s;
    s.io = io;
    s.file = &file;
    s.per_thread = SMALL_PER_THREAD;
    nph_atomic_store_u32(&s.failures, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker workers[SMALL_SUBMITTERS];
    test_threads_start(workers, SMALL_SUBMITTERS, small_submitter, &s);
    const u32 expected = SMALL_SUBMITTERS * SMALL_PER_THREAD;
    u32 completed = 0U;
    NphAsyncIoCompletion done[4];
    const NphStopwatch sw = nph_stopwatch_start();
    while (completed < expected && nph_stopwatch_elapsed_ns(&sw) < 60U * NPH_TIME_NS_PER_S) {
        const u32 n = nph_async_io_poll(io, done, 4U, 10U * NPH_TIME_NS_PER_MS);
        for (u32 i = 0; i < n; ++i) {
            NTEST_ASSERT_EQ_INT(done[i].result, NPH_RESULT_OK);
            NTEST_ASSERT_EQ_UINT(done[i].bytes, SMALL_READ);
        }
        completed += n;
    }
    test_threads_join(workers, SMALL_SUBMITTERS);
    NTEST_LOG("%u reads in %.2f ms", completed, nph_stopwatch_elapsed_ms(&sw));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(completed, expected);
    NTEST_ASSERT_EQ_UINT(nph_async_io_pending(io), 0U);
    for (u32 k = 0; k < expected; ++k) {
        NTEST_ASSERT_EQ_MEM(g_small + (u64)k * SMALL_READ, g_source + small_offset(k), SMALL_READ);
    }
    nph_file_close(&file);
    nph_async_io_destroy(io);
}

NTEST_F(AsyncFixture, a_shallow_queue_under_contention)
{
    contend_on_a_shallow_queue(f, NPH_ASYNC_IO_BACKEND_DEFAULT);
}

NTEST_F(AsyncFixture, a_shallow_thread_pool_queue_under_contention)
{
    contend_on_a_shallow_queue(f, NPH_ASYNC_IO_BACKEND_THREADS);
}

// White-box: the request pool's ring as a pop preempted midway leaves it.
// Slot 0's pop has claimed its cell and stalled; slot 1 was taken after it,
// and its read completes. Returning slot 1 lands on the stalled pop's cell,
// so the completion waits for that pop rather than taking the ring for
// full (the stress tests above meet this by chance; here it is certain).
typedef struct StalledPoll {
    NphAsyncIo *io;
    NphAsyncIoCompletion done;
    NphAtomicU32 polled;
} StalledPoll;

static void stalled_poller(u32 index, void *shared)
{
    NPH_UNUSED(index);
    StalledPoll *p = (StalledPoll *)shared;
    while (nph_async_io_poll(p->io, &p->done, 1U, 10U * NPH_TIME_NS_PER_MS) == 0U) {
    }
    nph_atomic_store_u32(&p->polled, 1U, NPH_MEMORY_ORDER_RELEASE);
}

NTEST_F(AsyncFixture, a_completion_waits_for_a_stalled_slot_pop)
{
    NphAsyncIoConfig config;
    memset(&config, 0, sizeof config);
    config.backend = NPH_ASYNC_IO_BACKEND_THREADS;
    config.queue_depth = 2U;
    config.worker_threads = 1U;
    NphAsyncIo *io = NULL;
    NTEST_ASSERT_EQ_INT(nph_async_io_create(f->alloc, &config, &io), NPH_RESULT_OK);
    NphFile file;
    NTEST_ASSERT_EQ_INT(nph_file_open(f->path, NPH_FILE_READ, &file), NPH_RESULT_OK);

    // The stalled pop: position 0 (slot 0) claimed, its cell not released.
    NphMpmcRing *slots = &io->free_slots;
    nph_atomic_store_u32(&slots->dequeue_pos, 1U, NPH_MEMORY_ORDER_RELAXED);
    u8 buf[SMALL_READ];
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, 4096U, buf, SMALL_READ, 7U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, 0U, buf, SMALL_READ, 8U), NPH_RESULT_FULL);

    StalledPoll p;
    memset(&p, 0, sizeof p);
    p.io = io;
    nph_atomic_store_u32(&p.polled, 0U, NPH_MEMORY_ORDER_RELAXED);
    TestWorker poller;
    test_threads_start(&poller, 1U, stalled_poller, &p);
    nph_thread_sleep_ns(50U * NPH_TIME_NS_PER_MS);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.polled, NPH_MEMORY_ORDER_ACQUIRE), 0U);
    NTEST_ASSERT_EQ_UINT(nph_async_io_pending(io), 1U);

    // The pop finishes: its cell passes to the next lap, and slot 1's
    // return goes through. Then the pop's owner gives slot 0 back.
    NphAtomicU32 *cell0 = (NphAtomicU32 *)slots->cells;
    nph_atomic_store_u32(cell0, slots->mask + 1U, NPH_MEMORY_ORDER_RELEASE);
    test_threads_join(&poller, 1U);
    const u32 slot0 = 0U;
    NTEST_ASSERT_TRUE(nph_mpmc_ring_push(slots, &slot0));

    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.polled, NPH_MEMORY_ORDER_ACQUIRE), 1U);
    NTEST_ASSERT_EQ_INT(p.done.result, NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(p.done.user, 7U);
    NTEST_ASSERT_EQ_UINT(p.done.bytes, SMALL_READ);
    NTEST_ASSERT_EQ_MEM(buf, g_source + 4096U, SMALL_READ);
    NTEST_ASSERT_EQ_UINT(nph_async_io_pending(io), 0U);
    NTEST_ASSERT_EQ_UINT(nph_mpmc_ring_count(slots), 2U);

    // Both slots serve reads again.
    u8 two[2][SMALL_READ];
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, 0U, two[0], SMALL_READ, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_async_io_read(io, &file, 512U, two[1], SMALL_READ, 1U), NPH_RESULT_OK);
    u32 got = 0U;
    NphAsyncIoCompletion done[2];
    while (got < 2U) {
        const u32 n = nph_async_io_poll(io, done + got, 2U - got, NPH_TIME_NS_PER_S);
        NTEST_ASSERT_GT_UINT(n, 0U);
        got += n;
    }
    NTEST_ASSERT_EQ_MEM(two[0], g_source, SMALL_READ);
    NTEST_ASSERT_EQ_MEM(two[1], g_source + 512U, SMALL_READ);
    nph_file_close(&file);
    nph_async_io_destroy(io);
}
