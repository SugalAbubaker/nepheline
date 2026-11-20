// SPDX-License-Identifier: Apache-2.0
// Glass whose transmission varies per texel (cook/glass.h; ADR-0107): the base colour texture made
// from a base colour image and a transmission image, texel by texel and across extents, and what
// it refuses.
#include <nepheline/math/scalar.h>

#include "test_cook_support.h"

#include <cook/glass.h>

// An RGBA8 image of `w` x `h` texels, 4 bytes each.
static void image_of(CookImage *img, u32 w, u32 h, const u8 *texels)
{
    NTEST_ASSERT_EQ_INT(cook_image_create(nph_allocator_system(), w, h, COOK_IMAGE_RGBA8, img),
                        NPH_RESULT_OK);
    memcpy(img->pixels, texels, (size_t)w * h * 4U);
}

// Of one extent: each texel's colour is the base colour's bytes as they were, and its alpha the
// base's times the share it keeps, one minus the factor times the transmission's red. A factor
// past one is one.
NTEST(glass, each_texel_keeps_its_share)
{
    NphAllocator *alloc = nph_allocator_system();
    static const u8 base_px[2 * 2 * 4] = {200U, 100U, 50U, 255U, 10U,  20U,  30U,  128U,
                                          0U,   0U,   0U,  255U, 255U, 255U, 255U, 0U};
    static const u8 trans_px[2 * 2 * 4] = {0U,   9U, 9U, 9U, 255U, 9U, 9U, 9U,
                                           128U, 9U, 9U, 9U, 64U,  9U, 9U, 9U};
    CookImage base;
    CookImage trans;
    image_of(&base, 2U, 2U, base_px);
    image_of(&trans, 2U, 2U, trans_px);
    CookImage out;
    NTEST_ASSERT_EQ_INT(cook_glass_texture(alloc, &base, &trans, 0.8F, &out), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(out.width, 2U);
    NTEST_ASSERT_EQ_UINT(out.height, 2U);
    NTEST_ASSERT_TRUE(out.format == COOK_IMAGE_RGBA8);
    const u8 *o = (const u8 *)out.pixels;
    for (u32 i = 0; i < 4U; ++i) {
        for (u32 c = 0; c < 3U; ++c) {
            NTEST_EXPECT_EQ_UINT(o[(u64)i * 4U + c], base_px[(u64)i * 4U + c]);
        }
        const f32 keep = 1.0F - 0.8F * (f32)trans_px[(u64)i * 4U] / 255.0F;
        NTEST_EXPECT_NEAR_F32((f32)o[(u64)i * 4U + 3U], (f32)base_px[(u64)i * 4U + 3U] * keep,
                              0.51F);
    }
    NTEST_EXPECT_EQ_UINT(o[3], 255U); // no transmission: all of it kept
    NTEST_EXPECT_EQ_UINT(o[7], 26U);  // half an alpha, a fifth of it kept
    NTEST_EXPECT_EQ_UINT(o[15], 0U);  // no alpha: none
    cook_image_free(&out);
    NTEST_ASSERT_EQ_INT(cook_glass_texture(alloc, &base, &trans, 1.5F, &out), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(((const u8 *)out.pixels)[7], 0U); // all of its light let through
    cook_image_free(&out);
    cook_image_free(&base);
    cook_image_free(&trans);
}

// Without a base colour the texture is white, its alpha the share alone. An input of another
// extent is read at the output's texel centres: a 1 x 1 base colour everywhere, its colour
// encoded again, and a 1 x 1 transmission over a base colour's texels kept whole.
NTEST(glass, a_missing_base_colour_is_white_and_other_extents_are_sampled)
{
    NphAllocator *alloc = nph_allocator_system();
    static const u8 trans_px[2 * 4] = {0U, 0U, 0U, 255U, 255U, 0U, 0U, 255U};
    CookImage trans;
    image_of(&trans, 2U, 1U, trans_px);
    CookImage out;
    NTEST_ASSERT_EQ_INT(cook_glass_texture(alloc, NULL, &trans, 1.0F, &out), NPH_RESULT_OK);
    const u8 *o = (const u8 *)out.pixels;
    for (u32 i = 0; i < 2U; ++i) {
        for (u32 c = 0; c < 3U; ++c) {
            NTEST_EXPECT_EQ_UINT(o[(u64)i * 4U + c], 255U);
        }
    }
    NTEST_EXPECT_EQ_UINT(o[3], 255U);
    NTEST_EXPECT_EQ_UINT(o[7], 0U);
    cook_image_free(&out);

    static const u8 base_px[4] = {255U, 0U, 128U, 200U};
    CookImage base;
    image_of(&base, 1U, 1U, base_px);
    NTEST_ASSERT_EQ_INT(cook_glass_texture(alloc, &base, &trans, 1.0F, &out), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(out.width, 2U);
    NTEST_ASSERT_EQ_UINT(out.height, 1U);
    o = (const u8 *)out.pixels;
    for (u32 i = 0; i < 2U; ++i) {
        NTEST_EXPECT_EQ_UINT(o[(u64)i * 4U], 255U);
        NTEST_EXPECT_EQ_UINT(o[(u64)i * 4U + 1U], 0U);
        NTEST_EXPECT_NEAR_F32((f32)o[(u64)i * 4U + 2U], 128.0F, 1.0F);
    }
    NTEST_EXPECT_EQ_UINT(o[3], 200U);
    NTEST_EXPECT_EQ_UINT(o[7], 0U);
    cook_image_free(&out);

    static const u8 wide_px[2 * 4] = {10U, 20U, 30U, 255U, 40U, 50U, 60U, 255U};
    static const u8 half_px[4] = {128U, 0U, 0U, 0U};
    CookImage wide;
    CookImage half;
    image_of(&wide, 2U, 1U, wide_px);
    image_of(&half, 1U, 1U, half_px);
    NTEST_ASSERT_EQ_INT(cook_glass_texture(alloc, &wide, &half, 1.0F, &out), NPH_RESULT_OK);
    o = (const u8 *)out.pixels;
    for (u32 i = 0; i < 8U; ++i) {
        if (i % 4U != 3U) {
            NTEST_EXPECT_EQ_UINT(o[i], wide_px[i]);
        }
    }
    NTEST_EXPECT_EQ_UINT(o[3], 127U); // 255 x (1 - 128/255)
    NTEST_EXPECT_EQ_UINT(o[7], 127U);
    cook_image_free(&out);
    cook_image_free(&wide);
    cook_image_free(&half);
    cook_image_free(&base);
    cook_image_free(&trans);
}

// What it cannot bake: no transmission image, an input not of 8-bit texels, a factor that is no
// number. The output is left empty.
NTEST(glass, refuses_what_it_cannot_bake)
{
    NphAllocator *alloc = nph_allocator_system();
    static const u8 px[4] = {255U, 255U, 255U, 255U};
    CookImage base;
    image_of(&base, 1U, 1U, px);
    CookImage hdr;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 1U, 1U, COOK_IMAGE_RGBA32F, &hdr), NPH_RESULT_OK);
    CookImage out;
    NTEST_EXPECT_EQ_INT(cook_glass_texture(alloc, &base, NULL, 1.0F, &out),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(out.pixels);
    NTEST_EXPECT_EQ_INT(cook_glass_texture(alloc, &hdr, &base, 1.0F, &out),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_glass_texture(alloc, &base, &hdr, 1.0F, &out),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(
        cook_glass_texture(alloc, &base, &base, nph_f32_from_bits(0x7FC00000U), &out),
        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(out.pixels);
    cook_image_free(&hdr);
    cook_image_free(&base);
}
