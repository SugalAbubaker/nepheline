// SPDX-License-Identifier: Apache-2.0
// Material previews (cook/preview.h): the sphere's picture of a material, the images it reads
// (reduced, given their levels, made from their files), and the cache file.
#include <nepheline/asset/format.h>
#include <nepheline/asset/texture.h>
#include <nepheline/platform/file.h>

#include <ntest/ntest.h>

#include "test_cook_support.h"

#include <cook/preview.h>
#include <stdio.h>
#include <string.h>

// One studio for every test: its tables take some milliseconds.
static const CookPreviewStudio *studio(void)
{
    static CookPreviewStudio s;
    static bool made = false;
    if (!made) {
        cook_preview_studio_init(&s);
        made = true;
    }
    return &s;
}

static const u8 *pixel_at(const u8 *picture, u32 x, u32 y)
{
    return picture + ((u64)y * COOK_PREVIEW_SIZE + x) * 4U;
}

// The brightest sum of a pixel's colour channels where the sphere covers it whole.
static u32 brightest(const u8 *picture)
{
    u32 best = 0U;
    for (u32 i = 0; i < COOK_PREVIEW_SIZE * COOK_PREVIEW_SIZE; ++i) {
        const u8 *p = picture + (u64)i * 4U;
        if (p[3] == 255U) {
            best = nph_max_u32(best, (u32)p[0] + p[1] + p[2]);
        }
    }
    return best;
}

// Slot `slot` of `images` made from RGBA8 texels, written as a TGA file and made as from files
// (cook_preview_images_make), with the import settings `settings` (NULL: none).
static void slot_from(CookPreviewImages *images, u32 slot, const u8 *rgba, u32 w, u32 h,
                      const char *settings)
{
    NphAllocator *alloc = nph_allocator_system();
    const u64 cap = 18U + (u64)w * h * 4U;
    u8 *tga = NPH_ALLOC_ARRAY(alloc, u8, cap);
    NTEST_ASSERT_NOT_NULL(tga);
    CookPreviewFiles files;
    memset(&files, 0, sizeof files);
    files.alloc = alloc;
    files.image[slot] = tga;
    files.image_size[slot] = test_write_tga(rgba, w, h, tga);
    char text[256];
    if (settings != NULL) {
        (void)snprintf(text, sizeof text, "%s", settings);
        files.settings[slot] = text;
        files.settings_size[slot] = strlen(text);
    }
    CookPreviewImages made;
    NTEST_ASSERT_EQ_INT(cook_preview_images_make(alloc, &files, &made), NPH_RESULT_OK);
    images->slots[slot] = made.slots[slot]; // the levels move over
    NPH_FREE_ARRAY(alloc, u8, tga, cap);
}

static void render(const CookMaterialSource *m, const CookPreviewImages *images, u8 *out)
{
    cook_preview_render(studio(), m, images, out);
}

NTEST(preview, a_white_sphere_is_lit_just_below_white_on_nothing)
{
    static u8 picture[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.base_color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    m.roughness = 1.0F;
    render(&m, NULL, picture);
    // Nothing around it; covered whole in the middle; its edge covered as far as it covers.
    const u8 *corner = pixel_at(picture, 0U, 0U);
    NTEST_EXPECT_TRUE(corner[0] == 0U && corner[1] == 0U && corner[2] == 0U && corner[3] == 0U);
    NTEST_EXPECT_EQ_UINT(pixel_at(picture, 64U, 64U)[3], 255U);
    u32 edge = 0U;
    for (u32 x = 0; x < COOK_PREVIEW_SIZE && edge == 0U; ++x) {
        edge = pixel_at(picture, x, 64U)[3];
    }
    NTEST_EXPECT_TRUE(edge > 0U && edge < 255U);
    // Lit near white where it faces the key, without burning out; darker away from it.
    const u32 peak = brightest(picture);
    NTEST_EXPECT_GT_UINT(peak, 3U * 200U);
    NTEST_EXPECT_LT_UINT(peak, 3U * 255U);
    const u8 *lit = pixel_at(picture, 40U, 30U);     // up and to the left: toward the key
    const u8 *shade = pixel_at(picture, 100U, 100U); // down and to the right
    NTEST_EXPECT_GT_UINT(lit[1], shade[1] + 40U);
}

NTEST(preview, the_base_colour_and_its_image_colour_it)
{
    static u8 picture[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.base_color = nph_vec4(0.8F, 0.05F, 0.05F, 1.0F);
    render(&m, NULL, picture);
    const u8 *c = pixel_at(picture, 64U, 64U);
    NTEST_EXPECT_GT_UINT(c[0], (u32)c[1] + 80U);
    NTEST_EXPECT_GT_UINT(c[0], (u32)c[2] + 80U);
    // White, with a blue image.
    m.base_color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    CookPreviewImages images;
    memset(&images, 0, sizeof images);
    static const u8 blue[4] = {20U, 40U, 230U, 255U};
    slot_from(&images, NPH_MATERIAL_TEXTURE_BASE_COLOR, blue, 1U, 1U, NULL);
    render(&m, &images, picture);
    c = pixel_at(picture, 64U, 64U);
    NTEST_EXPECT_GT_UINT(c[2], (u32)c[0] + 80U);
    cook_preview_images_free(&images);
}

NTEST(preview, metal_and_roughness_shape_the_highlight)
{
    static u8 smooth[COOK_PREVIEW_BYTES];
    static u8 rough[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.base_color = nph_vec4(0.9F, 0.9F, 0.9F, 1.0F);
    m.metallic = 1.0F;
    m.roughness = 0.1F;
    render(&m, NULL, smooth);
    m.roughness = 1.0F;
    render(&m, NULL, rough);
    NTEST_EXPECT_GT_UINT(brightest(smooth), brightest(rough));
    NTEST_EXPECT_GE_UINT(brightest(smooth), 3U * 245U); // the key's highlight
}

NTEST(preview, an_unlit_material_draws_its_base_colour_and_emission_adds_light)
{
    static u8 picture[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.unlit = 1;
    m.base_color = nph_vec4(0.5F, 0.25F, 1.0F, 1.0F);
    render(&m, NULL, picture);
    const u8 *c = pixel_at(picture, 64U, 64U);
    // sRGB of 0.5 and 0.25, and 1.
    NTEST_EXPECT_NEAR_F64((f64)c[0], 188.0, 1.0);
    NTEST_EXPECT_NEAR_F64((f64)c[1], 137.0, 1.0);
    NTEST_EXPECT_EQ_UINT(c[2], 255U);
    // Black, lit, glowing orange.
    m.unlit = 0;
    m.base_color = nph_vec4(0.0F, 0.0F, 0.0F, 1.0F);
    m.roughness = 1.0F;
    render(&m, NULL, picture);
    const u32 dark = pixel_at(picture, 64U, 64U)[0];
    m.emissive = nph_vec3(1.0F, 0.5F, 0.0F);
    m.emissive_strength = 1.0F;
    render(&m, NULL, picture);
    c = pixel_at(picture, 64U, 64U);
    NTEST_EXPECT_GT_UINT(c[0], dark + 100U);
    NTEST_EXPECT_GT_UINT(c[0], (u32)c[2] + 100U);
}

NTEST(preview, a_masked_material_is_cut_and_a_blended_one_keeps_its_alpha)
{
    static u8 picture[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.alpha = (u32)NPH_MATERIAL_ALPHA_MASK;
    m.alpha_cutoff = 0.5F;
    // Clear on the left half of the image, solid on the right; the left of the front reads it.
    static const u8 halves[8] = {255U, 255U, 255U, 0U, 255U, 255U, 255U, 255U};
    CookPreviewImages images;
    memset(&images, 0, sizeof images);
    slot_from(&images, NPH_MATERIAL_TEXTURE_BASE_COLOR, halves, 2U, 1U, NULL);
    m.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].nearest = 1U;
    m.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].wrap_u = (u8)NPH_MATERIAL_WRAP_CLAMP;
    render(&m, &images, picture);
    NTEST_EXPECT_EQ_UINT(pixel_at(picture, 32U, 64U)[3], 0U);
    NTEST_EXPECT_EQ_UINT(pixel_at(picture, 96U, 64U)[3], 255U);
    cook_preview_images_free(&images);
    // Blended at half: half the alpha, the colour as it is.
    cook_material_source_default(&m);
    m.alpha = (u32)NPH_MATERIAL_ALPHA_BLEND;
    m.base_color.w = 0.5F;
    render(&m, NULL, picture);
    NTEST_EXPECT_NEAR_F64((f64)pixel_at(picture, 64U, 64U)[3], 128.0, 1.0);
    NTEST_EXPECT_GT_UINT(pixel_at(picture, 64U, 64U)[1], 100U);
}

// Changes of the red channel across the picture's middle row, away from the edges.
static u32 transitions(const u8 *picture)
{
    u32 n = 0U;
    for (u32 x = 17U; x < 112U; ++x) {
        const bool a = pixel_at(picture, x - 1U, 64U)[0] >= 128U;
        const bool b = pixel_at(picture, x, 64U)[0] >= 128U;
        n += a != b ? 1U : 0U;
    }
    return n;
}

NTEST(preview, tiling_repeats_the_image_across_the_front)
{
    static u8 picture[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.base_color = nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    m.unlit = 1; // the image's own values
    static const u8 stripes[8] = {0U, 0U, 0U, 255U, 255U, 255U, 255U, 255U};
    CookPreviewImages images;
    memset(&images, 0, sizeof images);
    slot_from(&images, NPH_MATERIAL_TEXTURE_BASE_COLOR, stripes, 2U, 1U, NULL);
    m.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].nearest = 1U;
    render(&m, &images, picture);
    const u32 once = transitions(picture);
    NTEST_EXPECT_EQ_UINT(once, 1U); // black on the left of the front, white on its right
    // Four times across: the row's span, u from about 0.23 to 0.79, crosses five of the
    // stripes' edges (at 1, 1.5, 2, 2.5 and 3 tiles).
    m.tiling = nph_vec2(4.0F, 1.0F);
    render(&m, &images, picture);
    NTEST_EXPECT_EQ_UINT(transitions(picture), 5U);
    cook_preview_images_free(&images);
}

NTEST(preview, a_flat_normal_map_changes_nothing_and_a_tilted_one_does)
{
    static u8 plain[COOK_PREVIEW_BYTES];
    static u8 mapped[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    render(&m, NULL, plain);
    CookPreviewImages images;
    memset(&images, 0, sizeof images);
    static const u8 flat[4] = {128U, 128U, 255U, 255U};
    slot_from(&images, NPH_MATERIAL_TEXTURE_NORMAL, flat, 1U, 1U, NULL);
    render(&m, &images, mapped);
    u32 worst = 0U;
    for (u32 i = 0; i < COOK_PREVIEW_BYTES; ++i) {
        worst = nph_max_u32(worst, (u32)nph_i32_abs((i32)plain[i] - (i32)mapped[i]));
    }
    NTEST_EXPECT_LE_UINT(worst, 3U); // 128 is not quite 0.5: a hair's tilt
    cook_preview_images_free(&images);
    // Turned toward the image's right, all over: the shading moves.
    memset(&images, 0, sizeof images);
    static const u8 tilted[4] = {230U, 128U, 200U, 255U};
    slot_from(&images, NPH_MATERIAL_TEXTURE_NORMAL, tilted, 1U, 1U, NULL);
    render(&m, &images, mapped);
    u64 moved = 0U;
    for (u32 i = 0; i < COOK_PREVIEW_BYTES; ++i) {
        moved += (u64)nph_i32_abs((i32)plain[i] - (i32)mapped[i]);
    }
    NTEST_EXPECT_GT_UINT(moved, (u64)COOK_PREVIEW_BYTES); // more than one a byte, on average
    cook_preview_images_free(&images);
}

NTEST(preview, the_same_inputs_draw_the_same_bytes)
{
    static u8 a[COOK_PREVIEW_BYTES];
    static u8 b[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    m.metallic = 0.5F;
    m.roughness = 0.3F;
    m.rotation = 30.0F;
    CookPreviewImages images;
    memset(&images, 0, sizeof images);
    static const u8 grid[16] = {255U, 0U, 0U,   255U, 0U,   255U, 0U, 255U,
                                0U,   0U, 255U, 255U, 255U, 255U, 0U, 255U};
    slot_from(&images, NPH_MATERIAL_TEXTURE_BASE_COLOR, grid, 2U, 2U, NULL);
    render(&m, &images, a);
    render(&m, &images, b);
    NTEST_EXPECT_EQ_MEM(a, b, COOK_PREVIEW_BYTES);
    cook_preview_images_free(&images);
}

NTEST(preview, an_image_is_reduced_by_its_blocks_means_in_linear_light)
{
    NphAllocator *alloc = nph_allocator_system();
    // Four texels in a row, 0 and 128 twice: two blocks at most 2 a side.
    CookImage src;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 4U, 1U, COOK_IMAGE_RGBA8, &src), NPH_RESULT_OK);
    u8 *p = (u8 *)src.pixels;
    static const u8 reds[4] = {0U, 128U, 128U, 128U};
    for (u32 i = 0; i < 4U; ++i) {
        p[i * 4U + 0U] = reds[i];
        p[i * 4U + 3U] = (u8)(i * 85U);
    }
    CookImage out;
    NTEST_ASSERT_EQ_INT(cook_preview_reduce(alloc, &src, true, 2U, &out), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(out.width, 2U);
    NTEST_ASSERT_EQ_UINT(out.height, 1U);
    NTEST_EXPECT_EQ_INT(out.format, COOK_IMAGE_RGBA32F);
    const f32 *t = (const f32 *)out.pixels;
    const f32 dark = cook_srgb_to_linear(128.0F / 255.0F);
    NTEST_EXPECT_NEAR_F32(t[0], 0.5F * dark, 1e-6F); // in linear light: not 64's
    NTEST_EXPECT_NEAR_F32(t[4], dark, 1e-6F);
    NTEST_EXPECT_NEAR_F32(t[3], 85.0F / 510.0F, 1e-6F); // alpha is never transfer-coded
    cook_image_free(&out);
    // Data that is no colour, as it is.
    NTEST_ASSERT_EQ_INT(cook_preview_reduce(alloc, &src, false, 2U, &out), NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(((const f32 *)out.pixels)[0], 64.0F / 255.0F, 1e-6F);
    cook_image_free(&out);
    cook_image_free(&src);
    // Five wide, at most two: blocks of three, the last of two.
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 5U, 1U, COOK_IMAGE_RGBA8, &src), NPH_RESULT_OK);
    p = (u8 *)src.pixels;
    for (u32 i = 0; i < 5U; ++i) {
        p[(u64)i * 4U] = (u8)(i < 3U ? 30U : 90U);
    }
    NTEST_ASSERT_EQ_INT(cook_preview_reduce(alloc, &src, false, 2U, &out), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(out.width, 2U);
    NTEST_EXPECT_NEAR_F32(((const f32 *)out.pixels)[0], 30.0F / 255.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(((const f32 *)out.pixels)[4], 90.0F / 255.0F, 1e-6F);
    cook_image_free(&out);
    // Within the extent, converted alone; no extent, refused.
    NTEST_ASSERT_EQ_INT(cook_preview_reduce(alloc, &src, false, 256U, &out), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(out.width, 5U);
    cook_image_free(&out);
    NTEST_EXPECT_EQ_INT(cook_preview_reduce(alloc, &src, false, 0U, &out),
                        NPH_RESULT_INVALID_ARGUMENT);
    cook_image_free(&src);
}

NTEST(preview, images_are_made_from_their_files_with_their_levels)
{
    // An 8x8 colour image: four levels, its colour in linear light.
    u8 rgba[8 * 8 * 4];
    for (u32 i = 0; i < 64U; ++i) {
        rgba[i * 4U + 0U] = 128U;
        rgba[i * 4U + 1U] = 128U;
        rgba[i * 4U + 2U] = 128U;
        rgba[i * 4U + 3U] = 255U;
    }
    CookPreviewImages images;
    memset(&images, 0, sizeof images);
    slot_from(&images, NPH_MATERIAL_TEXTURE_BASE_COLOR, rgba, 8U, 8U, NULL);
    const CookPreviewImage *base = &images.slots[NPH_MATERIAL_TEXTURE_BASE_COLOR];
    NTEST_ASSERT_EQ_UINT(base->level_count, 4U);
    NTEST_EXPECT_EQ_UINT(base->levels[3].width, 1U);
    NTEST_EXPECT_NEAR_F32(((const f32 *)base->levels[0].pixels)[0],
                          cook_srgb_to_linear(128.0F / 255.0F), 1e-5F);
    // A normal map whose settings say DirectX's green: its green flipped, still unit length.
    static const u8 up[4] = {128U, 255U, 128U, 255U};
    slot_from(&images, NPH_MATERIAL_TEXTURE_NORMAL, up, 1U, 1U,
              "{\"texture\": 1, \"normal_green\": \"down\"}");
    const f32 *n = (const f32 *)images.slots[NPH_MATERIAL_TEXTURE_NORMAL].levels[0].pixels;
    NTEST_EXPECT_NEAR_F32(n[1], 0.0F, 0.01F);
    // A large one comes down to the extent: 300 wide by halves, then its levels.
    NphAllocator *alloc = nph_allocator_system();
    u8 *wide = NPH_ALLOC_ARRAY(alloc, u8, (u64)300U * 10U * 4U);
    NTEST_ASSERT_NOT_NULL(wide);
    memset(wide, 200, (size_t)300U * 10U * 4U);
    slot_from(&images, NPH_MATERIAL_TEXTURE_EMISSIVE, wide, 300U, 10U, NULL);
    NPH_FREE_ARRAY(alloc, u8, wide, (u64)300U * 10U * 4U);
    const CookPreviewImage *e = &images.slots[NPH_MATERIAL_TEXTURE_EMISSIVE];
    NTEST_EXPECT_EQ_UINT(e->levels[0].width, 150U);
    NTEST_EXPECT_EQ_UINT(e->levels[0].height, 5U);
    NTEST_EXPECT_EQ_UINT(e->levels[e->level_count - 1U].width, 1U);
    // Bytes that are no image leave their slot empty, and are no failure.
    static u8 junk[32] = {1U, 2U, 3U};
    CookPreviewFiles files;
    memset(&files, 0, sizeof files);
    files.alloc = alloc;
    files.image[NPH_MATERIAL_TEXTURE_OCCLUSION] = junk;
    files.image_size[NPH_MATERIAL_TEXTURE_OCCLUSION] = sizeof junk;
    CookPreviewImages none;
    NTEST_ASSERT_EQ_INT(cook_preview_images_make(alloc, &files, &none), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(none.slots[NPH_MATERIAL_TEXTURE_OCCLUSION].level_count, 0U);
    cook_preview_images_free(&none);
    cook_preview_images_free(&images);
}

NTEST(preview, a_materials_files_are_read_under_the_root)
{
    TempDir dir;
    test_temp_dir_make(&dir, "preview_files");
    char path[1400]; // the folder's 1300 and the longest name
    static const u8 white[4] = {255U, 255U, 255U, 255U};
    u8 tga[18 + 4];
    const u64 n = test_write_tga(white, 1U, 1U, tga);
    char folder[1300];
    test_path_in(&dir, "textures", folder, sizeof folder);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(folder), NPH_RESULT_OK);
    (void)snprintf(path, sizeof path, "%s/brick.tga", folder);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, n), NPH_RESULT_OK);
    (void)snprintf(path, sizeof path, "%s/brick.tga.import.json", folder);
    static const char settings[] = "{\"texture\": 1}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, settings, sizeof settings - 1U), NPH_RESULT_OK);
    CookMaterialSource m;
    cook_material_source_default(&m);
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].image, sizeof m.textures[0].image,
                   "textures/brick.tga");
    (void)snprintf(m.textures[NPH_MATERIAL_TEXTURE_NORMAL].image, sizeof m.textures[0].image,
                   "textures/missing.tga");
    NphAllocator *alloc = nph_allocator_system();
    CookPreviewFiles files;
    NTEST_ASSERT_EQ_INT(cook_preview_files_read(alloc, dir.dir, &m, &files), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(files.image_size[NPH_MATERIAL_TEXTURE_BASE_COLOR], n);
    NTEST_EXPECT_EQ_UINT(files.settings_size[NPH_MATERIAL_TEXTURE_BASE_COLOR],
                         sizeof settings - 1U);
    NTEST_EXPECT_NULL(files.image[NPH_MATERIAL_TEXTURE_NORMAL]); // drawn without it
    NTEST_EXPECT_NULL(files.image[NPH_MATERIAL_TEXTURE_EMISSIVE]);
    CookPreviewImages images;
    NTEST_ASSERT_EQ_INT(cook_preview_images_make(alloc, &files, &images), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(images.slots[NPH_MATERIAL_TEXTURE_BASE_COLOR].level_count, 1U);
    cook_preview_images_free(&images);
    cook_preview_files_free(&files);
    // A path that does not fit.
    static char root[4100];
    memset(root, 'r', sizeof root - 1U);
    root[sizeof root - 1U] = '\0';
    NTEST_EXPECT_EQ_INT(cook_preview_files_read(alloc, root, &m, &files), NPH_RESULT_OVERFLOW);
    test_temp_dir_remove(&dir);
}

NTEST(preview, the_cache_file_reads_back_for_its_key_alone)
{
    NphAllocator *alloc = nph_allocator_system();
    static u8 picture[COOK_PREVIEW_BYTES];
    static u8 back[COOK_PREVIEW_BYTES];
    CookMaterialSource m;
    cook_material_source_default(&m);
    render(&m, NULL, picture);
    TempDir dir;
    test_temp_dir_make(&dir, "preview_cache");
    char path[1300];
    test_path_in(&dir, "brick.nmat.nph", path, sizeof path);
    const u64 key = 0x1234567890ABCDEFULL;
    const NphGuid guid = nph_guid_from_cstr("materials/brick.nmat#preview");
    NTEST_ASSERT_EQ_INT(cook_preview_save(alloc, path, guid, key, picture), NPH_RESULT_OK);
    void *bytes = NULL;
    u64 size = 0U;
    NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, &bytes, &size), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_preview_read(bytes, size, key, back), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_MEM(back, picture, COOK_PREVIEW_BYTES);
    // Made for another key: stale, and nothing written.
    memset(back, 7, sizeof back);
    NTEST_EXPECT_EQ_INT(cook_preview_read(bytes, size, key + 1U, back), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_UINT(back[0], 7U);
    // A byte changed, cut short, nothing, or moved off its alignment: corrupt.
    u8 *b = (u8 *)bytes;
    b[size / 2U] ^= 0x40U;
    NTEST_EXPECT_EQ_INT(cook_preview_read(bytes, size, key, back), NPH_RESULT_CORRUPT);
    b[size / 2U] ^= 0x40U;
    NTEST_EXPECT_EQ_INT(cook_preview_read(bytes, size - 8U, key, back), NPH_RESULT_CORRUPT);
    NTEST_EXPECT_EQ_INT(cook_preview_read(NULL, 0U, key, back), NPH_RESULT_CORRUPT);
    u8 *shifted = NPH_ALLOC_ARRAY(alloc, u8, size + 16U);
    NTEST_ASSERT_NOT_NULL(shifted);
    memcpy(shifted + 1U, bytes, (size_t)size);
    NTEST_EXPECT_EQ_INT(cook_preview_read(shifted + 1U, size, key, back), NPH_RESULT_CORRUPT);
    NPH_FREE_ARRAY(alloc, u8, shifted, size + 16U);
    NTEST_ASSERT_EQ_INT(cook_preview_read(bytes, size, key, back), NPH_RESULT_OK);
    nph_file_free_all(alloc, bytes, size);
    // A texture of another size is no preview.
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION, guid, key);
    NphTextureHeader h;
    memset(&h, 0, sizeof h);
    h.width = 64U;
    h.height = 64U;
    h.mip_count = 1U;
    h.layer_count = 1U;
    h.format = (u32)NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB;
    NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_TEXTURE_TAG_HEADER, 0U, &h, sizeof h,
                                                  NPH_ASSET_CODEC_NONE, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_TEXTURE_TAG_MIP, 0U, picture,
                                                  (u64)64U * 64U * 4U, NPH_ASSET_CODEC_NONE, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_finish_alloc(&w, alloc, &bytes, &size), NPH_RESULT_OK);
    nph_asset_writer_free(&w);
    NTEST_EXPECT_EQ_INT(cook_preview_read(bytes, size, key, back), NPH_RESULT_CORRUPT);
    NPH_FREE(alloc, bytes, size, NPH_ASSET_BLOB_MAX_ALIGN);
    test_temp_dir_remove(&dir);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The fuzz target's seeds (scripts/fuzz_seeds.py) read for their header's key, so a seed that
// stops reading fails here rather than quietly in the nightly runs.
NTEST(preview, the_fuzz_corpus_reads)
{
    NphAllocator *alloc = nph_allocator_system();
    static u8 back[COOK_PREVIEW_BYTES];
    static const char *const seeds[2] = {"stored", "lz"};
    for (u32 i = 0; i < 2U; ++i) {
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/preview/%s", NPH_FUZZ_CORPUS_DIR, seeds[i]);
        void *bytes = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, &bytes, &size), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_INT(cook_preview_read(bytes, size, 0xC0FFEEULL, back), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(back[1], 7U); // the seed's pattern: byte i is 7 i
        NTEST_EXPECT_EQ_UINT(back[COOK_PREVIEW_BYTES - 1U],
                             (u32)((COOK_PREVIEW_BYTES - 1U) * 7U) & 0xFFU);
        nph_file_free_all(alloc, bytes, size);
    }
}
#endif
