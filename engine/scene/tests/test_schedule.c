// SPDX-License-Identifier: Apache-2.0
// The scheduler: ordering and constraints, stages, the dependency graph
// from access, parallel execution, command determinism across worker
// counts, change ticks, and failure reporting.

#include <nepheline/core/atomic.h>

#include "test_scene_support.h"

#include <stdio.h>

static NphSchedule *make_schedule(NphWorld *w, NphJobSystem *js, u64 scratch)
{
    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.world = w;
    sd.jobs = js;
    sd.alloc = nph_allocator_system();
    sd.scratch_bytes = scratch;
    NphSchedule *s = NULL;
    NTEST_ASSERT_EQ_INT(nph_schedule_create(&sd, &s), NPH_RESULT_OK);
    return s;
}

static void noop(NphSystemContext *ctx)
{
    NTEST_UNUSED(ctx);
}

static NphSystemDesc sys_desc(const char *name, const NphComponentId *reads, u32 nr,
                              const NphComponentId *writes, u32 nw)
{
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.run = noop;
    d.reads = reads;
    d.read_count = nr;
    d.writes = writes;
    d.write_count = nw;
    return d;
}

NTEST(schedule, create_and_validation)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    NphSchedule *s = NULL;
    NTEST_EXPECT_EQ_INT(nph_schedule_create(&sd, &s), NPH_RESULT_INVALID_ARGUMENT);
    s = make_schedule(w, NULL, 0U);

    NphSystemDesc d = sys_desc("a", NULL, 0U, NULL, 0U);
    u32 index = 0U;
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, &index), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(index, 0U);
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, &index), NPH_RESULT_ALREADY_EXISTS);
    NTEST_EXPECT_EQ_UINT(index, NPH_U32_MAX);

    d = sys_desc("", NULL, 0U, NULL, 0U);
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_INVALID_ARGUMENT);
    d = sys_desc("b", NULL, 0U, NULL, 0U);
    d.run = NULL; // no function
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_INVALID_ARGUMENT);
    d = sys_desc("b", NULL, 0U, NULL, 0U);
    d.flags = 1U << 7U;
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_INVALID_ARGUMENT);
    const NphComponentId bad = 250U;
    d = sys_desc("b", &bad, 1U, NULL, 0U);
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_INVALID_ARGUMENT);
    const NphComponentId hier = NPH_COMPONENT_ID_HIERARCHY;
    d = sys_desc("b", NULL, 0U, &hier, 1U); // internal: not writable
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_INVALID_ARGUMENT);
    d = sys_desc("b", &hier, 1U, NULL, 0U); // readable
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_OK);
    const NphQueryTerm t = nph_term_read(250U);
    d = sys_desc("c", NULL, 0U, NULL, 0U);
    d.terms = &t;
    d.term_count = 1U;
    NTEST_EXPECT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(nph_schedule_system_count(s), 2U);
    NTEST_EXPECT_EQ_STR(nph_schedule_system_name(s, 1U), "b");
    NTEST_EXPECT_EQ_UINT(nph_schedule_find_system(s, NPH_STR("b")), 1U);
    NTEST_EXPECT_EQ_UINT(nph_schedule_find_system(s, NPH_STR("zz")), NPH_U32_MAX);
    nph_schedule_destroy(s);
    nph_schedule_destroy(NULL);
    nph_world_destroy(w);
}

NTEST(schedule, constraints_order_and_errors)
{
    NphWorld *w = test_world_create(0U, NULL);
    NphSchedule *s = make_schedule(w, NULL, 0U);
    // Registered c, b, a; constraints a -> b -> c.
    const char *after_b[1] = {"b"};
    const char *before_b[1] = {"b"};
    NphSystemDesc c = sys_desc("c", NULL, 0U, NULL, 0U);
    c.after = after_b;
    c.after_count = 1U;
    NphSystemDesc b = sys_desc("b", NULL, 0U, NULL, 0U);
    NphSystemDesc a = sys_desc("a", NULL, 0U, NULL, 0U);
    a.before = before_b;
    a.before_count = 1U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &c, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &b, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &a, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_build(s), NPH_RESULT_OK);
    u32 order[3];
    nph_schedule_order(s, order);
    NTEST_EXPECT_EQ_UINT(order[0], 2U); // a
    NTEST_EXPECT_EQ_UINT(order[1], 1U); // b
    NTEST_EXPECT_EQ_UINT(order[2], 0U); // c
    // Constraints are dependencies even without shared data.
    NTEST_EXPECT_TRUE(nph_schedule_depends_on(s, 0U, 2U));
    NTEST_EXPECT_FALSE(nph_schedule_depends_on(s, 2U, 0U));
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_count(s), 1U);
    NTEST_EXPECT_EQ_STR(nph_schedule_error(s), "");

    // An unknown name.
    const char *after_x[1] = {"x"};
    NphSystemDesc d = sys_desc("d", NULL, 0U, NULL, 0U);
    d.after = after_x;
    d.after_count = 1U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_schedule_build(s), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_str_from_cstr(nph_schedule_error(s)), NPH_STR("'x'")));
    NTEST_EXPECT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_INVALID_ARGUMENT);
    nph_schedule_destroy(s);

    // A cycle.
    s = make_schedule(w, NULL, 0U);
    const char *after_q[1] = {"q"};
    const char *after_p[1] = {"p"};
    NphSystemDesc p = sys_desc("p", NULL, 0U, NULL, 0U);
    p.after = after_q;
    p.after_count = 1U;
    NphSystemDesc q = sys_desc("q", NULL, 0U, NULL, 0U);
    q.after = after_p;
    q.after_count = 1U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &p, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &q, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_schedule_build(s), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_str_from_cstr(nph_schedule_error(s)), NPH_STR("cycle")));
    nph_schedule_destroy(s);

    // Ordered relative to itself.
    s = make_schedule(w, NULL, 0U);
    const char *after_self[1] = {"self"};
    NphSystemDesc self = sys_desc("self", NULL, 0U, NULL, 0U);
    self.after = after_self;
    self.after_count = 1U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &self, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_schedule_build(s), NPH_RESULT_INVALID_ARGUMENT);
    nph_schedule_destroy(s);

    // An empty schedule runs.
    s = make_schedule(w, NULL, 0U);
    NTEST_EXPECT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

NTEST(schedule, dependencies_follow_access_and_stages_split)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphSchedule *s = make_schedule(w, NULL, 0U);
    const NphComponentId pos = ids.position;
    const NphComponentId vel = ids.velocity;
    const NphComponentId hp = ids.health;
    NphSystemDesc d0 = sys_desc("write_pos", NULL, 0U, &pos, 1U);
    NphSystemDesc d1 = sys_desc("read_pos", &pos, 1U, NULL, 0U);
    NphSystemDesc d2 = sys_desc("read_pos_2", &pos, 1U, NULL, 0U);
    NphSystemDesc d3 = sys_desc("write_vel", NULL, 0U, &vel, 1U);
    NphSystemDesc d4 = sys_desc("main", NULL, 0U, NULL, 0U);
    d4.flags = NPH_SYSTEM_MAIN_THREAD;
    NphSystemDesc d5 = sys_desc("write_hp", NULL, 0U, &hp, 1U);
    NphSystemDesc d6 = sys_desc("excl", NULL, 0U, NULL, 0U);
    d6.flags = NPH_SYSTEM_EXCLUSIVE;
    NphSystemDesc *all[7] = {&d0, &d1, &d2, &d3, &d4, &d5, &d6};
    for (u32 i = 0; i < 7U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, all[i], NULL), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_schedule_build(s), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_schedule_depends_on(s, 1U, 0U)); // read after write
    NTEST_EXPECT_TRUE(nph_schedule_depends_on(s, 2U, 0U));
    NTEST_EXPECT_FALSE(nph_schedule_depends_on(s, 2U, 1U)); // two readers
    NTEST_EXPECT_FALSE(nph_schedule_depends_on(s, 3U, 0U)); // disjoint
    NTEST_EXPECT_FALSE(nph_schedule_depends_on(s, 0U, 3U));
    // Stages: [0..3], [main], [5], [excl].
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_count(s), 4U);
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_of(s, 0U), 0U);
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_of(s, 3U), 0U);
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_of(s, 4U), 1U);
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_of(s, 5U), 2U);
    NTEST_EXPECT_EQ_UINT(nph_schedule_stage_of(s, 6U), 3U);
    // Across stages, a later stage depends on every earlier one.
    NTEST_EXPECT_TRUE(nph_schedule_depends_on(s, 5U, 3U));
    NTEST_EXPECT_FALSE(nph_schedule_depends_on(s, 3U, 5U));
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

typedef struct Exec {
    TestIds ids;
    NphAtomicU32 writers_inside; // systems writing position, running now
    NphAtomicU32 overlap;        // set when two of them overlapped
    NphAtomicU32 runs;
} Exec;

static void integrate_chunk(NphSystemContext *ctx, const NphChunkView *v)
{
    Exec *ex = (Exec *)ctx->user;
    TestPosition *p = NPH_CHUNK_COLUMN(v, 0, TestPosition);
    const TestVelocity *vel = NPH_CHUNK_COLUMN(v, 1, TestVelocity);
    for (u32 i = 0; i < v->count; ++i) {
        p[i].value = nph_vec3_mul_add(p[i].value, vel[i].value, (f32)ctx->dt);
    }
    nph_atomic_fetch_add_u32(&ex->runs, 1U, NPH_MEMORY_ORDER_RELAXED);
}

// A system writing position that checks no other position writer runs.
static void exclusive_writer(NphSystemContext *ctx)
{
    Exec *ex = (Exec *)ctx->user;
    if (nph_atomic_fetch_add_u32(&ex->writers_inside, 1U, NPH_MEMORY_ORDER_ACQ_REL) != 0U) {
        nph_atomic_store_u32(&ex->overlap, 1U, NPH_MEMORY_ORDER_RELAXED);
    }
    NphQueryIter it;
    nph_system_query_iter(ctx, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        TestPosition *p = NPH_CHUNK_COLUMN(&v, 0, TestPosition);
        for (u32 i = 0; i < v.count; ++i) {
            p[i].value.y += 1.0F;
        }
    }
    nph_atomic_fetch_sub_u32(&ex->writers_inside, 1U, NPH_MEMORY_ORDER_ACQ_REL);
}

static void run_integration(NphJobSystem *js, u64 *out_hash)
{
    static Exec ex;
    memset(&ex, 0, sizeof ex);
    const u32 n = 20000U * TEST_SCALE;
    NphWorld *w = test_world_create(n + 1024U, &ex.ids);
    const NphComponentId comps[2] = {ex.ids.position, ex.ids.velocity};
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, n, NULL), NPH_RESULT_OK);
    NphSchedule *s = make_schedule(w, js, 0U);

    const NphQueryTerm it_terms[2] = {nph_term_write(ex.ids.position),
                                      nph_term_read(ex.ids.velocity)};
    NphSystemDesc integ;
    memset(&integ, 0, sizeof integ);
    integ.name = "integrate";
    integ.run_chunk = integrate_chunk;
    integ.user = &ex;
    integ.terms = it_terms;
    integ.term_count = 2U;
    integ.chunks_per_job = 2U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &integ, NULL), NPH_RESULT_OK);
    const NphQueryTerm wt = nph_term_write(ex.ids.position);
    for (u32 k = 0; k < 4U; ++k) {
        NphSystemDesc d;
        memset(&d, 0, sizeof d);
        static const char *const names[4] = {"w0", "w1", "w2", "w3"};
        d.name = names[k];
        d.run = exclusive_writer;
        d.user = &ex;
        d.terms = &wt;
        d.term_count = 1U;
        NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_OK);
    }
    for (u32 frame = 0; frame < 5U; ++frame) {
        NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.5), NPH_RESULT_OK);
    }
    NTEST_EXPECT_EQ_UINT(nph_atomic_load_u32(&ex.overlap, NPH_MEMORY_ORDER_RELAXED), 0U);
    NTEST_EXPECT_GT_UINT(nph_atomic_load_u32(&ex.runs, NPH_MEMORY_ORDER_RELAXED), 0U);
    // Every entity: y = 5 * (0.5 * 1) + 5 * 4 = 22.5.
    NphQueryIter iter;
    NphQuery *q = NULL;
    const NphQueryDesc qd = {&wt, 1U};
    NTEST_ASSERT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_OK);
    nph_query_iter(q, &iter);
    NphChunkView v;
    while (nph_query_next(&iter, &v)) {
        const TestPosition *p = NPH_CHUNK_COLUMN(&v, 0, TestPosition);
        for (u32 i = 0; i < v.count; ++i) {
            NTEST_ASSERT_NEAR_F32(p[i].value.y, 22.5F, 0.0F);
        }
    }
    nph_query_destroy(q);
    NphSystemStats st;
    nph_schedule_system_stats(s, 0U, &st);
    NTEST_EXPECT_EQ_UINT(st.runs, 5U);
    NTEST_EXPECT_GT_UINT(st.last_chunks, 0U);
    *out_hash = nph_world_hash(w);
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

NTEST(schedule, parallel_run_is_correct_and_serialises_conflicts)
{
    u64 serial = 0U;
    run_integration(NULL, &serial);
    NphJobSystem *js = test_jobs_create(test_worker_count(8U));
    u64 parallel = 0U;
    run_integration(js, &parallel);
    NTEST_EXPECT_EQ_UINT(serial, parallel);
    nph_job_system_destroy(js);
}

// Spawns per chunk through command buffers: one child entity per entity
// whose position x is even, with a reference back to it.
typedef struct Spawner {
    TestIds ids;
} Spawner;

static void spawn_chunk(NphSystemContext *ctx, const NphChunkView *v)
{
    const Spawner *sp = (const Spawner *)ctx->user;
    const TestPosition *p = NPH_CHUNK_COLUMN(v, 0, TestPosition);
    NphCommandBuffer *cb = nph_system_commands(ctx);
    if (cb == NULL) {
        return;
    }
    for (u32 i = 0; i < v->count; ++i) {
        if (((u32)p[i].value.x & 1U) == 0U) {
            const NphEntity e = nph_commands_spawn(cb);
            const TestHealth h = {p[i].value.x, 1.0F, v->entities[i]};
            NPH_IGNORE_RESULT(nph_commands_add(cb, e, sp->ids.health, &h));
        } else {
            NPH_IGNORE_RESULT(nph_commands_add(cb, v->entities[i], sp->ids.tag_a, NULL));
        }
    }
}

static u64 run_spawner(NphJobSystem *js)
{
    static Spawner sp;
    const u32 n = 5000U * TEST_SCALE;
    NphWorld *w = test_world_create(2U * n + 1024U, &sp.ids); // two runs spawn n more
    NphEntity *e = NPH_ALLOC_ARRAY(nph_allocator_system(), NphEntity, n);
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, &sp.ids.position, 1U, n, e), NPH_RESULT_OK);
    for (u32 i = 0; i < n; ++i) {
        const TestPosition p = {{(f32)i, 0.0F, 0.0F}};
        NTEST_ASSERT_EQ_INT(nph_world_set(w, e[i], sp.ids.position, &p), NPH_RESULT_OK);
    }
    NphSchedule *s = make_schedule(w, js, NPH_MIB(4));
    const NphQueryTerm terms[2] = {nph_term_read(sp.ids.position), nph_term_without(sp.ids.tag_a)};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "spawner";
    d.run_chunk = spawn_chunk;
    d.user = &sp;
    d.terms = terms;
    d.term_count = 2U;
    d.chunks_per_job = 1U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), n + (n + 1U) / 2U);
    // The second run sees only the entities without the tag (the even ones).
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), n + 2U * ((n + 1U) / 2U));
    const u64 h = nph_world_hash(w);
    nph_schedule_destroy(s);
    NPH_FREE_ARRAY(nph_allocator_system(), NphEntity, e, n);
    nph_world_destroy(w);
    return h;
}

NTEST(schedule, commands_are_deterministic_across_worker_counts)
{
    const u64 serial = run_spawner(NULL);
    NphJobSystem *one = test_jobs_create(1U);
    const u64 single = run_spawner(one);
    nph_job_system_destroy(one);
    NphJobSystem *many = test_jobs_create(test_worker_count(8U));
    const u64 multi = run_spawner(many);
    nph_job_system_destroy(many);
    NTEST_EXPECT_EQ_UINT(serial, single);
    NTEST_EXPECT_EQ_UINT(serial, multi);
}

// ---------------------------------------------------------------------------
// Change ticks
// ---------------------------------------------------------------------------

typedef struct Counter {
    u32 chunks;
    u32 tick;
    u32 last_tick;
} Counter;

static void count_chunk(NphSystemContext *ctx, const NphChunkView *v)
{
    NTEST_UNUSED(v);
    Counter *c = (Counter *)ctx->user;
    c->chunks += 1U;
    c->tick = ctx->tick;
    c->last_tick = ctx->last_tick;
}

static void touch_one(NphSystemContext *ctx)
{
    const NphEntity *target = (const NphEntity *)ctx->user;
    TestPosition *p = NPH_WORLD_GET_MUT(ctx->world, *target, 5U, TestPosition); // position id
    if (p != NULL) {
        p->value.z += 1.0F;
    }
}

NTEST(schedule, change_filters_see_writes_since_the_previous_run)
{
    TestIds ids;
    NphWorld *w = test_world_create(1U << 16U, &ids);
    NTEST_ASSERT_EQ_UINT(ids.position, 5U);
    NphEntity e[3000];
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, &ids.position, 1U, 3000U, e), NPH_RESULT_OK);
    NphSchedule *s = make_schedule(w, NULL, 0U);
    static Counter counter;
    memset(&counter, 0, sizeof counter);
    static NphEntity target;
    target = e[2999];
    // touch (writes position of one entity) runs after the counter, so the
    // counter sees the write on the next run.
    const NphQueryTerm ct = nph_term_changed(nph_term_read(ids.position));
    NphSystemDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.name = "count";
    cd.run_chunk = count_chunk;
    cd.user = &counter;
    cd.terms = &ct;
    cd.term_count = 1U;
    const NphComponentId pos = ids.position;
    NphSystemDesc td = sys_desc("touch", NULL, 0U, &pos, 1U);
    td.run = touch_one;
    td.user = &target;
    u32 touch_index = 0U;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &cd, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &td, &touch_index), NPH_RESULT_OK);

    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    const u32 all = counter.chunks;
    NTEST_EXPECT_GT_UINT(all, 2U); // the spawn wrote everything
    NTEST_EXPECT_EQ_UINT(counter.last_tick, 0U);
    counter.chunks = 0U;
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(counter.chunks, 1U); // only the touched chunk
    NTEST_EXPECT_GT_UINT(counter.tick, counter.last_tick);

    // Disabled, touch writes nothing: the counter sees nothing.
    nph_schedule_set_enabled(s, touch_index, false);
    counter.chunks = 0U;
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(counter.chunks, 1U); // the write from the last run
    counter.chunks = 0U;
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(counter.chunks, 0U);
    NphSystemStats st;
    nph_schedule_system_stats(s, touch_index, &st);
    NTEST_EXPECT_EQ_UINT(st.runs, 2U); // the disabled runs do not count

    // A direct write between runs is seen.
    TestPosition *p = NPH_WORLD_GET_MUT(w, e[0], ids.position, TestPosition);
    p->value.x = 4.0F;
    counter.chunks = 0U;
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(counter.chunks, 1U);
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

// ---------------------------------------------------------------------------
// Stages, exclusive systems, failures
// ---------------------------------------------------------------------------

typedef struct StageLog {
    TestIds ids;
    u32 log[8];
    u32 count;
    bool frozen_seen;
} StageLog;

static void exclusive_spawn(NphSystemContext *ctx)
{
    StageLog *sl = (StageLog *)ctx->user;
    sl->log[sl->count++] = 1U;
    NphEntity e;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(ctx->world, &sl->ids.position, 1U, &e), NPH_RESULT_OK);
}

static void record_spawn(NphSystemContext *ctx)
{
    StageLog *sl = (StageLog *)ctx->user;
    sl->log[sl->count++] = 2U;
    NphCommandBuffer *cb = nph_system_commands(ctx);
    NTEST_ASSERT_NOT_NULL(cb);
    const NphEntity e = nph_commands_spawn(cb);
    NPH_IGNORE_RESULT(nph_commands_add(cb, e, sl->ids.velocity, NULL));
    NTEST_EXPECT_TRUE(nph_system_commands(ctx) == cb);
}

static void count_entities(NphSystemContext *ctx)
{
    StageLog *sl = (StageLog *)ctx->user;
    sl->log[sl->count++] = 100U + nph_world_entity_count(ctx->world);
}

NTEST(schedule, stages_apply_commands_between_them)
{
    StageLog sl;
    memset(&sl, 0, sizeof sl);
    NphWorld *w = test_world_create(0U, &sl.ids);
    NphSchedule *s = make_schedule(w, NULL, 0U);
    NphSystemDesc a = sys_desc("excl", NULL, 0U, NULL, 0U);
    a.run = exclusive_spawn;
    a.user = &sl;
    a.flags = NPH_SYSTEM_EXCLUSIVE;
    NphSystemDesc b = sys_desc("record", NULL, 0U, NULL, 0U);
    b.run = record_spawn;
    b.user = &sl;
    NphSystemDesc c = sys_desc("count", NULL, 0U, NULL, 0U);
    c.run = count_entities;
    c.user = &sl;
    c.flags = NPH_SYSTEM_MAIN_THREAD;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &a, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &b, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &c, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(sl.count, 3U);
    NTEST_EXPECT_EQ_UINT(sl.log[0], 1U);
    NTEST_EXPECT_EQ_UINT(sl.log[1], 2U);
    NTEST_EXPECT_EQ_UINT(sl.log[2], 102U); // both spawns visible in the next stage
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

static void structural_in_stage(NphSystemContext *ctx)
{
    NphEntity e;
    TEST_EXPECT_ASSERT(NPH_IGNORE_RESULT(nph_world_spawn(ctx->world, NULL, 0U, &e)));
}

static void report_failure(NphSystemContext *ctx)
{
    nph_system_report(ctx, NPH_RESULT_IO_ERROR);
    nph_system_report(ctx, NPH_RESULT_CORRUPT); // only the first is kept
}

NTEST(schedule, failures_and_misuse)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphSchedule *s = make_schedule(w, NULL, 0U);
    NphSystemDesc a = sys_desc("structural", NULL, 0U, NULL, 0U);
    a.run = structural_in_stage;
    NphSystemDesc b = sys_desc("fails", NULL, 0U, NULL, 0U);
    b.run = report_failure;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &a, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &b, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_IO_ERROR);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_str_from_cstr(nph_schedule_error(s)), NPH_STR("IO")));
    // The next run starts clean.
    nph_schedule_set_enabled(s, 1U, false);
    NTEST_EXPECT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OK);
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

static void flood_commands(NphSystemContext *ctx)
{
    NphCommandBuffer *cb = nph_system_commands(ctx);
    for (u32 i = 0; cb != NULL && i < 100000U; ++i) {
        if (nph_entity_is_null(nph_commands_spawn(cb))) {
            break;
        }
    }
}

NTEST(schedule, scratch_exhaustion_fails_the_run)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphSchedule *s = make_schedule(w, NULL, NPH_KIB(64));
    NphSystemDesc a = sys_desc("flood", NULL, 0U, NULL, 0U);
    a.run = flood_commands;
    NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &a, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_schedule_run(s, 0.0), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U);
    nph_schedule_destroy(s);
    nph_world_destroy(w);
}

NTEST(schedule, many_systems_on_the_job_system)
{
    // A wide graph: chains of readers and writers over 16 components; the
    // result must match a serial run.
    NphJobSystem *js = test_jobs_create(test_worker_count(8U));
    u64 hashes[2];
    for (u32 mode = 0; mode < 2U; ++mode) {
        static Exec ex;
        memset(&ex, 0, sizeof ex);
        NphWorld *w = test_world_create(1U << 16U, &ex.ids);
        const NphComponentId comps[2] = {ex.ids.position, ex.ids.velocity};
        NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 4000U, NULL), NPH_RESULT_OK);
        NphSchedule *s = make_schedule(w, mode == 0U ? NULL : js, 0U);
        const NphQueryTerm terms[2] = {nph_term_write(ex.ids.position),
                                       nph_term_read(ex.ids.velocity)};
        static char names[64][16];
        for (u32 i = 0; i < 64U; ++i) {
            (void)snprintf(names[i], sizeof names[i], "sys%u", i);
            NphSystemDesc d;
            memset(&d, 0, sizeof d);
            d.name = names[i];
            d.user = &ex;
            if (i % 4U == 0U) {
                d.run_chunk = integrate_chunk;
                d.terms = terms;
                d.term_count = 2U;
                d.chunks_per_job = 1U;
            } else {
                d.run = noop;
                const NphComponentId *r = i % 2U == 0U ? &ex.ids.velocity : &ex.ids.health;
                d.reads = r;
                d.read_count = 1U;
            }
            NTEST_ASSERT_EQ_INT(nph_schedule_add_system(s, &d, NULL), NPH_RESULT_OK);
        }
        for (u32 f = 0; f < 3U; ++f) {
            NTEST_ASSERT_EQ_INT(nph_schedule_run(s, 0.25), NPH_RESULT_OK);
        }
        hashes[mode] = nph_world_hash(w);
        nph_schedule_destroy(s);
        nph_world_destroy(w);
    }
    NTEST_EXPECT_EQ_UINT(hashes[0], hashes[1]);
    nph_job_system_destroy(js);
}
