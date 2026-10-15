// SPDX-License-Identifier: Apache-2.0
// Command buffers: deferred structural changes, pending handles and their
// remapping in ENTITY fields, skipped commands, sticky failures.

#include <nepheline/core/arena.h>

#include "test_scene_support.h"

NTEST(command, spawn_with_components_and_references)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 0U);
    NTEST_EXPECT_TRUE(nph_commands_is_empty(&cb));

    const NphEntity a = nph_commands_spawn(&cb);
    NTEST_EXPECT_TRUE(nph_commands_is_pending(w, a));
    NTEST_EXPECT_FALSE(nph_commands_is_pending(w, NPH_ENTITY_NULL));
    const TestPosition p = {{1.0F, 2.0F, 3.0F}};
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, a, ids.position, &p), NPH_RESULT_OK);
    // a refers to b, which is spawned later in the buffer: the handle b
    // will have is known now (pending handles are numbered in order).
    const NphEntity b_pending = {a.value + 1U};
    const TestHealth h = {5.0F, 6.0F, b_pending};
    NTEST_EXPECT_EQ_INT(nph_commands_set(&cb, a, ids.health, &h), NPH_RESULT_OK);
    const NphEntity b = nph_commands_spawn(&cb);
    NTEST_EXPECT_TRUE(nph_entity_equals(b, b_pending));
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, b, ids.tag_a, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, b, a), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U); // nothing happens until apply

    NphCommandStats st;
    NTEST_ASSERT_EQ_INT(nph_commands_apply(&cb, &st), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(st.applied, 6U);
    NTEST_EXPECT_EQ_UINT(st.skipped, 0U);
    NTEST_EXPECT_TRUE(nph_commands_is_empty(&cb));
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(w), 2U);

    const NphEntity ea = nph_world_entity_at(w, 0U);
    const NphEntity eb = nph_world_entity_at(w, 1U);
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, ea, ids.position, TestPosition)->value.y, 2.0F, 0.0F);
    const TestHealth *hh = NPH_WORLD_GET(w, ea, ids.health, TestHealth);
    NTEST_ASSERT_NOT_NULL(hh);
    NTEST_EXPECT_TRUE(nph_entity_equals(hh->attacker, eb));
    NTEST_EXPECT_TRUE(nph_world_has(w, eb, ids.tag_a));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, eb), ea));

    // Each entity was created directly in the archetype of the commands
    // that follow its spawn: {position, health} and {tag}, plus the two
    // with Hierarchy from the reparenting; no {} or {position} on the way.
    NphWorldStats ws;
    nph_world_get_stats(w, &ws);
    NTEST_EXPECT_EQ_UINT(ws.archetypes, 4U);
    nph_commands_free(&cb);
    nph_world_destroy(w);
}

NTEST(command, dead_targets_are_skipped)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity e;
    NphEntity f;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &f), NPH_RESULT_OK);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 256U); // small blocks: several
    NTEST_EXPECT_EQ_INT(nph_commands_despawn(&cb, e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_commands_despawn(&cb, e), NPH_RESULT_OK);               // already gone
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, e, ids.health, NULL), NPH_RESULT_OK); // dead
    NTEST_EXPECT_EQ_INT(nph_commands_remove(&cb, f, ids.health), NPH_RESULT_OK);    // absent
    NTEST_EXPECT_EQ_INT(nph_commands_remove(&cb, f, ids.position), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, e, f), NPH_RESULT_OK); // dead child
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, f, e), NPH_RESULT_OK); // dead parent
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, f, ids.velocity, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, f, ids.velocity, NULL), NPH_RESULT_OK); // present
    for (u32 i = 0; i < 40U; ++i) {
        const TestVelocity v = {{(f32)i, 0.0F, 0.0F}};
        NTEST_EXPECT_EQ_INT(nph_commands_set(&cb, f, ids.velocity, &v), NPH_RESULT_OK);
    }
    NphCommandStats st;
    NTEST_ASSERT_EQ_INT(nph_commands_apply(&cb, &st), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(st.applied, 1U + 1U + 2U + 40U);
    NTEST_EXPECT_EQ_UINT(st.skipped, 5U);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, e));
    NTEST_EXPECT_FALSE(nph_world_has(w, f, ids.position));
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, f, ids.velocity, TestVelocity)->value.x, 39.0F, 0.0F);
    // Applying an empty buffer is a no-op.
    NTEST_EXPECT_EQ_INT(nph_commands_apply(&cb, NULL), NPH_RESULT_OK);
    nph_commands_free(&cb);
    nph_world_destroy(w);
}

NTEST(command, invalid_recordings)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 0U);
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, NULL, 0U, &e), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_commands_despawn(&cb, NPH_ENTITY_NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, e, 250U, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_add(&cb, e, NPH_COMPONENT_ID_HIERARCHY, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_set(&cb, e, ids.position, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_set(&cb, e, ids.tag_a, NULL), NPH_RESULT_OK); // tags need none
    NTEST_EXPECT_EQ_INT(nph_commands_remove(&cb, e, 250U), NPH_RESULT_INVALID_ARGUMENT);
    const NphEntity foreign = {3U}; // a pending handle this buffer never issued
    NTEST_EXPECT_EQ_INT(nph_commands_despawn(&cb, foreign), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, e, foreign), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, e, NPH_ENTITY_NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_commands_apply(&cb, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_world_has(w, e, ids.tag_a));
    nph_commands_free(&cb);
    nph_world_destroy(w);
}

NTEST(command, cycles_are_skipped)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity a;
    NphEntity b;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, NULL, 0U, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, NULL, 0U, &b), NPH_RESULT_OK);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 0U);
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, b, a), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_commands_set_parent(&cb, a, b), NPH_RESULT_OK); // a cycle: skipped
    NphCommandStats st;
    NTEST_ASSERT_EQ_INT(nph_commands_apply(&cb, &st), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(st.applied, 1U);
    NTEST_EXPECT_EQ_UINT(st.skipped, 1U);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, b), a));
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_parent(w, a)));
    nph_commands_free(&cb);
    nph_world_destroy(w);
}

NTEST(command, allocation_failure_is_sticky)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    u8 memory[1024];
    NphArena arena;
    nph_arena_init(&arena, memory, sizeof memory);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_arena_allocator(&arena), 512U);
    NphResult r = NPH_RESULT_OK;
    u32 recorded = 0U;
    for (u32 i = 0; i < 100U && r == NPH_RESULT_OK; ++i) {
        const NphEntity e = nph_commands_spawn(&cb);
        r = nph_entity_is_null(e) ? cb.error : NPH_RESULT_OK;
        if (r == NPH_RESULT_OK) {
            const TestPosition p = {{1.0F, 1.0F, 1.0F}};
            r = nph_commands_add(&cb, e, ids.position, &p);
            recorded += 1U;
        }
    }
    NTEST_EXPECT_EQ_INT(r, NPH_RESULT_OUT_OF_MEMORY);
    NTEST_EXPECT_GT_UINT(recorded, 0U);
    // Later calls are ignored, apply reports the error and changes nothing.
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_commands_spawn(&cb)));
    NTEST_EXPECT_EQ_INT(nph_commands_despawn(&cb, nph_world_entity_at(w, 0U)),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_commands_apply(&cb, NULL), NPH_RESULT_OUT_OF_MEMORY);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U);
    // The buffer is usable again after the failure was reported.
    nph_arena_reset(&arena);
    NTEST_EXPECT_FALSE(nph_entity_is_null(nph_commands_spawn(&cb)));
    NTEST_EXPECT_EQ_INT(nph_commands_apply(&cb, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 1U);
    nph_commands_free(&cb);
    nph_arena_destroy(&arena);
    nph_world_destroy(w);
}

NTEST(command, world_failures_stop_the_apply)
{
    TestIds ids;
    NphWorld *w = test_world_create(16U, &ids);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 0U);
    for (u32 i = 0; i < 20U; ++i) {
        NTEST_ASSERT_FALSE(nph_entity_is_null(nph_commands_spawn(&cb)));
    }
    NTEST_EXPECT_EQ_INT(nph_commands_apply(&cb, NULL), NPH_RESULT_FULL);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 16U);
    nph_commands_free(&cb);
    nph_world_destroy(w);
}

NTEST(command, pending_handles_are_bounded_by_the_index_bits)
{
    // A 1024-entity world has 10 index bits: 1023 pending handles fit.
    TestIds ids;
    NphWorld *w = test_world_create(1024U, &ids);
    NphCommandBuffer cb;
    nph_commands_init(&cb, w, nph_allocator_system(), 0U);
    u32 n = 0U;
    while (!nph_entity_is_null(nph_commands_spawn(&cb))) {
        n += 1U;
    }
    NTEST_EXPECT_EQ_UINT(n, 1023U);
    NTEST_EXPECT_EQ_INT(cb.error, NPH_RESULT_FULL);
    NTEST_EXPECT_EQ_INT(nph_commands_apply(&cb, NULL), NPH_RESULT_FULL);
    nph_commands_free(&cb);
    nph_world_destroy(w);
}
