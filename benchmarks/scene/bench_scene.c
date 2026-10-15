// SPDX-License-Identifier: Apache-2.0
// Scene/ECS benchmarks (CLAUDE.md §1.5; §12 Phase 8 exit criterion
// "1M-entity benchmarks recorded"; docs/scene.md "Performance").
//
// Every figure is nanoseconds per entity (or per system, per command)
// unless the name says otherwise. The worlds hold a million entities in
// optimised builds; Debug builds (the CTest smoke run under the
// sanitizers) use 64 Ki so the suite stays fast.
//
//   spawn_batch_1M          batch spawn into one archetype (4 components)
//   spawn_single_1M         one nph_world_spawn per entity
//   despawn_1M              despawn every entity, in slot order
//   add_remove_tag_1M       add then remove a tag: two archetype moves each
//   get_random_1M           nph_world_get on random entities
//   query_read_1M           sum a component over every chunk, one thread
//   query_write_1M          position += velocity * dt, one thread
//   schedule_write_1M       the same as a run_chunk system on every worker
//   schedule_64_systems     a run of 64 empty systems in 4 dependency chains,
//                           per system
//   commands_spawn_1M       record and apply spawns with two components
//   transform_roots_1M      every root moved: the transform system's roots pass
//   transform_static_1M     nothing moved: the change filter skips every chunk
//                           (per entity, so the per-run cost is this x 1M)
//   transform_tree_1M       a forest of depth 4 (fan-out 8), every root moved
//   save_binary_1M          the binary scene format, write
//   load_binary_1M          the binary scene format, read into an empty world
//   save_text_64K           the text format, write (64 Ki entities)
//   load_text_64K           the text format, read

#include <nepheline/jobs/jobs.h>
#include <nepheline/scene/scene_io.h>
#include <nepheline/scene/schedule.h>
#include <nepheline/scene/transform.h>

#include <nbench/nbench.h>

#include <stdio.h>
#include <string.h>

#if NPH_CONFIG_DEBUG
#define BENCH_ENTITIES (1U << 16U)
#else
#define BENCH_ENTITIES (1U << 20U)
#endif
#define BENCH_TEXT_ENTITIES (1U << 16U)

#define BENCH_VEC_FIELDS(F) F(VEC3, value, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(BenchPosition, BENCH_VEC_FIELDS);
NPH_COMPONENT_STRUCT(BenchVelocity, BENCH_VEC_FIELDS);
NPH_COMPONENT_DECLARE(BenchPosition);
NPH_COMPONENT_DECLARE(BenchVelocity);
NPH_TAG_DECLARE(BenchTag);
NPH_COMPONENT_DEFINE(BenchPosition, BENCH_VEC_FIELDS, "bench.Position", 0U, NULL)
NPH_COMPONENT_DEFINE(BenchVelocity, BENCH_VEC_FIELDS, "bench.Velocity", 0U, NULL)
NPH_TAG_DEFINE(BenchTag, "bench.Tag", 0U)

typedef struct Ids {
    NphComponentId position;
    NphComponentId velocity;
    NphComponentId tag;
} Ids;

static NphWorld *bench_world(Ids *ids)
{
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.max_entities = BENCH_ENTITIES + 1024U;
    NphWorld *w = NULL;
    if (nph_world_create(&desc, &w) != NPH_RESULT_OK ||
        nph_world_register_component(w, BenchPosition_component(), &ids->position) !=
            NPH_RESULT_OK ||
        nph_world_register_component(w, BenchVelocity_component(), &ids->velocity) !=
            NPH_RESULT_OK ||
        nph_world_register_component(w, BenchTag_component(), &ids->tag) != NPH_RESULT_OK) {
        fprintf(stderr, "bench: world creation failed\n");
        nph_world_destroy(w);
        return NULL;
    }
    return w;
}

// A world of BENCH_ENTITIES entities with position, velocity, and both
// transforms.
static NphWorld *bench_populated(Ids *ids, NphEntity *out)
{
    NphWorld *w = bench_world(ids);
    if (w == NULL) {
        return NULL;
    }
    const NphComponentId comps[4] = {ids->position, ids->velocity, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    if (nph_world_spawn_batch(w, comps, 4U, BENCH_ENTITIES, out) != NPH_RESULT_OK) {
        fprintf(stderr, "bench: spawn failed\n");
        nph_world_destroy(w);
        return NULL;
    }
    return w;
}

static NphJobSystem *bench_jobs(void)
{
    NphJobSystemDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    NphJobSystem *js = NULL;
    if (nph_job_system_create(&desc, &js) != NPH_RESULT_OK) {
        fprintf(stderr, "bench: job system creation failed\n");
        return NULL;
    }
    return js;
}

static NphEntity *bench_entities(void)
{
    return NPH_ALLOC_ARRAY(nph_allocator_system(), NphEntity, BENCH_ENTITIES);
}

static void free_entities(NphEntity *e)
{
    NPH_FREE_ARRAY(nph_allocator_system(), NphEntity, e, BENCH_ENTITIES);
}

// ---------------------------------------------------------------------------
// Structural changes
// ---------------------------------------------------------------------------

NBENCH(spawn_batch_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = bench_world(&ids);
    if (w == NULL) {
        return;
    }
    const NphComponentId comps[4] = {ids.position, ids.velocity, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        nph_world_clear(w); // keeps the chunks: the steady state
        nbench_resume_timing(state);
        if (nph_world_spawn_batch(w, comps, 4U, BENCH_ENTITIES, NULL) != NPH_RESULT_OK) {
            fprintf(stderr, "bench: spawn failed\n");
            break;
        }
    }
    nph_world_destroy(w);
}

NBENCH(spawn_single_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = bench_world(&ids);
    if (w == NULL) {
        return;
    }
    const NphComponentId comps[4] = {ids.position, ids.velocity, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        nph_world_clear(w);
        nbench_resume_timing(state);
        for (u32 i = 0; i < BENCH_ENTITIES; ++i) {
            NphEntity e;
            if (nph_world_spawn(w, comps, 4U, &e) != NPH_RESULT_OK) {
                break;
            }
        }
    }
    nph_world_destroy(w);
}

NBENCH(despawn_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphEntity *e = bench_entities();
    NphWorld *w = bench_world(&ids);
    if (w == NULL || e == NULL) {
        nph_world_destroy(w);
        return;
    }
    const NphComponentId comps[4] = {ids.position, ids.velocity, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM};
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        if (nph_world_spawn_batch(w, comps, 4U, BENCH_ENTITIES, e) != NPH_RESULT_OK) {
            break;
        }
        nbench_resume_timing(state);
        for (u32 i = 0; i < BENCH_ENTITIES; ++i) {
            NPH_IGNORE_RESULT(nph_world_despawn(w, e[i]));
        }
    }
    nph_world_destroy(w);
    free_entities(e);
}

NBENCH(add_remove_tag_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphEntity *e = bench_entities();
    NphWorld *w = bench_populated(&ids, e);
    if (w == NULL || e == NULL) {
        nph_world_destroy(w);
        free_entities(e);
        return;
    }
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (nbench_keep_running(state)) {
        for (u32 i = 0; i < BENCH_ENTITIES; ++i) {
            NPH_IGNORE_RESULT(nph_world_add(w, e[i], ids.tag, NULL));
        }
        for (u32 i = 0; i < BENCH_ENTITIES; ++i) {
            NPH_IGNORE_RESULT(nph_world_remove(w, e[i], ids.tag));
        }
    }
    nph_world_destroy(w);
    free_entities(e);
}

NBENCH(get_random_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphEntity *e = bench_entities();
    NphWorld *w = bench_populated(&ids, e);
    if (w == NULL || e == NULL) {
        nph_world_destroy(w);
        free_entities(e);
        return;
    }
    u64 rng = 0x1234567ULL;
    nbench_set_items_per_iteration(state, 4096U);
    while (nbench_keep_running(state)) {
        f32 sum = 0.0F;
        for (u32 i = 0; i < 4096U; ++i) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            const BenchPosition *p = (const BenchPosition *)nph_world_get(
                w, e[(u32)(rng >> 33U) % BENCH_ENTITIES], ids.position);
            sum += p->value.x;
        }
        nbench_do_not_optimize(&sum);
    }
    nph_world_destroy(w);
    free_entities(e);
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

NBENCH(query_read_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = bench_populated(&ids, NULL);
    if (w == NULL) {
        return;
    }
    const NphQueryTerm t = nph_term_read(ids.position);
    const NphQueryDesc qd = {&t, 1U};
    NphQuery *q = NULL;
    NPH_IGNORE_RESULT(nph_query_create(w, &qd, &q));
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (q != NULL && nbench_keep_running(state)) {
        NphQueryIter it;
        nph_query_iter(q, &it);
        NphChunkView v;
        f32 sum = 0.0F;
        while (nph_query_next(&it, &v)) {
            const BenchPosition *p = NPH_CHUNK_COLUMN(&v, 0, BenchPosition);
            for (u32 i = 0; i < v.count; ++i) {
                sum += p[i].value.x + p[i].value.y + p[i].value.z;
            }
        }
        nbench_do_not_optimize(&sum);
    }
    nph_query_destroy(q);
    nph_world_destroy(w);
}

static void integrate(NphVec3 *NPH_RESTRICT pos, const NphVec3 *NPH_RESTRICT vel, u32 n, f32 dt)
{
    for (u32 i = 0; i < n; ++i) {
        pos[i].x += vel[i].x * dt;
        pos[i].y += vel[i].y * dt;
        pos[i].z += vel[i].z * dt;
    }
}

NBENCH(query_write_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = bench_populated(&ids, NULL);
    if (w == NULL) {
        return;
    }
    const NphQueryTerm t[2] = {nph_term_write(ids.position), nph_term_read(ids.velocity)};
    const NphQueryDesc qd = {t, 2U};
    NphQuery *q = NULL;
    NPH_IGNORE_RESULT(nph_query_create(w, &qd, &q));
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (q != NULL && nbench_keep_running(state)) {
        NphQueryIter it;
        nph_query_iter(q, &it);
        NphChunkView v;
        while (nph_query_next(&it, &v)) {
            integrate((NphVec3 *)v.columns[0], (const NphVec3 *)v.columns[1], v.count, 0.016F);
        }
        nbench_clobber_memory();
    }
    nph_query_destroy(q);
    nph_world_destroy(w);
}

static void integrate_chunk(NphSystemContext *ctx, const NphChunkView *v)
{
    integrate((NphVec3 *)v->columns[0], (const NphVec3 *)v->columns[1], v->count, (f32)ctx->dt);
}

NBENCH(schedule_write_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphJobSystem *js = bench_jobs();
    NphWorld *w = js != NULL ? bench_populated(&ids, NULL) : NULL;
    if (w == NULL) {
        nph_job_system_destroy(js);
        return;
    }
    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.world = w;
    sd.jobs = js;
    sd.alloc = nph_allocator_system();
    NphSchedule *s = NULL;
    const NphQueryTerm t[2] = {nph_term_write(ids.position), nph_term_read(ids.velocity)};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "integrate";
    d.run_chunk = integrate_chunk;
    d.terms = t;
    d.term_count = 2U;
    d.chunks_per_job = 16U;
    if (nph_schedule_create(&sd, &s) == NPH_RESULT_OK &&
        nph_schedule_add_system(s, &d, NULL) == NPH_RESULT_OK) {
        nbench_set_items_per_iteration(state, BENCH_ENTITIES);
        while (nbench_keep_running(state)) {
            NPH_IGNORE_RESULT(nph_schedule_run(s, 0.016));
        }
    }
    nph_schedule_destroy(s);
    nph_world_destroy(w);
    nph_job_system_destroy(js);
}

static void empty_system(NphSystemContext *ctx)
{
    NPH_UNUSED(ctx);
}

NBENCH(schedule_64_systems)
{
    Ids ids = {0U, 0U, 0U};
    NphJobSystem *js = bench_jobs();
    NphWorld *w = js != NULL ? bench_world(&ids) : NULL;
    if (w == NULL) {
        nph_job_system_destroy(js);
        return;
    }
    NphScheduleDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.world = w;
    sd.jobs = js;
    sd.alloc = nph_allocator_system();
    NphSchedule *s = NULL;
    NphResult r = nph_schedule_create(&sd, &s);
    // Four chains of 16: each system writes its chain's component, so the
    // chains run in parallel and each is sequential.
    static char names[64][16];
    const NphComponentId chain[4] = {ids.position, ids.velocity, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_NAME};
    for (u32 i = 0; r == NPH_RESULT_OK && i < 64U; ++i) {
        (void)snprintf(names[i], sizeof names[i], "s%u", i);
        NphSystemDesc d;
        memset(&d, 0, sizeof d);
        d.name = names[i];
        d.run = empty_system;
        d.writes = &chain[i % 4U];
        d.write_count = 1U;
        r = nph_schedule_add_system(s, &d, NULL);
    }
    if (r == NPH_RESULT_OK) {
        nbench_set_items_per_iteration(state, 64U);
        while (nbench_keep_running(state)) {
            NPH_IGNORE_RESULT(nph_schedule_run(s, 0.016));
        }
    }
    nph_schedule_destroy(s);
    nph_world_destroy(w);
    nph_job_system_destroy(js);
}

NBENCH(commands_spawn_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = bench_world(&ids);
    if (w == NULL) {
        return;
    }
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 1U << 20U);
    const BenchPosition p = {{1.0F, 2.0F, 3.0F}};
    const BenchVelocity v = {{0.0F, 1.0F, 0.0F}};
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (nbench_keep_running(state)) {
        nbench_pause_timing(state);
        nph_world_clear(w);
        nbench_resume_timing(state);
        for (u32 i = 0; i < BENCH_ENTITIES; ++i) {
            const NphEntity e = nph_commands_spawn(&cb);
            NPH_IGNORE_RESULT(nph_commands_add(&cb, e, ids.position, &p));
            NPH_IGNORE_RESULT(nph_commands_add(&cb, e, ids.velocity, &v));
        }
        if (nph_commands_apply(&cb, NULL) != NPH_RESULT_OK) {
            fprintf(stderr, "bench: apply failed\n");
            break;
        }
    }
    nph_commands_free(&cb);
    nph_world_destroy(w);
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

static void move_all_roots(NphWorld *w, NphQuery *q, u32 frame)
{
    NphQueryIter it;
    nph_query_iter(q, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        NphLocalTransform *l = NPH_CHUNK_COLUMN(&v, 0, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            l[i].translation.x = (f64)frame;
        }
    }
    NPH_UNUSED(w);
}

static NphQuery *roots_query(NphWorld *w)
{
    const NphQueryTerm t[2] = {nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM),
                               nph_term_without(NPH_COMPONENT_ID_HIERARCHY)};
    const NphQueryDesc qd = {t, 2U};
    NphQuery *q = NULL;
    NPH_IGNORE_RESULT(nph_query_create(w, &qd, &q));
    return q;
}

NBENCH(transform_roots_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphJobSystem *js = bench_jobs();
    NphWorld *w = js != NULL ? bench_populated(&ids, NULL) : NULL;
    NphTransformSystem *ts = NULL;
    NphQuery *q = w != NULL ? roots_query(w) : NULL;
    if (q != NULL && nph_transform_system_create(w, nph_allocator_system(), &ts) == NPH_RESULT_OK) {
        u32 frame = 0U;
        nbench_set_items_per_iteration(state, BENCH_ENTITIES);
        while (nbench_keep_running(state)) {
            nbench_pause_timing(state);
            move_all_roots(w, q, ++frame);
            nbench_resume_timing(state);
            NPH_IGNORE_RESULT(nph_transform_system_update(ts, js));
        }
    }
    nph_transform_system_destroy(ts);
    nph_query_destroy(q);
    nph_world_destroy(w);
    nph_job_system_destroy(js);
}

NBENCH(transform_static_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphJobSystem *js = bench_jobs();
    NphWorld *w = js != NULL ? bench_populated(&ids, NULL) : NULL;
    NphTransformSystem *ts = NULL;
    if (w != NULL && nph_transform_system_create(w, nph_allocator_system(), &ts) == NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(nph_transform_system_update(ts, js)); // everything once
        nbench_set_items_per_iteration(state, BENCH_ENTITIES);
        while (nbench_keep_running(state)) {
            NPH_IGNORE_RESULT(nph_transform_system_update(ts, js));
        }
    }
    nph_transform_system_destroy(ts);
    nph_world_destroy(w);
    nph_job_system_destroy(js);
}

NBENCH(transform_tree_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphJobSystem *js = bench_jobs();
    NphEntity *e = bench_entities();
    NphWorld *w = js != NULL && e != NULL ? bench_populated(&ids, e) : NULL;
    NphTransformSystem *ts = NULL;
    NphQuery *q = NULL;
    if (w != NULL) {
        // A forest of depth 4: entity i's parent is (i - 1) / 8 within
        // blocks of 4681 (1 + 8 + 64 + 512 + 4096) entities.
        const u32 block = 4681U;
        NphResult r = NPH_RESULT_OK;
        for (u32 i = 0; r == NPH_RESULT_OK && i < BENCH_ENTITIES; ++i) {
            const u32 local = i % block;
            if (local != 0U) {
                r = nph_world_set_parent(w, e[i], e[i - local + (local - 1U) / 8U]);
            }
        }
        // Roots of the forest: those whose Hierarchy has no parent.
        const NphQueryTerm t[2] = {nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM),
                                   nph_term_read(NPH_COMPONENT_ID_HIERARCHY)};
        const NphQueryDesc qd = {t, 2U};
        NPH_IGNORE_RESULT(nph_query_create(w, &qd, &q));
        if (r != NPH_RESULT_OK || q == NULL ||
            nph_transform_system_create(w, nph_allocator_system(), &ts) != NPH_RESULT_OK) {
            fprintf(stderr, "bench: tree setup failed\n");
        }
    }
    if (ts != NULL) {
        u32 frame = 0U;
        nbench_set_items_per_iteration(state, BENCH_ENTITIES);
        while (nbench_keep_running(state)) {
            nbench_pause_timing(state);
            frame += 1U;
            NphQueryIter it;
            nph_query_iter(q, &it);
            NphChunkView v;
            while (nph_query_next(&it, &v)) {
                NphLocalTransform *l = NPH_CHUNK_COLUMN(&v, 0, NphLocalTransform);
                const NphHierarchy *h = NPH_CHUNK_COLUMN(&v, 1, NphHierarchy);
                for (u32 i = 0; i < v.count; ++i) {
                    if (nph_entity_is_null(h[i].parent)) {
                        l[i].translation.y = (f64)frame;
                    }
                }
            }
            nbench_resume_timing(state);
            NPH_IGNORE_RESULT(nph_transform_system_update(ts, js));
        }
    }
    nph_transform_system_destroy(ts);
    nph_query_destroy(q);
    nph_world_destroy(w);
    nph_job_system_destroy(js);
    free_entities(e);
}

// ---------------------------------------------------------------------------
// Scene files
// ---------------------------------------------------------------------------

NBENCH(save_binary_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = bench_populated(&ids, NULL);
    if (w == NULL) {
        return;
    }
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    nbench_set_items_per_iteration(state, BENCH_ENTITIES);
    while (nbench_keep_running(state)) {
        nph_bin_writer_clear(&bw);
        if (nph_scene_save_binary(w, NULL, &bw, NULL) != NPH_RESULT_OK) {
            break;
        }
    }
    nph_bin_writer_free(&bw);
    nph_world_destroy(w);
}

NBENCH(load_binary_1M)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *src = bench_populated(&ids, NULL);
    Ids ids2 = {0U, 0U, 0U};
    NphWorld *w = src != NULL ? bench_world(&ids2) : NULL;
    if (w == NULL) {
        nph_world_destroy(src);
        return;
    }
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    if (nph_scene_save_binary(src, NULL, &bw, NULL) == NPH_RESULT_OK) {
        nbench_set_items_per_iteration(state, BENCH_ENTITIES);
        while (nbench_keep_running(state)) {
            nbench_pause_timing(state);
            nph_world_clear(w);
            nbench_resume_timing(state);
            if (nph_scene_load_binary(w, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw), NULL,
                                      NULL) != NPH_RESULT_OK) {
                break;
            }
        }
    }
    nph_bin_writer_free(&bw);
    nph_world_destroy(w);
    nph_world_destroy(src);
}

static NphWorld *text_world(Ids *ids)
{
    NphWorld *w = bench_world(ids);
    if (w == NULL) {
        return NULL;
    }
    const NphComponentId comps[3] = {ids->position, NPH_COMPONENT_ID_NAME,
                                     NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NphEntity e;
    for (u32 i = 0; i < BENCH_TEXT_ENTITIES; ++i) {
        if (nph_world_spawn(w, comps, 3U, &e) != NPH_RESULT_OK) {
            break;
        }
        BenchPosition *p = (BenchPosition *)nph_world_get_mut(w, e, ids->position);
        p->value = nph_vec3((f32)i * 0.25F, 1.5F, -(f32)i);
        NphName *n = (NphName *)nph_world_get_mut(w, e, NPH_COMPONENT_ID_NAME);
        char text[32];
        (void)snprintf(text, sizeof text, "entity %u", i);
        nph_text_set(&n->text, nph_str_from_cstr(text));
    }
    return w;
}

NBENCH(save_text_64K)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *w = text_world(&ids);
    if (w == NULL) {
        return;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    nbench_set_items_per_iteration(state, BENCH_TEXT_ENTITIES);
    while (nbench_keep_running(state)) {
        nph_sb_clear(&sb);
        if (nph_scene_save_text(w, NULL, &sb, NULL) != NPH_RESULT_OK) {
            break;
        }
    }
    nph_sb_free(&sb);
    nph_world_destroy(w);
}

NBENCH(load_text_64K)
{
    Ids ids = {0U, 0U, 0U};
    NphWorld *src = text_world(&ids);
    Ids ids2 = {0U, 0U, 0U};
    NphWorld *w = src != NULL ? bench_world(&ids2) : NULL;
    if (w == NULL) {
        nph_world_destroy(src);
        return;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    if (nph_scene_save_text(src, NULL, &sb, NULL) == NPH_RESULT_OK) {
        nbench_set_items_per_iteration(state, BENCH_TEXT_ENTITIES);
        while (nbench_keep_running(state)) {
            nbench_pause_timing(state);
            nph_world_clear(w);
            nbench_resume_timing(state);
            if (nph_scene_load_text(w, nph_sb_str(&sb), NULL, NULL) != NPH_RESULT_OK) {
                break;
            }
        }
    }
    nph_sb_free(&sb);
    nph_world_destroy(w);
    nph_world_destroy(src);
}
