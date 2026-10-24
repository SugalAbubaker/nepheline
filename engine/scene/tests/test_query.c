// SPDX-License-Identifier: Apache-2.0
// Queries: validation, archetype matching (including archetypes created
// later), iteration, optional columns, change filters and tick stamps.

#include "test_scene_support.h"

// White-box access for the tick tests.
#include "scene_internal.h"

static NphQuery *make_query(NphWorld *w, const NphQueryTerm *terms, u32 count)
{
    const NphQueryDesc qd = {terms, count};
    NphQuery *q = NULL;
    NTEST_ASSERT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_OK);
    return q;
}

static u32 count_rows(const NphQuery *q)
{
    NphQueryIter it;
    nph_query_iter(q, &it);
    NphChunkView v;
    u32 n = 0U;
    while (nph_query_next(&it, &v)) {
        n += v.count;
    }
    return n;
}

NTEST(query, validation)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphQuery *q = NULL;
    NphQueryTerm t[NPH_QUERY_MAX_TERMS + 1U];
    NphQueryDesc qd = {t, 0U};
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    for (u32 i = 0; i < NPH_QUERY_MAX_TERMS + 1U; ++i) {
        t[i] = nph_term_with((NphComponentId)i);
    }
    qd.term_count = NPH_QUERY_MAX_TERMS + 1U;
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);

    qd.term_count = 1U;
    t[0] = nph_term_read(250U); // unregistered
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[0] = nph_term_read(ids.tag_a); // data access to a tag
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[0] = nph_term_without(ids.position); // nothing required
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[0] = nph_term_optional(ids.position, NPH_ACCESS_READ);
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[0] = nph_term_changed(nph_term_with(ids.position)); // filter without access
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);

    qd.term_count = 2U;
    t[0] = nph_term_read(ids.position);
    t[1] = nph_term_write(ids.position); // the same component twice
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[1] = nph_term_without(ids.health);
    t[1].access = NPH_ACCESS_READ; // an excluded term with access
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[1] = nph_term_without(ids.health);
    t[1].changed = 1U;
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_INVALID_ARGUMENT);
    t[1] = nph_term_without(ids.health);
    NTEST_EXPECT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_PTR(nph_query_world(q), w);
    NTEST_EXPECT_EQ_UINT(nph_query_term_count(q), 2U);
    NTEST_EXPECT_EQ_UINT(nph_query_terms(q)[1].component, ids.health);
    nph_query_destroy(q);
    nph_query_destroy(NULL);
    nph_world_destroy(w);
}

NTEST(query, matching_follows_new_archetypes)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    const NphQueryTerm terms[3] = {nph_term_read(ids.position), nph_term_without(ids.tag_a),
                                   nph_term_optional(ids.health, NPH_ACCESS_WRITE)};
    NphQuery *q = make_query(w, terms, 3U); // before any archetype exists
    const NphQueryTerm pos_only = nph_term_with(ids.position);
    NphQuery *all_pos = make_query(w, &pos_only, 1U);
    NTEST_EXPECT_EQ_UINT(nph_query_archetype_count(q), 0U);

    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK);
    const NphComponentId ph[2] = {ids.position, ids.health};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, ph, 2U, &e), NPH_RESULT_OK);
    const NphComponentId pt[2] = {ids.position, ids.tag_a};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, pt, 2U, &e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.velocity, 1U, &e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_query_archetype_count(q), 2U);
    NTEST_EXPECT_EQ_UINT(nph_query_entity_count(q), 2U);
    NTEST_EXPECT_EQ_UINT(nph_query_entity_count(all_pos), 3U);
    NTEST_EXPECT_EQ_UINT(count_rows(q), 2U);

    // Columns: the optional health is NULL where absent; terms without
    // access have none.
    NphQueryIter it;
    nph_query_iter(q, &it);
    NphChunkView v;
    u32 with_health = 0U;
    while (nph_query_next(&it, &v)) {
        NTEST_EXPECT_NOT_NULL(v.columns[0]);
        NTEST_EXPECT_NULL(v.columns[1]);
        with_health += v.columns[2] != NULL ? 1U : 0U;
        NTEST_EXPECT_EQ_UINT(v.count, 1U);
        NTEST_EXPECT_NOT_NULL(v.chunk);
    }
    NTEST_EXPECT_EQ_UINT(with_health, 1U);
    nph_query_iter(all_pos, &it);
    while (nph_query_next(&it, &v)) {
        NTEST_EXPECT_NULL(v.columns[0]);
    }

    // A query created after the archetypes finds them too.
    NphQuery *late = make_query(w, terms, 3U);
    NTEST_EXPECT_EQ_UINT(nph_query_archetype_count(late), 2U);
    nph_query_destroy(late);
    nph_query_destroy(all_pos);
    nph_query_destroy(q);
    nph_world_destroy(w);
}

NTEST(query, collect_and_view_match_iteration)
{
    TestIds ids;
    NphWorld *w = test_world_create(1U << 16U, &ids);
    const NphComponentId comps[2] = {ids.position, ids.velocity};
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 3000U, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, &ids.position, 1U, 1000U, NULL), NPH_RESULT_OK);
    const NphQueryTerm t = nph_term_write(ids.position);
    NphQuery *q = make_query(w, &t, 1U);
    const u32 n = nph_query_collect_chunks(q, 0U, NULL, 0U);
    NTEST_ASSERT_GT_UINT(n, 2U);
    void *chunks[64];
    NTEST_ASSERT_LE_UINT(n, 64U);
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, 0U, chunks, 1U), n); // reports the total
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, 0U, chunks, 64U), n);
    u32 total = 0U;
    for (u32 i = 0; i < n; ++i) {
        NphChunkView v;
        nph_query_chunk_view(q, chunks[i], 1U, &v);
        TestPosition *p = NPH_CHUNK_COLUMN(&v, 0, TestPosition);
        for (u32 r = 0; r < v.count; ++r) {
            p[r].value.x = 1.0F;
        }
        total += v.count;
    }
    NTEST_EXPECT_EQ_UINT(total, 4000U);
    nph_query_destroy(q);
    nph_world_destroy(w);
}

NTEST(query, an_entity_names_its_chunk_and_row)
{
    TestIds ids;
    NphWorld *w = test_world_create(1U << 16U, &ids);
    NphEntity moving[600];
    const NphComponentId comps[2] = {ids.position, ids.velocity};
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 600U, moving), NPH_RESULT_OK);
    NphEntity still;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &still), NPH_RESULT_OK);
    const NphQueryTerm terms[2] = {nph_term_write(ids.position), nph_term_read(ids.velocity)};
    NphQuery *q = make_query(w, terms, 2U);
    NTEST_ASSERT_GT_UINT(nph_query_collect_chunks(q, 0U, NULL, 0U), 1U); // rows in several chunks

    // Each entity's chunk and row hold it, and writing there writes it (at
    // a tick after the spawns' stamps).
    (void)nph_world_advance_tick(w);
    const u32 tick = nph_world_advance_tick(w);
    for (u32 i = 0; i < 600U; i += 7U) {
        u32 row = NPH_U32_MAX;
        void *chunk = nph_world_entity_chunk(w, moving[i], &row);
        NTEST_ASSERT_NOT_NULL(chunk);
        NTEST_ASSERT_TRUE(nph_query_matches_chunk(q, chunk));
        NphChunkView v;
        nph_query_chunk_view(q, chunk, tick, &v);
        NTEST_ASSERT_LT_UINT(row, v.count);
        NTEST_EXPECT_EQ_UINT(v.entities[row].value, moving[i].value);
        NPH_CHUNK_COLUMN(&v, 0, TestPosition)[row].value.x = (f32)i;
    }
    for (u32 i = 0; i < 600U; i += 7U) {
        const TestPosition *p = NPH_WORLD_GET(w, moving[i], ids.position, TestPosition);
        NTEST_EXPECT_NEAR_F32(p->value.x, (f32)i, 0.0F);
    }
    // The view stamped the chunks it showed.
    const NphQueryTerm changed = nph_term_changed(nph_term_read(ids.position));
    NphQuery *watch = make_query(w, &changed, 1U);
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(watch, tick - 1U, NULL, 0U),
                         nph_query_collect_chunks(q, 0U, NULL, 0U));

    // Another archetype's chunk is not the query's; a dead entity has none.
    void *other = nph_world_entity_chunk(w, still, NULL);
    NTEST_ASSERT_NOT_NULL(other);
    NTEST_EXPECT_FALSE(nph_query_matches_chunk(q, other));
    NTEST_EXPECT_TRUE(nph_query_matches_chunk(watch, other));
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, moving[3]), NPH_RESULT_OK);
    u32 row = 77U;
    NTEST_EXPECT_NULL(nph_world_entity_chunk(w, moving[3], &row));
    NTEST_EXPECT_EQ_UINT(row, 77U);
    // The entity that filled its row is found where it is now.
    void *chunk = nph_world_entity_chunk(w, moving[599], &row);
    NphChunkView v;
    nph_query_chunk_view(q, chunk, tick, &v);
    NTEST_EXPECT_EQ_UINT(v.entities[row].value, moving[599].value);
    nph_query_destroy(watch);
    nph_query_destroy(q);
    nph_world_destroy(w);
}

NTEST(query, change_filters_follow_writes)
{
    TestIds ids;
    NphWorld *w = test_world_create(1U << 16U, &ids);
    NphEntity a[2000];
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, &ids.position, 1U, 2000U, a), NPH_RESULT_OK);
    NphEntity h;
    const NphComponentId ph[2] = {ids.position, ids.health};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, ph, 2U, &h), NPH_RESULT_OK);

    const NphQueryTerm terms[2] = {
        nph_term_changed(nph_term_read(ids.position)),
        nph_term_changed(nph_term_optional(ids.health, NPH_ACCESS_READ))};
    NphQuery *q = make_query(w, terms, 2U);
    const u32 all_chunks = nph_query_collect_chunks(q, 0U, NULL, 0U);
    NTEST_ASSERT_EQ_UINT(all_chunks, 3U); // two chunks of positions, one with health

    // Everything was written by the spawns at tick 1.
    u32 since = nph_world_advance_tick(w); // a "system run" at tick 1
    NTEST_EXPECT_EQ_UINT(since, 1U);
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, 0U, NULL, 0U), all_chunks);
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, since, NULL, 0U), 0U);

    // A direct write marks exactly its chunk.
    TestPosition *p = NPH_WORLD_GET_MUT(w, a[1500], ids.position, TestPosition);
    p->value.x = 2.0F;
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, since, NULL, 0U), 1U);
    since = nph_world_advance_tick(w);
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, since, NULL, 0U), 0U);

    // Writing the optional component counts as a change of that archetype.
    const TestHealth hv = {1.0F, 1.0F, {0U}};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, h, ids.health, &hv), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, since, NULL, 0U), 1U);
    since = nph_world_advance_tick(w);

    // Iterating with write access stamps what it visits.
    const NphQueryTerm wt = nph_term_write(ids.position);
    NphQuery *writer = make_query(w, &wt, 1U);
    const u32 write_tick = nph_world_advance_tick(w);
    NphQueryIter it;
    nph_query_iter_since(writer, 0U, write_tick, &it);
    NphChunkView v;
    u32 visited = 0U;
    while (visited < 2U && nph_query_next(&it, &v)) {
        visited += 1U;
    }
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, since, NULL, 0U), 2U);

    // The filter in an iteration.
    nph_query_iter_since(q, since, write_tick, &it);
    u32 filtered = 0U;
    while (nph_query_next(&it, &v)) {
        filtered += 1U;
    }
    NTEST_EXPECT_EQ_UINT(filtered, 2U);
    // A plain iteration ignores the filter.
    nph_query_iter(q, &it);
    filtered = 0U;
    while (nph_query_next(&it, &v)) {
        filtered += 1U;
    }
    NTEST_EXPECT_EQ_UINT(filtered, all_chunks);

    // Wrap-around comparison.
    NTEST_EXPECT_TRUE(nph_world_tick_newer(1U, 0xFFFFFFF0U));
    NTEST_EXPECT_FALSE(nph_world_tick_newer(0xFFFFFFF0U, 1U));
    NTEST_EXPECT_FALSE(nph_world_tick_newer(5U, 5U));

    nph_query_destroy(writer);
    nph_query_destroy(q);
    nph_world_destroy(w);
}

NTEST(query, clamping_old_ticks)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK); // stamped 1
    const NphQueryTerm t = nph_term_changed(nph_term_read(ids.position));
    NphQuery *q = make_query(w, &t, 1U);
    // White box: jump the tick 2^31 + a little ahead, as a very long run
    // would. Without the clamp, the chunk stamped at tick 1 looks newer
    // than a recent `since` once the difference passes 2^31.
    w->tick = 0x90000000U;
    const u32 recent = w->tick - 5U;
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, recent, NULL, 0U), 1U); // the misreading
    u32 since[2] = {0U, recent};
    nph_world_clamp_ticks(w, since, 2U);
    NTEST_EXPECT_EQ_UINT(since[0], w->tick - (1U << 30U));
    NTEST_EXPECT_EQ_UINT(since[1], recent); // recent ticks are left alone
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, recent, NULL, 0U), 0U);
    // An old `since` still sees the (clamped) write as a change.
    NTEST_EXPECT_EQ_UINT(nph_query_collect_chunks(q, since[0] - 1U, NULL, 0U), 1U);
    nph_query_destroy(q);
    nph_world_destroy(w);
}

NTEST(query, structural_change_during_iteration_asserts)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK);
    const NphQueryTerm t = nph_term_read(ids.position);
    NphQuery *q = make_query(w, &t, 1U);
    NphQueryIter it;
    nph_query_iter(q, &it);
    NphChunkView v;
    NTEST_ASSERT_TRUE(nph_query_next(&it, &v));
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.velocity, 1U, &e), NPH_RESULT_OK);
    TEST_EXPECT_ASSERT((void)nph_query_next(&it, &v));
    nph_query_destroy(q);
    nph_world_destroy(w);
}
