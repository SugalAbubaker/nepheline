// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/texture.h>

#include "test_cook_support.h"

#include <cook/texture.h>

// A 16x16 image: a smooth colour gradient with a diagonal edge, alpha 255.
static void make_gradient(NphAllocator *alloc, u32 w, u32 h, CookImage *out)
{
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, w, h, COOK_IMAGE_RGBA8, out), NPH_RESULT_OK);
    u8 *p = (u8 *)out->pixels;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8 *t = p + ((u64)y * w + x) * 4U;
            const bool upper = x > y;
            t[0] = (u8)(upper ? 200U + x : 20U + x * 3U);
            t[1] = (u8)(y * 255U / nph_max_u32(h - 1U, 1U));
            t[2] = (u8)(upper ? 40U : 180U);
            t[3] = 255U;
        }
    }
}

// Decodes a BC7 level back to RGBA8 for comparison.
static void decode_bc7_level(const u8 *blocks, u32 w, u32 h, u8 *rgba)
{
    const u32 bw = (w + 3U) / 4U;
    for (u32 by = 0; by < (h + 3U) / 4U; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            CookBlockRgba8 out;
            NTEST_ASSERT_TRUE(cook_bc7_decode(blocks + ((u64)by * bw + bx) * 16U, &out));
            for (u32 j = 0; j < 4U; ++j) {
                for (u32 i = 0; i < 4U; ++i) {
                    const u32 x = bx * 4U + i;
                    const u32 y = by * 4U + j;
                    if (x < w && y < h) {
                        memcpy(rgba + ((u64)y * w + x) * 4U, out.texels[j * 4U + i], 4U);
                    }
                }
            }
        }
    }
}

NTEST(texture, color_textures_become_bc7_srgb_with_a_mip_chain)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    make_gradient(alloc, 16U, 16U, &img);
    CookTextureSettings s;
    cook_texture_settings_default(&s);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                          nph_guid_from_cstr("tex"), 1U);
    NTEST_ASSERT_EQ_INT(cook_texture(alloc, &img, &s, NULL, &w), NPH_RESULT_OK);
    TestAsset ta;
    test_asset_from_writer(alloc, &w, &ta);
    nph_asset_writer_free(&w);
    NphTextureView v;
    NTEST_ASSERT_EQ_INT(nph_texture_view(&ta.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->width, 16U);
    NTEST_ASSERT_EQ_UINT(v.header->mip_count, 5U);
    NTEST_ASSERT_EQ_UINT(v.header->format, NPH_TEXTURE_FORMAT_BC7_SRGB);
    NTEST_ASSERT_EQ_UINT(v.header->flags, 0U);
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[0], 16U * 16U); // 16 blocks of 16 bytes
    NTEST_ASSERT_EQ_UINT(v.mip_sizes[4], 16U);       // 1x1 is one block
    u8 back[16 * 16 * 4];
    decode_bc7_level((const u8 *)v.mips[0], 16U, 16U, back);
    const f64 psnr = test_psnr8(back, (const u8 *)img.pixels, sizeof back);
    NTEST_LOG("BC7 sRGB gradient PSNR %.1f dB", psnr);
    NTEST_ASSERT_GE_F64(psnr, 38.0);
    // The last mip is the average colour, roughly.
    u8 one[4 * 4];
    decode_bc7_level((const u8 *)v.mips[4], 1U, 1U, one);
    NTEST_ASSERT_GT_UINT(one[1], 100U);
    NTEST_ASSERT_LT_UINT(one[1], 160U);
    test_asset_free(&ta);
    cook_image_free(&img);
}

NTEST(texture, other_usages_pick_their_formats)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    make_gradient(alloc, 8U, 8U, &img);
    const struct {
        CookTextureUsage usage;
        b32 uncompressed;
        NphTextureFormat format;
        u32 flags;
    } cases[] = {
        {COOK_TEXTURE_LINEAR, 0, NPH_TEXTURE_FORMAT_BC7_UNORM, 0U},
        {COOK_TEXTURE_NORMAL, 0, NPH_TEXTURE_FORMAT_BC5_UNORM, (u32)NPH_TEXTURE_FLAG_NORMAL_MAP},
        {COOK_TEXTURE_MASK, 0, NPH_TEXTURE_FORMAT_BC4_UNORM, 0U},
        {COOK_TEXTURE_COLOR, 1, NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB, 0U},
        {COOK_TEXTURE_LINEAR, 1, NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM, 0U},
        {COOK_TEXTURE_NORMAL, 1, NPH_TEXTURE_FORMAT_R8G8_UNORM, (u32)NPH_TEXTURE_FLAG_NORMAL_MAP},
        {COOK_TEXTURE_MASK, 1, NPH_TEXTURE_FORMAT_R8_UNORM, 0U},
    };
    for (u32 c = 0; c < NPH_ARRAY_COUNT(cases); ++c) {
        CookTextureSettings s;
        cook_texture_settings_default(&s);
        s.usage = cases[c].usage;
        s.uncompressed = cases[c].uncompressed;
        s.no_mips = 1;
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                              nph_guid_from_cstr("t"), 1U);
        NTEST_ASSERT_EQ_INT(cook_texture(alloc, &img, &s, NULL, &w), NPH_RESULT_OK);
        TestAsset ta;
        test_asset_from_writer(alloc, &w, &ta);
        nph_asset_writer_free(&w);
        NphTextureView v;
        NTEST_ASSERT_EQ_INT(nph_texture_view(&ta.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.header->format, (u32)cases[c].format);
        NTEST_ASSERT_EQ_UINT(v.header->mip_count, 1U);
        NTEST_ASSERT_EQ_UINT(v.header->flags, cases[c].flags);
        if (cases[c].format == NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM) {
            NTEST_ASSERT_EQ_MEM(v.mips[0], img.pixels, img.size); // linear: bytes untouched
        }
        if (cases[c].format == NPH_TEXTURE_FORMAT_R8_UNORM) {
            NTEST_ASSERT_EQ_UINT(((const u8 *)v.mips[0])[5],
                                 ((const u8 *)img.pixels)[(u64)5U * 4U]);
        }
        test_asset_free(&ta);
    }
    cook_image_free(&img);
}

NTEST(texture, hdr_becomes_bc6h_or_rgba16f)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 8U, 4U, COOK_IMAGE_RGBA32F, &img), NPH_RESULT_OK);
    f32 *p = (f32 *)img.pixels;
    for (u32 i = 0; i < 32U; ++i) {
        p[(u64)i * 4U] = 0.1F + (f32)i;
        p[i * 4U + 1U] = 5.0F;
        p[i * 4U + 2U] = 40.0F - (f32)i;
        p[i * 4U + 3U] = 1.0F;
    }
    for (u32 uncompressed = 0; uncompressed < 2U; ++uncompressed) {
        CookTextureSettings s;
        cook_texture_settings_default(&s);
        s.usage = COOK_TEXTURE_HDR;
        s.uncompressed = (b32)uncompressed;
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                              nph_guid_from_cstr("hdr"), 1U);
        NTEST_ASSERT_EQ_INT(cook_texture(alloc, &img, &s, NULL, &w), NPH_RESULT_OK);
        TestAsset ta;
        test_asset_from_writer(alloc, &w, &ta);
        nph_asset_writer_free(&w);
        NphTextureView v;
        NTEST_ASSERT_EQ_INT(nph_texture_view(&ta.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.header->mip_count, 4U);
        if (uncompressed != 0U) {
            NTEST_ASSERT_EQ_UINT(v.header->format, NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT);
            const u16 *h = (const u16 *)v.mips[0];
            NTEST_ASSERT_NEAR_F32(nph_f16_to_f32(h[5U * 4U + 2U]), 35.0F, 0.05F);
        } else {
            NTEST_ASSERT_EQ_UINT(v.header->format, NPH_TEXTURE_FORMAT_BC6H_UFLOAT);
            CookBlockRgbHalf out;
            NTEST_ASSERT_TRUE(cook_bc6h_decode((const u8 *)v.mips[0], &out));
            NTEST_ASSERT_NEAR_F32(nph_f16_to_f32(out.texels[0][1]), 5.0F, 0.5F);
        }
        test_asset_free(&ta);
    }
    // An 8-bit image cannot be cooked as HDR directly, nor float as colour.
    CookTextureSettings s;
    cook_texture_settings_default(&s);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                          nph_guid_from_cstr("x"), 1U);
    NTEST_ASSERT_EQ_INT(cook_texture(alloc, &img, &s, NULL, &w), NPH_RESULT_INVALID_ARGUMENT);
    nph_asset_writer_free(&w);
    cook_image_free(&img);
}

NTEST(texture, odd_sizes_and_limits_and_jobs)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    make_gradient(alloc, 37U, 21U, &img);
    CookTextureSettings s;
    cook_texture_settings_default(&s);
    s.max_extent = 16U;
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = alloc;
    jd.worker_count = 2U;
    NphJobSystem *jobs = NULL;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &jobs), NPH_RESULT_OK);
    void *serial = NULL;
    u64 serial_size = 0U;
    for (u32 with_jobs = 0; with_jobs < 2U; ++with_jobs) {
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                              nph_guid_from_cstr("odd"), 1U);
        NTEST_ASSERT_EQ_INT(cook_texture(alloc, &img, &s, with_jobs != 0U ? jobs : NULL, &w),
                            NPH_RESULT_OK);
        void *image = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_asset_writer_finish_alloc(&w, alloc, &image, &size), NPH_RESULT_OK);
        nph_asset_writer_free(&w);
        if (with_jobs == 0U) {
            serial = image;
            serial_size = size;
            NphAssetFileInfo info;
            NTEST_ASSERT_EQ_INT(nph_asset_file_validate(image, size, &info), NPH_RESULT_OK);
            const NphAssetBlobEntry *head = nph_asset_file_find(&info, NPH_TEXTURE_TAG_HEADER, 0U);
            NphTextureHeader h;
            memcpy(&h, nph_asset_file_blob(image, head), sizeof h);
            NTEST_ASSERT_EQ_UINT(h.width, 9U); // 37 -> 18 -> 9
            NTEST_ASSERT_EQ_UINT(h.height, 5U);
            NTEST_ASSERT_EQ_UINT(h.mip_count, 4U);
        } else {
            // The job-encoded output is byte-identical to the serial one.
            NTEST_ASSERT_EQ_UINT(size, serial_size);
            NTEST_ASSERT_EQ_MEM(image, serial, size);
            NPH_FREE(alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
        }
    }
    NPH_FREE(alloc, serial, serial_size, NPH_ASSET_BLOB_MAX_ALIGN);
    nph_job_system_destroy(jobs);
    cook_image_free(&img);
    NTEST_ASSERT_EQ_INT(cook_texture_format(COOK_TEXTURE_HDR, false),
                        NPH_TEXTURE_FORMAT_BC6H_UFLOAT);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const CookTextureUsage bad_usage = (CookTextureUsage)9;
    NTEST_ASSERT_EQ_INT(cook_texture_format(bad_usage, false), NPH_TEXTURE_FORMAT_UNDEFINED);
}
