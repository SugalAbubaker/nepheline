// SPDX-License-Identifier: Apache-2.0
// Scene files: the text format (exact form, round trips, every field kind,
// errors and their rollback) and the binary format (round trips,
// corruption, layout mismatches).

#include <nepheline/core/hash.h>
#include <nepheline/scene/scene_io.h>

#include "test_scene_support.h"

#include <math.h>
#include <stdio.h>

static NphStrBuilder sb_new(void)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    return sb;
}

static void save_text(const NphWorld *w, NphStrBuilder *sb)
{
    nph_sb_clear(sb);
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, NULL, sb, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(res.error, "");
}

static NphResult load_text(NphWorld *w, const char *text, NphSceneResult *res)
{
    return nph_scene_load_text(w, nph_str_from_cstr(text), NULL, res);
}

// A world with every kind of field, a hierarchy whose child order differs
// from slot order, references, and odd values.
static void build_rich(NphWorld *w, const TestIds *ids)
{
    NphEntity e[6];
    for (u32 i = 0; i < 6U; ++i) {
        const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, NPH_COMPONENT_ID_NAME,
                                         ids->position};
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, i == 5U ? 0U : 3U, &e[i]), NPH_RESULT_OK);
        if (i < 5U) {
            NphName n;
            char text[16];
            (void)snprintf(text, sizeof text, "node %u", i);
            nph_text_set(&n.text, nph_str_from_cstr(text));
            NTEST_ASSERT_EQ_INT(nph_world_set(w, e[i], NPH_COMPONENT_ID_NAME, &n), NPH_RESULT_OK);
            NphLocalTransform l = nph_local_transform_identity();
            l.translation = nph_dvec3(0.1 * i, 1.0e7 + i, -2.5);
            l.rotation = nph_quat_from_rotation_y(0.3F * (f32)i);
            NTEST_ASSERT_EQ_INT(nph_world_set(w, e[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, &l),
                                NPH_RESULT_OK);
        }
    }
    // Children of e0 attached in the order e3, e1; e2 under e1.
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[3], e[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[1], e[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[2], e[1]), NPH_RESULT_OK);

    TestAllKinds a;
    memset(&a, 0, sizeof a);
    a.flag = 1;
    a.u8v = 255U;
    a.u16v = 65535U;
    a.u32v = 4000000000U;
    a.u64v = NPH_U64_MAX;
    a.i8v = -128;
    a.i16v = -32768;
    a.i32v = NPH_I32_MIN;
    a.i64v = NPH_I64_MIN;
    a.f32v = 0.1F;
    a.f64v = -0.0;
    a.v2 = nph_vec2((f32)NAN, (f32)INFINITY);
    a.v3 = nph_vec3(-(f32)INFINITY, 1.0e-30F, 3.4028235e38F);
    a.v4 = nph_vec4(1.0F, 2.0F, 3.0F, 4.0F);
    a.q = nph_quat_identity();
    a.d3 = nph_dvec3(1.0 / 3.0, 1.0e300, -5e-324);
    a.target = e[2];
    NTEST_ASSERT_EQ_INT(
        nph_str_table_intern(nph_world_strings(w), NPH_STR("assets/crate.mesh"), &a.sid),
        NPH_RESULT_OK);
    nph_text_set(&a.label, NPH_STR("caf\xC3\xA9 \"quoted\" \\ \n"));
    a.scratch = 42.0F; // transient: not saved
    NTEST_ASSERT_EQ_INT(nph_world_add(w, e[4], ids->all, &a), NPH_RESULT_OK);
    // A string ID the world cannot name, a tag, and a health with a ref.
    TestAllKinds b = a;
    b.sid.value = 0x0123456789ABCDEFULL;
    b.target = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_add(w, e[5], ids->all, &b), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_add(w, e[5], ids->tag_a, NULL), NPH_RESULT_OK);
    const TestHealth h = {50.0F, 75.0F, e[0]};
    NTEST_ASSERT_EQ_INT(nph_world_add(w, e[3], ids->health, &h), NPH_RESULT_OK);
}

NTEST(scene_io, text_round_trip_is_byte_identical)
{
    TestIds ids;
    NphWorld *a = test_world_create(0U, &ids);
    build_rich(a, &ids);
    NphStrBuilder first = sb_new();
    save_text(a, &first);

    TestIds ids_b;
    NphWorld *b = test_world_create(0U, &ids_b);
    NphSceneResult res;
    NphEntity created[16];
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.out_entities = created;
    ld.out_capacity = 16U;
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(b, nph_sb_str(&first), &ld, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(res.error, "");
    NTEST_EXPECT_EQ_UINT(res.entity_count, 6U);
    NphStrBuilder second = sb_new();
    save_text(b, &second);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&second), nph_sb_cstr(&first));

    // The values survived, including the odd ones.
    const TestAllKinds *all = NULL;
    const TestAllKinds *all_unnamed = NULL;
    for (u32 i = 0; i < 6U; ++i) {
        const TestAllKinds *x = NPH_WORLD_GET(b, created[i], ids_b.all, TestAllKinds);
        if (x != NULL && nph_world_has(b, created[i], ids_b.tag_a)) {
            all_unnamed = x;
        } else if (x != NULL) {
            all = x;
        }
    }
    NTEST_ASSERT_NOT_NULL(all);
    NTEST_ASSERT_NOT_NULL(all_unnamed);
    NTEST_EXPECT_EQ_UINT(all->u64v, NPH_U64_MAX);
    NTEST_EXPECT_EQ_INT(all->i64v, NPH_I64_MIN);
    NTEST_EXPECT_TRUE(isnan(all->v2.x));
    NTEST_EXPECT_TRUE(isinf(all->v2.y) && all->v2.y > 0.0F);
    NTEST_EXPECT_TRUE(signbit(all->f64v));
    NTEST_EXPECT_EQ_UINT(nph_f32_to_bits(all->f32v), nph_f32_to_bits(0.1F));
    NTEST_EXPECT_EQ_UINT(nph_f64_to_bits(all->d3.z), nph_f64_to_bits(-5e-324));
    NTEST_EXPECT_NEAR_F32(all->scratch, 0.0F, 0.0F); // transient: the default
    NTEST_EXPECT_TRUE(
        nph_str_equals(nph_text_str(&all->label), NPH_STR("caf\xC3\xA9 \"quoted\" \\ \n")));
    NTEST_EXPECT_TRUE(nph_str_equals(nph_str_table_lookup(nph_world_strings(b), all->sid),
                                     NPH_STR("assets/crate.mesh")));
    NTEST_EXPECT_EQ_UINT(all_unnamed->sid.value, 0x0123456789ABCDEFULL);
    NTEST_EXPECT_TRUE(nph_world_is_alive(b, all->target));
    NTEST_EXPECT_EQ_UINT(nph_world_depth(b, all->target), 2U);

    // Child order: e0's children are e3 then e1, as attached.
    const NphEntity root = created[0];
    const NphEntity c0 = nph_world_first_child(b, root);
    NTEST_EXPECT_TRUE(nph_world_has(b, c0, ids_b.health)); // e3 has the health
    NTEST_EXPECT_TRUE(
        nph_entity_equals(NPH_WORLD_GET(b, c0, ids_b.health, TestHealth)->attacker, root));

    nph_sb_free(&first);
    nph_sb_free(&second);
    nph_world_destroy(a);
    nph_world_destroy(b);
}

NTEST(scene_io, text_form_is_stable)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity p;
    NphEntity c;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &c), NPH_RESULT_OK); // slot 0
    const NphComponentId comps[2] = {ids.health, ids.tag_a};
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 2U, &p), NPH_RESULT_OK); // slot 1
    const TestPosition pos = {{1.5F, 0.1F, -2.0F}};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, c, ids.position, &pos), NPH_RESULT_OK);
    const TestHealth h = {10.0F, 20.0F, c};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, p, ids.health, &h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, c, p), NPH_RESULT_OK);
    NphStrBuilder sb = sb_new();
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.compact = 1;
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, &sd, &sb, NULL), NPH_RESULT_OK);
    // p is the only root (slot 1), c follows it as its child: p gets id 0.
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&sb),
                        "{\"nepheline_scene\":1,\"entities\":["
                        "{\"id\":0,\"components\":{\"test.Health\":{\"current\":10,\"maximum\":20,"
                        "\"attacker\":1},\"test.TagA\":{}}},"
                        "{\"id\":1,\"parent\":0,\"components\":{\"test.Position\":{\"value\":"
                        "[1.5,0.1,-2]}}}]}\n");
    nph_sb_clear(&sb);
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, NULL, &sb, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_sb_str(&sb), NPH_STR("\n  \"entities\": [\n")));
    nph_sb_free(&sb);
    nph_world_destroy(w);
}

NTEST(scene_io, subsets_drop_outside_references)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity e[3];
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.health, 1U, &e[i]), NPH_RESULT_OK);
    }
    const TestHealth h = {1.0F, 1.0F, e[0]};
    NTEST_ASSERT_EQ_INT(nph_world_set(w, e[2], ids.health, &h), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[2], e[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, e[1], e[2]), NPH_RESULT_OK);
    // Save e2 and e1 only: e2 becomes a root, its reference to e0 null.
    const NphEntity subset[3] = {e[1], e[2], e[2]};
    NphSceneSaveDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.entities = subset;
    sd.entity_count = 3U;
    sd.compact = 1;
    NphStrBuilder sb = sb_new();
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(nph_scene_save_text(w, &sd, &sb, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(res.entity_count, 2U);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_sb_str(&sb), NPH_STR("\"attacker\":null")));
    NTEST_EXPECT_TRUE(nph_str_contains(nph_sb_str(&sb), NPH_STR("{\"id\":1,\"parent\":0,")));
    NTEST_EXPECT_FALSE(nph_str_contains(nph_sb_str(&sb), NPH_STR("{\"id\":0,\"parent\"")));
    // Loading adds to a world that already has entities.
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(w, nph_sb_str(&sb), NULL, &res), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 5U);
    nph_sb_free(&sb);
    nph_world_destroy(w);
}

typedef struct BadCase {
    const char *text;
    NphResult expect;
    const char *message; // a substring of the error
} BadCase;

NTEST(scene_io, text_errors_roll_back)
{
    TestIds ids;
    NphWorld *w = test_world_create(64U, &ids);
    NphEntity keep;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, &ids.position, 1U, &keep), NPH_RESULT_OK);
    const u64 before = nph_world_hash(w);
    static const BadCase cases[] = {
        {"{", NPH_RESULT_PARSE_ERROR, "line 1"},
        {"[]", NPH_RESULT_PARSE_ERROR, "nepheline_scene"},
        {"{\"nepheline_scene\": 2, \"entities\": []}", NPH_RESULT_VERSION_MISMATCH, "version 2"},
        {"{\"nepheline_scene\": 1}", NPH_RESULT_PARSE_ERROR, "entities"},
        {"{\"nepheline_scene\": 1, \"entities\": [5]}", NPH_RESULT_PARSE_ERROR, "#0"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": -1}]}", NPH_RESULT_PARSE_ERROR, "id"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 1}, {\"id\": 1}]}",
         NPH_RESULT_PARSE_ERROR, "twice"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"parent\": 7}]}",
         NPH_RESULT_NOT_FOUND, "parent"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"parent\": 0}]}",
         NPH_RESULT_NOT_FOUND, "parent"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"parent\": 1}, {\"id\": 1, "
         "\"parent\": 0}]}",
         NPH_RESULT_PARSE_ERROR, "cycle"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"Nope\": {}}}]}",
         NPH_RESULT_NOT_FOUND, "Nope"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": []}]}",
         NPH_RESULT_PARSE_ERROR, "components"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"Hierarchy\": "
         "{}}}]}",
         NPH_RESULT_PARSE_ERROR, "cannot be listed"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.Health\": "
         "3}}]}",
         NPH_RESULT_TYPE_MISMATCH, "not an object"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.Health\": "
         "{\"current\": \"x\"}}}]}",
         NPH_RESULT_TYPE_MISMATCH, "test.Health.current"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.Health\": "
         "{\"attacker\": 9}}}]}",
         NPH_RESULT_NOT_FOUND, "attacker"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"u8v\": 256}}}]}",
         NPH_RESULT_OVERFLOW, "out of range"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"f32v\": 1e39}}}]}",
         NPH_RESULT_OVERFLOW, "f32v"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"v3\": [1, 2]}}}]}",
         NPH_RESULT_TYPE_MISMATCH, "v3"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"label\": \"0123456789012345678901234567890123456789012345678901234567890123\"}}}]}",
         NPH_RESULT_OVERFLOW, "label"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"label\": \"a\\u0000b\"}}}]}",
         NPH_RESULT_TYPE_MISMATCH, "label"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"sid\": {\"hash\": \"xyz\"}}}}]}",
         NPH_RESULT_TYPE_MISMATCH, "sid"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"components\": {\"test.AllKinds\": "
         "{\"sid\": \"\xFF\"}}}]}", // a byte that is not UTF-8
         NPH_RESULT_TYPE_MISMATCH, "sid"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"removed\": []}]}",
         NPH_RESULT_PARSE_ERROR, "without"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"prefab\": {\"name\": \"p\", "
         "\"index\": 0, \"root\": 0}}]}",
         NPH_RESULT_NOT_FOUND, "prefab 'p'"},
        {"{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, \"prefab\": 1}]}",
         NPH_RESULT_PARSE_ERROR, "prefab"},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(cases); ++i) {
        NphSceneResult res;
        const NphResult r = load_text(w, cases[i].text, &res);
        NTEST_EXPECT_EQ_INT(r, cases[i].expect);
        if (!nph_str_contains(nph_str_from_cstr(res.error), nph_str_from_cstr(cases[i].message))) {
            NTEST_ADD_FAILURE("case %u: error \"%s\" does not mention \"%s\"", i, res.error,
                              cases[i].message);
        }
        NTEST_EXPECT_EQ_UINT(res.entity_count, 0U);
        NTEST_ASSERT_EQ_UINT(nph_world_hash(w), before);
    }
    // A capacity failure part-way also rolls back.
    NphStrBuilder big = sb_new();
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&big, "{\"nepheline_scene\": 1, \"entities\": ["),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < 100U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&big, "%s{\"id\": %u}", i > 0U ? "," : "", i),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&big, "]}"), NPH_RESULT_OK);
    NphSceneResult res;
    NTEST_EXPECT_EQ_INT(load_text(w, nph_sb_cstr(&big), &res), NPH_RESULT_FULL);
    NTEST_EXPECT_EQ_UINT(nph_world_hash(w), before);
    nph_sb_free(&big);

    // Unknown fields are ignored (renamed fields lose their value).
    NTEST_EXPECT_EQ_INT(load_text(w,
                                  "{\"nepheline_scene\": 1, \"entities\": [{\"id\": 0, "
                                  "\"components\": {\"test.Health\": {\"old_name\": 3, "
                                  "\"current\": 4}}}]}",
                                  &res),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), 2U);
    nph_world_destroy(w);
}

NTEST(scene_io, field_kinds_accept_their_forms)
{
    TestIds ids;
    NphWorld *w = test_world_create(0U, &ids);
    NphEntity out[2];
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.out_entities = out;
    ld.out_capacity = 2U;
    NphSceneResult res;
    const char *text = "{\"nepheline_scene\": 1, \"entities\": [{\"id\": 7, \"components\": "
                       "{\"test.AllKinds\": {\"flag\": true, \"u64v\": \"18446744073709551615\", "
                       "\"i8v\": -5, \"f64v\": \"-inf\", \"v2\": [\"nan\", 1], \"target\": 9, "
                       "\"sid\": {\"hash\": \"00000000000000ff\"}, \"scratch\": 5}}}, "
                       "{\"id\": 9, \"components\": {\"test.AllKinds\": {\"sid\": null, "
                       "\"target\": null, \"u64v\": 12}}}]}";
    NTEST_ASSERT_EQ_INT(nph_scene_load_text(w, nph_str_from_cstr(text), &ld, &res), NPH_RESULT_OK);
    const TestAllKinds *a = NPH_WORLD_GET(w, out[0], ids.all, TestAllKinds);
    NTEST_ASSERT_NOT_NULL(a);
    NTEST_EXPECT_EQ_INT(a->flag, 1);
    NTEST_EXPECT_EQ_UINT(a->u64v, NPH_U64_MAX);
    NTEST_EXPECT_EQ_INT(a->i8v, -5);
    NTEST_EXPECT_TRUE(isinf(a->f64v) && a->f64v < 0.0);
    NTEST_EXPECT_TRUE(isnan(a->v2.x));
    NTEST_EXPECT_TRUE(nph_entity_equals(a->target, out[1]));
    NTEST_EXPECT_EQ_UINT(a->sid.value, 0xFFU);
    NTEST_EXPECT_NEAR_F32(a->scratch, 0.0F, 0.0F); // transient fields are not read
    const TestAllKinds *b = NPH_WORLD_GET(w, out[1], ids.all, TestAllKinds);
    NTEST_EXPECT_EQ_UINT(b->u64v, 12U);
    NTEST_EXPECT_TRUE(nph_entity_is_null(b->target));
    nph_world_destroy(w);
}

// ---------------------------------------------------------------------------
// Binary
// ---------------------------------------------------------------------------

static void save_binary(const NphWorld *w, NphBinWriter *bw)
{
    nph_bin_writer_clear(bw);
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(nph_scene_save_binary(w, NULL, bw, &res), NPH_RESULT_OK);
}

NTEST(scene_io, binary_round_trip_matches_the_text)
{
    TestIds ids;
    NphWorld *a = test_world_create(0U, &ids);
    build_rich(a, &ids);
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    save_binary(a, &bw);

    TestIds ids_b;
    NphWorld *b = test_world_create(0U, &ids_b);
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(
        nph_scene_load_binary(b, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw), NULL, &res),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(res.error, "");
    NTEST_EXPECT_EQ_UINT(res.entity_count, 6U);
    NphStrBuilder ta = sb_new();
    NphStrBuilder tb = sb_new();
    save_text(a, &ta);
    save_text(b, &tb);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&tb), nph_sb_cstr(&ta));
    // Saving the loaded world gives the same bytes.
    NphBinWriter bw2;
    nph_bin_writer_init(&bw2, nph_allocator_system());
    save_binary(b, &bw2);
    NTEST_ASSERT_EQ_UINT(nph_bin_writer_len(&bw2), nph_bin_writer_len(&bw));
    NTEST_EXPECT_EQ_MEM(nph_bin_writer_data(&bw2), nph_bin_writer_data(&bw),
                        nph_bin_writer_len(&bw));
    nph_bin_writer_free(&bw2);
    nph_sb_free(&ta);
    nph_sb_free(&tb);
    nph_bin_writer_free(&bw);
    nph_world_destroy(a);
    nph_world_destroy(b);
}

NTEST(scene_io, binary_rejects_damage_and_mismatches)
{
    TestIds ids;
    NphWorld *a = test_world_create(0U, &ids);
    build_rich(a, &ids);
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    save_binary(a, &bw);
    const u64 len = nph_bin_writer_len(&bw);
    u8 *copy = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, len);
    NTEST_ASSERT_NOT_NULL(copy);

    TestIds ids_b;
    NphWorld *b = test_world_create(0U, &ids_b);
    NphSceneResult res;
    // Every single-byte flip is caught by the checksum.
    for (u64 i = 0; i < len; i += 7U) {
        memcpy(copy, nph_bin_writer_data(&bw), (size_t)len);
        copy[i] ^= 0x40U;
        NTEST_ASSERT_EQ_INT(nph_scene_load_binary(b, copy, len, NULL, &res), NPH_RESULT_CORRUPT);
    }
    // Truncation.
    NTEST_EXPECT_EQ_INT(nph_scene_load_binary(b, nph_bin_writer_data(&bw), len - 9U, NULL, &res),
                        NPH_RESULT_CORRUPT);
    NTEST_EXPECT_EQ_INT(nph_scene_load_binary(b, NULL, 0U, NULL, &res), NPH_RESULT_CORRUPT);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(b), 0U);

    // Another version, with a valid checksum.
    memcpy(copy, nph_bin_writer_data(&bw), (size_t)len);
    const u32 v2 = 2U;
    memcpy(copy + 4U, &v2, sizeof v2);
    const u32 crc = nph_crc32c(copy, len - 4U, 0U);
    memcpy(copy + len - 4U, &crc, sizeof crc);
    NTEST_EXPECT_EQ_INT(nph_scene_load_binary(b, copy, len, NULL, &res),
                        NPH_RESULT_VERSION_MISMATCH);

    // A world without the test components, and one with another layout.
    NphWorld *bare = test_world_create(0U, NULL);
    NTEST_EXPECT_EQ_INT(nph_scene_load_binary(bare, nph_bin_writer_data(&bw), len, NULL, &res),
                        NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_TRUE(nph_str_contains(nph_str_from_cstr(res.error), NPH_STR("test.")));
    static const NphFieldDesc moved[1] = {{"value", NPH_FIELD_VEC3, 4U, 12U, 0U, 0.0F, 0.0F}};
    static const NphComponentDesc other = {"test.Position", 16U, 4U, moved, 1U, 0U, NULL};
    NphComponentId id;
    NTEST_ASSERT_EQ_INT(nph_world_register_component(bare, &other, &id), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_scene_load_binary(bare, nph_bin_writer_data(&bw), len, NULL, &res),
                        NPH_RESULT_VERSION_MISMATCH);
    NTEST_EXPECT_EQ_UINT(nph_world_entity_count(bare), 0U);

    // A world too small.
    NphWorld *tiny = test_world_create(4U, &ids_b);
    NTEST_EXPECT_EQ_INT(nph_scene_load_binary(tiny, nph_bin_writer_data(&bw), len, NULL, &res),
                        NPH_RESULT_FULL);

    NPH_FREE_ARRAY(nph_allocator_system(), u8, copy, len);
    nph_bin_writer_free(&bw);
    nph_world_destroy(tiny);
    nph_world_destroy(bare);
    nph_world_destroy(a);
    nph_world_destroy(b);
}

NTEST(scene_io, binary_loads_a_large_world_by_columns)
{
    TestIds ids;
    NphWorld *a = test_world_create(1U << 17U, &ids);
    const u32 n = 20000U * TEST_SCALE / 2U;
    NphEntity *e = NPH_ALLOC_ARRAY(nph_allocator_system(), NphEntity, n);
    NTEST_ASSERT_NOT_NULL(e);
    const NphComponentId comps[3] = {ids.position, ids.health, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NTEST_ASSERT_EQ_INT(nph_world_spawn_batch(a, comps, 3U, n, e), NPH_RESULT_OK);
    for (u32 i = 0; i < n; ++i) {
        const TestHealth h = {(f32)i, 1.0F, e[(i * 7U) % n]};
        NTEST_ASSERT_EQ_INT(nph_world_set(a, e[i], ids.health, &h), NPH_RESULT_OK);
        if (i % 3U == 0U && i > 0U) {
            NTEST_ASSERT_EQ_INT(nph_world_set_parent(a, e[i], e[i / 3U]), NPH_RESULT_OK);
        }
    }
    NphBinWriter bw;
    nph_bin_writer_init(&bw, nph_allocator_system());
    save_binary(a, &bw);
    TestIds ids_b;
    NphWorld *b = test_world_create(1U << 17U, &ids_b);
    NphSceneResult res;
    NTEST_ASSERT_EQ_INT(
        nph_scene_load_binary(b, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw), NULL, &res),
        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(res.entity_count, n);
    NphStrBuilder ta = sb_new();
    NphStrBuilder tb = sb_new();
    save_text(a, &ta);
    save_text(b, &tb);
    NTEST_EXPECT_EQ_UINT(ta.len, tb.len);
    NTEST_EXPECT_TRUE(nph_str_equals(nph_sb_str(&ta), nph_sb_str(&tb)));
    nph_sb_free(&ta);
    nph_sb_free(&tb);
    nph_bin_writer_free(&bw);
    NPH_FREE_ARRAY(nph_allocator_system(), NphEntity, e, n);
    nph_world_destroy(a);
    nph_world_destroy(b);
}

// ---------------------------------------------------------------------------
// The fuzz seed corpora (scripts/fuzz_seeds_scene.py) are an independent
// description of both formats: every seed must load.
// ---------------------------------------------------------------------------

#ifdef NPH_FUZZ_CORPUS_DIR

static u8 *read_seed(const char *dir, const char *name, u64 *out_size)
{
    char path[512];
    (void)snprintf(path, sizeof path, "%s/%s/%s", NPH_FUZZ_CORPUS_DIR, dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        NTEST_FAIL("cannot open %s", path);
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    u8 *data = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, (u64)size + 1U);
    NTEST_ASSERT_NOT_NULL(data);
    NTEST_ASSERT_EQ_UINT(fread(data, 1U, (size_t)size, f), (u64)size);
    (void)fclose(f);
    *out_size = (u64)size;
    return data;
}

static const NphPrefab *resolve_one(void *user, NphStrId id)
{
    const NphPrefab *p = (const NphPrefab *)user;
    return nph_str_id_equals(nph_prefab_id(p), id) ? p : NULL;
}

NTEST(scene_io, fuzz_seeds_load)
{
    TestIds ids;
    NphWorld *registry = test_world_create(0U, &ids);
    // The fuzz targets' "crate" (tests/fuzz/fuzz_scene_common.h).
    NphPrefab *crate = NULL;
    NTEST_ASSERT_EQ_INT(
        nph_prefab_create(NPH_STR("crate"), registry, nph_allocator_system(), 16U, &crate),
        NPH_RESULT_OK);
    NphWorld *pw = nph_prefab_world(crate);
    const NphComponentId rc[3] = {ids.position, ids.health, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    const NphComponentId lc[2] = {ids.tag_a, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
    NphEntity root;
    NphEntity lid;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(pw, rc, 3U, &root), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(pw, lc, 2U, &lid), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(pw, lid, root), NPH_RESULT_OK);

    static const char *const text_seeds[] = {"basic", "all_kinds", "prefab", "forward_refs",
                                             "empty"};
    static const u32 text_counts[] = {3U, 2U, 5U, 3U, 0U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(text_seeds); ++i) {
        u64 size = 0U;
        u8 *data = read_seed("scene_text", text_seeds[i], &size);
        NphWorld *w = test_world_create(0U, &ids);
        NphSceneLoadDesc ld;
        memset(&ld, 0, sizeof ld);
        ld.resolve = resolve_one;
        ld.resolve_user = crate;
        NphSceneResult res;
        const NphResult r = nph_scene_load_text(w, nph_str((const char *)data, size), &ld, &res);
        if (r != NPH_RESULT_OK) {
            NTEST_ADD_FAILURE("scene_text/%s: %s", text_seeds[i], res.error);
        }
        NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), text_counts[i]);
        nph_world_destroy(w);
        NPH_FREE_ARRAY(nph_allocator_system(), u8, data, size + 1U);
    }
    static const char *const bin_seeds[] = {"basic", "instance", "empty"};
    static const u32 bin_counts[] = {4U, 2U, 0U};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bin_seeds); ++i) {
        u64 size = 0U;
        u8 *data = read_seed("scene_bin", bin_seeds[i], &size);
        NphWorld *w = test_world_create(0U, &ids);
        NphSceneResult res;
        const NphResult r = nph_scene_load_binary(w, data, size, NULL, &res);
        if (r != NPH_RESULT_OK) {
            NTEST_ADD_FAILURE("scene_bin/%s: %s", bin_seeds[i], res.error);
        }
        NTEST_EXPECT_EQ_UINT(nph_world_entity_count(w), bin_counts[i]);
        if (i == 0U && r == NPH_RESULT_OK) {
            // The child's name has a multi-byte character; the health refers
            // to the root, which is the entity of file id 0.
            const NphEntity e0 = nph_world_entity_at(w, 0U);
            NTEST_EXPECT_TRUE(nph_str_equals(
                nph_text_str(&NPH_WORLD_GET(w, e0, NPH_COMPONENT_ID_NAME, NphName)->text),
                NPH_STR("root")));
            const TestHealth *h =
                NPH_WORLD_GET(w, nph_world_entity_at(w, 2U), ids.health, TestHealth);
            NTEST_ASSERT_NOT_NULL(h);
            NTEST_EXPECT_TRUE(nph_entity_equals(h->attacker, e0));
            NTEST_EXPECT_TRUE(
                nph_entity_equals(nph_world_parent(w, nph_world_entity_at(w, 1U)), e0));
        }
        nph_world_destroy(w);
        NPH_FREE_ARRAY(nph_allocator_system(), u8, data, size + 1U);
    }
    nph_prefab_destroy(crate);
    nph_world_destroy(registry);
}

#endif
