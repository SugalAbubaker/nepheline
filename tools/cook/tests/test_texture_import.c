// SPDX-License-Identifier: Apache-2.0
// Import settings for images (cook/texture_import.h; ADR-0105): read, written, refused where they
// must be, and what they do to an image's texture: its usage when cooked alone, its size and
// compression, and a DirectX normal map's green.
#include <nepheline/asset/texture.h>

#include "test_cook_support.h"

#include <cook/cooker.h>
#include <cook/material.h>
#include <cook/texture_import.h>

static NphResult read_settings(const char *text, CookTextureImport *out)
{
    return cook_texture_import_read(nph_allocator_system(), nph_str_from_cstr(text), out);
}

NTEST(texture_import, reads_writes_and_applies)
{
    CookTextureImport s;
    NTEST_ASSERT_EQ_INT(read_settings("{\"texture\": 1, \"usage\": \"normal\", \"max_extent\": "
                                      "512, \"uncompressed\": true, \"mips\": false, "
                                      "\"normal_green\": \"down\"}",
                                      &s),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(s.has_usage != 0 && s.usage == COOK_TEXTURE_NORMAL);
    NTEST_EXPECT_TRUE(s.has_max_extent != 0 && s.max_extent == 512U);
    NTEST_EXPECT_TRUE(s.has_uncompressed != 0 && s.uncompressed != 0);
    NTEST_EXPECT_TRUE(s.has_mips != 0 && s.mips == 0);
    NTEST_EXPECT_TRUE(s.green_down != 0);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_texture_import_write(&s, &sb), NPH_RESULT_OK);
    CookTextureImport back;
    NTEST_ASSERT_EQ_INT(read_settings(nph_sb_cstr(&sb), &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_texture_import_equal(&back, &s));
    nph_sb_free(&sb);

    // Over the cooker's: each setting that is set; the usage only for the image alone.
    CookTextureSettings base;
    cook_texture_settings_default(&base);
    base.usage = COOK_TEXTURE_MASK;
    CookTextureSettings t = cook_texture_import_apply(&s, &base, false);
    NTEST_EXPECT_EQ_INT(t.usage, COOK_TEXTURE_MASK);
    NTEST_EXPECT_EQ_UINT(t.max_extent, 512U);
    NTEST_EXPECT_TRUE(t.uncompressed != 0 && t.no_mips != 0);
    t = cook_texture_import_apply(&s, &base, true);
    NTEST_EXPECT_EQ_INT(t.usage, COOK_TEXTURE_NORMAL);
    // Nothing set: the cooker's alone, and the file of its version alone.
    cook_texture_import_default(&s);
    base.max_extent = 64U;
    t = cook_texture_import_apply(&s, &base, true);
    NTEST_EXPECT_EQ_INT(t.usage, COOK_TEXTURE_MASK);
    NTEST_EXPECT_EQ_UINT(t.max_extent, 64U);
    NTEST_EXPECT_TRUE(t.uncompressed == base.uncompressed && t.no_mips == base.no_mips);
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_texture_import_write(&s, &sb), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&sb), "{\n  \"texture\": 1\n}\n");
    nph_sb_free(&sb);
    // "up" is the default, and reads as nothing set.
    NTEST_ASSERT_EQ_INT(read_settings("{\"texture\": 1, \"normal_green\": \"up\"}", &back),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_texture_import_equal(&back, &s));
}

NTEST(texture_import, the_reader_refuses_what_it_must)
{
    static const char *const bad[] = {
        "",
        "{}",
        "{\"texture\": \"1\"}",
        "{\"texture\": 1, \"texture\": 1}",
        "{\"texture\": 1, \"srgb\": true}",
        "{\"texture\": 1, \"usage\": \"albedo\"}",
        "{\"texture\": 1, \"usage\": 2}",
        "{\"texture\": 1, \"max_extent\": -1}",
        "{\"texture\": 1, \"max_extent\": 20000}",
        "{\"texture\": 1, \"max_extent\": 1.5}",
        "{\"texture\": 1, \"uncompressed\": 1}",
        "{\"texture\": 1, \"mips\": \"yes\"}",
        "{\"texture\": 1, \"normal_green\": \"left\"}",
        "{\"texture\": 1, \"mips\": true, \"mips\": false}",
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad); ++i) {
        CookTextureImport s;
        const NphResult r = read_settings(bad[i], &s);
        if (r != NPH_RESULT_PARSE_ERROR) {
            NTEST_FAIL("settings %u read as %s: %s", i, nph_result_string(r), bad[i]);
        }
    }
    CookTextureImport s;
    NTEST_EXPECT_EQ_INT(read_settings("{\"texture\": 2, \"crop\": true}", &s),
                        NPH_RESULT_VERSION_MISMATCH);
    // The writer refuses settings out of range.
    cook_texture_import_default(&s);
    s.has_max_extent = 1;
    s.max_extent = COOK_TEXTURE_IMPORT_MAX_EXTENT + 1U;
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_EXPECT_EQ_INT(cook_texture_import_write(&s, &sb), NPH_RESULT_CORRUPT);
    nph_sb_free(&sb);
}

// ---------------------------------------------------------------------------
// What they do to a cooked texture
// ---------------------------------------------------------------------------

typedef struct ImportCook {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
} ImportCook;

NTEST_SETUP(ImportCook)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "texture_import");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
}

NTEST_TEARDOWN(ImportCook)
{
    test_temp_dir_remove(&f->dir);
}

static Cooker *import_cooker(ImportCook *f)
{
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.texture.lz = NPH_LZ_LEVEL_FAST;
    Cooker *c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &c), NPH_RESULT_OK);
    return c;
}

// A 4x4 TGA named `name` in the project, of one colour; its path in `path`.
static void write_flat(ImportCook *f, const char *name, u8 r, u8 g, u8 b, char *path, u64 cap)
{
    u8 rgba[4 * 4 * 4];
    for (u32 i = 0; i < 16U; ++i) {
        const u64 t = (u64)i * 4U;
        rgba[t] = r;
        rgba[t + 1U] = g;
        rgba[t + 2U] = b;
        rgba[t + 3U] = 255U;
    }
    u8 tga[18 + sizeof rgba];
    const u64 len = test_write_tga(rgba, 4U, 4U, tga);
    test_path_in(&f->dir, name, path, cap);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, len), NPH_RESULT_OK);
}

static void write_settings(const char *image_path, const char *json)
{
    char path[1400];
    (void)snprintf(path, sizeof path, "%s.import.json", image_path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, json, (u64)strlen(json)), NPH_RESULT_OK);
}

NTEST_F(ImportCook, an_image_alone_takes_its_usage_and_settings)
{
    char path[1200];
    write_flat(f, "bump.tga", 128U, 128U, 255U, path, sizeof path);
    write_settings(path, "{\"texture\": 1, \"usage\": \"normal\", \"uncompressed\": true, "
                         "\"max_extent\": 2}");
    Cooker *c = import_cooker(f);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK); // the cooker's colour asked
    NTEST_EXPECT_NULL(cook_db_find(cooker_db(c), NPH_STR("bump.tga"), NPH_STR("color")));
    const CookDbEntry *e = cook_db_find(cooker_db(c), NPH_STR("bump.tga"), NPH_STR("normal"));
    NTEST_ASSERT_NOT_NULL(e);
    char cooked[1500];
    (void)snprintf(cooked, sizeof cooked, "%s/%s", f->out_dir, e->output);
    TestAsset a;
    test_asset_from_file(f->alloc, cooked, &a);
    NphTextureView v;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(v.header->format, NPH_TEXTURE_FORMAT_R8G8_UNORM);
    NTEST_EXPECT_EQ_UINT(v.header->width, 2U);
    NTEST_EXPECT_EQ_UINT(v.header->height, 2U);
    test_asset_free(&a);
    // Current; a change to the settings cooks it again.
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 1U);
    write_settings(path, "{\"texture\": 1, \"usage\": \"normal\", \"uncompressed\": true}");
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 2U);
    // Bad settings fail the image.
    write_settings(path, "{\"texture\": 1, \"usage\": \"bumpy\"}");
    NTEST_EXPECT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).failed, 1U);
    cooker_destroy(c);
}

NTEST_F(ImportCook, a_directx_normal_map_in_a_material_has_its_green_flipped)
{
    char path[1200];
    // A unit normal, (0, 0.6, 0.8): the cooker renormalizes normal maps.
    write_flat(f, "dx_n.tga", 128U, 204U, 230U, path, sizeof path);
    // Its usage set to colour: in a material, the slot says the data kind.
    write_settings(path, "{\"texture\": 1, \"usage\": \"color\", \"uncompressed\": true, "
                         "\"mips\": false, \"normal_green\": \"down\"}");
    CookMaterialSource m;
    cook_material_source_default(&m);
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_NORMAL].image, COOK_MATERIAL_IMAGE_MAX,
                   "dx_n.tga");
    char material[1200];
    test_path_in(&f->dir, "wall.nmat", material, sizeof material);
    NTEST_ASSERT_EQ_INT(cook_material_source_save(&m, f->alloc, material), NPH_RESULT_OK);
    Cooker *c = import_cooker(f);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, material), NPH_RESULT_OK);
    const CookDbEntry *e = cook_db_find(cooker_db(c), NPH_STR("dx_n.tga"), NPH_STR("normal"));
    NTEST_ASSERT_NOT_NULL(e);
    char cooked[1500];
    (void)snprintf(cooked, sizeof cooked, "%s/%s", f->out_dir, e->output);
    TestAsset a;
    test_asset_from_file(f->alloc, cooked, &a);
    NphTextureView v;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->format, NPH_TEXTURE_FORMAT_R8G8_UNORM);
    NTEST_ASSERT_EQ_UINT(v.header->mip_count, 1U);
    // Green from 0.6 to -0.6: 204 to 51, within the renormalization's rounding.
    const u8 *texels = (const u8 *)v.mips[0];
    for (u32 i = 0; i < 16U; ++i) {
        const u64 t = (u64)i * 2U;
        NTEST_EXPECT_TRUE(texels[t] >= 127U && texels[t] <= 129U);
        NTEST_EXPECT_TRUE(texels[t + 1U] >= 50U && texels[t + 1U] <= 52U);
    }
    test_asset_free(&a);
    cooker_destroy(c);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The fuzz target's seeds (scripts/fuzz_seeds_cook.py) read.
NTEST(texture_import, the_fuzz_corpus_reads)
{
    static const char *const seeds[] = {"version_only", "normal_directx", "every_key"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(seeds); ++i) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/texture_import/%s", NPH_FUZZ_CORPUS_DIR, seeds[i]);
        void *data = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_all(path, nph_allocator_system(), &data, &size),
                            NPH_RESULT_OK);
        CookTextureImport s;
        const NphResult r =
            cook_texture_import_read(nph_allocator_system(), nph_str((const char *)data, size), &s);
        nph_file_free_all(nph_allocator_system(), data, size);
        if (r != NPH_RESULT_OK) {
            NTEST_FAIL("seed %s: %s", seeds[i], nph_result_string(r));
        }
    }
}
#endif
