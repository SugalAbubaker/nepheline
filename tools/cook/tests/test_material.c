// SPDX-License-Identifier: Apache-2.0
// Material sources (cook/material.h; ADR-0105): the `.nmat` file read, written, and refused where
// it must be; its transform and its header; and the cooker's material, its images cooked by slot.
#include <nepheline/asset/material.h>
#include <nepheline/asset/texture.h>

#include "test_cook_support.h"

#include <cook/cooker.h>
#include <cook/material.h>

static NphResult read_text(const char *text, CookMaterialSource *out)
{
    return cook_material_source_read(nph_allocator_system(), nph_str_from_cstr(text), out);
}

// ---------------------------------------------------------------------------
// The source
// ---------------------------------------------------------------------------

NTEST(material_source, the_default_writes_every_key_in_its_order)
{
    CookMaterialSource m;
    cook_material_source_default(&m);
    NTEST_ASSERT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_OK);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_material_source_write(&m, &sb), NPH_RESULT_OK);
    static const char expected[] = "{\n"
                                   "  \"nmat\": 1,\n"
                                   "  \"base_color\": [0.8, 0.8, 0.8, 1],\n"
                                   "  \"metallic\": 0,\n"
                                   "  \"roughness\": 0.5,\n"
                                   "  \"emissive\": [0, 0, 0],\n"
                                   "  \"emissive_strength\": 1,\n"
                                   "  \"normal_scale\": 1,\n"
                                   "  \"occlusion_strength\": 1,\n"
                                   "  \"alpha\": \"opaque\",\n"
                                   "  \"alpha_cutoff\": 0.5,\n"
                                   "  \"double_sided\": false,\n"
                                   "  \"unlit\": false,\n"
                                   "  \"tiling\": [1, 1],\n"
                                   "  \"rotation\": 0,\n"
                                   "  \"offset\": [0, 0],\n"
                                   "  \"textures\": {}\n"
                                   "}\n";
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&sb), expected);
    // And a document of the version alone is the default.
    CookMaterialSource back;
    NTEST_ASSERT_EQ_INT(read_text("{\"nmat\": 1}", &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_material_source_equal(&back, &m));
    nph_sb_free(&sb);
}

// Every field away from its default, and three slots.
static void every_field(CookMaterialSource *m)
{
    cook_material_source_default(m);
    m->base_color = nph_vec4(0.25F, 0.5F, 0.75F, 0.875F);
    m->metallic = 1.0F;
    m->roughness = 0.125F;
    m->emissive = nph_vec3(1.0F, 0.5F, 0.0F);
    m->emissive_strength = 40.0F;
    m->normal_scale = -0.75F;
    m->occlusion_strength = 0.625F;
    m->alpha = (u32)NPH_MATERIAL_ALPHA_MASK;
    m->alpha_cutoff = 0.375F;
    m->double_sided = 1;
    m->unlit = 1;
    m->tiling = nph_vec2(4.0F, -2.0F);
    m->rotation = 33.5F;
    m->offset = nph_vec2(0.25F, -0.5F);
    (void)snprintf(m->textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].image, COOK_MATERIAL_IMAGE_MAX,
                   "textures/brick.png");
    CookMaterialTexture *normal = &m->textures[NPH_MATERIAL_TEXTURE_NORMAL];
    (void)snprintf(normal->image, COOK_MATERIAL_IMAGE_MAX, "textures/deep/brick_n.tga");
    normal->wrap_u = (u8)NPH_MATERIAL_WRAP_CLAMP;
    normal->wrap_v = (u8)NPH_MATERIAL_WRAP_MIRROR;
    normal->nearest = 1U;
    (void)snprintf(m->textures[NPH_MATERIAL_TEXTURE_EMISSIVE].image, COOK_MATERIAL_IMAGE_MAX,
                   "glow.jpg");
}

NTEST(material_source, reads_back_what_it_writes)
{
    CookMaterialSource m;
    every_field(&m);
    NTEST_ASSERT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_OK);
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_material_source_write(&m, &sb), NPH_RESULT_OK);
    CookMaterialSource back;
    NTEST_ASSERT_EQ_INT(read_text(nph_sb_cstr(&sb), &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_material_source_equal(&back, &m));
    NTEST_EXPECT_EQ_STR(back.textures[NPH_MATERIAL_TEXTURE_NORMAL].image,
                        "textures/deep/brick_n.tga");
    NTEST_EXPECT_EQ_UINT(back.textures[NPH_MATERIAL_TEXTURE_NORMAL].wrap_v,
                         NPH_MATERIAL_WRAP_MIRROR);
    // The slots in their order, and nothing of the unused ones.
    const char *text = nph_sb_cstr(&sb);
    const char *base = strstr(text, "\"base_color\": {");
    const char *normal = strstr(text, "\"normal\": {");
    const char *emissive = strstr(text, "\"emissive\": {");
    NTEST_ASSERT_TRUE(base != NULL && normal != NULL && emissive != NULL);
    NTEST_EXPECT_TRUE(base < normal && normal < emissive);
    NTEST_EXPECT_TRUE(strstr(text, "\"occlusion\"") == NULL);
    NTEST_EXPECT_TRUE(strstr(text, "\"wrap\": [\"clamp\", \"mirror\"]") != NULL);
    NTEST_EXPECT_TRUE(strstr(text, "\"filter\": \"nearest\"") != NULL);
    // Writing what was read gives the same text: one canonical form.
    NphStrBuilder again;
    nph_sb_init(&again, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(cook_material_source_write(&back, &again), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_STR(nph_sb_cstr(&again), text);
    // A change is a different material; an unused slot's wrap is not part of it.
    back.roughness = 0.25F;
    NTEST_EXPECT_FALSE(cook_material_source_equal(&back, &m));
    back.roughness = m.roughness;
    back.textures[NPH_MATERIAL_TEXTURE_OCCLUSION].wrap_u = (u8)NPH_MATERIAL_WRAP_CLAMP;
    NTEST_EXPECT_TRUE(cook_material_source_equal(&back, &m));
    nph_sb_free(&again);
    nph_sb_free(&sb);
}

NTEST(material_source, the_reader_refuses_what_it_must)
{
    // Some documents are long enough to be split across lines, on purpose.
    // NOLINTBEGIN(bugprone-suspicious-missing-comma)
    static const char *const bad[] = {
        "",
        "[]",
        "{\"nmat\": \"1\"}",
        "{\"base_color\": [1, 1, 1, 1]}", // no version
        "{\"nmat\": 1, \"nmat\": 1}",
        "{\"nmat\": 1, \"albedo\": [1, 1, 1, 1]}",
        "{\"nmat\": 1, \"metallic\": 0, \"metallic\": 1}",
        "{\"nmat\": 1, \"base_color\": [1, 1, 1]}",
        "{\"nmat\": 1, \"base_color\": [1, 1, 1.5, 1]}",
        "{\"nmat\": 1, \"base_color\": [1, 1, \"1\", 1]}",
        "{\"nmat\": 1, \"metallic\": -0.25}",
        "{\"nmat\": 1, \"roughness\": 2}",
        "{\"nmat\": 1, \"emissive\": [0, 0, 2]}",
        "{\"nmat\": 1, \"emissive_strength\": -1}",
        "{\"nmat\": 1, \"emissive_strength\": 2e6}",
        "{\"nmat\": 1, \"normal_scale\": 1000}",
        "{\"nmat\": 1, \"occlusion_strength\": 1.5}",
        "{\"nmat\": 1, \"alpha\": \"masked\"}",
        "{\"nmat\": 1, \"alpha\": 1}",
        "{\"nmat\": 1, \"alpha_cutoff\": -0.5}",
        "{\"nmat\": 1, \"double_sided\": 1}",
        "{\"nmat\": 1, \"unlit\": \"no\"}",
        "{\"nmat\": 1, \"tiling\": [0, 1]}",
        "{\"nmat\": 1, \"tiling\": [1, 0.00001]}",
        "{\"nmat\": 1, \"tiling\": [1e5, 1]}",
        "{\"nmat\": 1, \"tiling\": [1]}",
        "{\"nmat\": 1, \"rotation\": 1e5}",
        "{\"nmat\": 1, \"offset\": [0, 2e4]}",
        "{\"nmat\": 1, \"textures\": []}",
        "{\"nmat\": 1, \"textures\": {\"albedo\": {\"image\": \"a.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": null}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a.png\"}, "
        "\"normal\": {\"image\": \"b.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a.png\", \"size\": 2}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a.png\", \"wrap\": [\"repeat\"]}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a.png\", "
        "\"wrap\": [\"repeat\", \"bounce\"]}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a.png\", \"filter\": \"cubic\"}}}",
        // Keys that are not images inside the project.
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"../a.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"/a.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a//b.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a\\\\b.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"C:/a.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"./a.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a.wav\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"a\\u0000.png\"}}}",
        "{\"nmat\": 1, \"textures\": {\"normal\": {\"image\": \"\"}}}",
    };
    // NOLINTEND(bugprone-suspicious-missing-comma)
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad); ++i) {
        CookMaterialSource m;
        const NphResult r = read_text(bad[i], &m);
        if (r != NPH_RESULT_PARSE_ERROR) {
            NTEST_FAIL("document %u read as %s: %s", i, nph_result_string(r), bad[i]);
        }
    }
    // Another version, whatever else it holds.
    CookMaterialSource m;
    NTEST_EXPECT_EQ_INT(read_text("{\"nmat\": 2, \"glass\": true}", &m),
                        NPH_RESULT_VERSION_MISMATCH);
}

NTEST(material_source, image_keys_are_relative_paths_of_images)
{
    static const char *const good[] = {
        "a.png", "textures/brick_01.PNG", "a/b/c d.jpeg", "x.tga", "sky.hdr", "set.dds", "old.bmp"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(good); ++i) {
        NTEST_EXPECT_TRUE(cook_material_image_key_valid(nph_str_from_cstr(good[i])));
    }
    static const char *const bad[] = {"",           "a",        "a.txt",    "/a.png",
                                      "a/",         "a//b.png", "../a.png", "a/./b.png",
                                      "a/../b.png", "C:/a.png", "a\\b.png", "a\tb.png"};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad); ++i) {
        if (cook_material_image_key_valid(nph_str_from_cstr(bad[i]))) {
            NTEST_FAIL("%s taken for an image key", bad[i]);
        }
    }
    // As long as the key has room for its NUL.
    char longest[COOK_MATERIAL_IMAGE_MAX + 1U];
    memset(longest, 'a', sizeof longest);
    memcpy(longest + COOK_MATERIAL_IMAGE_MAX - 5U, ".png", 5U);
    NTEST_EXPECT_TRUE(cook_material_image_key_valid(nph_str_from_cstr(longest)));
    memset(longest, 'a', sizeof longest);
    memcpy(longest + COOK_MATERIAL_IMAGE_MAX - 4U, ".png", 5U);
    NTEST_EXPECT_FALSE(cook_material_image_key_valid(nph_str_from_cstr(longest)));
}

NTEST(material_source, validation_refuses_what_the_reader_would)
{
    CookMaterialSource m;
    every_field(&m);
    NTEST_ASSERT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_OK);
    m.alpha = 3U;
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    every_field(&m);
    m.unlit = 2;
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    every_field(&m);
    m.tiling.y = 0.0F;
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    every_field(&m);
    m.metallic = nph_f32_from_bits(0x7FC00000U);
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    every_field(&m);
    m.textures[NPH_MATERIAL_TEXTURE_NORMAL].wrap_u = 3U;
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    every_field(&m);
    (void)snprintf(m.textures[0].image, COOK_MATERIAL_IMAGE_MAX, "../outside.png");
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    every_field(&m);
    memset(m.textures[0].image, 'a', COOK_MATERIAL_IMAGE_MAX); // no NUL
    NTEST_EXPECT_EQ_INT(cook_material_source_validate(&m), NPH_RESULT_CORRUPT);
    // The writer writes no source that would not read.
    NphStrBuilder sb;
    nph_sb_init(&sb, nph_allocator_system());
    NTEST_EXPECT_EQ_INT(cook_material_source_write(&m, &sb), NPH_RESULT_CORRUPT);
    nph_sb_free(&sb);
}

NTEST(material_source, the_transform_is_khr_texture_transforms)
{
    CookMaterialSource m;
    cook_material_source_default(&m);
    NphMaterialUvTransform t = cook_material_source_uv(&m);
    NTEST_EXPECT_TRUE(nph_material_uv_is_identity(&t)); // bit for bit
    m.tiling = nph_vec2(2.0F, 3.0F);
    m.offset = nph_vec2(0.25F, 0.5F);
    t = cook_material_source_uv(&m);
    NTEST_EXPECT_NEAR_F32(t.m00, 2.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(t.m01, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(t.m02, 0.25F, 0.0F);
    NTEST_EXPECT_NEAR_F32(t.m10, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(t.m11, 3.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(t.m12, 0.5F, 0.0F);
    // Turned a quarter: KHR_texture_transform's (u', v') = (sy v + ox, -sx u + oy), the glTF
    // importer's composition. The corner (1, 0) goes to (0.25, -1.5).
    m.rotation = 90.0F;
    t = cook_material_source_uv(&m);
    NTEST_EXPECT_NEAR_F32(t.m00, 0.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(t.m01, 3.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(t.m10, -2.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(t.m11, 0.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(t.m00 * 1.0F + t.m02, 0.25F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(t.m10 * 1.0F + t.m12, -1.5F, 1e-6F);
}

NTEST(material_source, the_header_holds_every_field)
{
    CookMaterialSource m;
    every_field(&m);
    NphGuid textures[NPH_MATERIAL_TEXTURE_COUNT];
    for (u32 i = 0; i < NPH_MATERIAL_TEXTURE_COUNT; ++i) {
        textures[i] = nph_guid_from_cstr(i == 0U ? "t0" : i == 2U ? "t2" : "unused");
    }
    textures[NPH_MATERIAL_TEXTURE_EMISSIVE] = nph_guid_from_cstr("t4");
    NphMaterialHeader h;
    cook_material_source_header(&m, textures, &h);
    NTEST_EXPECT_TRUE(nph_vec4_eq(h.base_color_factor, m.base_color));
    NTEST_EXPECT_TRUE(nph_vec3_eq(h.emissive_factor, m.emissive));
    NTEST_EXPECT_NEAR_F32(h.emissive_strength, 40.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.metallic_factor, 1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.roughness_factor, 0.125F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.alpha_cutoff, 0.375F, 0.0F);
    NTEST_EXPECT_EQ_UINT(h.alpha_mode, NPH_MATERIAL_ALPHA_MASK);
    NTEST_EXPECT_EQ_UINT(h.flags, NPH_MATERIAL_FLAG_DOUBLE_SIDED | NPH_MATERIAL_FLAG_UNLIT);
    NTEST_EXPECT_TRUE(h.alpha_key == 0U); // no mesh's ray triangles depend on it
    NTEST_EXPECT_TRUE(nph_guid_equals(h.textures[0].texture, nph_guid_from_cstr("t0")));
    NTEST_EXPECT_TRUE(
        nph_guid_is_null(h.textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].texture));
    NTEST_EXPECT_TRUE(nph_guid_is_null(h.textures[NPH_MATERIAL_TEXTURE_OCCLUSION].texture));
    const NphMaterialTextureRef *normal = &h.textures[NPH_MATERIAL_TEXTURE_NORMAL];
    NTEST_EXPECT_TRUE(nph_guid_equals(normal->texture, nph_guid_from_cstr("t2")));
    NTEST_EXPECT_EQ_UINT(normal->wrap_u, NPH_MATERIAL_WRAP_CLAMP);
    NTEST_EXPECT_EQ_UINT(normal->wrap_v, NPH_MATERIAL_WRAP_MIRROR);
    NTEST_EXPECT_EQ_UINT(normal->nearest, 1U);
    NTEST_EXPECT_NEAR_F32(normal->scale, -0.75F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale, 0.625F, 0.0F);
    NTEST_EXPECT_NEAR_F32(h.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].scale, 1.0F, 0.0F);
    // Each slot's usage: as the glTF importer's.
    NTEST_EXPECT_EQ_INT(cook_material_slot_usage(NPH_MATERIAL_TEXTURE_BASE_COLOR),
                        COOK_TEXTURE_COLOR);
    NTEST_EXPECT_EQ_INT(cook_material_slot_usage(NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS),
                        COOK_TEXTURE_LINEAR);
    NTEST_EXPECT_EQ_INT(cook_material_slot_usage(NPH_MATERIAL_TEXTURE_NORMAL), COOK_TEXTURE_NORMAL);
    NTEST_EXPECT_EQ_INT(cook_material_slot_usage(NPH_MATERIAL_TEXTURE_OCCLUSION),
                        COOK_TEXTURE_MASK);
    NTEST_EXPECT_EQ_INT(cook_material_slot_usage(NPH_MATERIAL_TEXTURE_EMISSIVE),
                        COOK_TEXTURE_COLOR);
}

NTEST(material_source, saves_whole_and_loads)
{
    TempDir dir;
    test_temp_dir_make(&dir, "material_save");
    char path[1200];
    test_path_in(&dir, "brick.nmat", path, sizeof path);
    CookMaterialSource m;
    every_field(&m);
    NphAllocator *alloc = nph_allocator_system();
    NTEST_ASSERT_EQ_INT(cook_material_source_save(&m, alloc, path), NPH_RESULT_OK);
    CookMaterialSource back;
    NTEST_ASSERT_EQ_INT(cook_material_source_load(alloc, path, &back), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(cook_material_source_equal(&back, &m));
    // A source that does not validate leaves the file as it was, and no part behind.
    m.roughness = 2.0F;
    NTEST_EXPECT_EQ_INT(cook_material_source_save(&m, alloc, path), NPH_RESULT_CORRUPT);
    NTEST_ASSERT_EQ_INT(cook_material_source_load(alloc, path, &back), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(back.roughness, 0.125F, 0.0F);
    char part[1300];
    (void)snprintf(part, sizeof part, "%s.part", path);
    NTEST_EXPECT_FALSE(nph_fs_exists(part));
    test_path_in(&dir, "missing.nmat", path, sizeof path);
    NTEST_EXPECT_EQ_INT(cook_material_source_load(alloc, path, &back), NPH_RESULT_NOT_FOUND);
    test_temp_dir_remove(&dir);
}

// ---------------------------------------------------------------------------
// The cooker's material
// ---------------------------------------------------------------------------

typedef struct MaterialCook {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
} MaterialCook;

NTEST_SETUP(MaterialCook)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "material_cook");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
}

NTEST_TEARDOWN(MaterialCook)
{
    test_temp_dir_remove(&f->dir);
}

static Cooker *material_cooker(MaterialCook *f)
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

// A 4x4 TGA at source key `key` of one colour.
static void write_image(MaterialCook *f, const char *key, u8 r, u8 g, u8 b)
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
    char path[1300];
    (void)snprintf(path, sizeof path, "%s/%s", f->dir.dir, key);
    char dir[1300];
    memcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    NTEST_ASSERT_NOT_NULL(slash);
    *slash = '\0';
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, len), NPH_RESULT_OK);
}

static void write_material(MaterialCook *f, const char *key, const CookMaterialSource *m,
                           char *path, u64 cap)
{
    (void)snprintf(path, (size_t)cap, "%s/%s", f->dir.dir, key);
    char dir[1300];
    (void)snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    NTEST_ASSERT_NOT_NULL(slash);
    *slash = '\0';
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(dir), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_material_source_save(m, f->alloc, path), NPH_RESULT_OK);
}

// The cooked asset the database holds for (`key`, `sub`), read into `out`.
static const CookDbEntry *cooked(MaterialCook *f, Cooker *c, const char *key, const char *sub,
                                 TestAsset *out)
{
    const CookDbEntry *e =
        cook_db_find(cooker_db(c), nph_str_from_cstr(key), nph_str_from_cstr(sub));
    if (e == NULL || out == NULL) {
        return e;
    }
    char path[1500];
    (void)snprintf(path, sizeof path, "%s/%s", f->out_dir, e->output);
    test_asset_from_file(f->alloc, path, out);
    return e;
}

static u32 texture_format(MaterialCook *f, Cooker *c, const char *key, const char *sub)
{
    TestAsset a;
    const CookDbEntry *e = cooked(f, c, key, sub, &a);
    if (e == NULL) {
        return 0U;
    }
    NphTextureView v;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&a.asset, &v), NPH_RESULT_OK);
    const u32 format = v.header->format;
    test_asset_free(&a);
    return format;
}

NTEST_F(MaterialCook, cooks_the_material_and_its_images_by_slot)
{
    write_image(f, "textures/brick.tga", 200U, 80U, 60U);
    write_image(f, "textures/brick_n.tga", 128U, 128U, 255U);
    write_image(f, "textures/orm.tga", 255U, 140U, 0U);
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.tiling = nph_vec2(2.0F, 2.0F);
    m.roughness = 0.75F;
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].image, COOK_MATERIAL_IMAGE_MAX,
                   "textures/brick.tga");
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_NORMAL].image, COOK_MATERIAL_IMAGE_MAX,
                   "textures/brick_n.tga");
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS].image,
                   COOK_MATERIAL_IMAGE_MAX, "textures/orm.tga");
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_OCCLUSION].image, COOK_MATERIAL_IMAGE_MAX,
                   "textures/orm.tga");
    char path[1300];
    write_material(f, "materials/brick.nmat", &m, path, sizeof path);
    NTEST_EXPECT_EQ_INT(cooker_source_kind(path), COOKER_SOURCE_MATERIAL);

    Cooker *c = material_cooker(f);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 4U); // the material and three textures
    // Each image as its slot needs it, under its own key; the occlusion shares the
    // metallic-roughness texture.
    NTEST_EXPECT_EQ_UINT(texture_format(f, c, "textures/brick.tga", "color"),
                         NPH_TEXTURE_FORMAT_BC7_SRGB);
    NTEST_EXPECT_EQ_UINT(texture_format(f, c, "textures/brick_n.tga", "normal"),
                         NPH_TEXTURE_FORMAT_BC5_UNORM);
    NTEST_EXPECT_EQ_UINT(texture_format(f, c, "textures/orm.tga", "linear"),
                         NPH_TEXTURE_FORMAT_BC7_UNORM);
    NTEST_EXPECT_NULL(cooked(f, c, "textures/orm.tga", "mask", NULL));

    TestAsset a;
    const CookDbEntry *e = cooked(f, c, "materials/brick.nmat", "material", &a);
    NTEST_ASSERT_NOT_NULL(e);
    NTEST_EXPECT_TRUE(
        nph_guid_equals(e->guid, nph_guid_from_cstr("materials/brick.nmat#material")));
    // Known before the cook, from the key: what the editor names a material by (ADR-0105).
    NTEST_EXPECT_TRUE(
        nph_guid_equals(e->guid, cooker_asset_guid("materials/brick.nmat", "material")));
    NTEST_EXPECT_EQ_STR(e->output, "materials/brick_nmat/material.nph");
    NTEST_EXPECT_EQ_UINT(a.asset.header.type_version, NPH_MATERIAL_TYPE_VERSION);
    NphMaterialView v;
    NTEST_ASSERT_EQ_INT(nph_material_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(v.header->roughness_factor, 0.75F, 0.0F);
    NTEST_EXPECT_TRUE(nph_guid_equals(v.header->textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture,
                                      nph_guid_from_cstr("textures/brick.tga#color")));
    NTEST_EXPECT_TRUE(nph_guid_equals(v.header->textures[NPH_MATERIAL_TEXTURE_NORMAL].texture,
                                      nph_guid_from_cstr("textures/brick_n.tga#normal")));
    NTEST_EXPECT_TRUE(nph_guid_equals(v.header->textures[NPH_MATERIAL_TEXTURE_OCCLUSION].texture,
                                      nph_guid_from_cstr("textures/orm.tga#linear")));
    NTEST_ASSERT_TRUE(v.uv != NULL);
    const NphMaterialUvTransform uv = nph_material_view_uv(&v);
    NTEST_EXPECT_NEAR_F32(uv.m00, 2.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(uv.m11, 2.0F, 0.0F);
    NTEST_EXPECT_EQ_UINT(e->dep_count, 3U); // the three textures
    test_asset_free(&a);

    // Current: nothing cooked. A change to the material cooks it alone; untiled, it has no
    // transform.
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 4U);
    m.tiling = nph_vec2(1.0F, 1.0F);
    write_material(f, "materials/brick.nmat", &m, path, sizeof path);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 5U);
    NTEST_ASSERT_NOT_NULL(cooked(f, c, "materials/brick.nmat", "material", &a));
    NTEST_ASSERT_EQ_INT(nph_material_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(v.uv == NULL);
    test_asset_free(&a);
    // A change to an image cooks its texture alone.
    write_image(f, "textures/brick.tga", 10U, 20U, 30U);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).cooked, 6U);
    cooker_destroy(c);
}

NTEST_F(MaterialCook, a_directory_cooks_its_materials_and_bad_ones_fail)
{
    write_image(f, "textures/brick.tga", 200U, 80U, 60U);
    CookMaterialSource m;
    cook_material_source_default(&m);
    (void)snprintf(m.textures[0].image, COOK_MATERIAL_IMAGE_MAX, "textures/brick.tga");
    char path[1300];
    write_material(f, "materials/a.nmat", &m, path, sizeof path);
    Cooker *c = material_cooker(f);
    char materials[1300];
    test_path_in(&f->dir, "materials", materials, sizeof materials);
    NTEST_ASSERT_EQ_INT(cooker_cook_directory(c, materials), NPH_RESULT_OK);
    NTEST_EXPECT_NOT_NULL(cooked(f, c, "materials/a.nmat", "material", NULL));
    NTEST_EXPECT_NOT_NULL(cooked(f, c, "textures/brick.tga", "color", NULL));

    // An image that is missing fails the material, which stays uncooked.
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_NORMAL].image, COOK_MATERIAL_IMAGE_MAX,
                   "textures/missing.png");
    write_material(f, "materials/b.nmat", &m, path, sizeof path);
    NTEST_EXPECT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_NULL(cooked(f, c, "materials/b.nmat", "material", NULL));
    // As does a file that is not one.
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, "{\"nmat\": 1, \"shiny\": true}", 26U),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cooker_cook_file(c, path), NPH_RESULT_PARSE_ERROR);
    NTEST_EXPECT_EQ_UINT(cooker_stats(c).failed, 2U);
    cooker_destroy(c);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The fuzz target's seeds (scripts/fuzz_seeds_cook.py) read, so a seed that stops reading fails
// here rather than quietly in the nightly runs.
NTEST(material_source, the_fuzz_corpus_reads)
{
    static const struct {
        const char *name;
        u32 slots; // texture slots used
    } seeds[] = {{"version_only", 0U},
                 {"default", 0U},
                 {"every_field", 5U},
                 {"tiled_turned", 0U},
                 {"masked_foliage", 1U}};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(seeds); ++i) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/nmat/%s", NPH_FUZZ_CORPUS_DIR, seeds[i].name);
        CookMaterialSource m;
        const NphResult r = cook_material_source_load(nph_allocator_system(), path, &m);
        if (r != NPH_RESULT_OK) {
            NTEST_FAIL("seed %s: %s", seeds[i].name, nph_result_string(r));
        }
        u32 slots = 0U;
        for (u32 s = 0; s < NPH_MATERIAL_TEXTURE_COUNT; ++s) {
            slots += m.textures[s].image[0] != '\0' ? 1U : 0U;
        }
        NTEST_EXPECT_EQ_UINT(slots, seeds[i].slots);
    }
}
#endif
