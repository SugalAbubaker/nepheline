// SPDX-License-Identifier: Apache-2.0
// Copies of a world that keep its entity handles (hot reload migrates a
// world to new component layouts this way, play-in-editor copies the
// edited world), and loads that skip the components a world no longer
// registers.

#include <nepheline/scene/scene_io.h>

#include "test_scene_support.h"

// A world with a hierarchy, an entity reference, and holes (stale
// handles).
static void build(NphWorld *w, const TestIds *ids, NphEntity *out_live, u32 *out_live_count,
                  NphEntity *out_stale, u32 *out_stale_count)
{
    NphEntity e[8];
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, ids->position,
                                     ids->health};
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, i % 2U == 0U ? 3U : 2U, &e[i]),
                            NPH_RESULT_OK);
        const TestPosition p = {{(f32)i, 0.5F, -2.0F}};
        NTEST_ASSERT_EQ_INT(nph_world_set(w, e[i], ids->position, &p), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[3], e[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[5], e[3]), NPH_RESULT_OK);
    const TestHealth h = {10.0F, 20.0F, e[5]};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, e[2], ids->health, &h), NPH_RESULT_OK);
    // Holes: e1 and e6 go, so their handles are stale.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[1]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, e[6]), NPH_RESULT_OK);
    u32 live = 0U;
    for (u32 i = 0; i < 8U; ++i) {
        if (i != 1U && i != 6U) {
            out_live[live++] = e[i];
        }
    }
    *out_live_count = live;
    out_stale[0] = e[1];
    out_stale[1] = e[6];
    *out_stale_count = 2U;
}

static NphWorld *copy_world(const NphWorld *src, bool text, u32 event_capacity)
{
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.max_entities = nph_world_entity_capacity(src);
    wd.event_capacity = event_capacity;
    wd.registry_from = src;
    NphWorld *dst = test_world_create_desc(&wd, NULL);
    NTEST_ASSERT_EQ_INT(nph_world_adopt_handles(dst, src), NPH_RESULT_OK);
    NphEntity order[64];
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.out_entities = order;
    sd.out_capacity = 64U;
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.handles = order;
    NphSceneResult res;
    if (text) {
        NphStrBuilder sb;
        nph_sb_init(&sb, nph_allocator_system());
        NTEST_ASSERT_EQ_INT(nph_scene_save_text(src, &sd, &sb, &res), NPH_RESULT_OK);
        ld.handle_count = res.entity_count;
        NTEST_ASSERT_EQ_INT(nph_scene_load_text(dst, nph_sb_str(&sb), &ld, &res), NPH_RESULT_OK);
        nph_sb_free(&sb);
    } else {
        NphBinWriter bw;
        nph_bin_writer_init(&bw, nph_allocator_system());
        NTEST_ASSERT_EQ_INT(nph_scene_save_binary(src, &sd, &bw, &res), NPH_RESULT_OK);
        ld.handle_count = res.entity_count;
        NTEST_ASSERT_EQ_INT(nph_scene_load_binary(dst, nph_bin_writer_data(&bw),
                                                  nph_bin_writer_len(&bw), &ld, &res),
                            NPH_RESULT_OK);
        nph_bin_writer_free(&bw);
    }
    NTEST_EXPECT_EQ_STR(res.error, "");
    return dst;
}

static void save_text(const NphWorld *w, NphStrBuilder *sb)
{
    nph_sb_clear(sb);
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, NULL, sb, NULL), NPH_RESULT_OK);
}

NTEST(scene_copy, copies_keep_handles_references_and_stale_handles)
{
    for (u32 format = 0; format < 2U; ++format) {
        TestIds ids;
        NphWorld *a = test_world_create(64U, &ids);
        NphEntity live[8];
        NphEntity stale[2];
        u32 live_count = 0U;
        u32 stale_count = 0U;
        build(a, &ids, live, &live_count, stale, &stale_count);
        NphWorld *b = copy_world(a, format == 0U, 0U);
        NTEST_ASSERT_EQ_UINT(nph_world_entity_count(b), live_count);
        for (u32 i = 0; i < live_count; ++i) {
            NTEST_ASSERT_TRUE(nph_world_is_alive(b, live[i]));
            NTEST_EXPECT_EQ_UINT(nph_world_entity_index(b, live[i]),
                                 nph_world_entity_index(a, live[i]));
        }
        for (u32 i = 0; i < stale_count; ++i) {
            NTEST_EXPECT_FALSE(nph_world_is_alive(b, stale[i]));
        }
        // The hierarchy and the reference name the same handles.
        NTEST_EXPECT_TRUE(nph_entity_equals(nph_world_parent(b, live[2]), live[0]));
        const TestHealth *h = NPH_WORLD_GET(b, live[1], ids.health, TestHealth);
        NTEST_ASSERT_NOT_NULL(h);
        NTEST_EXPECT_TRUE(nph_entity_equals(h->attacker, live[4]));
        // Equal worlds: equal files.
        NphStrBuilder ta;
        NphStrBuilder tb;
        nph_sb_init(&ta, nph_allocator_system());
        nph_sb_init(&tb, nph_allocator_system());
        save_text(a, &ta);
        save_text(b, &tb);
        NTEST_EXPECT_EQ_STR(nph_sb_cstr(&tb), nph_sb_cstr(&ta));
        nph_sb_free(&ta);
        nph_sb_free(&tb);
        // Both issue the same handles from here on.
        for (u32 i = 0; i < 4U; ++i) {
            NphEntity ea;
            NphEntity eb;
            NTEST_ASSERT_EQ_INT(nph_world_spawn(a, &ids.position, 1U, &ea), NPH_RESULT_OK);
            NTEST_ASSERT_EQ_INT(nph_world_spawn(b, &ids.position, 1U, &eb), NPH_RESULT_OK);
            NTEST_EXPECT_EQ_UINT(ea.value, eb.value);
        }
        nph_world_destroy(b);
        nph_world_destroy(a);
    }
}

NTEST(scene_copy, a_copy_needs_an_empty_world_and_reserved_handles)
{
    TestIds ids;
    NphWorld *a = test_world_create(64U, &ids);
    NphEntity live[8];
    NphEntity stale[2];
    u32 live_count = 0U;
    u32 stale_count = 0U;
    build(a, &ids, live, &live_count, stale, &stale_count);
    // Adopting needs an empty world of the same capacity.
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.max_entities = 64U;
    wd.registry_from = a;
    NphWorld *b = test_world_create_desc(&wd, NULL);
    NphEntity extra;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(b, &ids.position, 1U, &extra), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_world_adopt_handles(b, a), NPH_RESULT_INVALID_ARGUMENT);
    nph_world_destroy(b);
    wd.max_entities = 128U;
    b = test_world_create_desc(&wd, NULL);
    NTEST_EXPECT_EQ_INT(nph_world_adopt_handles(b, a), NPH_RESULT_INVALID_ARGUMENT);
    nph_world_destroy(b);

    NphEntity order[16];
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.out_entities = order;
    sd.out_capacity = 16U;
    NphSceneResult res;
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(a, &sd, &sb, &res), NPH_RESULT_OK);
    const u32 n = res.entity_count;
    NTEST_ASSERT_EQ_UINT(n, live_count);
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_binary(a, &sd, &bw, &res), NPH_RESULT_OK);
    NphEntity handles[16];
    wd.max_entities = 64U;
    for (u32 c = 0; c < 4U; ++c) {
        for (u32 format = 0; format < 2U; ++format) {
            b = test_world_create_desc(&wd, NULL);
            NTEST_ASSERT_EQ_INT(nph_world_adopt_handles(b, a), NPH_RESULT_OK);
            memcpy(handles, order, sizeof(NphEntity) * n);
            NphSceneLoadDesc ld;
            memset(&ld, 0, sizeof ld);
            ld.handles = handles;
            ld.handle_count = n;
            if (c == 0U) {
                ld.handle_count = n - 1U; // too few
            } else if (c == 1U) {
                handles[2] = stale[0]; // not reserved: stale in the source
            } else if (c == 2U) {
                handles[3] = handles[1]; // twice
            } else {
                handles[0].value = 0U; // null
            }
            const NphResult r =
                format == 0U
                    ? nph_scene_load_text(b, nph_sb_str(&sb), &ld, &res)
                    : nph_scene_load_binary(b, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw),
                                            &ld, &res);
            NTEST_EXPECT_EQ_INT(r, NPH_RESULT_INVALID_ARGUMENT);
            NTEST_EXPECT_TRUE(res.error[0] != '\0');
            nph_world_destroy(b);
        }
    }
    nph_bin_writer_free(&bw);
    nph_sb_free(&sb);
    nph_world_destroy(a);
}

static const NphPrefab *resolve_one(void *user, NphStrId id)
{
    const NphPrefab *p = (const NphPrefab *)user;
    return nph_str_id_equals(nph_prefab_id(p), id) ? p : NULL;
}

NTEST(scene_copy, unknown_components_can_be_skipped)
{
    // A world that knows Position but none of the other test components:
    // the game that defined them changed.
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    NphWorld *w = test_world_create_desc(&wd, NULL);
    NphComponentId position;
    NTEST_ASSERT_EQ_INT(nph_world_register_component(w, TestPosition_component(), &position),
                        NPH_RESULT_OK);
    // A crate prefab in that world's registry, and an instance of it that
    // overrides, adds, and removes components the world does not know.
    NphPrefab *crate = NULL;
    NTEST_ASSERT_EQ_INT(nph_prefab_create(NPH_STR("crate"), w, nph_allocator_system(), 0U, &crate),
                        NPH_RESULT_OK);
    NphEntity pe;
    const NphComponentId pc[2] = {position, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(nph_prefab_world(crate), pc, 2U, &pe), NPH_RESULT_OK);
    static const char text[] =
        "{\"nepheline_scene\": 1, \"entities\": ["
        "{\"id\": 0, \"components\": {\"test.Position\": {\"value\": [1, 2, 3]},"
        " \"test.Health\": {\"current\": 5}, \"test.TagA\": {}}},"
        "{\"id\": 1, \"prefab\": {\"name\": \"crate\", \"index\": 0, \"root\": 1},"
        " \"overrides\": {\"test.Health\": {\"current\": 7},"
        " \"test.Position\": {\"value\": [4, 5, 6]}},"
        " \"components\": {\"test.TagB\": {}}, \"removed\": [\"test.Velocity\"]}"
        "]}";
    NphEntity created[2];
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.resolve = resolve_one;
    ld.resolve_user = crate;
    ld.out_entities = created;
    ld.out_capacity = 2U;
    NphSceneResult res;
    // By default an unknown component fails the load, which changes nothing.
    NTEST_EXPECT_EQ_INT(nph_scene_load_text(w, nph_str_from_cstr(text), &ld, &res),
                        NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_str_from_cstr(res.error), NPH_STR("test.Health")));
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 0U);
    // Skipped, the rest loads.
    ld.skip_unknown_components = 1;
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(w, nph_str_from_cstr(text), &ld, &res),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(res.error, "");
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(w), 2U);
    NphComponentId comps[8];
    NTEST_EXPECT_EQ_UINT(nph_world_entity_components(w, created[0], comps, 8U), 1U);
    const TestPosition *p0 = NPH_WORLD_GET(w, created[0], position, TestPosition);
    NTEST_ASSERT_NOT_NULL(p0);
    NTEST_EXPECT_NEAR_F32(p0->value.z, 3.0F, 0.0F);
    const TestPosition *p1 = NPH_WORLD_GET(w, created[1], position, TestPosition);
    NTEST_ASSERT_NOT_NULL(p1);
    NTEST_EXPECT_NEAR_F32(p1->value.x, 4.0F, 0.0F);
    NTEST_EXPECT_TRUE(nph_world_has(w, created[1], NPH_COMPONENT_ID_PREFAB_INSTANCE));
    nph_prefab_destroy(crate);
    nph_world_destroy(w);
}
