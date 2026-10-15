// SPDX-License-Identifier: Apache-2.0
// The world: registry, entities, archetype moves, chunk density, events,
// hashing, and a randomized comparison against a reference model.

#include <nepheline/core/arena.h>

#include "test_scene_support.h"

#include <stdio.h>

NTEST(world, create_rejects_bad_descriptors)
{
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    NphWorld *w = NULL;
    NTEST_EXPECT_EQ_INT(nph_world_create(&desc, &w), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(w);
    desc.alloc = nph_allocator_system();
    desc.max_entities = NPH_ECS_MAX_ENTITIES + 1U;
    NTEST_EXPECT_EQ_INT(nph_world_create(&desc, &w), NPH_RESULT_INVALID_ARGUMENT);
    nph_world_destroy(NULL);
}

NTEST(world, builtins_have_fixed_ids)
{
    NphWorld *w = test_world_create(0U, NULL);
    NTEST_EXPECT_EQ_UINT(nph_world_component_count(w), NPH_COMPONENT_BUILTIN_COUNT);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_capacity(w), NPH_ECS_DEFAULT_MAX_ENTITIES);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR("Name")), NPH_COMPONENT_ID_NAME);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR("LocalTransform")),
                         NPH_COMPONENT_ID_LOCAL_TRANSFORM);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR("WorldTransform")),
                         NPH_COMPONENT_ID_WORLD_TRANSFORM);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR("Hierarchy")),
                         NPH_COMPONENT_ID_HIERARCHY);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR("PrefabInstance")),
                         NPH_COMPONENT_ID_PREFAB_INSTANCE);
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(w, NPH_STR("Nope")), NPH_COMPONENT_NONE);
    NTEST_EXPECT_EQ_PTR(nph_world_component_desc(w, NPH_COMPONENT_ID_NAME), NphName_component());
    NTEST_EXPECT_NULL(nph_world_component_desc(w, 200U));
    nph_world_destroy(w);
}

NTEST(world, registration_is_idempotent_and_checks_layouts)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NTEST_EXPECT_EQ_UINT(ids.position, NPH_COMPONENT_BUILTIN_COUNT);
    NphComponentId again = 0U;
    NTEST_EXPECT_EQ_INT(nph_world_register_component(w, TestPosition_component(), &again),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(again, ids.position);

    // Same name, another layout.
    static const NphFieldDesc fields[1] = {{"other", NPH_FIELD_F32, 0U, 4U, 0U, 0.0F, 0.0F}};
    const NphComponentDesc clash = {"test.Position", 4U, 4U, fields, 1U, 0U, NULL};
    NTEST_EXPECT_EQ_INT(nph_world_register_component(w, &clash, &again), NPH_RESULT_ALREADY_EXISTS);
    NTEST_EXPECT_EQ_UINT(again, NPH_COMPONENT_NONE);

    const NphComponentDesc bad = {"bad name", 4U, 4U, fields, 1U, 0U, NULL};
    NTEST_EXPECT_EQ_INT(nph_world_register_component(w, &bad, &again), NPH_RESULT_INVALID_ARGUMENT);
    nph_world_destroy(w);
}

NTEST(world, registry_fills_at_the_limit)
{
    NphWorld *w = test_world_create(0U, NULL);
    static char names[NPH_ECS_MAX_COMPONENTS][16];
    static NphComponentDesc descs[NPH_ECS_MAX_COMPONENTS];
    u32 registered = NPH_COMPONENT_BUILTIN_COUNT;
    for (u32 i = 0; i < NPH_ECS_MAX_COMPONENTS; ++i) {
        (void)snprintf(names[i], sizeof names[i], "tag.%u", i);
        const NphComponentDesc d = {names[i], 0U, 1U, NULL, 0U, 0U, NULL};
        descs[i] = d;
        NphComponentId id;
        const NphResult r = nph_world_register_component(w, &descs[i], &id);
        if (registered < NPH_ECS_MAX_COMPONENTS) {
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
            NTEST_EXPECT_EQ_UINT(id, registered);
            registered += 1U;
        } else {
            NTEST_EXPECT_EQ_INT(r, NPH_RESULT_FULL);
        }
    }
    NTEST_EXPECT_EQ_UINT(nph_world_component_count(w), NPH_ECS_MAX_COMPONENTS);
    // The highest id works in an archetype.
    const NphComponentId top = NPH_ECS_MAX_COMPONENTS - 1U;
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &top, 1U, &e), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_world_has(w, e, top));
    nph_world_destroy(w);
}

NTEST(world, spawn_get_set_has)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    const NphComponentId comps[3] = {ids.velocity, ids.position, ids.velocity}; // a duplicate
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &e), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_entity_is_null(e));
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, e));
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 1U);
    NTEST_EXPECT_TRUE(nph_world_has(w, e, ids.position));
    NTEST_EXPECT_TRUE(nph_world_has(w, e, ids.velocity));
    NTEST_EXPECT_FALSE(nph_world_has(w, e, ids.health));
    NTEST_EXPECT_FALSE(nph_world_has(w, e, 250U));

    // Defaults.
    const TestVelocity *v = NPH_WORLD_GET(w, e, ids.velocity, TestVelocity);
    NTEST_ASSERT_NOT_NULL(v);
    NTEST_EXPECT_NEAR_F32(v->value.y, 1.0F, 0.0F);
    NTEST_EXPECT_NULL(nph_world_get(w, e, ids.health));

    const TestPosition p = {{1.0F, 2.0F, 3.0F}};
    NTEST_EXPECT_EQ_INT(nph_world_set(w, e, ids.position, &p), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.position, TestPosition)->value.z, 3.0F, 0.0F);

    TestPosition *pm = NPH_WORLD_GET_MUT(w, e, ids.position, TestPosition);
    NTEST_ASSERT_NOT_NULL(pm);
    pm->value.x = 9.0F;
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.position, TestPosition)->value.x, 9.0F, 0.0F);

    // set adds a missing component.
    const TestHealth h = {5.0F, 10.0F, e};
    NTEST_EXPECT_EQ_INT(nph_world_set(w, e, ids.health, &h), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_entity_equals(NPH_WORLD_GET(w, e, ids.health, TestHealth)->attacker, e));
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.position, TestPosition)->value.x, 9.0F, 0.0F);

    NphComponentId list[8];
    NTEST_ASSERT_EQ_UINT(nph_world_entity_components(w, e, list, 8U), 3U);
    NTEST_EXPECT_EQ_UINT(list[0], ids.position);
    NTEST_EXPECT_EQ_UINT(list[1], ids.velocity);
    NTEST_EXPECT_EQ_UINT(list[2], ids.health);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_components(w, e, list, 1U), 3U);

    // Bad arguments.
    NTEST_EXPECT_EQ_INT(nph_world_set(w, e, ids.position, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_world_set(w, e, 250U, &p), NPH_RESULT_INVALID_ARGUMENT);
    const NphComponentId bad = 250U;
    NphEntity none;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, &bad, 1U, &none), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, NULL, 1U, &none), NPH_RESULT_INVALID_ARGUMENT);
    // Hierarchy is internal: added without a value, never set.
    NphHierarchy hv;
    memset(&hv, 0, sizeof hv);
    NTEST_EXPECT_EQ_INT(nph_world_set(w, e, NPH_COMPONENT_ID_HIERARCHY, &hv),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_world_add(w, e, NPH_COMPONENT_ID_HIERARCHY, &hv),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_world_add(w, e, NPH_COMPONENT_ID_HIERARCHY, NULL), NPH_RESULT_OK);
    TEST_EXPECT_ASSERT(NTEST_EXPECT_NULL(nph_world_get_mut(w, e, NPH_COMPONENT_ID_HIERARCHY)));
    nph_world_destroy(w);
}

NTEST(world, add_and_remove_move_between_archetypes)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK);
    const TestPosition p = {{4.0F, 5.0F, 6.0F}};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, e, ids.position, &p), NPH_RESULT_OK);

    const TestHealth h = {1.0F, 2.0F, {0U}};
    NTEST_EXPECT_EQ_INT(nph_world_add(w, e, ids.health, &h), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_add(w, e, ids.health, &h), NPH_RESULT_ALREADY_EXISTS);
    NTEST_EXPECT_EQ_INT(nph_world_add(w, e, ids.tag_a, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_add(w, e, ids.velocity, NULL), NPH_RESULT_OK); // default
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.velocity, TestVelocity)->value.y, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.position, TestPosition)->value.y, 5.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.health, TestHealth)->maximum, 2.0F, 0.0F);
    NTEST_EXPECT_TRUE(nph_world_has(w, e, ids.tag_a));
    NTEST_EXPECT_NULL(nph_world_get(w, e, ids.tag_a)); // tags have no data
    NTEST_EXPECT_EQ_INT(nph_world_set(w, e, ids.tag_a, NULL), NPH_RESULT_OK);

    NTEST_EXPECT_EQ_INT(nph_world_remove(w, e, ids.health), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_remove(w, e, ids.health), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(nph_world_remove(w, e, 250U), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_FALSE(nph_world_has(w, e, ids.health));
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, e, ids.position, TestPosition)->value.z, 6.0F, 0.0F);
    NTEST_EXPECT_EQ_INT(nph_world_remove(w, e, ids.tag_a), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_remove(w, e, ids.velocity), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_remove(w, e, ids.position), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_components(w, e, NULL, 0U), 0U);
    NTEST_EXPECT_TRUE(nph_world_is_alive(w, e));
    nph_world_destroy(w);
}

NTEST(world, despawned_handles_are_stale)
{
    TestIds ids;
    NphWorld *w = test_world_create(16U, &ids);
    NphEntity a;
    NphEntity b;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &b), NPH_RESULT_OK);
    const TestPosition pb = {{7.0F, 0.0F, 0.0F}};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, b, ids.position, &pb), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_despawn(w, a), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, a));
    NTEST_EXPECT_EQ_INT(nph_world_despawn(w, a), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(nph_world_despawn(w, NPH_ENTITY_NULL), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_NULL(nph_world_get(w, a, ids.position));
    NTEST_EXPECT_EQ_INT(nph_world_set(w, a, ids.position, &pb), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_INT(nph_world_add(w, a, ids.health, NULL), NPH_RESULT_NOT_FOUND);
    // b was moved into a's row; its record follows.
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, b, ids.position, TestPosition)->value.x, 7.0F, 0.0F);

    // Fill the world; the freed slot comes back with a new generation.
    NphEntity more[15];
    for (u32 i = 0; i < 15U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, NULL, 0U, &more[i]), NPH_RESULT_OK);
        NTEST_EXPECT_NE_UINT(more[i].value, a.value);
    }
    NphEntity full;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, NULL, 0U, &full), NPH_RESULT_FULL);
    NTEST_EXPECT_EQ_INT(nph_world_spawn_batch(w, NULL, 0U, 1U, NULL), NPH_RESULT_FULL);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, a));
    TEST_EXPECT_ASSERT((void)nph_world_entity_index(w, a));
    nph_world_destroy(w);
}

NTEST(world, slot_iteration_and_indices)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity e[5];
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e[i]), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(nph_world_entity_index(w, e[i]), i);
    }
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[2]), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_slot_count(w), 5U);
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_entity_at(w, 1U), e[1]));
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_entity_at(w, 2U)));
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_world_entity_at(w, 99U)));
    nph_world_destroy(w);
}

NTEST(world, chunks_stay_dense)
{
    TestIds ids;
    NphWorld *w = test_world_create(1U << 16U, &ids);
    const u32 n = 5000U;
    static NphEntity e[5000];
    const NphComponentId comps[2] = {ids.position, ids.velocity};
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, n, e), NPH_RESULT_OK);
    for (u32 i = 0; i < n; ++i) {
        TestPosition p = {{(f32)i, 0.0F, 0.0F}};
        NTEST_ASSERT_EQ_INT(nph_world_set(w, e[i], ids.position, &p), NPH_RESULT_OK);
    }
    NphWorldStats st;
    nph_world_get_stats(w, &st);
    const u32 full_chunks = st.chunks;
    // Remove every other entity; the rest keep their values and pack into
    // half the chunks.
    for (u32 i = 0; i < n; i += 2U) {
        NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[i]), NPH_RESULT_OK);
    }
    for (u32 i = 1; i < n; i += 2U) {
        NTEST_ASSERT_NEAR_F32(NPH_WORLD_GET(w, e[i], ids.position, TestPosition)->value.x, (f32)i,
                              0.0F);
    }
    nph_world_get_stats(w, &st);
    NTEST_EXPECT_EQ_UINT(st.entities, n / 2U);
    NTEST_EXPECT_LE_UINT(st.chunks, (full_chunks + 1U) / 2U + 1U);

    // Iterate: every chunk but the last of the archetype is full.
    const NphQueryTerm t = nph_term_read(ids.position);
    const NphQueryDesc qd = {&t, 1U};
    NphQuery *q;
    NTEST_ASSERT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_OK);
    NphQueryIter it;
    nph_query_iter(q, &it);
    NphChunkView v;
    u32 chunks = 0U;
    u32 total = 0U;
    u32 partial = 0U;
    u32 first_count = 0U;
    while (nph_query_next(&it, &v)) {
        first_count = chunks == 0U ? v.count : first_count;
        partial += v.count != first_count ? 1U : 0U;
        total += v.count;
        chunks += 1U;
    }
    NTEST_EXPECT_EQ_UINT(total, n / 2U);
    NTEST_EXPECT_LE_UINT(partial, 1U);
    nph_query_destroy(q);

    // Trim gives back slabs that hold no live chunk.
    nph_world_clear(w);
    nph_world_get_stats(w, &st);
    NTEST_EXPECT_EQ_UINT(st.entities, 0U);
    NTEST_EXPECT_EQ_UINT(st.chunks, 0U);
    NTEST_EXPECT_GT_UINT(st.free_chunks, 0U);
    NTEST_EXPECT_FALSE(nph_world_is_alive(w, e[1]));
    nph_world_trim(w);
    nph_world_get_stats(w, &st);
    NTEST_EXPECT_EQ_UINT(st.slabs, 0U);
    NTEST_EXPECT_EQ_UINT(st.free_chunks, 0U);
    NTEST_EXPECT_EQ_UINT(st.slab_bytes, 0U);
    // And the world is still usable.
    NphEntity again;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, comps, 2U, &again), NPH_RESULT_OK);
    nph_world_destroy(w);
}

NTEST(world, trim_keeps_slabs_in_use)
{
    TestIds ids;
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.chunks_per_slab = 2U;
    NphWorld *w = test_world_create_desc(&desc, &ids);
    // Five archetypes: five chunks over three slabs.
    NphEntity e[5];
    const NphComponentId sets[5] = {ids.position, ids.velocity, ids.health, ids.all, ids.big};
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &sets[i], 1U, &e[i]), NPH_RESULT_OK);
    }
    NphWorldStats st;
    nph_world_get_stats(w, &st);
    NTEST_EXPECT_EQ_UINT(st.slabs, 3U);
    NTEST_EXPECT_EQ_UINT(st.free_chunks, 1U);
    // Free the chunks of the first slab (entities 0 and 1).
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[1]), NPH_RESULT_OK);
    nph_world_trim(w);
    nph_world_get_stats(w, &st);
    NTEST_EXPECT_EQ_UINT(st.slabs, 2U);
    NTEST_EXPECT_EQ_UINT(st.free_chunks, 1U);
    for (u32 i = 2; i < 5U; ++i) {
        NTEST_EXPECT_TRUE(nph_world_has(w, e[i], sets[i]));
    }
    NphEntity f;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &f), NPH_RESULT_OK);
    nph_world_destroy(w);
}

NTEST(world, spawn_batch_is_all_or_nothing)
{
    TestIds ids;
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.max_chunks = 2U;
    desc.chunks_per_slab = 1U;
    NphWorld *w = test_world_create_desc(&desc, &ids);
    const NphComponentId comps[2] = {ids.position, ids.health};
    NphEntity one;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 2U, &one), NPH_RESULT_OK);
    // More than two chunks' worth: refused, nothing created.
    NTEST_EXPECT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 10000U, NULL),
                        NPH_RESULT_OUT_OF_MEMORY);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 1U);
    NTEST_EXPECT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 0U, NULL), NPH_RESULT_OK);
    // Exactly what fits.
    NphWorldStats st;
    nph_world_get_stats(w, &st);
    NTEST_EXPECT_EQ_UINT(st.chunks, 1U);
    NphEntity batch[64];
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(w, comps, 2U, 64U, batch), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 65U);
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_TRUE(nph_world_is_alive(w, batch[i]));
        NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, batch[i], ids.health, TestHealth)->current, 100.0F,
                              0.0F);
    }
    // A spawn into a third archetype needs a third chunk.
    NphEntity other;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, &ids.velocity, 1U, &other), NPH_RESULT_OK);
    nph_world_get_stats(w, &st);
    if (st.chunks >= 2U) {
        NphEntity none;
        NTEST_EXPECT_EQ_INT(nph_world_spawn(w, &ids.big, 1U, &none), NPH_RESULT_OUT_OF_MEMORY);
        // A move that needs a chunk fails and leaves the entity as it was.
        NTEST_EXPECT_EQ_INT(nph_world_add(w, other, ids.big, NULL), NPH_RESULT_OUT_OF_MEMORY);
        NTEST_EXPECT_TRUE(nph_world_has(w, other, ids.velocity));
        NTEST_EXPECT_FALSE(nph_world_has(w, other, ids.big));
    }
    nph_world_destroy(w);
}

NTEST(world, archetype_too_large_for_a_chunk)
{
    NphWorld *w = test_world_create(0U, NULL);
    static const NphFieldDesc f[1] = {{"x", NPH_FIELD_F32, 0U, 4U, 0U, 0.0F, 0.0F}};
    static char names[9][16];
    static NphComponentDesc descs[9];
    NphComponentId ids[9];
    for (u32 i = 0; i < 9U; ++i) {
        (void)snprintf(names[i], sizeof names[i], "big.%u", i);
        const NphComponentDesc d = {names[i], NPH_ECS_MAX_COMPONENT_SIZE, 4U, f, 1U, 0U, NULL};
        descs[i] = d;
        NTEST_ASSERT_EQ_INT(nph_world_register_component(w, &descs[i], &ids[i]), NPH_RESULT_OK);
    }
    NphEntity e;
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, ids, 7U, &e), NPH_RESULT_OK); // 7 x 2 KiB fits once
    NTEST_EXPECT_EQ_INT(nph_world_spawn(w, ids, 9U, &e), NPH_RESULT_INVALID_ARGUMENT);
    nph_world_destroy(w);
}

NTEST(world, events_record_changes)
{
    TestIds ids;
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.event_capacity = 6U;
    NphWorld *w = test_world_create_desc(&desc, &ids);
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &e), NPH_RESULT_OK);
    const TestHealth h = {1.0F, 1.0F, {0U}};
    NTEST_ASSERT_EQ_INT(nph_world_add(w, e, ids.health, &h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set(w, e, ids.health, &h), NPH_RESULT_OK);
    NTEST_ASSERT_NOT_NULL(nph_world_get_mut(w, e, ids.position));
    NTEST_ASSERT_EQ_INT(nph_world_remove(w, e, ids.health), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e), NPH_RESULT_OK);
    NphEntity extra;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, NULL, 0U, &extra), NPH_RESULT_OK); // dropped

    u32 count = 0U;
    u32 dropped = 0U;
    const NphWorldEvent *ev = nph_world_events(w, &count, &dropped);
    NTEST_ASSERT_EQ_UINT(count, 6U);
    NTEST_EXPECT_EQ_UINT(dropped, 1U);
    const u32 expect[6] = {NPH_WORLD_EVENT_SPAWNED, NPH_WORLD_EVENT_ADDED,
                           NPH_WORLD_EVENT_SET,     NPH_WORLD_EVENT_SET,
                           NPH_WORLD_EVENT_REMOVED, NPH_WORLD_EVENT_DESPAWNED};
    for (u32 i = 0; i < 6U; ++i) {
        NTEST_EXPECT_EQ_UINT(ev[i].type, expect[i]);
        NTEST_EXPECT_TRUE(nph_entity_equals(ev[i].entity, e));
    }
    NTEST_EXPECT_EQ_UINT(ev[1].component, ids.health);
    NTEST_EXPECT_EQ_UINT(ev[3].component, ids.position);
    NTEST_EXPECT_EQ_UINT(ev[0].component, NPH_COMPONENT_NONE);
    nph_world_clear_events(w);
    (void)nph_world_events(w, &count, &dropped);
    NTEST_EXPECT_EQ_UINT(count, 0U);
    NTEST_EXPECT_EQ_UINT(dropped, 0U);
    nph_world_destroy(w);

    // Off by default.
    NphWorld *quiet = test_world_create(0U, &ids);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(quiet, NULL, 0U, &e), NPH_RESULT_OK);
    (void)nph_world_events(quiet, &count, NULL);
    NTEST_EXPECT_EQ_UINT(count, 0U);
    nph_world_destroy(quiet);
}

NTEST(world, hash_compares_contents)
{
    TestIds ids;
    NphWorld *a = test_world_create(0U, &ids);
    NphWorld *b = test_world_create(0U, &ids);
    NTEST_EXPECT_EQ_UINT(nph_world_hash(a), nph_world_hash(b));
    for (u32 k = 0; k < 2U; ++k) {
        NphWorld *w = k == 0U ? a : b;
        for (u32 i = 0; i < 100U; ++i) {
            NphEntity e;
            const NphComponentId comps[2] = {ids.position, i % 3U == 0U ? ids.tag_a : ids.health};
            NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 2U, &e), NPH_RESULT_OK);
            const TestPosition p = {{(f32)i, 1.0F, 2.0F}};
            NTEST_ASSERT_EQ_INT(nph_world_set(w, e, ids.position, &p), NPH_RESULT_OK);
        }
    }
    NTEST_EXPECT_EQ_UINT(nph_world_hash(a), nph_world_hash(b));
    TestPosition *p = NPH_WORLD_GET_MUT(b, nph_world_entity_at(b, 50U), ids.position, TestPosition);
    p->value.y = 1.5F;
    NTEST_EXPECT_NE_UINT(nph_world_hash(a), nph_world_hash(b));
    p->value.y = 1.0F;
    NTEST_EXPECT_EQ_UINT(nph_world_hash(a), nph_world_hash(b));
    NTEST_ASSERT_EQ_INT(nph_world_add(b, nph_world_entity_at(b, 50U), ids.tag_b, NULL),
                        NPH_RESULT_OK);
    NTEST_EXPECT_NE_UINT(nph_world_hash(a), nph_world_hash(b));
    nph_world_destroy(a);
    nph_world_destroy(b);
}

NTEST(world, registry_copies_into_another_world)
{
    TestIds ids;
    NphWorld *a = test_world_create(0U, &ids);
    NphWorldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.registry_from = a;
    NphWorld *b = test_world_create_desc(&desc, NULL);
    NTEST_EXPECT_EQ_UINT(nph_world_component_count(b), nph_world_component_count(a));
    NTEST_EXPECT_EQ_UINT(nph_world_find_component(b, NPH_STR("test.Health")), ids.health);
    nph_world_destroy(b);
    nph_world_destroy(a);
}

NTEST(world, out_of_memory_paths_leave_the_world_intact)
{
    // A small arena runs out at every step of world growth in turn; every
    // failure must leave the world consistent.
    for (u64 size = NPH_KIB(64); size < NPH_MIB(4); size += NPH_KIB(61)) {
        NphArena arena;
        NTEST_ASSERT_EQ_INT(nph_arena_init_from(&arena, nph_allocator_system(), size),
                            NPH_RESULT_OK);
        NphWorldDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.alloc = nph_arena_allocator(&arena);
        desc.max_entities = 1024U;
        desc.chunks_per_slab = 1U;
        NphWorld *w = NULL;
        const NphResult created = nph_world_create(&desc, &w);
        if (created == NPH_RESULT_OK) {
            TestIds ids;
            NphResult r = nph_world_register_component(w, TestPosition_component(), &ids.position);
            r = r == NPH_RESULT_OK
                    ? nph_world_register_component(w, TestHealth_component(), &ids.health)
                    : r;
            u32 alive = 0U;
            for (u32 i = 0; r == NPH_RESULT_OK && i < 1000U; ++i) {
                NphEntity e;
                const NphComponentId comps[2] = {ids.position, ids.health};
                r = nph_world_spawn(w, comps, 1U + (i % 2U), &e);
                alive += r == NPH_RESULT_OK ? 1U : 0U;
                if (r == NPH_RESULT_OK && i % 7U == 0U) {
                    r = nph_world_add(w, e, i % 2U == 0U ? ids.health : NPH_COMPONENT_ID_NAME,
                                      NULL);
                }
            }
            NTEST_EXPECT_TRUE(r == NPH_RESULT_OK || r == NPH_RESULT_OUT_OF_MEMORY);
            NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), alive);
            (void)nph_world_hash(w); // walks everything
            nph_world_destroy(w);
        } else {
            NTEST_EXPECT_EQ_INT(created, NPH_RESULT_OUT_OF_MEMORY);
        }
        nph_arena_destroy(&arena);
    }
}

// ---------------------------------------------------------------------------
// A randomized comparison against a reference model
// ---------------------------------------------------------------------------

#define MODEL_MAX 600U

typedef struct Model {
    NphEntity e[MODEL_MAX];
    bool alive[MODEL_MAX];
    bool has_pos[MODEL_MAX];
    bool has_health[MODEL_MAX];
    bool has_tag[MODEL_MAX];
    f32 pos[MODEL_MAX];
    f32 health[MODEL_MAX];
    u32 count;
} Model;

static void model_check(NphWorld *w, const TestIds *ids, const Model *m, NphQuery *q_pos)
{
    u32 alive = 0U;
    u32 with_pos = 0U;
    for (u32 i = 0; i < m->count; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_is_alive(w, m->e[i]), m->alive[i]);
        if (!m->alive[i]) {
            continue;
        }
        alive += 1U;
        NTEST_ASSERT_EQ_INT(nph_world_has(w, m->e[i], ids->position), m->has_pos[i]);
        NTEST_ASSERT_EQ_INT(nph_world_has(w, m->e[i], ids->health), m->has_health[i]);
        NTEST_ASSERT_EQ_INT(nph_world_has(w, m->e[i], ids->tag_a), m->has_tag[i]);
        if (m->has_pos[i]) {
            with_pos += 1U;
            NTEST_ASSERT_NEAR_F32(NPH_WORLD_GET(w, m->e[i], ids->position, TestPosition)->value.x,
                                  m->pos[i], 0.0F);
        }
        if (m->has_health[i]) {
            const TestHealth *h = NPH_WORLD_GET(w, m->e[i], ids->health, TestHealth);
            NTEST_ASSERT_NEAR_F32(h->current, m->health[i], 0.0F);
            NTEST_ASSERT_TRUE(nph_entity_equals(h->attacker, m->e[i]));
        }
    }
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(w), alive);
    NTEST_ASSERT_EQ_UINT(nph_query_entity_count(q_pos), with_pos);
    // Iteration visits each entity with a position once.
    NphQueryIter it;
    nph_query_iter(q_pos, &it);
    NphChunkView v;
    u32 seen = 0U;
    while (nph_query_next(&it, &v)) {
        const TestPosition *p = NPH_CHUNK_COLUMN(&v, 0, TestPosition);
        for (u32 r = 0; r < v.count; ++r) {
            const NphEntity e = v.entities[r];
            NTEST_ASSERT_TRUE(nph_world_is_alive(w, e));
            NTEST_ASSERT_NEAR_F32(p[r].value.x,
                                  NPH_WORLD_GET(w, e, ids->position, TestPosition)->value.x, 0.0F);
        }
        seen += v.count;
    }
    NTEST_ASSERT_EQ_UINT(seen, with_pos);
}

NTEST(world, randomized_against_a_model)
{
    TestIds ids;
    NphWorld *w = test_world_create(1024U, &ids);
    const NphQueryTerm t = nph_term_read(ids.position);
    const NphQueryDesc qd = {&t, 1U};
    NphQuery *q;
    NTEST_ASSERT_EQ_INT(nph_query_create(w, &qd, &q), NPH_RESULT_OK);
    static Model m;
    memset(&m, 0, sizeof m);
    u64 rng = 0xC0FFEEULL;
    const u32 steps = 3000U * TEST_SCALE;
    for (u32 step = 0; step < steps; ++step) {
        const u32 op = test_rng_below(&rng, 10U);
        const u32 pick = m.count > 0U ? test_rng_below(&rng, m.count) : 0U;
        if (op < 3U && m.count < MODEL_MAX) {
            NphComponentId comps[3];
            u32 n = 0U;
            const u32 bits = test_rng_below(&rng, 8U);
            const u32 i = m.count++;
            m.has_pos[i] = (bits & 1U) != 0U;
            m.has_health[i] = (bits & 2U) != 0U;
            m.has_tag[i] = (bits & 4U) != 0U;
            if (m.has_pos[i]) {
                comps[n++] = ids.position;
            }
            if (m.has_health[i]) {
                comps[n++] = ids.health;
            }
            if (m.has_tag[i]) {
                comps[n++] = ids.tag_a;
            }
            NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, n, &m.e[i]), NPH_RESULT_OK);
            m.alive[i] = true;
            m.pos[i] = 0.0F;
            m.health[i] = 100.0F;
            if (m.has_health[i]) {
                TestHealth h = {100.0F, 100.0F, m.e[i]};
                NTEST_ASSERT_EQ_INT(nph_world_set(w, m.e[i], ids.health, &h), NPH_RESULT_OK);
            }
        } else if (m.count == 0U || !m.alive[pick]) {
            continue;
        } else if (op == 3U) {
            NTEST_ASSERT_EQ_INT(nph_world_despawn(w, m.e[pick]), NPH_RESULT_OK);
            m.alive[pick] = false;
        } else if (op == 4U) {
            if (m.has_pos[pick]) {
                NTEST_ASSERT_EQ_INT(nph_world_remove(w, m.e[pick], ids.position), NPH_RESULT_OK);
            } else {
                const TestPosition p = {{(f32)step, 0.0F, 0.0F}};
                NTEST_ASSERT_EQ_INT(nph_world_add(w, m.e[pick], ids.position, &p), NPH_RESULT_OK);
                m.pos[pick] = (f32)step;
            }
            m.has_pos[pick] = !m.has_pos[pick];
        } else if (op == 5U) {
            if (m.has_health[pick]) {
                NTEST_ASSERT_EQ_INT(nph_world_remove(w, m.e[pick], ids.health), NPH_RESULT_OK);
            } else {
                const TestHealth h = {(f32)step, 1.0F, m.e[pick]};
                NTEST_ASSERT_EQ_INT(nph_world_add(w, m.e[pick], ids.health, &h), NPH_RESULT_OK);
                m.health[pick] = (f32)step;
            }
            m.has_health[pick] = !m.has_health[pick];
        } else if (op == 6U) {
            if (m.has_tag[pick]) {
                NTEST_ASSERT_EQ_INT(nph_world_remove(w, m.e[pick], ids.tag_a), NPH_RESULT_OK);
            } else {
                NTEST_ASSERT_EQ_INT(nph_world_add(w, m.e[pick], ids.tag_a, NULL), NPH_RESULT_OK);
            }
            m.has_tag[pick] = !m.has_tag[pick];
        } else {
            const TestPosition p = {{(f32)(step * 3U), 0.0F, 0.0F}};
            NTEST_ASSERT_EQ_INT(nph_world_set(w, m.e[pick], ids.position, &p), NPH_RESULT_OK);
            m.pos[pick] = (f32)(step * 3U);
            m.has_pos[pick] = true;
        }
        if (step % 97U == 0U) {
            model_check(w, &ids, &m, q);
        }
    }
    model_check(w, &ids, &m, q);
    nph_query_destroy(q);
    nph_world_destroy(w);
}
