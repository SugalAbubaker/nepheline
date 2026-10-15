// SPDX-License-Identifier: Apache-2.0
#include "test_cook_support.h"

#include <cook/database.h>

NTEST(database, set_find_replace_and_remove)
{
    CookDb db;
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, nph_allocator_system()), NPH_RESULT_OK);
    const NphGuid g1 = nph_guid_from_cstr("a#mesh/0");
    const NphGuid g2 = nph_guid_from_cstr("a#mesh/1");
    const NphGuid deps[2] = {g2, nph_guid_from_cstr("tex")};
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/0"),
                                    NPH_STR("a/mesh_0.nph"), g1, NPH_ASSET_TYPE_MESH, 11U, 22U,
                                    deps, 2U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/1"),
                                    NPH_STR("a/mesh_1.nph"), g2, NPH_ASSET_TYPE_MESH, 33U, 22U,
                                    NULL, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 2U);
    NTEST_ASSERT_TRUE(db.dirty);
    const CookDbEntry *e = cook_db_find(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/0"));
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_ASSERT_EQ_STR(e->output, "a/mesh_0.nph");
    NTEST_ASSERT_EQ_UINT(e->dep_count, 2U);
    NTEST_ASSERT_TRUE(nph_guid_equals(e->deps[1], nph_guid_from_cstr("tex")));
    NTEST_ASSERT_TRUE(cook_db_entry_is_current(e, 11U, 22U));
    NTEST_ASSERT_FALSE(cook_db_entry_is_current(e, 12U, 22U));
    NTEST_ASSERT_FALSE(cook_db_entry_is_current(e, 11U, 23U));
    NTEST_ASSERT_FALSE(cook_db_entry_is_current(NULL, 11U, 22U));
    NTEST_ASSERT_EQ_PTR(cook_db_find_guid(&db, g2),
                        cook_db_find(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/1")));
    NTEST_ASSERT_NULL(cook_db_find(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/2")));
    NTEST_ASSERT_NULL(cook_db_find(&db, NPH_STR("models/b.gltf"), NPH_STR("mesh/0")));
    // Replacing keeps one entry; a GUID moving to another key replaces too.
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/0"),
                                    NPH_STR("a/mesh_0.nph"), g1, NPH_ASSET_TYPE_MESH, 99U, 22U,
                                    NULL, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 2U);
    NTEST_ASSERT_EQ_UINT(cook_db_find_guid(&db, g1)->content_hash, 99U);
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/renamed"),
                                    NPH_STR("a/mesh_r.nph"), g1, NPH_ASSET_TYPE_MESH, 1U, 2U, NULL,
                                    0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 2U);
    NTEST_ASSERT_NULL(cook_db_find(&db, NPH_STR("models/a.gltf"), NPH_STR("mesh/0")));
    NTEST_ASSERT_EQ_UINT(cook_db_remove_source(&db, NPH_STR("models/a.gltf")), 2U);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 0U);
    NTEST_ASSERT_EQ_UINT(cook_db_remove_source(&db, NPH_STR("models/a.gltf")), 0U);
    cook_db_free(&db);
    cook_db_free(&db);
}

NTEST(database, saves_and_loads_sorted_json)
{
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "db");
    char path[1200];
    test_path_in(&dir, COOK_DB_FILE_NAME, path, sizeof path);
    CookDb db;
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&db, path), NPH_RESULT_OK); // missing: empty
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 0U);
    const NphGuid dep = nph_guid_from_cstr("dep");
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("z.gltf"), NPH_STR("scene"),
                                    NPH_STR("z/scene_z.nph"), nph_guid_from_cstr("z#scene"),
                                    NPH_ASSET_TYPE_SCENE, 0x0123456789ABCDEFULL,
                                    0xFEDCBA9876543210ULL, &dep, 1U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("a.png"), NPH_STR(""),
                                    NPH_STR("a/tex_color_a.nph"), nph_guid_from_cstr("a#"),
                                    NPH_ASSET_TYPE_TEXTURE, 5U, 6U, NULL, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_save(&db, path, false), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(db.dirty);
    NTEST_ASSERT_EQ_INT(cook_db_save(&db, path, false), NPH_RESULT_OK); // nothing to do
    // Sorted by output path: the texture comes first.
    NTEST_ASSERT_EQ_STR(cook_db_entry(&db, 0)->output, "a/tex_color_a.nph");
    void *text = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, &text, &size), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_contains(nph_str((const char *)text, size),
                                       NPH_STR("\"content_hash\": \"0123456789abcdef\"")));
    NTEST_ASSERT_TRUE(
        nph_str_contains(nph_str((const char *)text, size), NPH_STR("\"type\": \"SCEN\"")));
    nph_file_free_all(alloc, text, size);
    cook_db_free(&db);

    CookDb back;
    NTEST_ASSERT_EQ_INT(cook_db_init(&back, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&back, path), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&back), 2U);
    NTEST_ASSERT_FALSE(back.dirty);
    const CookDbEntry *e = cook_db_find(&back, NPH_STR("z.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_ASSERT_EQ_UINT(e->content_hash, 0x0123456789ABCDEFULL);
    NTEST_ASSERT_EQ_UINT(e->settings_hash, 0xFEDCBA9876543210ULL);
    NTEST_ASSERT_EQ_UINT(e->type, NPH_ASSET_TYPE_SCENE);
    NTEST_ASSERT_EQ_UINT(e->dep_count, 1U);
    NTEST_ASSERT_TRUE(nph_guid_equals(e->deps[0], dep));
    NTEST_ASSERT_TRUE(nph_guid_equals(e->guid, nph_guid_from_cstr("z#scene")));
    NTEST_ASSERT_NOT_NULL(cook_db_find(&back, NPH_STR("a.png"), nph_str_empty()));
    cook_db_free(&back);

    // A damaged file is reported, a wrong version too.
    const char truncated[] = "{\"version\": 1, \"assets\": [";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, truncated, strlen(truncated)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_init(&back, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&back, path), NPH_RESULT_PARSE_ERROR);
    cook_db_free(&back);
    const char newer[] = "{\"version\": 7, \"assets\": []}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, newer, strlen(newer)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_init(&back, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&back, path), NPH_RESULT_VERSION_MISMATCH);
    cook_db_free(&back);
    const char incomplete[] = "{\"version\": 1, \"assets\": [{\"output\": \"x\"}]}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, incomplete, strlen(incomplete)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_init(&back, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&back, path), NPH_RESULT_PARSE_ERROR);
    cook_db_free(&back);
    test_temp_dir_remove(&dir);
}

NTEST(database, outputs_belong_to_one_asset_and_entries_can_be_removed)
{
    CookDb db;
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, nph_allocator_system()), NPH_RESULT_OK);
    const NphGuid ga = nph_guid_from_cstr("a.gltf#scene");
    const NphGuid gb = nph_guid_from_cstr("b.gltf#scene");
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("a.gltf"), NPH_STR("scene"),
                                    NPH_STR("x/scene.nph"), ga, NPH_ASSET_TYPE_SCENE, 1U, 2U, NULL,
                                    0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_PTR(cook_db_find_output(&db, NPH_STR("x/scene.nph")),
                        cook_db_find_guid(&db, ga));
    NTEST_ASSERT_NULL(cook_db_find_output(&db, NPH_STR("x/other.nph")));
    // Another asset may not take an output that is spoken for...
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("b.gltf"), NPH_STR("scene"),
                                    NPH_STR("x/scene.nph"), gb, NPH_ASSET_TYPE_SCENE, 1U, 2U, NULL,
                                    0U),
                        NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 1U);
    // ...but the owner may rewrite it, and move to a new one.
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("a.gltf"), NPH_STR("scene"),
                                    NPH_STR("x/scene2.nph"), ga, NPH_ASSET_TYPE_SCENE, 3U, 2U, NULL,
                                    0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_NULL(cook_db_find_output(&db, NPH_STR("x/scene.nph")));
    NTEST_ASSERT_NOT_NULL(cook_db_find_output(&db, NPH_STR("x/scene2.nph")));
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("b.gltf"), NPH_STR("scene"),
                                    NPH_STR("x/scene.nph"), gb, NPH_ASSET_TYPE_SCENE, 1U, 2U, NULL,
                                    0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 2U);
    // A GUID moving onto a key that holds another GUID: the GUID's old
    // entry goes, so no GUID is recorded twice.
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("a.gltf"), NPH_STR("scene"),
                                    NPH_STR("x/scene2.nph"), gb, NPH_ASSET_TYPE_SCENE, 5U, 2U, NULL,
                                    0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 1U);
    NTEST_ASSERT_NULL(cook_db_find(&db, NPH_STR("b.gltf"), NPH_STR("scene")));
    NTEST_ASSERT_EQ_STR(cook_db_find_guid(&db, gb)->source, "a.gltf");
    // Removal by key.
    NTEST_ASSERT_TRUE(cook_db_remove(&db, NPH_STR("a.gltf"), NPH_STR("scene")));
    NTEST_ASSERT_FALSE(cook_db_remove(&db, NPH_STR("a.gltf"), NPH_STR("scene")));
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 0U);
    NTEST_ASSERT_NULL(cook_db_find_guid(&db, gb));
    NTEST_ASSERT_NULL(cook_db_find_output(&db, NPH_STR("x/scene2.nph")));
    cook_db_free(&db);
}

NTEST(database, many_dependencies_survive_a_round_trip)
{
    // A scene with hundreds of meshes and materials: every dependency is
    // saved and loaded back (a load that refused long lists would throw
    // the whole database away and recook everything on every run).
    NphAllocator *alloc = nph_allocator_system();
    TempDir dir;
    test_temp_dir_make(&dir, "db_deps");
    char path[1200];
    test_path_in(&dir, COOK_DB_FILE_NAME, path, sizeof path);
    NphGuid deps[300];
    for (u32 i = 0; i < 300U; ++i) {
        char text[32];
        snprintf(text, sizeof text, "big.gltf#mesh/%u", i);
        deps[i] = nph_guid_from_cstr(text);
    }
    CookDb db;
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, alloc), NPH_RESULT_OK);
    for (u32 i = 0; i < 300U; ++i) {
        char sub[32];
        char out[64];
        snprintf(sub, sizeof sub, "mesh/%u", i);
        snprintf(out, sizeof out, "big_gltf/mesh_%u.nph", i);
        NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("big.gltf"), nph_str_from_cstr(sub),
                                        nph_str_from_cstr(out), deps[i], NPH_ASSET_TYPE_MESH, 1U,
                                        2U, NULL, 0U),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(cook_db_set(&db, NPH_STR("big.gltf"), NPH_STR("scene"),
                                    NPH_STR("big_gltf/scene.nph"), nph_guid_from_cstr("big#scene"),
                                    NPH_ASSET_TYPE_SCENE, 1U, 2U, deps, 300U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_save(&db, path, false), NPH_RESULT_OK);
    cook_db_free(&db);
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&db, path), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 301U);
    const CookDbEntry *scene = cook_db_find(&db, NPH_STR("big.gltf"), NPH_STR("scene"));
    NTEST_ASSERT_NOT_NULL(scene);
    NTEST_ASSERT_EQ_UINT(scene->dep_count, 300U);
    NTEST_ASSERT_TRUE(nph_guid_equals(scene->deps[299], deps[299]));
    NTEST_ASSERT_NOT_NULL(cook_db_find_output(&db, NPH_STR("big_gltf/mesh_299.nph")));
    cook_db_free(&db);
    // A file in which two assets claim one output is refused whole.
    const char twice[] =
        "{\"version\": 1, \"assets\": ["
        "{\"output\": \"o.nph\", \"source\": \"a\", \"sub\": \"\", \"guid\": "
        "\"00000000000000000000000000000001\", \"type\": \"MESH\", \"content_hash\": "
        "\"0000000000000001\", \"settings_hash\": \"0000000000000001\", \"deps\": []},"
        "{\"output\": \"o.nph\", \"source\": \"b\", \"sub\": \"\", \"guid\": "
        "\"00000000000000000000000000000002\", \"type\": \"MESH\", \"content_hash\": "
        "\"0000000000000001\", \"settings_hash\": \"0000000000000001\", \"deps\": []}]}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, twice, strlen(twice)), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_init(&db, alloc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_db_load(&db, path), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_UINT(cook_db_count(&db), 0U); // all or nothing
    cook_db_free(&db);
    test_temp_dir_remove(&dir);
}
