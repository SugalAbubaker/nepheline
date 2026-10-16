// SPDX-License-Identifier: Apache-2.0
// Suspended entities (world.h "Suspension"): taken out of the world with
// their handles kept, so an editor's undo brings a deleted entity back as
// itself.

#include <nepheline/scene/query.h>
#include <nepheline/scene/scene_io.h>
#include <nepheline/scene/transform.h>

#include "test_scene_support.h"

static NphWorld *events_world(TestIds *ids, u32 max_entities)
{
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.max_entities = max_entities;
    wd.event_capacity = 256U;
    return test_world_create_desc(&wd, ids);
}

// Entities a query over `id` sees.
static u32 seen_by_query(NphWorld *w, NphComponentId id)
{
    const NphQueryTerm term = nph_term_read(id);
    NphQueryDesc qd;
    qd.terms = &term;
    qd.term_count = 1U;
    NphQuery *q = NULL;
    NTEST_ASSERT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_OK);
    const u32 n = nph_query_entity_count(q);
    nph_query_destroy(q);
    return n;
}

// The entities iteration over slots finds.
static u32 iterated(const NphWorld *w)
{
    u32 n = 0U;
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        n += nph_entity_is_null(nph_world_entity_at(w, i)) ? 0U : 1U;
    }
    return n;
}

static void expect_one_event(NphWorld *w, NphWorldEventType type, NphEntity e)
{
    u32 count = 0U;
    u32 dropped = 0U;
    const NphWorldEvent *ev = nph_world_events(w, &count, &dropped);
    u32 matching = 0U;
    for (u32 i = 0; i < count; ++i) {
        if (ev[i].type == (u32)type && nph_entity_equals(ev[i].entity, e)) {
            ++matching;
        }
    }
    NTEST_EXPECT_EQ_UINT(matching, 1U);
    nph_world_clear_events(w);
}

NTEST(world_suspend, a_suspended_entity_is_gone_but_keeps_its_handle)
{
    TestIds ids;
    NphWorld *w = events_world(&ids, 64U);
    const NphComponentId comps[2] = {ids.position, ids.health};
    NphEntity a = NPH_ENTITY_NULL;
    NphEntity b = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 2U, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &b), NPH_RESULT_OK);
    const TestPosition p = {{1.0F, 2.0F, 3.0F}};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, a, ids.position, &p), NPH_RESULT_OK);
    const u32 slot = nph_world_entity_index(w, a);
    nph_world_clear_events(w);

    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, a), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, a));
    NTEST_EXPECT_TRUE(nph_world_is_suspended(w, a));
    NTEST_EXPECT_FALSE(nph_world_is_suspended(w, b));
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 1U);
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(w), 1U);
    NTEST_EXPECT_NULL(nph_world_get(w, a, ids.position));
    NTEST_EXPECT_FALSE(nph_world_has(w, a, ids.position));
    NTEST_EXPECT_EQ_UINT(iterated(w), 1U);
    NTEST_EXPECT_EQ_UINT(seen_by_query(w, ids.position), 1U);
    NphWorldStats stats;
    nph_world_get_stats(w, &stats);
    NTEST_EXPECT_EQ_UINT(stats.entities, 1U);
    NTEST_EXPECT_EQ_UINT(stats.suspended, 1U);
    expect_one_event(w, NPH_WORLD_EVENT_DESPAWNED, a);

    // Nothing else takes its slot.
    for (u32 i = 0; i < 40U; ++i) {
        NphEntity e = NPH_ENTITY_NULL;
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &e), NPH_RESULT_OK);
        NTEST_ASSERT_NE_UINT(nph_world_entity_index(w, e), slot);
    }
    nph_world_clear_events(w);

    // It comes back as itself, with its components at their defaults.
    NTEST_ASSERT_EQ_INT(nph_world_resume(w, a, comps, 2U), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, a));
    NTEST_EXPECT_FALSE(nph_world_is_suspended(w, a));
    NTEST_EXPECT_EQ_UINT(nph_world_entity_index(w, a), slot);
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(w), 0U);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 42U);
    const TestPosition *back = NPH_WORLD_GET(w, a, ids.position, TestPosition);
    NTEST_ASSERT_NOT_NULL(back);
    TestPosition defaults;
    nph_component_init_default(TestPosition_component(), &defaults);
    NTEST_EXPECT_EQ_MEM(back, &defaults, sizeof defaults);
    NTEST_EXPECT_TRUE(nph_world_has(w, a, ids.health));
    expect_one_event(w, NPH_WORLD_EVENT_SPAWNED, a);
    nph_world_destroy(w);
}

NTEST(world_suspend, children_become_roots_and_the_parent_comes_back)
{
    TestIds ids;
    NphWorld *w = events_world(&ids, 64U);
    const NphComponentId comps[1] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NphEntity parent = NPH_ENTITY_NULL;
    NphEntity child = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &parent), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &child), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, child, parent), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, parent), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, child)));
    NTEST_EXPECT_EQ_UINT(nph_world_depth(w, child), 0U);
    NTEST_ASSERT_EQ_INT(nph_world_resume(w, parent, comps, 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, child, parent), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, child), parent));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_first_child(w, parent), child));
    // A suspended child leaves its parent the same way.
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, child), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_first_child(w, parent)));
    nph_world_destroy(w);
}

NTEST(world_suspend, release_lets_the_slot_go)
{
    TestIds ids;
    NphWorld *w = events_world(&ids, 64U);
    const NphComponentId comps[1] = {ids.position};
    NphEntity a = NPH_ENTITY_NULL;
    NphEntity other = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &other), NPH_RESULT_OK);
    const u32 slot = nph_world_entity_index(w, a);
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, a), NPH_RESULT_OK);
    nph_world_release(w, a);
    NTEST_EXPECT_FALSE(nph_world_is_suspended(w, a));
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, a));
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(w), 0U);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 1U);
    nph_world_release(w, a);     // again: nothing
    nph_world_release(w, other); // a live entity: nothing
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, other));
    NTEST_EXPECT_EQ_INT(nph_world_suspend(w, a), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(nph_world_resume(w, a, comps, 1U), NPH_RESULT_NOT_FOUND);
    // The slot is free for a new entity, under a new handle. (The pool hands
    // out unused slots first: filling the world gets to it.)
    bool reused = false;
    const u32 room = nph_world_entity_capacity(w) - nph_world_entity_count(w);
    for (u32 i = 0; i < room && !reused; ++i) {
        NphEntity e = NPH_ENTITY_NULL;
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 1U, &e), NPH_RESULT_OK);
        if (nph_world_entity_index(w, e) == slot) {
            reused = true;
            NTEST_EXPECT_FALSE(nph_entity_equals(e, a));
        }
    }
    NTEST_EXPECT_TRUE(reused);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, a));
    nph_world_destroy(w);
}

NTEST(world_suspend, mistakes_are_refused_and_capacity_counts_suspended_handles)
{
    TestIds ids;
    NphWorld *w = events_world(&ids, 4U);
    const NphComponentId comps[1] = {ids.position};
    NphEntity e[4];
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 1U, 4U, e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_suspend(w, NPH_ENTITY_NULL), NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, e[1]), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_suspend(w, e[1]), NPH_RESULT_NOT_FOUND);           // twice
    NTEST_EXPECT_EQ_INT(nph_world_resume(w, e[0], comps, 1U), NPH_RESULT_NOT_FOUND); // alive
    // An unknown component: refused, and the entity stays suspended.
    const NphComponentId unknown = 200U;
    NTEST_EXPECT_EQ_INT(nph_world_resume(w, e[1], &unknown, 1U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_TRUE(nph_world_is_suspended(w, e[1]));
    // The suspended handle holds its place against the capacity.
    NphEntity extra = NPH_ENTITY_NULL;
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 3U);
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, comps, 1U, &extra), NPH_RESULT_FULL);
    nph_world_release(w, e[1]);
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, comps, 1U, &extra), NPH_RESULT_OK);
    // No components at all is a valid way back.
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, e[2]), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_resume(w, e[2], NULL, 0U), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, e[2]));
    NTEST_EXPECT_FALSE(nph_world_has(w, e[2], ids.position));
    nph_world_destroy(w);
}

NTEST(world_suspend, saves_skip_them_and_copies_and_clears_keep_them)
{
    TestIds ids;
    NphWorld *w = events_world(&ids, 64U);
    const NphComponentId comps[2] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, ids.position};
    NphEntity e[3];
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 3U, e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_suspend(w, e[1]), NPH_RESULT_OK);

    // A save writes the live entities only.
    NphEntity order[8];
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.out_entities = order;
    sd.out_capacity = 8U;
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, &sd, &sb, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(res.entity_count, 2U);

    // A copy that keeps handles keeps the suspended one suspended.
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.max_entities = 64U;
    wd.registry_from = w;
    NphWorld *copy = test_world_create_desc(&wd, NULL);
    NTEST_ASSERT_EQ_INT(nph_world_adopt_handles(copy, w), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(copy), 0U);
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(copy), 3U);
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.handles = order;
    ld.handle_count = res.entity_count;
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(copy, nph_sb_str(&sb), &ld, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(copy), 2U);
    NTEST_EXPECT_EQ_UINT(nph_world_suspended_count(copy), 1U);
    NTEST_EXPECT_TRUE(nph_world_is_suspended(copy, e[1]));
    NTEST_ASSERT_EQ_INT(nph_world_resume(copy, e[1], comps, 2U), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_world_is_alive(copy, e[1]));
    nph_sb_free(&sb);
    nph_world_destroy(copy);

    // Clearing the world leaves it suspended too: its owner decides.
    nph_world_clear(w);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U);
    NTEST_EXPECT_TRUE(nph_world_is_suspended(w, e[1]));
    NTEST_ASSERT_EQ_INT(nph_world_resume(w, e[1], comps, 2U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 1U);
    nph_world_destroy(w);
}
