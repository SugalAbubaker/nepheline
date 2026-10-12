// SPDX-License-Identifier: Apache-2.0
// Job system benchmarks (CLAUDE.md §1.5; §12 Phase 4 exit criterion "scales
// near-linearly with core count"; docs/jobs.md "Benchmarks").
//
//   counter_signal       add then signal a counter nobody waits on
//   run_wait_1_main      one job from the main thread: queue, wake a worker,
//                        run, wake the main thread
//   run_wait_1_worker    one child from inside a job: queue, park the fiber,
//                        run, switch back
//   run_wait_batch_256   256 trivial jobs from the main thread, per job
//   fork_256_worker      256 children from inside a job (the spawner's own
//                        deque; the other workers steal), per job
//   fork_join_depth_8    a binary tree of 511 nodes, each waiting on its two
//                        children, per node
//   io_run_wait_1        one I/O job and back
//   parallel_for_1M      sum one million integers in batches of 4096, per element
//   scaling_w<N>         a fixed amount of compute (256 pieces of 100 000
//                        dependent multiply-adds) on N workers: elapsed per
//                        run, so the speedup over scaling_w1 is the scaling
//
// All but the scaling series use the default worker count (logical cores
// minus one).

#include <nepheline/jobs/jobs.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/time.h>

#include <nbench/nbench.h>

#include <stdio.h>
#include <string.h>

static NphJobSystem *bench_create(u32 workers, u32 io_threads)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.worker_count = workers;
    desc.io_thread_count = io_threads;
    NphJobSystem *js = NULL;
    if (nph_job_system_create(&desc, &js) != NPH_RESULT_OK) {
        fprintf(stderr, "bench: job system creation failed\n");
        return NULL;
    }
    return js;
}

static void noop_job(void *user)
{
    NPH_UNUSED(user);
}

// Runs `decl` once from the calling thread and waits for it.
static void run_one(NphJobSystem *js, const NphJobDecl *decl)
{
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    if (nph_job_run(js, decl, 1U, NPH_JOB_PRIORITY_NORMAL, &c) == NPH_RESULT_OK) {
        nph_job_wait(js, &c);
    }
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

NBENCH(counter_signal)
{
    NphJobSystem *js = bench_create(1U, 1U);
    if (js == NULL) {
        return;
    }
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    while (nbench_keep_running(state)) {
        nph_job_counter_add(&c, 1U);
        nph_job_counter_signal(js, &c);
    }
    nbench_do_not_optimize_u64(nph_job_counter_value(&c));
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Spawn and wait
// ---------------------------------------------------------------------------

NBENCH(run_wait_1_main)
{
    NphJobSystem *js = bench_create(0U, 1U);
    if (js == NULL) {
        return;
    }
    const NphJobDecl decl = {noop_job, NULL, "noop"};
    while (nbench_keep_running(state)) {
        run_one(js, &decl);
    }
    nph_job_system_destroy(js);
}

typedef struct Driver {
    NphJobSystem *js;
    u32 items;
} Driver;

static void run_wait_1_driver(void *user)
{
    const Driver *d = (const Driver *)user;
    const NphJobDecl decl = {noop_job, NULL, "noop"};
    for (u32 i = 0; i < d->items; ++i) {
        run_one(d->js, &decl);
    }
}

NBENCH(run_wait_1_worker)
{
    NphJobSystem *js = bench_create(0U, 1U);
    if (js == NULL) {
        return;
    }
    Driver d = {js, 1000U};
    const NphJobDecl decl = {run_wait_1_driver, &d, "driver"};
    nbench_set_items_per_iteration(state, d.items);
    while (nbench_keep_running(state)) {
        run_one(js, &decl);
    }
    nph_job_system_destroy(js);
}

enum {
    BATCH = 256
};

NBENCH(run_wait_batch_256)
{
    NphJobSystem *js = bench_create(0U, 1U);
    if (js == NULL) {
        return;
    }
    static NphJobDecl decls[BATCH];
    for (u32 i = 0; i < BATCH; ++i) {
        decls[i].fn = noop_job;
        decls[i].user = NULL;
        decls[i].name = "noop";
    }
    nbench_set_items_per_iteration(state, BATCH);
    while (nbench_keep_running(state)) {
        NphJobCounter c = NPH_JOB_COUNTER_INIT;
        if (nph_job_run(js, decls, BATCH, NPH_JOB_PRIORITY_NORMAL, &c) == NPH_RESULT_OK) {
            nph_job_wait(js, &c);
        }
    }
    nph_job_system_destroy(js);
}

static void fork_256_driver(void *user)
{
    const Driver *d = (const Driver *)user;
    static NphJobDecl decls[BATCH];
    for (u32 i = 0; i < BATCH; ++i) {
        decls[i].fn = noop_job;
        decls[i].user = NULL;
        decls[i].name = "noop";
    }
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    if (nph_job_run(d->js, decls, BATCH, NPH_JOB_PRIORITY_NORMAL, &c) == NPH_RESULT_OK) {
        nph_job_wait(d->js, &c);
    }
}

NBENCH(fork_256_worker)
{
    NphJobSystem *js = bench_create(0U, 1U);
    if (js == NULL) {
        return;
    }
    Driver d = {js, BATCH};
    const NphJobDecl decl = {fork_256_driver, &d, "driver"};
    nbench_set_items_per_iteration(state, BATCH);
    while (nbench_keep_running(state)) {
        run_one(js, &decl);
    }
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Fork-join tree
// ---------------------------------------------------------------------------

typedef struct TreeNode {
    NphJobSystem *js;
    u32 level;
    u32 depth;
    u64 result;
} TreeNode;

static void tree_job(void *user)
{
    TreeNode *n = (TreeNode *)user;
    if (n->level == n->depth) {
        n->result = 1U;
        return;
    }
    TreeNode kids[2];
    NphJobDecl decls[2];
    for (u32 i = 0; i < 2U; ++i) {
        kids[i].js = n->js;
        kids[i].level = n->level + 1U;
        kids[i].depth = n->depth;
        kids[i].result = 0U;
        decls[i].fn = tree_job;
        decls[i].user = &kids[i];
        decls[i].name = "tree";
    }
    NphJobCounter c = NPH_JOB_COUNTER_INIT;
    if (nph_job_run(n->js, decls, 2U, NPH_JOB_PRIORITY_NORMAL, &c) == NPH_RESULT_OK) {
        nph_job_wait(n->js, &c);
        n->result = kids[0].result + kids[1].result;
    } else {
        tree_job(&kids[0]);
        tree_job(&kids[1]);
        n->result = kids[0].result + kids[1].result;
    }
}

NBENCH(fork_join_depth_8)
{
    NphJobSystem *js = bench_create(0U, 1U);
    if (js == NULL) {
        return;
    }
    TreeNode root = {js, 0U, 8U, 0U};
    const NphJobDecl decl = {tree_job, &root, "root"};
    nbench_set_items_per_iteration(state, 511U); // nodes
    while (nbench_keep_running(state)) {
        run_one(js, &decl);
    }
    nbench_do_not_optimize_u64(root.result);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// I/O threads
// ---------------------------------------------------------------------------

NBENCH(io_run_wait_1)
{
    NphJobSystem *js = bench_create(1U, 1U);
    if (js == NULL) {
        return;
    }
    const NphJobDecl decl = {noop_job, NULL, "io"};
    while (nbench_keep_running(state)) {
        NphJobCounter c = NPH_JOB_COUNTER_INIT;
        if (nph_job_run_io(js, &decl, 1U, &c) == NPH_RESULT_OK) {
            nph_job_wait(js, &c);
        }
    }
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// parallel_for
// ---------------------------------------------------------------------------

enum {
    ELEMENTS = 1U << 20U
};

typedef struct Sum {
    const u32 *values;
    NphAtomicU64 total;
} Sum;

static void sum_range(void *user, u32 begin, u32 end)
{
    Sum *s = (Sum *)user;
    u64 acc = 0U;
    for (u32 i = begin; i < end; ++i) {
        acc += s->values[i];
    }
    nph_atomic_fetch_add_u64(&s->total, acc, NPH_MEMORY_ORDER_RELAXED);
}

NBENCH(parallel_for_1M)
{
    NphJobSystem *js = bench_create(0U, 1U);
    if (js == NULL) {
        return;
    }
    u32 *values = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, ELEMENTS);
    if (values == NULL) {
        nph_job_system_destroy(js);
        return;
    }
    for (u32 i = 0; i < ELEMENTS; ++i) {
        values[i] = i & 0xFFU;
    }
    Sum s;
    s.values = values;
    nph_atomic_store_u64(&s.total, 0U, NPH_MEMORY_ORDER_RELAXED);
    nbench_set_items_per_iteration(state, ELEMENTS);
    while (nbench_keep_running(state)) {
        NphJobCounter c = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(js, ELEMENTS, 4096U, sum_range, &s, NPH_JOB_PRIORITY_NORMAL, &c) ==
            NPH_RESULT_OK) {
            nph_job_wait(js, &c);
        }
    }
    nbench_do_not_optimize_u64(nph_atomic_load_u64(&s.total, NPH_MEMORY_ORDER_RELAXED));
    NPH_FREE_ARRAY(nph_allocator_system(), u32, values, ELEMENTS);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Scaling with the worker count
// ---------------------------------------------------------------------------

enum {
    PIECES = 256,
    STEPS = 100000
};

typedef struct Compute {
    u32 out[PIECES];
} Compute;

// A dependent chain the compiler cannot vectorise or shorten: the same
// amount of work per piece regardless of the worker count.
static void compute_piece(void *user, u32 begin, u32 end)
{
    Compute *c = (Compute *)user;
    for (u32 p = begin; p < end; ++p) {
        u32 x = p + 1U;
        for (u32 i = 0; i < STEPS; ++i) {
            x = x * 1664525U + 1013904223U;
        }
        c->out[p] = x;
    }
}

static void scaling(NbenchState *state, u32 workers)
{
    NphJobSystem *js = bench_create(workers, 1U);
    if (js == NULL) {
        return;
    }
    char label[32];
    snprintf(label, sizeof label, "%u workers, %u cores", workers, nph_cpu_logical_count());
    nbench_set_label(state, label);
    Compute c;
    memset(&c, 0, sizeof c);
    while (nbench_keep_running(state)) {
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(js, PIECES, 1U, compute_piece, &c, NPH_JOB_PRIORITY_NORMAL,
                                 &done) == NPH_RESULT_OK) {
            nph_job_wait(js, &done);
        }
    }
    nbench_do_not_optimize(c.out);
    nph_job_system_destroy(js);
}

NBENCH(scaling_w1)
{
    scaling(state, 1U);
}

NBENCH(scaling_w2)
{
    scaling(state, 2U);
}

NBENCH(scaling_w4)
{
    scaling(state, 4U);
}

NBENCH(scaling_w8)
{
    scaling(state, 8U);
}

NBENCH(scaling_w16)
{
    scaling(state, 16U);
}
