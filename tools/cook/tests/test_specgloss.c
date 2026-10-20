// SPDX-License-Identifier: Apache-2.0
// Specular-glossiness to metallic-roughness (cook/specgloss.h): the cases
// the conversion keeps exactly (dielectrics, metals, and any grey material
// made by the inverse model), and the textures texel by texel.
#include <nepheline/math/scalar.h>

#include "test_cook_support.h"

#include <cook/specgloss.h>

static CookSpecGloss sg_of(NphVec4 diffuse, NphVec3 specular, f32 glossiness)
{
    CookSpecGloss sg;
    sg.diffuse = diffuse;
    sg.specular = specular;
    sg.glossiness = glossiness;
    return sg;
}

// A dielectric's specular (0.04) leaves the diffuse colour as the base
// colour, metallic 0; a lower specular takes less of the light, so the base
// colour is the diffuse over what a dielectric's would leave.
NTEST(specgloss, dielectrics_keep_their_diffuse_colour)
{
    const NphVec4 diffuse = nph_vec4(0.6F, 0.3F, 0.1F, 0.8F);
    CookMetalRough m =
        cook_specgloss_to_metalrough(sg_of(diffuse, nph_vec3(0.04F, 0.04F, 0.04F), 0.7F));
    NTEST_EXPECT_NEAR_F32(m.metallic, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m.base_color.x, 0.6F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(m.base_color.y, 0.3F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(m.base_color.z, 0.1F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(m.base_color.w, 0.8F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m.roughness, 0.3F, 1e-6F);
    m = cook_specgloss_to_metalrough(sg_of(diffuse, nph_vec3(0.02F, 0.02F, 0.02F), 1.0F));
    NTEST_EXPECT_NEAR_F32(m.metallic, 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(m.base_color.x, 0.6F * 0.98F / 0.96F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(m.roughness, 0.0F, 0.0F);
}

// A black diffuse with a coloured specular is a metal of that colour.
NTEST(specgloss, metals_keep_their_specular_colour)
{
    const NphVec3 gold = nph_vec3(1.0F, 0.766F, 0.336F);
    const CookMetalRough m =
        cook_specgloss_to_metalrough(sg_of(nph_vec4(0.0F, 0.0F, 0.0F, 1.0F), gold, 0.5F));
    NTEST_EXPECT_NEAR_F32(m.metallic, 1.0F, 1e-5F);
    NTEST_EXPECT_NEAR_F32(m.base_color.x, gold.x, 1e-5F);
    NTEST_EXPECT_NEAR_F32(m.base_color.y, gold.y, 1e-5F);
    NTEST_EXPECT_NEAR_F32(m.base_color.z, gold.z, 1e-5F);
}

// The inverse model: specular = lerp(0.04, base, metallic), diffuse = base
// (1 - 0.04) (1 - metallic) / (1 - the specular's largest channel). A grey
// material made by it comes back exactly (its brightnesses are its values,
// and the metallic share is the quadratic's non-negative root).
NTEST(specgloss, grey_materials_round_trip)
{
    for (u32 bi = 0; bi < 9U; ++bi) {
        const f32 base = 0.05F + 0.11F * (f32)bi;
        for (u32 mi = 0; mi <= 4U; ++mi) {
            const f32 metallic = 0.25F * (f32)mi;
            const f32 specular = 0.04F + (base - 0.04F) * metallic;
            const f32 diffuse =
                base * 0.96F * (1.0F - metallic) / nph_f32_max(1.0F - specular, 1e-6F);
            const CookMetalRough m =
                cook_specgloss_to_metalrough(sg_of(nph_vec4(diffuse, diffuse, diffuse, 1.0F),
                                                   nph_vec3(specular, specular, specular), 0.4F));
            NTEST_EXPECT_NEAR_F32(m.metallic, metallic, 2e-4F);
            NTEST_EXPECT_NEAR_F32(m.base_color.x, base, 2e-4F);
            NTEST_EXPECT_NEAR_F32(m.base_color.y, base, 2e-4F);
            NTEST_EXPECT_NEAR_F32(m.base_color.z, base, 2e-4F);
            NTEST_EXPECT_NEAR_F32(m.roughness, 0.6F, 1e-6F);
        }
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static void make_rgba8(CookImage *img, u32 w, u32 h, u32 seed)
{
    NTEST_ASSERT_EQ_INT(cook_image_create(nph_allocator_system(), w, h, COOK_IMAGE_RGBA8, img),
                        NPH_RESULT_OK);
    u8 *px = (u8 *)img->pixels;
    for (u32 i = 0; i < w * h * 4U; ++i) {
        px[i] = (u8)((i * 37U + seed * 101U) & 0xFFU);
    }
}

static f32 decode(u8 v)
{
    return cook_srgb_to_linear((f32)v / 255.0F);
}

static void expect_texel(const CookImage *base, const CookImage *mr, u32 i, CookSpecGloss sg)
{
    const CookMetalRough m = cook_specgloss_to_metalrough(sg);
    const u8 *b = (const u8 *)base->pixels + (u64)i * 4U;
    const u8 *t = (const u8 *)mr->pixels + (u64)i * 4U;
    const f32 want[3] = {cook_linear_to_srgb(m.base_color.x), cook_linear_to_srgb(m.base_color.y),
                         cook_linear_to_srgb(m.base_color.z)};
    for (u32 ch = 0; ch < 3U; ++ch) {
        NTEST_EXPECT_NEAR_F32((f32)b[ch], want[ch] * 255.0F, 0.51F);
    }
    NTEST_EXPECT_NEAR_F32((f32)b[3], m.base_color.w * 255.0F, 0.51F);
    NTEST_EXPECT_EQ_UINT(t[0], 255U);
    NTEST_EXPECT_NEAR_F32((f32)t[1], m.roughness * 255.0F, 0.51F);
    NTEST_EXPECT_NEAR_F32((f32)t[2], m.metallic * 255.0F, 0.51F);
    NTEST_EXPECT_EQ_UINT(t[3], 255U);
}

// Images of one extent: each texel is the conversion of the two texels
// under it (colour decoded from sRGB, alpha as stored), times the factors.
NTEST(specgloss, textures_convert_texel_by_texel)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage diffuse;
    CookImage specgloss;
    make_rgba8(&diffuse, 4U, 3U, 1U);
    make_rgba8(&specgloss, 4U, 3U, 2U);
    CookSpecGloss factors =
        sg_of(nph_vec4(0.9F, 0.8F, 1.0F, 1.0F), nph_vec3(1.0F, 0.5F, 1.0F), 0.75F);
    CookImage base;
    CookImage mr;
    NTEST_ASSERT_EQ_INT(cook_specgloss_textures(alloc, &diffuse, &specgloss, &factors, &base, &mr),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(base.width, 4U);
    NTEST_ASSERT_EQ_UINT(base.height, 3U);
    NTEST_ASSERT_EQ_UINT(mr.width, 4U);
    const u8 *d = (const u8 *)diffuse.pixels;
    const u8 *s = (const u8 *)specgloss.pixels;
    for (u32 i = 0; i < 12U; ++i) {
        const u8 *dt = d + (u64)i * 4U;
        const u8 *st = s + (u64)i * 4U;
        expect_texel(&base, &mr, i,
                     sg_of(nph_vec4(decode(dt[0]) * 0.9F, decode(dt[1]) * 0.8F, decode(dt[2]),
                                    (f32)dt[3] / 255.0F),
                           nph_vec3(decode(st[0]), decode(st[1]) * 0.5F, decode(st[2])),
                           (f32)st[3] / 255.0F * 0.75F));
    }
    cook_image_free(&base);
    cook_image_free(&mr);
    cook_image_free(&diffuse);
    cook_image_free(&specgloss);
}

// The outputs take the larger extent; a smaller input is sampled
// bilinearly (a uniform one reads its value everywhere), a missing one is
// its factor alone.
NTEST(specgloss, other_extents_and_missing_images)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage diffuse;
    make_rgba8(&diffuse, 4U, 2U, 3U);
    CookImage flat;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 2U, 3U, COOK_IMAGE_RGBA8, &flat), NPH_RESULT_OK);
    u8 *fp = (u8 *)flat.pixels;
    for (u32 i = 0; i < 6U; ++i) {
        fp[i * 4U + 0U] = 200U;
        fp[i * 4U + 1U] = 150U;
        fp[i * 4U + 2U] = 100U;
        fp[i * 4U + 3U] = 128U;
    }
    const CookSpecGloss ones =
        sg_of(nph_vec4(1.0F, 1.0F, 1.0F, 1.0F), nph_vec3(1.0F, 1.0F, 1.0F), 1.0F);
    CookImage base;
    CookImage mr;
    NTEST_ASSERT_EQ_INT(cook_specgloss_textures(alloc, &diffuse, &flat, &ones, &base, &mr),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(base.width, 4U);
    NTEST_ASSERT_EQ_UINT(base.height, 3U); // the specular-glossiness map's height
    // A uniform map samples to its value whatever the bilinear weights, so
    // every texel's roughness is its glossiness's complement.
    const u8 *t = (const u8 *)mr.pixels;
    for (u32 i = 0; i < 12U; ++i) {
        NTEST_EXPECT_NEAR_F32((f32)t[i * 4U + 1U], (1.0F - 128.0F / 255.0F) * 255.0F, 0.51F);
    }
    cook_image_free(&base);
    cook_image_free(&mr);
    // No diffuse image: the diffuse factor everywhere.
    const CookSpecGloss half =
        sg_of(nph_vec4(0.5F, 0.5F, 0.5F, 1.0F), nph_vec3(1.0F, 1.0F, 1.0F), 1.0F);
    NTEST_ASSERT_EQ_INT(cook_specgloss_textures(alloc, NULL, &flat, &half, &base, &mr),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(base.width, 2U);
    NTEST_ASSERT_EQ_UINT(base.height, 3U);
    for (u32 i = 0; i < 6U; ++i) {
        expect_texel(&base, &mr, i,
                     sg_of(nph_vec4(0.5F, 0.5F, 0.5F, 1.0F),
                           nph_vec3(decode(200U), decode(150U), decode(100U)), 128.0F / 255.0F));
    }
    cook_image_free(&base);
    cook_image_free(&mr);
    // Nothing to convert, or not 8-bit texels.
    NTEST_EXPECT_EQ_INT(cook_specgloss_textures(alloc, NULL, NULL, &ones, &base, &mr),
                        NPH_RESULT_INVALID_ARGUMENT);
    CookImage hdr;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 2U, 2U, COOK_IMAGE_RGBA32F, &hdr), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(cook_specgloss_textures(alloc, &hdr, NULL, &ones, &base, &mr),
                        NPH_RESULT_INVALID_ARGUMENT);
    cook_image_free(&hdr);
    cook_image_free(&flat);
    cook_image_free(&diffuse);
}
