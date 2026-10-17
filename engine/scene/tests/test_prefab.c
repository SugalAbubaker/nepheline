// SPDX-License-Identifier: Apache-2.0
// Prefabs: instantiation (values, hierarchy, internal references), saving
// instances as differences, and a prefab change reaching instances on
// reload except where they override it.

#include <nepheline/scene/scene_io.h>

#include "test_scene_support.h"

typedef struct Library {
    const NphPrefab *prefabs[4];
    u32 count;
} Library;

static const NphPrefab *library_resolve(void *user, NphStrId id)
{
    const Library *lib = (const Library *)user;
    for (u32 i = 0; i < lib->count; ++i) {
        if (nph_str_id_equals(nph_prefab_id(lib->prefabs[i]), id)) {
            return lib->prefabs[i];
        }
    }
    return NULL;
}

// A "crate" prefab: a root with a position and health whose attacker is
// its own lid, and a lid child with a tag.
static NphPrefab *make_crate(const NphWorld *registry, const TestIds *ids, f32 max_health)
{
    NphPrefab *p = NULL;
    NTEST_ASSERT_EQ_INT(
        nph_prefab_create(NPH_STR("crate"), registry, nph_allocator_system(), 0U, &p),
        NPH_RESULT_OK);
    NphWorld *pw = nph_prefab_world(p);
    NphEntity root;
    NphEntity lid;
    const NphComponentId rc[3] = {ids->position, ids->health, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(pw, rc, 3U, &root), NPH_RESULT_OK);
    const NphComponentId lc[2] = {ids->tag_a, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(pw, lc, 2U, &lid), NPH_RESULT_OK);
    const TestHealth h = {max_health, max_health, lid};
    NTEST_ASSERT_EQ_INT(nph_world_set(pw, root, ids->health, &h), NPH_RESULT_OK);
    const TestPosition pos = {{1.0F, 2.0F, 3.0F}};
    NTEST_ASSERT_EQ_INT(nph_world_set(pw, root, ids->position, &pos), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(pw, lid, root), NPH_RESULT_OK);
    return p;
}

NTEST(prefab, create_and_inspect)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphPrefab *p = NULL;
    NTEST_EXPECT_EQ_INT(nph_prefab_create(NPH_STR(""), w, nph_allocator_system(), 0U, &p),
                        NPH_RESULT_INVALID_ARGUMENT);
    p = make_crate(w, &ids, 100.0F);
    NTEST_EXPECT_TRUE(nph_str_equals(nph_prefab_name(p), NPH_STR("crate")));
    NTEST_EXPECT_EQ_UINT(nph_prefab_id(p).value, nph_str_id(NPH_STR("crate")).value);
    NTEST_EXPECT_EQ_UINT(nph_prefab_entity_count(p), 2U);
    NTEST_EXPECT_FALSE(nph_entity_is_null(nph_prefab_entity(p, 1U)));
    NTEST_EXPECT_TRUE(nph_entity_is_null(nph_prefab_entity(p, 2U)));
    NTEST_EXPECT_EQ_PTR(nph_prefab_world_const(p), nph_prefab_world(p));
    nph_prefab_destroy(p);
    nph_prefab_destroy(NULL);
    nph_world_destroy(w);
}

NTEST(prefab, instantiate_copies_values_hierarchy_and_references)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphPrefab *p = make_crate(w, &ids, 100.0F);
    NphEntity root_a;
    NphEntity all_b[2];
    NTEST_ASSERT_EQ_INT(nph_prefab_instantiate(p, w, &root_a, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_prefab_instantiate(p, w, NULL, all_b), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 4U);

    const NphEntity lid_a = nph_world_first_child(w, root_a);
    NTEST_ASSERT_FALSE(nph_entity_is_null(lid_a));
    NTEST_EXPECT_TRUE(nph_world_has(w, lid_a, ids.tag_a));
    const TestHealth *h = NPH_WORLD_GET(w, root_a, ids.health, TestHealth);
    NTEST_EXPECT_TRUE(nph_entity_equals(h->attacker, lid_a)); // points into its own instance
    NTEST_EXPECT_NEAR_F32(NPH_WORLD_GET(w, root_a, ids.position, TestPosition)->value.y, 2.0F,
                          0.0F);
    const NphPrefabInstance *pi =
        NPH_WORLD_GET(w, lid_a, NPH_COMPONENT_ID_PREFAB_INSTANCE, NphPrefabInstance);
    NTEST_ASSERT_NOT_NULL(pi);
    NTEST_EXPECT_EQ_UINT(pi->index, 1U);
    NTEST_EXPECT_TRUE(nph_entity_equals(pi->root, root_a));
    NTEST_EXPECT_TRUE(
        nph_str_equals(nph_str_table_lookup(nph_world_strings(w), pi->prefab), NPH_STR("crate")));
    // The second instance is independent.
    NTEST_EXPECT_TRUE(
        nph_entity_equals(NPH_WORLD_GET(w, all_b[0], ids.health, TestHealth)->attacker, all_b[1]));
    NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w, all_b[1]), all_b[0]));

    // A world with another registry is refused.
    NphWorld *other = test_world_create(0U, NULL);
    NphEntity none;
    NTEST_EXPECT_EQ_INT(nph_prefab_instantiate(p, other, &none, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(other), 0U);
    // A world too small is refused whole.
    NphWorld *tiny = test_world_create(1U, &ids);
    NTEST_EXPECT_EQ_INT(nph_prefab_instantiate(p, tiny, &none, NULL), NPH_RESULT_FULL);
    nph_world_destroy(tiny);
    nph_world_destroy(other);
    nph_prefab_destroy(p);
    nph_world_destroy(w);
}

NTEST(prefab, instances_save_as_differences_and_follow_the_prefab)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphPrefab *p = make_crate(w, &ids, 100.0F);
    Library lib = {{p}, 1U};

    NphEntity a[2];
    NphEntity b[2];
    NTEST_ASSERT_EQ_INT(nph_prefab_instantiate(p, w, NULL, a), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_prefab_instantiate(p, w, NULL, b), NPH_RESULT_OK);
    // Instance b: move it, add a velocity, remove the lid's tag.
    TestPosition *pos = NPH_WORLD_GET_MUT(w, b[0], ids.position, TestPosition);
    pos->value.x = 9.0F;
    NTEST_ASSERT_EQ_INT(nph_world_add(w, b[0], ids.velocity, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_remove(w, b[1], ids.tag_a), NPH_RESULT_OK);

    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.resolve = library_resolve;
    sd.resolve_user = &lib;
    sd.compact = 1;
    NphStrBuilder text;
    nph_sb_init(&text, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, &sd, &text, NULL), NPH_RESULT_OK);
    const NphStr t = nph_sb_str(&text);
    // Instance a is the prefab exactly: nothing but the reference.
    NTEST_EXPECT_TRUE(nph_str_contains(
        t, NPH_STR("{\"id\":0,\"prefab\":{\"name\":\"crate\",\"index\":0,\"root\":0}}")));
    NTEST_EXPECT_TRUE(nph_str_contains(
        t,
        NPH_STR("{\"id\":1,\"parent\":0,\"prefab\":{\"name\":\"crate\",\"index\":1,\"root\":0}}")));
    // Instance b: one field, one added component, one removed component.
    NTEST_EXPECT_TRUE(
        nph_str_contains(t, NPH_STR("\"overrides\":{\"test.Position\":{\"value\":[9,2,3]}}")));
    NTEST_EXPECT_TRUE(nph_str_contains(t, NPH_STR("\"components\":{\"test.Velocity\":")));
    NTEST_EXPECT_TRUE(nph_str_contains(t, NPH_STR("\"removed\":[\"test.TagA\"]")));
    // The health's attacker points inside the instance, as in the prefab:
    // not an override.
    NTEST_EXPECT_FALSE(nph_str_contains(t, NPH_STR("test.Health")));

    // Round trip with the same prefab.
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.resolve = library_resolve;
    ld.resolve_user = &lib;
    NphWorld *w2 = test_world_create(0U, &ids);
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(w2, t, &ld, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(res.error, "");
    NphStrBuilder again;
    nph_sb_init(&again, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w2, &sd, &again, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&again), nph_sb_cstr(&text));

    // The prefab changes: max health 250. Instances that did not override
    // the health pick it up; b's override of its position stays.
    NphPrefab *p2 = make_crate(w, &ids, 250.0F);
    TestPosition *pp = NPH_WORLD_GET_MUT(nph_prefab_world(p2), nph_prefab_entity(p2, 0U),
                                         ids.position, TestPosition);
    pp->value.y = 7.0F;
    Library lib2 = {{p2}, 1U};
    ld.resolve_user = &lib2;
    NphWorld *w3 = test_world_create(0U, &ids);
    NphEntity created[4];
    ld.out_entities = created;
    ld.out_capacity = 4U;
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(w3, t, &ld, &res), NPH_RESULT_OK);
    for (u32 i = 0; i < 4U; ++i) {
        const TestHealth *h = NPH_WORLD_GET(w3, created[i], ids.health, TestHealth);
        if (h != NULL) {
            NTEST_EXPECT_NEAR_F32(h->maximum, 250.0F, 0.0F);
            // The internal reference is rebuilt for each instance.
            NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(w3, h->attacker), created[i]));
        }
    }
    const TestPosition *pa = NPH_WORLD_GET(w3, created[0], ids.position, TestPosition);
    const TestPosition *pb = NPH_WORLD_GET(w3, created[2], ids.position, TestPosition);
    NTEST_EXPECT_NEAR_F32(pa->value.y, 7.0F, 0.0F); // not overridden: the new prefab value
    NTEST_EXPECT_NEAR_F32(pb->value.x, 9.0F, 0.0F); // overridden
    NTEST_EXPECT_NEAR_F32(pb->value.y, 2.0F, 0.0F); // the whole field was overridden
    NTEST_EXPECT_TRUE(nph_world_has(w3, created[2], ids.velocity));
    NTEST_EXPECT_FALSE(nph_world_has(w3, created[3], ids.tag_a));
    NTEST_EXPECT_TRUE(nph_world_has(w3, created[1], ids.tag_a));

    // Without a resolver, saving writes instances in full, marked so; they
    // load exactly as written without the prefab, and a save that has the
    // prefab writes the differences again.
    NphSceneSaveDesc full;
    memset(&full, 0, sizeof full);
    full.compact = 1;
    nph_sb_clear(&again);
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, &full, &again, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_sb_str(&again), NPH_STR("\"full\":true}")));
    NTEST_EXPECT_TRUE(
        nph_str_contains(nph_sb_str(&again), NPH_STR("\"components\":{\"LocalTransform\"")));
    NphWorld *w4 = test_world_create(0U, &ids);
    NTEST_EXPECT_EQ_INT(nph_scene_load_text(w4, nph_sb_str(&again), NULL, &res), NPH_RESULT_OK);
    ld.resolve_user = &lib;
    ld.out_entities = NULL;
    NphStrBuilder third;
    nph_sb_init(&third, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w4, &sd, &third, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&third), nph_sb_cstr(&text));

    // A bad instance index.
    NTEST_EXPECT_EQ_INT(
        nph_scene_load_text(w4,
                            NPH_STR("{\"nepheline_scene\":1,\"entities\":[{\"id\":0,"
                                    "\"prefab\":{\"name\":\"crate\",\"index\":5,"
                                    "\"root\":0}}]}"),
                            &ld, &res),
        NPH_RESULT_NOT_FOUND);
    // Two entities claiming the same place in one instance: both load
    // (references inside the instance resolve to the first).
    NTEST_EXPECT_EQ_INT(
        nph_scene_load_text(w4,
                            NPH_STR("{\"nepheline_scene\":1,\"entities\":[{\"id\":0,"
                                    "\"prefab\":{\"name\":\"crate\",\"index\":0,"
                                    "\"root\":0}},{\"id\":1,\"prefab\":{\"name\":"
                                    "\"crate\",\"index\":0,\"root\":0}}]}"),
                            &ld, &res),
        NPH_RESULT_OK);
    // An override of a component the instance does not have.
    NTEST_EXPECT_EQ_INT(
        nph_scene_load_text(w4,
                            NPH_STR("{\"nepheline_scene\":1,\"entities\":[{\"id\":0,"
                                    "\"prefab\":{\"name\":\"crate\",\"index\":1,"
                                    "\"root\":0},\"overrides\":{\"test.Health\":{}}}]}"),
                            &ld, &res),
        NPH_RESULT_PARSE_ERROR);

    nph_sb_free(&text);
    nph_sb_free(&again);
    nph_sb_free(&third);
    nph_world_destroy(w4);
    nph_world_destroy(w3);
    nph_world_destroy(w2);
    nph_prefab_destroy(p2);
    nph_prefab_destroy(p);
    nph_world_destroy(w);
}

// A PrefabInstance that names no prefab (the null ID) cannot come from
// nph_prefab_instantiate, but a binary scene file may hold one. Its text
// form, "name": null, must load (the nightly fuzzer found the text loader
// refusing what the text saver wrote for it,
// tests/fuzz/corpus/scene_bin/regression_null_prefab_name).
NTEST(prefab, an_instance_of_no_prefab_round_trips_through_text)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    const NphComponentId comps[2] = {NPH_COMPONENT_ID_PREFAB_INSTANCE,
                                     NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 2U, &e), NPH_RESULT_OK);
    NphPrefabInstance pi;
    memset(&pi, 0, sizeof pi);
    pi.prefab = NPH_STR_ID_NULL;
    pi.root = e;
    NTEST_ASSERT_EQ_INT(nph_world_set(w, e, NPH_COMPONENT_ID_PREFAB_INSTANCE, &pi), NPH_RESULT_OK);

    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.compact = 1;
    NphStrBuilder text;
    nph_sb_init(&text, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, &sd, &text, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_str_contains(
        nph_sb_str(&text),
        NPH_STR("\"prefab\":{\"name\":null,\"index\":0,\"root\":0,\"full\":true}")));

    NphWorld *w2 = test_world_create(0U, &ids);
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(w2, nph_sb_str(&text), NULL, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(res.error, "");
    NTEST_ASSERT_EQ_UINT(res.entity_count, 1U);
    NphStrBuilder again;
    nph_sb_init(&again, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w2, &sd, &again, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&again), nph_sb_cstr(&text));

    // Without "full" the instance needs its prefab, and the null ID names
    // none: not found, whatever the resolver holds, and not a parse error.
    NphPrefab *p = make_crate(w, &ids, 100.0F);
    Library lib = {{p}, 1U};
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.resolve = library_resolve;
    ld.resolve_user = &lib;
    NphWorld *w3 = test_world_create(0U, &ids);
    NTEST_EXPECT_EQ_INT(
        nph_scene_load_text(w3,
                            NPH_STR("{\"nepheline_scene\":1,\"entities\":[{\"id\":0,"
                                    "\"prefab\":{\"name\":null,\"index\":0,\"root\":0}}]}"),
                            &ld, &res),
        NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_str_from_cstr(res.error), NPH_STR("(null)")));
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w3), 0U);
    // A name that is neither text, a hash, nor null is still malformed.
    NTEST_EXPECT_EQ_INT(
        nph_scene_load_text(w3,
                            NPH_STR("{\"nepheline_scene\":1,\"entities\":[{\"id\":0,"
                                    "\"prefab\":{\"name\":7,\"index\":0,\"root\":0}}]}"),
                            &ld, &res),
        NPH_RESULT_PARSE_ERROR);

    nph_sb_free(&text);
    nph_sb_free(&again);
    nph_world_destroy(w3);
    nph_world_destroy(w2);
    nph_prefab_destroy(p);
    nph_world_destroy(w);
}
