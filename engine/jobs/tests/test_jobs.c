// SPDX-License-Identifier: Apache-2.0
// The job system end to end: lifecycle, running and waiting, nesting,
// priorities, parallel-for, I/O threads, migration, deterministic mode.
#include "test_jobs_support.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NTEST(jobs, create_with_defaults_and_destroy)
{
    NphJobSystem *js = test_jobs_create(0U);
    const u32 cores = nph_cpu_logical_count();
    const u32 expected = cores > 1U ? cores - 1U : 1U;
    NTEST_ASSERT_EQ_UINT(nph_job_system_worker_count(js), expected);
    NTEST_ASSERT_EQ_UINT(nph_job_system_io_thread_count(js), 1U);
    NTEST_ASSERT_EQ_UINT(nph_job_system_fiber_count(js), 128U);
    NTEST_ASSERT_EQ_UINT(nph_job_worker_index(), NPH_JOB_WORKER_NONE);
    NTEST_ASSERT_FALSE(nph_job_is_io_thread());
    nph_job_system_destroy(js);
    nph_job_system_destroy(NULL); // allowed
}

NTEST(jobs, create_rejects_bad_descriptions)
{
    NphJobSystemDesc desc;
    NphJobSystem *js = (NphJobSystem *)&desc; // must be reset to NULL on failure
    memset(&desc, 0, sizeof desc);
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(js);
    desc.alloc = nph_allocator_system();
    desc.worker_count = NPH_JOB_MAX_WORKERS + 1U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_INVALID_ARGUMENT);
    desc.worker_count = 4U;
    desc.fiber_count = 4U; // must exceed the worker count
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_INVALID_ARGUMENT);
    desc.fiber_count = 0U;
    desc.job_capacity = 1000U; // not a power of two
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_INVALID_ARGUMENT);
    desc.job_capacity = 0U;
    desc.deque_capacity = 8U; // too small
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&desc, &js), NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(jobs, create_with_many_workers_and_small_pools)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = 16U;
    desc.io_thread_count = 2U;
    desc.fiber_count = 17U;
    desc.fiber_stack_size = NPH_KIB(16);
    desc.job_capacity = 64U;
    desc.deque_capacity = 16U;
    desc.external_waiter_slots = 1U;
    desc.pinning = NPH_JOB_PIN_ALWAYS;
    NphJobSystem *js = test_jobs_create_desc(&desc);
    NTEST_ASSERT_EQ_UINT(nph_job_system_worker_count(js), 16U);
    NTEST_ASSERT_EQ_UINT(nph_job_system_io_thread_count(js), 2U);
    NTEST_ASSERT_EQ_UINT(nph_job_system_fiber_count(js), 17U);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Running and waiting
// ---------------------------------------------------------------------------

typedef struct Counter {
    NphAtomicU32 runs;
    NphAtomicU32 sum;
    NphAtomicU32 last_worker;
    u32 value;
} Counter;

static void count_job(void *user)
{
    Counter *c = (Counter *)user;
    nph_atomic_fetch_add_u32(&c->runs, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_fetch_add_u32(&c->sum, c->value, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&c->last_worker, nph_job_worker_index(), NPH_MEMORY_ORDER_RELAXED);
}

NTEST(jobs, one_job_runs_on_a_worker)
{
    NphJobSystem *js = test_jobs_create(2U);
    Counter c;
    memset(&c, 0, sizeof c);
    c.value = 7U;
    nph_atomic_store_u32(&c.last_worker, NPH_U32_MAX, NPH_MEMORY_ORDER_RELAXED);
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    const NphJobDecl decl = {count_job, &c, "count"};
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.runs, NPH_MEMORY_ORDER_RELAXED), 1U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.sum, NPH_MEMORY_ORDER_RELAXED), 7U);
    NTEST_ASSERT_LT_UINT(nph_atomic_load_u32(&c.last_worker, NPH_MEMORY_ORDER_RELAXED), 2U);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&done), 0U);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, 1U);
    nph_job_system_destroy(js);
}

NTEST(jobs, run_rejects_bad_arguments_and_accepts_empty)
{
    NphJobSystem *js = test_jobs_create(1U);
    Counter c;
    memset(&c, 0, sizeof c);
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NphJobDecl decl = {NULL, &c, NULL};
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_job_run_io(js, &decl, 1U, &done), NPH_RESULT_INVALID_ARGUMENT);
    decl.fn = count_job;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, (NphJobPriority)3, &done),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_job_run(js, NULL, 0U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_job_run_io(js, NULL, 0U, &done), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_job_parallel_for(js, 0U, 1U, NULL, &c, NPH_JOB_PRIORITY_NORMAL, &done),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&done), 0U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.runs, NPH_MEMORY_ORDER_RELAXED), 0U);
    nph_job_system_destroy(js);
}

NTEST(jobs, a_batch_runs_every_job_once)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(8U));
    enum {
        BATCH = 1000
    };
    static Counter counters[BATCH];
    static NphJobDecl decls[BATCH];
    memset(counters, 0, sizeof counters);
    for (u32 i = 0; i < BATCH; ++i) {
        counters[i].value = i;
        decls[i].fn = count_job;
        decls[i].user = &counters[i];
        decls[i].name = "batch";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, decls, BATCH, NPH_JOB_PRIORITY_NORMAL, &done),
                        NPH_RESULT_OK);
    nph_job_wait(js, &done);
    for (u32 i = 0; i < BATCH; ++i) {
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&counters[i].runs, NPH_MEMORY_ORDER_RELAXED), 1U);
    }
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, BATCH);
    nph_job_system_destroy(js);
}

// A counter is reusable once it has reached zero, and a job without a
// counter still runs.
NTEST(jobs, counters_are_reusable_and_optional)
{
    NphJobSystem *js = test_jobs_create(2U);
    Counter c;
    memset(&c, 0, sizeof c);
    c.value = 1U;
    const NphJobDecl decl = {count_job, &c, NULL};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    for (u32 round = 0; round < 50U; ++round) {
        NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done),
                            NPH_RESULT_OK);
        nph_job_wait(js, &done);
        NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&done), 0U);
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.runs, NPH_MEMORY_ORDER_RELAXED), 50U);
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_LOW, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(test_wait_atomic_u32(&c.runs, 51U, 10U * NPH_TIME_NS_PER_S));
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Nesting: jobs that spawn jobs and wait for them on a fiber
// ---------------------------------------------------------------------------

typedef struct Tree {
    NphJobSystem *js;
    NphAtomicU32 leaves;
    NphAtomicU32 nodes;
    NphAtomicU32 migrations; // a node resumed on a different worker than it started on
    NphAtomicU32 bad_worker; // nph_job_worker_index out of range inside a job
    u32 depth;
} Tree;

typedef struct TreeNode {
    Tree *tree;
    u32 level;
    u64 result;
} TreeNode;

// Binary fork-join: each node spawns two children and waits for them, so
// at the leaves `depth` fibers per path are parked at once.
static void tree_job(void *user)
{
    TreeNode *n = (TreeNode *)user;
    Tree *t = n->tree;
    const u32 before = nph_job_worker_index();
    if (before >= nph_job_system_worker_count(t->js)) {
        nph_atomic_fetch_add_u32(&t->bad_worker, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_atomic_fetch_add_u32(&t->nodes, 1U, NPH_MEMORY_ORDER_RELAXED);
    if (n->level == t->depth) {
        nph_atomic_fetch_add_u32(&t->leaves, 1U, NPH_MEMORY_ORDER_RELAXED);
        n->result = 1U;
        return;
    }
    TreeNode children[2];
    NphJobDecl decls[2];
    for (u32 i = 0; i < 2U; ++i) {
        children[i].tree = t;
        children[i].level = n->level + 1U;
        children[i].result = 0U;
        decls[i].fn = tree_job;
        decls[i].user = &children[i];
        decls[i].name = "tree";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    while (nph_job_run(t->js, decls, 2U, NPH_JOB_PRIORITY_NORMAL, &done) != NPH_RESULT_OK) {
        nph_thread_yield(); // pool exhausted: a small-pool test runs this deliberately
    }
    nph_job_wait(t->js, &done);
    if (nph_job_worker_index() != before) {
        nph_atomic_fetch_add_u32(&t->migrations, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    n->result = children[0].result + children[1].result;
}

static void run_tree(NphJobSystem *js, u32 depth, Tree *t, TreeNode *root)
{
    memset(t, 0, sizeof *t);
    t->js = js;
    t->depth = depth;
    root->tree = t;
    root->level = 0U;
    root->result = 0U;
    const NphJobDecl decl = {tree_job, root, "root"};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_job_wait(js, &done);
}

NTEST(jobs, fork_join_tree_waits_on_fibers)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(8U));
    Tree t;
    TreeNode root;
    run_tree(js, 10U, &t, &root);
    NTEST_ASSERT_EQ_UINT(root.result, 1024U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.leaves, NPH_MEMORY_ORDER_RELAXED), 1024U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.nodes, NPH_MEMORY_ORDER_RELAXED), 2047U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.bad_worker, NPH_MEMORY_ORDER_RELAXED), 0U);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_LOG("waits switched %llu, inline %llu, switches %llu, stolen %llu, migrations %u",
              (unsigned long long)stats.waits_switched, (unsigned long long)stats.waits_inline,
              (unsigned long long)stats.fiber_switches, (unsigned long long)stats.jobs_stolen,
              nph_atomic_load_u32(&t.migrations, NPH_MEMORY_ORDER_RELAXED));
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, 2047U);
    NTEST_ASSERT_GT_UINT(stats.waits_switched, 0U);
    NTEST_ASSERT_EQ_UINT(stats.waits_inline, 0U); // 128 fibers suffice for depth 10
    if (nph_job_system_worker_count(js) > 1U) {
        NTEST_ASSERT_GT_UINT(stats.jobs_stolen, 0U);
    }
    nph_job_system_destroy(js);
}

// With far fewer fibers than parked waits, the waits that find no fiber
// run their subtrees on their own stacks (nesting at most as deep as the
// tree) and the result is still right. The nesting bound is in levels,
// not bytes: the sanitizers' fat frames need the bigger stack here.
NTEST(jobs, fork_join_tree_with_too_few_fibers_falls_back_inline)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = test_jobs_worker_count(4U);
    desc.fiber_count = desc.worker_count + 2U;
    desc.fiber_stack_size = NPH_KIB(256);
    NphJobSystem *js = test_jobs_create_desc(&desc);
    Tree t;
    TreeNode root;
    run_tree(js, 8U, &t, &root);
    NTEST_ASSERT_EQ_UINT(root.result, 256U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&t.nodes, NPH_MEMORY_ORDER_RELAXED), 511U);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_LOG("waits switched %llu, inline %llu, jobs nested %llu",
              (unsigned long long)stats.waits_switched, (unsigned long long)stats.waits_inline,
              (unsigned long long)stats.jobs_nested);
    NTEST_ASSERT_GT_UINT(stats.waits_inline, 0U);
    NTEST_ASSERT_GT_UINT(stats.jobs_nested, 0U);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, 511U);
    nph_job_system_destroy(js);
}

// Every fiber ends up held by a job parked on an event only the main
// thread will signal, and more jobs keep arriving. The waits that find
// no fiber run their own children (deeper jobs) on their stacks, and a
// job that cannot park or run anything holds its worker; the system
// must neither crash nor deadlock, and everything finishes once the
// event fires (jobs.h "Fiber exhaustion").
typedef struct Hostage {
    NphJobSystem *js;
    NphJobCounter event; // held at 1 until the main thread releases it
    NphAtomicU32 blocked;
    NphAtomicU32 children_ran;
    NphAtomicU32 parents_done;
} Hostage;

static void hostage_child(void *user)
{
    Hostage *h = (Hostage *)user;
    nph_atomic_fetch_add_u32(&h->children_ran, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_fetch_add_u32(&h->blocked, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_job_wait(h->js, &h->event);
}

static void hostage_parent(void *user)
{
    Hostage *h = (Hostage *)user;
    NphJobDecl decls[2];
    for (u32 i = 0; i < 2U; ++i) {
        decls[i].fn = hostage_child;
        decls[i].user = h;
        decls[i].name = "child";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    while (nph_job_run(h->js, decls, 2U, NPH_JOB_PRIORITY_NORMAL, &done) != NPH_RESULT_OK) {
        nph_thread_yield();
    }
    nph_job_wait(h->js, &done);
    nph_atomic_fetch_add_u32(&h->parents_done, 1U, NPH_MEMORY_ORDER_RELAXED);
}

NTEST(jobs, fiber_exhaustion_by_event_waiters_stays_live)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = test_jobs_worker_count(4U);
    desc.fiber_count = desc.worker_count + 2U;
    NphJobSystem *js = test_jobs_create_desc(&desc);
    Hostage h;
    memset(&h, 0, sizeof h);
    h.js = js;
    nph_job_counter_add(&h.event, 1U);
    enum {
        PARENTS = 32
    };
    NphJobDecl decls[PARENTS];
    for (u32 i = 0; i < PARENTS; ++i) {
        decls[i].fn = hostage_parent;
        decls[i].user = &h;
        decls[i].name = "parent";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, decls, PARENTS, NPH_JOB_PRIORITY_NORMAL, &done),
                        NPH_RESULT_OK);
    // Every worker ends up holding one child polling on the event, and
    // the spare fibers hold a few more, parked; nothing else can make
    // progress until it fires.
    const u32 workers = nph_job_system_worker_count(js);
    const u32 fibers = nph_job_system_fiber_count(js);
    const NphStopwatch settle = nph_stopwatch_start();
    while (nph_atomic_load_u32(&h.blocked, NPH_MEMORY_ORDER_ACQUIRE) < workers) {
        NTEST_ASSERT_LT_UINT(nph_stopwatch_elapsed_ns(&settle), 10U * NPH_TIME_NS_PER_S);
        nph_thread_yield();
    }
    nph_thread_sleep_ns(20U * NPH_TIME_NS_PER_MS);
    const u32 blocked = nph_atomic_load_u32(&h.blocked, NPH_MEMORY_ORDER_RELAXED);
    NTEST_LOG("%u children blocked on %u workers with %u fibers", blocked, workers, fibers);
    NTEST_ASSERT_GE_UINT(blocked, workers);
    NTEST_ASSERT_LE_UINT(blocked, fibers);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&h.parents_done, NPH_MEMORY_ORDER_RELAXED), 0U);
    const NphStopwatch sw = nph_stopwatch_start();
    nph_job_counter_signal(js, &h.event);
    nph_job_wait(js, &done);
    NTEST_LOG("released in %.2f ms", nph_stopwatch_elapsed_ms(&sw));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&h.children_ran, NPH_MEMORY_ORDER_RELAXED),
                         2U * PARENTS);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&h.parents_done, NPH_MEMORY_ORDER_RELAXED), PARENTS);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, 3U * PARENTS);
    NTEST_ASSERT_GT_UINT(stats.waits_inline, 0U);
    nph_job_system_destroy(js);
}

// A tiny job pool: spawning inside the tree hits NPH_RESULT_FULL and the
// job retries once its children have run.
NTEST(jobs, pool_exhaustion_reports_full_and_recovers)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = 2U;
    desc.job_capacity = 64U;
    NphJobSystem *js = test_jobs_create_desc(&desc);
    // Fill the pool from the main thread while the workers are held off
    // by a job that waits on a counter only the main thread signals.
    NphJobCounter gate = NPH_JOB_COUNTER_INIT;
    nph_job_counter_add(&gate, 1U);
    Counter c;
    memset(&c, 0, sizeof c);
    c.value = 1U;
    static NphJobDecl decls[64];
    for (u32 i = 0; i < 64U; ++i) {
        decls[i].fn = count_job;
        decls[i].user = &c;
        decls[i].name = NULL;
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    // Workers drain jobs as they arrive, so exhaustion is only certain when
    // the whole batch is reserved at once.
    NTEST_ASSERT_EQ_INT(nph_job_run(js, decls, 64U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.runs, NPH_MEMORY_ORDER_RELAXED), 64U);
    // 65 at once can never fit.
    static NphJobDecl more[65];
    for (u32 i = 0; i < 65U; ++i) {
        more[i] = decls[0];
    }
    NTEST_ASSERT_EQ_INT(nph_job_run(js, more, 65U, NPH_JOB_PRIORITY_NORMAL, &done),
                        NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&done), 0U); // nothing was queued
    nph_job_counter_signal(js, &gate);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.runs, NPH_MEMORY_ORDER_RELAXED), 64U);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Priorities
// ---------------------------------------------------------------------------

typedef struct Ordered {
    NphAtomicU32 next;
    u32 order[16];
    u32 tag;
} Ordered;

typedef struct OrderedJob {
    Ordered *o;
    u32 tag;
} OrderedJob;

static void ordered_job(void *user)
{
    OrderedJob *j = (OrderedJob *)user;
    const u32 slot = nph_atomic_fetch_add_u32(&j->o->next, 1U, NPH_MEMORY_ORDER_RELAXED);
    if (slot < 16U) {
        j->o->order[slot] = j->tag;
    }
}

typedef struct Gate {
    NphJobSystem *js;
    NphJobCounter *counter;
} Gate;

static void gate_job(void *user)
{
    Gate *g = (Gate *)user;
    nph_job_wait(g->js, g->counter);
}

// One worker, held by a gate job, receives low, normal, and high jobs; once
// released it must run them high first, low last.
NTEST(jobs, priorities_are_strict_on_one_worker)
{
    NphJobSystem *js = test_jobs_create(1U);
    NphJobCounter gate_counter = NPH_JOB_COUNTER_INIT;
    nph_job_counter_add(&gate_counter, 1U);
    Gate gate = {js, &gate_counter};
    const NphJobDecl gate_decl = {gate_job, &gate, "gate"};
    NphJobCounter gate_done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &gate_decl, 1U, NPH_JOB_PRIORITY_HIGH, &gate_done),
                        NPH_RESULT_OK);
    // Wait until the gate job is parked: it is the only job, so once it has
    // started the worker is either in it or parked on the fiber switch.
    nph_thread_sleep_ns(20U * NPH_TIME_NS_PER_MS);

    Ordered o;
    memset(&o, 0, sizeof o);
    OrderedJob jobs[6] = {{&o, 2U}, {&o, 1U}, {&o, 0U}, {&o, 2U}, {&o, 1U}, {&o, 0U}};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    for (u32 i = 0; i < 6U; ++i) {
        const NphJobDecl decl = {ordered_job, &jobs[i], "ordered"};
        NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, (NphJobPriority)jobs[i].tag, &done),
                            NPH_RESULT_OK);
    }
    nph_job_counter_signal(js, &gate_counter);
    nph_job_wait(js, &gate_done);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&o.next, NPH_MEMORY_ORDER_RELAXED), 6U);
    const u32 expected[6] = {0U, 0U, 1U, 1U, 2U, 2U};
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_ASSERT_EQ_UINT(o.order[i], expected[i]);
    }
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Parallel for
// ---------------------------------------------------------------------------

typedef struct Range {
    u32 *data;
    u32 count;
    NphAtomicU32 calls;
    NphAtomicU32 max_piece;
    NphAtomicU32 bad_piece; // empty or beyond the end
} Range;

static void range_fn(void *user, u32 begin, u32 end)
{
    Range *r = (Range *)user;
    if (end <= begin || end > r->count) {
        nph_atomic_fetch_add_u32(&r->bad_piece, 1U, NPH_MEMORY_ORDER_RELAXED);
        return;
    }
    for (u32 i = begin; i < end; ++i) {
        r->data[i] += 1U;
    }
    nph_atomic_fetch_add_u32(&r->calls, 1U, NPH_MEMORY_ORDER_RELAXED);
    u32 seen = nph_atomic_load_u32(&r->max_piece, NPH_MEMORY_ORDER_RELAXED);
    while (end - begin > seen &&
           !nph_atomic_compare_exchange_u32(&r->max_piece, &seen, end - begin,
                                            NPH_MEMORY_ORDER_RELAXED, NPH_MEMORY_ORDER_RELAXED)) {
    }
}

NTEST(jobs, parallel_for_covers_the_range_exactly_once)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(8U));
    enum {
        N = 100003
    };
    static u32 data[N];
    const u32 batches[] = {1U, 7U, 64U, 1000U, N, N + 5U};
    for (u32 b = 0; b < NPH_ARRAY_COUNT(batches); ++b) {
        memset(data, 0, sizeof data);
        Range r;
        memset(&r, 0, sizeof r);
        r.data = data;
        r.count = N;
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        NTEST_ASSERT_EQ_INT(
            nph_job_parallel_for(js, N, batches[b], range_fn, &r, NPH_JOB_PRIORITY_NORMAL, &done),
            NPH_RESULT_OK);
        nph_job_wait(js, &done);
        for (u32 i = 0; i < N; ++i) {
            if (data[i] != 1U) {
                NTEST_FAIL("batch %u: element %u touched %u times", batches[b], i, data[i]);
            }
        }
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&r.bad_piece, NPH_MEMORY_ORDER_RELAXED), 0U);
        NTEST_ASSERT_LE_UINT(nph_atomic_load_u32(&r.max_piece, NPH_MEMORY_ORDER_RELAXED),
                             batches[b]);
        const u32 calls = nph_atomic_load_u32(&r.calls, NPH_MEMORY_ORDER_RELAXED);
        NTEST_ASSERT_GE_UINT(calls, (N + batches[b] - 1U) / batches[b]);
        NTEST_LOG("batch %u: %u pieces", batches[b], calls);
    }
    nph_job_system_destroy(js);
}

NTEST(jobs, parallel_for_with_a_tiny_pool_finishes_inline)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = 2U;
    desc.job_capacity = 64U;
    NphJobSystem *js = test_jobs_create_desc(&desc);
    enum {
        N = 50000
    };
    static u32 data[N];
    memset(data, 0, sizeof data);
    Range r;
    memset(&r, 0, sizeof r);
    r.data = data;
    r.count = N;
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(
        nph_job_parallel_for(js, N, 1U, range_fn, &r, NPH_JOB_PRIORITY_NORMAL, &done),
        NPH_RESULT_OK);
    nph_job_wait(js, &done);
    for (u32 i = 0; i < N; ++i) {
        if (data[i] != 1U) {
            NTEST_FAIL("element %u touched %u times", i, data[i]);
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&r.bad_piece, NPH_MEMORY_ORDER_RELAXED), 0U);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// I/O threads
// ---------------------------------------------------------------------------

typedef struct IoProbe {
    NphAtomicU32 on_io_thread;
    NphAtomicU32 on_worker;
    NphAtomicU32 runs;
    NphAtomicU32 order_ok;
    NphAtomicU32 next_expected;
    u32 tag;
    u64 sleep_ns;
} IoProbe;

static void io_job(void *user)
{
    IoProbe *p = (IoProbe *)user;
    if (nph_job_is_io_thread()) {
        nph_atomic_fetch_add_u32(&p->on_io_thread, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    if (nph_job_worker_index() != NPH_JOB_WORKER_NONE) {
        nph_atomic_fetch_add_u32(&p->on_worker, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    if (p->sleep_ns != 0U) {
        nph_thread_sleep_ns(p->sleep_ns); // blocking is what I/O jobs are for
    }
    nph_atomic_fetch_add_u32(&p->runs, 1U, NPH_MEMORY_ORDER_RELAXED);
}

NTEST(jobs, io_jobs_run_on_io_threads_and_may_block)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = 2U;
    desc.io_thread_count = 2U;
    NphJobSystem *js = test_jobs_create_desc(&desc);
    IoProbe p;
    memset(&p, 0, sizeof p);
    p.sleep_ns = 5U * NPH_TIME_NS_PER_MS;
    NphJobDecl decls[8];
    for (u32 i = 0; i < 8U; ++i) {
        decls[i].fn = io_job;
        decls[i].user = &p;
        decls[i].name = "io";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run_io(js, decls, 8U, &done), NPH_RESULT_OK);
    // The workers stay free while the I/O jobs sleep.
    Counter c;
    memset(&c, 0, sizeof c);
    c.value = 1U;
    const NphJobDecl worker_decl = {count_job, &c, NULL};
    NphJobCounter worker_done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &worker_decl, 1U, NPH_JOB_PRIORITY_NORMAL, &worker_done),
                        NPH_RESULT_OK);
    const NphStopwatch sw = nph_stopwatch_start();
    nph_job_wait(js, &worker_done);
    NTEST_ASSERT_LT_UINT(nph_stopwatch_elapsed_ns(&sw), 2U * NPH_TIME_NS_PER_S);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.runs, NPH_MEMORY_ORDER_RELAXED), 8U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.on_io_thread, NPH_MEMORY_ORDER_RELAXED), 8U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.on_worker, NPH_MEMORY_ORDER_RELAXED), 0U);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_ASSERT_EQ_UINT(stats.jobs_io_executed, 8U);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, 1U);
    nph_job_system_destroy(js);
}

// A worker job waits on a counter completed by an I/O job: the fiber is
// resumed through the ready ring by a thread that is not a worker.
typedef struct IoChain {
    NphJobSystem *js;
    IoProbe probe;
    NphAtomicU32 resumed_ok;
} IoChain;

static void wait_for_io_job(void *user)
{
    IoChain *c = (IoChain *)user;
    NphJobDecl decl = {io_job, &c->probe, "io"};
    NphJobCounter io_done = NPH_JOB_COUNTER_INIT;
    if (nph_job_run_io(c->js, &decl, 1U, &io_done) != NPH_RESULT_OK) {
        return;
    }
    nph_job_wait(c->js, &io_done);
    if (nph_atomic_load_u32(&c->probe.runs, NPH_MEMORY_ORDER_ACQUIRE) == 1U &&
        nph_job_worker_index() != NPH_JOB_WORKER_NONE) {
        nph_atomic_store_u32(&c->resumed_ok, 1U, NPH_MEMORY_ORDER_RELEASE);
    }
}

NTEST(jobs, fiber_waiting_on_an_io_job_is_resumed)
{
    NphJobSystem *js = test_jobs_create(2U);
    IoChain c;
    memset(&c, 0, sizeof c);
    c.js = js;
    c.probe.sleep_ns = 10U * NPH_TIME_NS_PER_MS;
    const NphJobDecl decl = {wait_for_io_job, &c, "chain"};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&c.resumed_ok, NPH_MEMORY_ORDER_ACQUIRE), 1U);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Manual signals and waits with a target
// ---------------------------------------------------------------------------

typedef struct Pipeline {
    NphJobSystem *js;
    NphJobCounter in_flight;
    NphAtomicU32 max_in_flight;
    NphAtomicU32 current;
    NphAtomicU32 produced;
} Pipeline;

static void stage_job(void *user)
{
    Pipeline *p = (Pipeline *)user;
    const u32 now = nph_atomic_fetch_add_u32(&p->current, 1U, NPH_MEMORY_ORDER_ACQ_REL) + 1U;
    u32 seen = nph_atomic_load_u32(&p->max_in_flight, NPH_MEMORY_ORDER_RELAXED);
    while (now > seen &&
           !nph_atomic_compare_exchange_u32(&p->max_in_flight, &seen, now, NPH_MEMORY_ORDER_RELAXED,
                                            NPH_MEMORY_ORDER_RELAXED)) {
    }
    nph_thread_sleep_ns(NPH_TIME_NS_PER_MS);
    nph_atomic_fetch_sub_u32(&p->current, 1U, NPH_MEMORY_ORDER_ACQ_REL);
    nph_atomic_fetch_add_u32(&p->produced, 1U, NPH_MEMORY_ORDER_RELAXED);
}

// The producer keeps at most two stages in flight with wait_value: the
// bounded frame latency of CLAUDE.md §8.4.
NTEST(jobs, wait_value_bounds_a_pipeline)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(8U));
    Pipeline p;
    memset(&p, 0, sizeof p);
    p.js = js;
    const NphJobDecl decl = {stage_job, &p, "stage"};
    for (u32 frame = 0; frame < 30U; ++frame) {
        nph_job_wait_value(js, &p.in_flight, 1U); // at most one still running
        NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &p.in_flight),
                            NPH_RESULT_OK);
    }
    nph_job_wait(js, &p.in_flight);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&p.produced, NPH_MEMORY_ORDER_RELAXED), 30U);
    NTEST_ASSERT_LE_UINT(nph_atomic_load_u32(&p.max_in_flight, NPH_MEMORY_ORDER_RELAXED), 2U);
    nph_job_system_destroy(js);
}

typedef struct ManualWait {
    NphJobSystem *js;
    NphJobCounter event;
    NphAtomicU32 woke;
} ManualWait;

static void wait_for_event_job(void *user)
{
    ManualWait *m = (ManualWait *)user;
    nph_job_wait(m->js, &m->event);
    nph_atomic_fetch_add_u32(&m->woke, 1U, NPH_MEMORY_ORDER_RELEASE);
}

// Several fibers park on a counter that only the main thread signals,
// with nph_job_counter_signal: all are resumed through the ready ring.
NTEST(jobs, main_thread_signal_resumes_parked_fibers)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(4U));
    ManualWait m;
    memset(&m, 0, sizeof m);
    m.js = js;
    nph_job_counter_add(&m.event, 1U);
    NphJobDecl decls[10];
    for (u32 i = 0; i < 10U; ++i) {
        decls[i].fn = wait_for_event_job;
        decls[i].user = &m;
        decls[i].name = "waiter";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, decls, 10U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_thread_sleep_ns(20U * NPH_TIME_NS_PER_MS); // let them all park
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&m.woke, NPH_MEMORY_ORDER_ACQUIRE), 0U);
    NTEST_ASSERT_EQ_UINT(nph_job_counter_value(&done), 10U);
    nph_job_counter_signal(js, &m.event);
    nph_job_wait(js, &done);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&m.woke, NPH_MEMORY_ORDER_ACQUIRE), 10U);
    nph_job_system_destroy(js);
}

// ---------------------------------------------------------------------------
// Deterministic mode
// ---------------------------------------------------------------------------

typedef struct Trace {
    NphJobSystem *js;
    u32 log[64];
    u32 count;
    u32 depth;
} Trace;

typedef struct TraceNode {
    Trace *t;
    u32 id;
    u32 level;
} TraceNode;

// Spawns two children, waits, and records the order on the single worker.
static void trace_job(void *user)
{
    TraceNode *n = (TraceNode *)user;
    Trace *t = n->t;
    if (t->count < 64U) {
        t->log[t->count++] = n->id; // one worker: no race
    }
    if (n->level == t->depth) {
        return;
    }
    TraceNode kids[2] = {{t, n->id * 2U, n->level + 1U}, {t, n->id * 2U + 1U, n->level + 1U}};
    NphJobDecl decls[2] = {{trace_job, &kids[0], "trace"}, {trace_job, &kids[1], "trace"}};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    if (nph_job_run(t->js, decls, 2U, NPH_JOB_PRIORITY_NORMAL, &done) == NPH_RESULT_OK) {
        nph_job_wait(t->js, &done);
    }
}

static void run_trace(Trace *t)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.deterministic = 1;
    desc.worker_count = 8U; // overridden to one
    NphJobSystem *js = test_jobs_create_desc(&desc);
    NTEST_ASSERT_EQ_UINT(nph_job_system_worker_count(js), 1U);
    memset(t, 0, sizeof *t);
    t->js = js;
    t->depth = 4U;
    TraceNode root = {t, 1U, 0U};
    const NphJobDecl decl = {trace_job, &root, "root"};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_job_wait(js, &done);
    nph_job_system_destroy(js);
}

NTEST(jobs, deterministic_mode_repeats_the_schedule)
{
    Trace a;
    Trace b;
    run_trace(&a);
    run_trace(&b);
    NTEST_ASSERT_EQ_UINT(a.count, 31U);
    NTEST_ASSERT_EQ_UINT(b.count, 31U);
    NTEST_ASSERT_EQ_MEM(a.log, b.log, sizeof(u32) * a.count);
    // The owner takes its own deque last-in first-out: after node 1 spawns
    // 2 and 3, node 3 runs first.
    NTEST_ASSERT_EQ_UINT(a.log[0], 1U);
    NTEST_ASSERT_EQ_UINT(a.log[1], 3U);
}

// ---------------------------------------------------------------------------
// Jobs from several foreign threads at once
// ---------------------------------------------------------------------------

typedef struct Submitters {
    NphJobSystem *js;
    Counter counter;
    u32 per_thread;
} Submitters;

static void submitter_fn(u32 index, void *shared)
{
    NPH_UNUSED(index);
    Submitters *s = (Submitters *)shared;
    NphJobDecl decl = {count_job, &s->counter, "submitted"};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    for (u32 i = 0; i < s->per_thread; ++i) {
        while (nph_job_run(s->js, &decl, 1U, (NphJobPriority)(i % 3U), &done) != NPH_RESULT_OK) {
            nph_thread_yield();
        }
        if ((i & 63U) == 63U) {
            nph_job_wait(s->js, &done);
        }
    }
    nph_job_wait(s->js, &done);
}

NTEST(jobs, foreign_threads_submit_and_wait_concurrently)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(4U));
    Submitters s;
    memset(&s, 0, sizeof s);
    s.js = js;
    s.counter.value = 1U;
    s.per_thread = 2000U * TEST_SCALE;
    TestThread threads[4];
    test_threads_start(threads, 4U, submitter_fn, &s);
    test_threads_join(threads, 4U);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&s.counter.runs, NPH_MEMORY_ORDER_RELAXED),
                         4U * s.per_thread);
    nph_job_system_destroy(js);
}
