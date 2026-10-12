// SPDX-License-Identifier: Apache-2.0
// Platform benchmarks (CLAUDE.md §1.5; docs/platform.md "Benchmarks").
//
//   fiber_switch              one context switch (ping-pong, two fibers)
//   mutex_lock_unlock         uncontended lock + unlock
//   semaphore_post_wait       post then wait on the same thread (fast path)
//   semaphore_round_trip      hand-off to another thread and back
//   spin_lock_acquire_release uncontended, for comparison with the mutex
//   thread_create_join        start a thread that returns at once, join it
//   time_now_ns               one monotonic clock reading
//   time_cycles               one rdtsc
//   vmem_commit_decommit_64k  commit 16 pages, touch one, decommit
//   file_read_at_4k           positional read of 4 KiB from the page cache
//   file_write_at_4k          positional write of 4 KiB (no flush)
//   async_read_4k_depth_1     submit one 4 KiB read, poll it
//   async_read_4k_depth_32    32 reads in flight, per read
//   async_read_4k_threads_32  the same on the thread-pool backend

#include <nepheline/platform/async_io.h>
#include <nepheline/platform/fiber.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/vmem.h>

#include <nbench/nbench.h>

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Fibers
// ---------------------------------------------------------------------------

typedef struct PingPong {
    NphFiber main;
    NphFiber other;
    u64 count;
} PingPong;

static void pong_fn(void *user)
{
    PingPong *p = (PingPong *)user;
    for (;;) {
        p->count += 1U;
        nph_fiber_switch(&p->other, &p->main);
    }
}

NBENCH(fiber_switch)
{
    PingPong p;
    memset(&p, 0, sizeof p);
    nph_fiber_init_thread(&p.main);
    NphFiberDesc desc;
    desc.fn = pong_fn;
    desc.user = &p;
    desc.stack_size = 0U;
    desc.name = "bench";
    if (nph_fiber_create(&p.other, &desc) != NPH_RESULT_OK) {
        nph_fiber_release_thread(&p.main);
        return;
    }
    nbench_set_items_per_iteration(state, 2U); // there and back
    nbench_set_label(state, nph_fiber_backend_name());
    while (nbench_keep_running(state)) {
        nph_fiber_switch(&p.main, &p.other);
    }
    nbench_do_not_optimize_u64(p.count);
    nph_fiber_destroy(&p.other);
    nph_fiber_release_thread(&p.main);
}

// ---------------------------------------------------------------------------
// Synchronisation
// ---------------------------------------------------------------------------

NBENCH(mutex_lock_unlock)
{
    NphMutex m;
    nph_mutex_init(&m);
    u64 counter = 0U;
    while (nbench_keep_running(state)) {
        nph_mutex_lock(&m);
        counter += 1U;
        nph_mutex_unlock(&m);
    }
    nbench_do_not_optimize_u64(counter);
    nph_mutex_destroy(&m);
}

NBENCH(spin_lock_acquire_release)
{
    NphSpinLock lock;
    nph_spin_lock_init(&lock);
    u64 counter = 0U;
    while (nbench_keep_running(state)) {
        nph_spin_lock_acquire(&lock);
        counter += 1U;
        nph_spin_lock_release(&lock);
    }
    nbench_do_not_optimize_u64(counter);
}

NBENCH(semaphore_post_wait)
{
    NphSemaphore s;
    if (nph_semaphore_init(&s, 0U) != NPH_RESULT_OK) {
        return;
    }
    while (nbench_keep_running(state)) {
        nph_semaphore_post(&s);
        nph_semaphore_wait(&s);
    }
    nph_semaphore_destroy(&s);
}

typedef struct RoundTrip {
    NphSemaphore ping;
    NphSemaphore pong;
    NphAtomicU32 stop;
} RoundTrip;

static void round_trip_worker(void *user)
{
    RoundTrip *r = (RoundTrip *)user;
    for (;;) {
        nph_semaphore_wait(&r->ping);
        if (nph_atomic_load_u32(&r->stop, NPH_MEMORY_ORDER_ACQUIRE) != 0U) {
            return;
        }
        nph_semaphore_post(&r->pong);
    }
}

NBENCH(semaphore_round_trip)
{
    RoundTrip r;
    if (nph_semaphore_init(&r.ping, 0U) != NPH_RESULT_OK ||
        nph_semaphore_init(&r.pong, 0U) != NPH_RESULT_OK) {
        return;
    }
    nph_atomic_store_u32(&r.stop, 0U, NPH_MEMORY_ORDER_RELAXED);
    NphThreadDesc desc;
    desc.fn = round_trip_worker;
    desc.user = &r;
    desc.name = "bench-pong";
    desc.stack_size = 0U;
    NphThread t;
    if (nph_thread_create(&t, &desc) != NPH_RESULT_OK) {
        return;
    }
    while (nbench_keep_running(state)) {
        nph_semaphore_post(&r.ping);
        nph_semaphore_wait(&r.pong);
    }
    nph_atomic_store_u32(&r.stop, 1U, NPH_MEMORY_ORDER_RELEASE);
    nph_semaphore_post(&r.ping);
    nph_thread_join(&t);
    nph_semaphore_destroy(&r.pong);
    nph_semaphore_destroy(&r.ping);
}

static void empty_thread(void *user)
{
    NPH_UNUSED(user);
}

NBENCH(thread_create_join)
{
    NphThreadDesc desc;
    desc.fn = empty_thread;
    desc.user = NULL;
    desc.name = "bench-empty";
    desc.stack_size = 0U;
    while (nbench_keep_running(state)) {
        NphThread t;
        if (nph_thread_create(&t, &desc) == NPH_RESULT_OK) {
            nph_thread_join(&t);
        }
    }
}

// ---------------------------------------------------------------------------
// Clocks and memory
// ---------------------------------------------------------------------------

NBENCH(time_now_ns)
{
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_time_now_ns();
    }
    nbench_do_not_optimize_u64(acc);
}

NBENCH(time_cycles)
{
    u64 acc = 0U;
    while (nbench_keep_running(state)) {
        acc += nph_time_cycles();
    }
    nbench_do_not_optimize_u64(acc);
}

NBENCH(vmem_commit_decommit_64k)
{
    const u64 size = NPH_KIB(64);
    void *base = NULL;
    if (nph_vmem_reserve(NPH_MIB(1), &base) != NPH_RESULT_OK) {
        return;
    }
    while (nbench_keep_running(state)) {
        if (nph_vmem_commit(base, size) == NPH_RESULT_OK) {
            ((volatile u8 *)base)[0] = 1U;
            (void)nph_vmem_decommit(base, size);
        }
    }
    (void)nph_vmem_release(base, NPH_MIB(1));
}

// ---------------------------------------------------------------------------
// Files: a 4 MiB scratch file in the temp directory, read from the page
// cache (what the asset streamer sees for warm data).
// ---------------------------------------------------------------------------

#define BENCH_FILE_SIZE NPH_MIB(4)
#define BENCH_BLOCK     ((u64)4096U)

typedef struct BenchFile {
    char path[1024];
    NphFile file;
    bool ok;
} BenchFile;

static u8 g_block[BENCH_BLOCK];
static u8 g_blocks[32][BENCH_BLOCK];

static void bench_file_open(BenchFile *bf, u32 mode)
{
    bf->ok = false;
    char tmp[800];
    if (nph_fs_temp_directory(tmp, sizeof tmp, NULL) != NPH_RESULT_OK) {
        return;
    }
    snprintf(bf->path, sizeof bf->path, "%s/nph_platform_bench_%llu.bin", tmp,
             (unsigned long long)nph_platform_process_id());
    if (!nph_fs_exists(bf->path)) {
        NphFile f;
        if (nph_file_open(bf->path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_TRUNCATE, &f) !=
            NPH_RESULT_OK) {
            return;
        }
        static u8 chunk[NPH_KIB(64)];
        memset(chunk, 0x42, sizeof chunk);
        for (u64 off = 0; off < BENCH_FILE_SIZE; off += sizeof chunk) {
            if (nph_file_write(&f, chunk, sizeof chunk, NULL) != NPH_RESULT_OK) {
                nph_file_close(&f);
                return;
            }
        }
        nph_file_close(&f);
    }
    bf->ok = nph_file_open(bf->path, mode, &bf->file) == NPH_RESULT_OK;
}

static void bench_file_close(BenchFile *bf)
{
    nph_file_close(&bf->file);
    (void)nph_fs_remove_file(bf->path);
}

NBENCH(file_read_at_4k)
{
    BenchFile bf;
    bench_file_open(&bf, NPH_FILE_READ | NPH_FILE_RANDOM);
    if (!bf.ok) {
        return;
    }
    u64 offset = 0U;
    while (nbench_keep_running(state)) {
        u64 got = 0U;
        (void)nph_file_read_at(&bf.file, offset, g_block, BENCH_BLOCK, &got);
        offset = (offset + 64U * BENCH_BLOCK) % BENCH_FILE_SIZE;
        nbench_do_not_optimize(g_block);
    }
    bench_file_close(&bf);
}

NBENCH(file_write_at_4k)
{
    BenchFile bf;
    bench_file_open(&bf, NPH_FILE_READ | NPH_FILE_WRITE | NPH_FILE_RANDOM);
    if (!bf.ok) {
        return;
    }
    u64 offset = 0U;
    while (nbench_keep_running(state)) {
        (void)nph_file_write_at(&bf.file, offset, g_block, BENCH_BLOCK, NULL);
        offset = (offset + 64U * BENCH_BLOCK) % BENCH_FILE_SIZE;
    }
    bench_file_close(&bf);
}

static void async_read_depth(NbenchState *state, NphAsyncIoBackend backend, u32 depth)
{
    BenchFile bf;
    bench_file_open(&bf, NPH_FILE_READ | NPH_FILE_RANDOM | NPH_FILE_ASYNC);
    if (!bf.ok) {
        return;
    }
    NphAsyncIoConfig config;
    config.backend = backend;
    config.queue_depth = 64U;
    config.worker_threads = 4U;
    NphAsyncIo *io = NULL;
    if (nph_async_io_create(nph_allocator_system(), &config, &io) != NPH_RESULT_OK) {
        bench_file_close(&bf);
        return;
    }
    nbench_set_label(state, nph_async_io_backend_name(nph_async_io_backend(io)));
    nbench_set_items_per_iteration(state, depth);
    u64 offset = 0U;
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < depth; ++i) {
            (void)nph_async_io_read(io, &bf.file, offset, g_blocks[i], (u32)BENCH_BLOCK, i);
            offset = (offset + 64U * BENCH_BLOCK) % BENCH_FILE_SIZE;
        }
        u32 done = 0U;
        while (done < depth) {
            NphAsyncIoCompletion out[32];
            done += nph_async_io_poll(io, out, depth - done, NPH_TIME_NS_PER_S);
        }
    }
    nph_async_io_destroy(io);
    bench_file_close(&bf);
}

NBENCH(async_read_4k_depth_1)
{
    async_read_depth(state, NPH_ASYNC_IO_BACKEND_DEFAULT, 1U);
}

NBENCH(async_read_4k_depth_32)
{
    async_read_depth(state, NPH_ASYNC_IO_BACKEND_DEFAULT, 32U);
}

NBENCH(async_read_4k_threads_32)
{
    async_read_depth(state, NPH_ASYNC_IO_BACKEND_THREADS, 32U);
}
