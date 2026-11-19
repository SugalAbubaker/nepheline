// SPDX-License-Identifier: Apache-2.0
// Import settings for scenes (cook/scene_import.h; ADR-0107): read, written, refused where they
// must be, and what they do to a scene's cook: its images' size, cooking again when they change,
// and failing the scene when they do not read.
#include <nepheline/asset/texture.h>

#include "test_cook_support.h"

#include <cook/cooker.h>
#include <cook/scene_import.h>

static NphResult read_settings(const char *text, CookSceneImport *out)
{
    return cook_scene_import_read(nph_allocator_system(), nph_str_from_cstr(text), out);
}

NTEST(scene_import, reads_writes_and_applies)
{
    CookSceneImport s;
    NTEST_ASSERT_EQ_INT(read_settings("{\"scene\": 1, \"collision\": \"hull\", \"hull_vertices\": "
                                      "24, \"lods\": false, \"voxels\": true, "
                                      "\"texture_max_extent\": 512, \"texture_uncompressed\": "
                                      "true, \"texture_mips\": false}",
                                      &s),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cook_scene_import_collision(&s), COOK_COLLISION_HULL);
    NTEST_EXPECT_EQ_UINT(cook_scene_import_hull_vertices(&s), 24U);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_scene_import_write(&s, &sb), NPH_RESULT_OK);
    CookSceneImport back;
    NTEST_ASSERT_EQ_INT(read_settings(nph_sb_cstr(&sb), &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_scene_import_equal(&back, &s));
    nph_sb_free(&sb);
    // Over the cooker's: each setting that is set; an image keeps its slot's usage.
    CookMeshSettings mesh;
    cook_mesh_settings_default(&mesh);
    const CookMeshSettings m = cook_scene_import_mesh(&s, &mesh);
    NTEST_EXPECT_TRUE(m.no_lods != 0 && m.voxels != 0);
    NTEST_EXPECT_EQ_UINT(m.meshlet_vertices, mesh.meshlet_vertices);
    CookTextureSettings base;
    cook_texture_settings_default(&base);
    base.usage = COOK_TEXTURE_NORMAL;
    const CookTextureSettings t = cook_scene_import_texture(&s, &base);
    NTEST_EXPECT_EQ_INT(t.usage, COOK_TEXTURE_NORMAL);
    NTEST_EXPECT_EQ_UINT(t.max_extent, 512U);
    NTEST_EXPECT_TRUE(t.uncompressed != 0 && t.no_mips != 0);
    // Nothing set: the cooker's alone, no collision, the default budget, the version alone.
    cook_scene_import_default(&s);
    NTEST_EXPECT_EQ_INT(cook_scene_import_collision(&s), COOK_COLLISION_NONE);
    NTEST_EXPECT_EQ_UINT(cook_scene_import_hull_vertices(&s), COOK_SCENE_HULL_VERTICES_DEFAULT);
    mesh.no_lods = 1;
    NTEST_EXPECT_TRUE(cook_scene_import_mesh(&s, &mesh).no_lods != 0);
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_scene_import_write(&s, &sb), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&sb), "{\n  \"scene\": 1\n}\n");
    nph_sb_free(&sb);
    // The kinds by name, both ways.
    CookCollisionKind kind = COOK_COLLISION_NONE;
    NTEST_EXPECT_TRUE(cook_collision_kind_parse(NPH_STR("mesh"), &kind));
    NTEST_EXPECT_EQ_INT(kind, COOK_COLLISION_MESH);
    NTEST_EXPECT_EQ_STR(cook_collision_kind_name(COOK_COLLISION_HULL), "hull");
    NTEST_EXPECT_FALSE(cook_collision_kind_parse(NPH_STR("box"), &kind));
}

NTEST(scene_import, the_reader_refuses_what_it_must)
{
    static const char *const bad[] = {
        "",
        "{}",
        "{\"scene\": \"1\"}",
        "{\"scene\": 1, \"scene\": 1}",
        "{\"scene\": 1, \"scale\": 0.01}",
        "{\"scene\": 1, \"collision\": \"box\"}",
        "{\"scene\": 1, \"collision\": 1}",
        "{\"scene\": 1, \"hull_vertices\": 3}",
        "{\"scene\": 1, \"hull_vertices\": 257}",
        "{\"scene\": 1, \"hull_vertices\": 16.5}",
        "{\"scene\": 1, \"lods\": 1}",
        "{\"scene\": 1, \"voxels\": \"no\"}",
        "{\"scene\": 1, \"texture_max_extent\": -1}",
        "{\"scene\": 1, \"texture_max_extent\": 20000}",
        "{\"scene\": 1, \"texture_mips\": true, \"texture_mips\": false}",
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad); ++i) {
        CookSceneImport s;
        const NphResult r = read_settings(bad[i], &s);
        if (r != NPH_RESULT_PARSE_ERROR) {
            NTEST_FAIL("settings %u read as %s: %s", i, nph_result_string(r), bad[i]);
        }
    }
    CookSceneImport s;
    NTEST_EXPECT_EQ_INT(read_settings("{\"scene\": 2, \"split\": true}", &s),
                        NPH_RESULT_VERSION_MISMATCH);
    // The writer refuses settings out of range.
    cook_scene_import_default(&s);
    s.has_hull_vertices = 1;
    s.hull_vertices = COOK_SCENE_HULL_VERTICES_MAX + 1U;
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_EXPECT_EQ_INT(cook_scene_import_write(&s, &sb), NPH_RESULT_CORRUPT);
    nph_sb_free(&sb);
}

// ---------------------------------------------------------------------------
// What they do to a scene's cook
// ---------------------------------------------------------------------------

typedef struct SceneImportCook {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
} SceneImportCook;

NTEST_SETUP(SceneImportCook)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "scene_import");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
}

NTEST_TEARDOWN(SceneImportCook)
{
    test_temp_dir_remove(&f->dir);
}

static void write_settings(const char *scene_path, const char *json)
{
    char path[1400];
    (void)snprintf(path, sizeof path, "%s.import.json", scene_path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, json, (u64)strlen(json)), NPH_RESULT_OK);
}

// The width of the texture the scene's base colour image cooked to.
static u32 base_width(SceneImportCook *f, const Cooker *c)
{
    const CookDbEntry *e =
        cook_db_find(cooker_db(c), NPH_STR("box.gltf"), NPH_STR("image/0/color"));
    NTEST_ASSERT_NOT_NULL(e);
    char cooked[1500];
    (void)snprintf(cooked, sizeof cooked, "%s/%s", f->out_dir, e->output);
    TestAsset a;
    test_asset_from_file(f->alloc, cooked, &a);
    NphTextureView v;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&a.asset, &v), NPH_RESULT_OK);
    const u32 width = v.header->width;
    test_asset_free(&a);
    return width;
}

NTEST_F(SceneImportCook, a_scene_cooks_under_its_settings_and_again_when_they_change)
{
    test_write_gltf(&f->dir, "box", false);
    char path[1200];
    test_path_in(&f->dir, "box.gltf", path, sizeof path);
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.texture.lz = NPH_LZ_LEVEL_FAST;
    desc.mesh.lz = NPH_LZ_LEVEL_FAST;
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    // Without settings, its 8-texel image whole.
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(base_width(f, c), 8U);
    const u32 cooked = cooker_stats(c).cooked;
    // Its images capped at 2: the scene is cooked again, and the image is 2 wide.
    write_settings(path, "{\"scene\": 1, \"texture_max_extent\": 2}");
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_GT_UINT(cooker_stats(c).cooked, cooked);
    NTEST_EXPECT_EQ_UINT(base_width(f, c), 2U);
    // Current: nothing cooked; the cooker's settings are its own again after the cook.
    const u32 again = cooker_stats(c).cooked;
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, again);
    // Settings that do not read fail the scene, and say so.
    write_settings(path, "{\"scene\": 1, \"collision\": \"box\"}");
    NTEST_EXPECT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).failed, 1U);
    cooker_destroy(c);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The fuzz target's seeds (scripts/fuzz_seeds_cook.py) read, so a seed that stops reading fails
// here rather than quietly in the nightly runs.
NTEST(scene_import, the_fuzz_corpus_reads)
{
    static const char *const seeds[] = {"version_only", "props", "every_key"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(seeds); ++i) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/scene_import/%s", NPH_FUZZ_CORPUS_DIR, seeds[i]);
        void *bytes = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &bytes, &size),
                            NPH_RESULT_OK);
        CookSceneImport s;
        const NphResult r =
            cook_scene_import_read(nph_allocator_system(), nph_str((const char *)bytes, size), &s);
        nph_file_free_all(nph_allocator_system(), bytes, size);
        if (r != NPH_RESULT_OK) {
            NTEST_FAIL("seed %s: %s", seeds[i], nph_result_string(r));
        }
    }
}
#endif
