// SPDX-License-Identifier: Apache-2.0
// Stress: millions of jobs, deep fork-join, random mixes of spawning and
// waiting, and repeated create/destroy (CLAUDE.md §8.4 "stress tests with
// millions of jobs").
#include "test_jobs_support.h"

typedef struct Tally {
    NphAtomicU64 runs;
    NphAtomicU64 sum;
} Tally;

typedef struct TallyJob {
    Tally *tally;
    u32 value;
} TallyJob;

static void tally_job(void *user)
{
    TallyJob *j = (TallyJob *)user;
    nph_atomic_fetch_add_u64(&j->tally->runs, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_fetch_add_u64(&j->tally->sum, j->value, NPH_MEMORY_ORDER_RELAXED);
}

// Batches of 4096 tiny jobs, submitted from the main thread as fast as the
// pool allows, until millions have run.
NTEST(jobs_stress, millions_of_tiny_jobs)
{
    NphJobSystem *js = test_jobs_create(test_jobs_worker_count(16U));
    enum {
        BATCH = 4096
    };
    static TallyJob jobs[BATCH];
    static NphJobDecl decls[BATCH];
    Tally tally;
    memset(&tally, 0, sizeof tally);
    u64 expected_sum = 0U;
    for (u32 i = 0; i < BATCH; ++i) {
        jobs[i].tally = &tally;
        jobs[i].value = i;
        decls[i].fn = tally_job;
        decls[i].user = &jobs[i];
        decls[i].name = "tiny";
        expected_sum += i;
    }
    const u32 batches = 64U * TEST_SCALE; // 2M jobs at full scale
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    const NphStopwatch sw = nph_stopwatch_start();
    for (u32 b = 0; b < batches; ++b) {
        // A batch of 4096 needs a quarter of the pool; wait for the previous
        // batches to drain enough before reusing the declarations.
        nph_job_wait_value(js, &done, BATCH);
        while (nph_job_run(js, decls, BATCH, NPH_JOB_PRIORITY_NORMAL, &done) != NPH_RESULT_OK) {
            nph_job_wait_value(js, &done, BATCH / 2U);
        }
    }
    nph_job_wait(js, &done);
    const u64 elapsed = nph_stopwatch_elapsed_ns(&sw);
    const u64 total = (u64)batches * BATCH;
    NTEST_LOG("%llu jobs in %.1f ms: %.0f ns per job on %u workers", (unsigned long long)total,
              nph_time_ns_to_ms(elapsed), (f64)elapsed / (f64)total,
              nph_job_system_worker_count(js));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&tally.runs, NPH_MEMORY_ORDER_RELAXED), total);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&tally.sum, NPH_MEMORY_ORDER_RELAXED),
                         expected_sum * batches);
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, total);
    nph_job_system_destroy(js);
}

// A wide fork-join: every node spawns `fanout` children and waits. With
// fanout 8 and depth 5 that is 37 449 nodes and 4 681 parked waits at
// the deepest level, far more than the fibers, so both the fiber path and
// the inline fallback are exercised under load, on the default stack.
typedef struct Fan {
    NphJobSystem *js;
    NphAtomicU64 nodes;
    NphAtomicU32 failures;
    u32 fanout;
    u32 depth;
} Fan;

typedef struct FanNode {
    Fan *fan;
    u32 level;
    u64 result;
} FanNode;

#define FAN_MAX 8U

static void fan_job(void *user)
{
    FanNode *n = (FanNode *)user;
    Fan *f = n->fan;
    nph_atomic_fetch_add_u64(&f->nodes, 1U, NPH_MEMORY_ORDER_RELAXED);
    if (n->level == f->depth) {
        n->result = 1U;
        return;
    }
    FanNode kids[FAN_MAX];
    NphJobDecl decls[FAN_MAX];
    for (u32 i = 0; i < f->fanout; ++i) {
        kids[i].fan = f;
        kids[i].level = n->level + 1U;
        kids[i].result = 0U;
        decls[i].fn = fan_job;
        decls[i].user = &kids[i];
        decls[i].name = "fan";
    }
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    u32 spins = 0U;
    while (nph_job_run(f->js, decls, f->fanout, NPH_JOB_PRIORITY_NORMAL, &done) != NPH_RESULT_OK) {
        if (++spins > 100000U) {
            nph_atomic_fetch_add_u32(&f->failures, 1U, NPH_MEMORY_ORDER_RELAXED);
            n->result = 0U;
            return;
        }
        nph_thread_yield();
    }
    nph_job_wait(f->js, &done);
    u64 sum = 0U;
    for (u32 i = 0; i < f->fanout; ++i) {
        sum += kids[i].result;
    }
    n->result = sum;
}

NTEST(jobs_stress, wide_fork_join_exceeds_the_fiber_pool)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = test_jobs_worker_count(16U);
    desc.fiber_stack_size = NPH_KIB(256); // nested waits under the sanitizers' fat frames
    NphJobSystem *js = test_jobs_create_desc(&desc);
    Fan f;
    memset(&f, 0, sizeof f);
    f.js = js;
    f.fanout = 8U;
    f.depth = TEST_SCALE >= 4U ? 5U : 4U;
    FanNode root = {&f, 0U, 0U};
    const NphJobDecl decl = {fan_job, &root, "root"};
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    const NphStopwatch sw = nph_stopwatch_start();
    NTEST_ASSERT_EQ_INT(nph_job_run(js, &decl, 1U, NPH_JOB_PRIORITY_NORMAL, &done), NPH_RESULT_OK);
    nph_job_wait(js, &done);
    u64 expected_nodes = 0U;
    u64 level_nodes = 1U;
    for (u32 l = 0; l <= f.depth; ++l) {
        expected_nodes += level_nodes;
        level_nodes *= f.fanout;
    }
    const u64 leaves = level_nodes / f.fanout;
    NphJobStats stats;
    nph_job_system_get_stats(js, &stats);
    NTEST_LOG("%llu nodes in %.1f ms; waits switched %llu, inline %llu, nested %llu, "
              "stolen %llu, parks %llu",
              (unsigned long long)expected_nodes, nph_stopwatch_elapsed_ms(&sw),
              (unsigned long long)stats.waits_switched, (unsigned long long)stats.waits_inline,
              (unsigned long long)stats.jobs_nested, (unsigned long long)stats.jobs_stolen,
              (unsigned long long)stats.worker_parks);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u32(&f.failures, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_ASSERT_EQ_UINT(root.result, leaves);
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&f.nodes, NPH_MEMORY_ORDER_RELAXED), expected_nodes);
    NTEST_ASSERT_EQ_UINT(stats.jobs_executed, expected_nodes);
    nph_job_system_destroy(js);
}

// Random mix: each job flips a coin to spawn children at a random priority,
// wait on them, do a little work, or signal a shared event other jobs are
// parked on. Checks that every spawned job runs exactly once.
typedef struct Mix {
    NphJobSystem *js;
    NphAtomicU64 spawned;
    NphAtomicU64 ran;
    NphAtomicU64 waits;
    NphJobCounter event;
    NphAtomicU32 event_waiters;
    NphAtomicU32 stop_spawning;
} Mix;

typedef struct MixJob {
    Mix *mix;
    u64 seed;
    u32 depth;
} MixJob;

static void mix_job(void *user)
{
    MixJob *j = (MixJob *)user;
    Mix *m = j->mix;
    nph_atomic_fetch_add_u64(&m->ran, 1U, NPH_MEMORY_ORDER_RELAXED);
    u64 rng = j->seed;
    const u32 choice = (u32)(test_rng_next(&rng) % 8U);
    if (j->depth < 6U && choice < 4U &&
        nph_atomic_load_u32(&m->stop_spawning, NPH_MEMORY_ORDER_RELAXED) == 0U) {
        MixJob kids[4];
        NphJobDecl decls[4];
        const u32 n = (u32)(test_rng_next(&rng) % 4U) + 1U;
        for (u32 i = 0; i < n; ++i) {
            kids[i].mix = m;
            kids[i].seed = test_rng_next(&rng);
            kids[i].depth = j->depth + 1U;
            decls[i].fn = mix_job;
            decls[i].user = &kids[i];
            decls[i].name = "mix";
        }
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        const NphJobPriority prio = (NphJobPriority)(test_rng_next(&rng) % 3U);
        if (nph_job_run(m->js, decls, n, prio, &done) == NPH_RESULT_OK) {
            nph_atomic_fetch_add_u64(&m->spawned, n, NPH_MEMORY_ORDER_RELAXED);
            nph_atomic_fetch_add_u64(&m->waits, 1U, NPH_MEMORY_ORDER_RELAXED);
            nph_job_wait(m->js, &done);
        }
    } else if (choice == 4U) {
        // Park on the shared event; the main thread pulses it every
        // millisecond and releases everyone parked so far.
        nph_atomic_fetch_add_u32(&m->event_waiters, 1U, NPH_MEMORY_ORDER_RELAXED);
        nph_job_wait(m->js, &m->event);
    } else if (choice == 5U) {
        nph_thread_yield(); // an OS-level pause on a worker: a hiccup the others absorb
    } else {
        volatile u32 x = 0U;
        for (u32 i = 0; i < 200U; ++i) {
            x += i;
        }
    }
}

NTEST(jobs_stress, random_spawn_wait_signal_mix)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.worker_count = test_jobs_worker_count(16U);
    desc.fiber_stack_size = NPH_KIB(256); // nested waits under the sanitizers' fat frames
    NphJobSystem *js = test_jobs_create_desc(&desc);
    Mix m;
    memset(&m, 0, sizeof m);
    m.js = js;
    nph_job_counter_add(&m.event, 1U);
    enum {
        ROOTS = 512
    };
    static MixJob roots[ROOTS];
    static NphJobDecl decls[ROOTS];
    u64 seed = 0x1234U;
    const u32 rounds = 4U * TEST_SCALE;
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    for (u32 round = 0; round < rounds; ++round) {
        for (u32 i = 0; i < ROOTS; ++i) {
            roots[i].mix = &m;
            roots[i].seed = test_rng_next(&seed);
            roots[i].depth = 0U;
            decls[i].fn = mix_job;
            decls[i].user = &roots[i];
            decls[i].name = "root";
        }
        NTEST_ASSERT_EQ_INT(nph_job_run(js, decls, ROOTS, NPH_JOB_PRIORITY_NORMAL, &done),
                            NPH_RESULT_OK);
        nph_atomic_fetch_add_u64(&m.spawned, ROOTS, NPH_MEMORY_ORDER_RELAXED);
        // Pulse the event (the main thread is its only signaller) until the
        // round has finished, so everything parked on it gets released.
        while (nph_job_counter_value(&done) != 0U) {
            nph_job_counter_signal(js, &m.event);
            nph_job_counter_add(&m.event, 1U); // re-arm for the next waiters
            nph_thread_sleep_ns(NPH_TIME_NS_PER_MS);
        }
        nph_job_wait(js, &done);
    }
    nph_atomic_store_u32(&m.stop_spawning, 1U, NPH_MEMORY_ORDER_RELAXED);
    nph_job_counter_signal(js, &m.event);
    NTEST_LOG("%llu jobs, %llu fiber waits, %u event waiters",
              (unsigned long long)nph_atomic_load_u64(&m.ran, NPH_MEMORY_ORDER_RELAXED),
              (unsigned long long)nph_atomic_load_u64(&m.waits, NPH_MEMORY_ORDER_RELAXED),
              nph_atomic_load_u32(&m.event_waiters, NPH_MEMORY_ORDER_RELAXED));
    NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&m.ran, NPH_MEMORY_ORDER_RELAXED),
                         nph_atomic_load_u64(&m.spawned, NPH_MEMORY_ORDER_RELAXED));
    nph_job_system_destroy(js);
}

// Create, use, destroy, repeat: no leaked threads, fibers, or slots.
NTEST(jobs_stress, repeated_create_and_destroy)
{
    for (u32 round = 0; round < 8U; ++round) {
        NphJobSystemDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.worker_count = (round % 4U) + 1U;
        desc.fiber_count = 8U;
        desc.job_capacity = 256U;
        desc.io_thread_count = 1U + (round & 1U);
        NphJobSystem *js = test_jobs_create_desc(&desc);
        Tally tally;
        memset(&tally, 0, sizeof tally);
        TallyJob job = {&tally, 3U};
        NphJobDecl decls[32];
        for (u32 i = 0; i < 32U; ++i) {
            decls[i].fn = tally_job;
            decls[i].user = &job;
            decls[i].name = "t";
        }
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        NTEST_ASSERT_EQ_INT(nph_job_run(js, decls, 32U, NPH_JOB_PRIORITY_NORMAL, &done),
                            NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_job_run_io(js, decls, 8U, &done), NPH_RESULT_OK);
        nph_job_wait(js, &done);
        NTEST_ASSERT_EQ_UINT(nph_atomic_load_u64(&tally.runs, NPH_MEMORY_ORDER_RELAXED), 40U);
        nph_job_system_destroy(js);
    }
}
