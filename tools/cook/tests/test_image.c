// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/log.h>

#include "test_cook_support.h"

#include <cook/image.h>

NTEST(image, decodes_tga_from_memory_and_file)
{
    NphAllocator *alloc = nph_allocator_system();
    u8 rgba[4 * 3 * 4];
    for (u32 i = 0; i < 12U; ++i) {
        rgba[(u64)i * 4U] = (u8)(i * 20U);
        rgba[i * 4U + 1U] = (u8)(255U - i * 10U);
        rgba[i * 4U + 2U] = (u8)(i * 3U);
        rgba[i * 4U + 3U] = (u8)(200U + i);
    }
    u8 tga[18 + 4 * 3 * 4];
    const u64 len = test_write_tga(rgba, 4U, 3U, tga);
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_decode(alloc, tga, len, &img), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(img.width, 4U);
    NTEST_ASSERT_EQ_UINT(img.height, 3U);
    NTEST_ASSERT_EQ_INT(img.format, COOK_IMAGE_RGBA8);
    NTEST_ASSERT_EQ_UINT(img.source_channels, 4U);
    NTEST_ASSERT_EQ_MEM(img.pixels, rgba, sizeof rgba);
    cook_image_free(&img);
    cook_image_free(&img);
    NTEST_ASSERT_FALSE(cook_image_data_is_hdr(tga, len));

    TempDir dir;
    test_temp_dir_make(&dir, "image");
    char path[1200];
    test_path_in(&dir, "t.tga", path, sizeof path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, tga, len), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_image_load(alloc, path, &img), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_MEM(img.pixels, rgba, sizeof rgba);
    cook_image_free(&img);
    test_path_in(&dir, "missing.tga", path, sizeof path);
    NTEST_ASSERT_EQ_INT(cook_image_load(alloc, path, &img), NPH_RESULT_NOT_FOUND);
    test_temp_dir_remove(&dir);

    const u8 junk[] = "this is not an image";
    NTEST_ASSERT_EQ_INT(cook_image_decode(alloc, junk, sizeof junk, &img), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(cook_image_decode(alloc, NULL, 0U, &img), NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(image, decodes_hdr_to_float)
{
    NphAllocator *alloc = nph_allocator_system();
    // RGBE shares one exponent per texel: channels within a factor of 256.
    f32 rgb[2 * 2 * 3] = {1.0F, 2.0F, 4.0F, 0.5F,   0.25F, 2.0F,
                          0.0F, 0.0F, 0.0F, 300.0F, 3.0F,  30.0F};
    u8 hdr[128];
    const u64 len = test_write_hdr(rgb, 2U, 2U, hdr);
    NTEST_ASSERT_TRUE(cook_image_data_is_hdr(hdr, len));
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_decode(alloc, hdr, len, &img), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(img.format, COOK_IMAGE_RGBA32F);
    const f32 *p = (const f32 *)img.pixels;
    for (u32 i = 0; i < 4U; ++i) {
        // RGBE keeps 8 bits of mantissa per channel against the texel's
        // shared exponent, which the largest channel sets: every channel
        // is accurate to one step of that scale (the writer truncates), so
        // 3.0 next to 300.0 comes back as 2.0.
        const f32 largest =
            nph_f32_max3(rgb[(u64)i * 3U], rgb[(u64)i * 3U + 1U], rgb[(u64)i * 3U + 2U]);
        for (u32 c = 0; c < 3U; ++c) {
            const f32 expect = rgb[i * 3U + c];
            NTEST_ASSERT_NEAR_F32(p[i * 4U + c], expect, largest / 128.0F + 1e-3F);
        }
        NTEST_ASSERT_NEAR_F32(p[i * 4U + 3U], 1.0F, 0.0F);
    }
    cook_image_free(&img);
}

NTEST(image, srgb_transfer_and_conversions_round_trip)
{
    NTEST_ASSERT_NEAR_F32(cook_srgb_to_linear(0.0F), 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(cook_srgb_to_linear(1.0F), 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(cook_srgb_to_linear(0.5F), 0.2140F, 1e-3F);
    NTEST_ASSERT_NEAR_F32(cook_linear_to_srgb(0.2140F), 0.5F, 1e-3F);
    for (u32 i = 0; i <= 100U; ++i) {
        const f32 v = (f32)i / 100.0F;
        NTEST_ASSERT_NEAR_F32(cook_linear_to_srgb(cook_srgb_to_linear(v)), v, 1e-5F);
    }
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 16U, 16U, COOK_IMAGE_RGBA8, &img), NPH_RESULT_OK);
    u8 *px = (u8 *)img.pixels;
    for (u32 i = 0; i < 256U; ++i) {
        px[(u64)i * 4U] = (u8)i;
        px[i * 4U + 1U] = (u8)(255U - i);
        px[i * 4U + 2U] = (u8)(i / 2U);
        px[i * 4U + 3U] = (u8)(i * 7U);
    }
    for (u32 srgb = 0; srgb < 2U; ++srgb) {
        CookImage f;
        CookImage back;
        NTEST_ASSERT_EQ_INT(cook_image_to_f32(alloc, &img, srgb != 0U, &f), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(cook_image_to_rgba8(alloc, &f, srgb != 0U, &back), NPH_RESULT_OK);
        // Every 8-bit value survives a trip through float.
        NTEST_ASSERT_EQ_MEM(back.pixels, img.pixels, img.size);
        if (srgb != 0U) {
            const f32 *lin = (const f32 *)f.pixels;
            NTEST_ASSERT_NEAR_F32(lin[(u64)128U * 4U], cook_srgb_to_linear(128.0F / 255.0F), 1e-6F);
            NTEST_ASSERT_NEAR_F32(lin[128U * 4U + 3U], (f32)(128U * 7U % 256U) / 255.0F, 1e-6F);
        }
        cook_image_free(&f);
        cook_image_free(&back);
    }
    cook_image_free(&img);
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 0U, 4U, COOK_IMAGE_RGBA8, &img),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(
        cook_image_create(alloc, 4U, COOK_IMAGE_MAX_EXTENT + 1U, COOK_IMAGE_RGBA8, &img),
        NPH_RESULT_INVALID_ARGUMENT);
}

NTEST(image, downsampling_averages_by_area)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage src;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 4U, 2U, COOK_IMAGE_RGBA32F, &src), NPH_RESULT_OK);
    f32 *p = (f32 *)src.pixels;
    for (u32 i = 0; i < 8U; ++i) {
        p[(u64)i * 4U] = (f32)i;
        p[i * 4U + 3U] = 1.0F;
    }
    CookImage half;
    NTEST_ASSERT_EQ_INT(cook_image_downsample(alloc, &src, &half), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(half.width, 2U);
    NTEST_ASSERT_EQ_UINT(half.height, 1U);
    const f32 *q = (const f32 *)half.pixels;
    NTEST_ASSERT_NEAR_F32(q[0], (0.0F + 1.0F + 4.0F + 5.0F) / 4.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(q[4], (2.0F + 3.0F + 6.0F + 7.0F) / 4.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(q[3], 1.0F, 0.0F);
    cook_image_free(&half);
    cook_image_free(&src);

    // Odd sizes: 5x3 -> 2x1, each destination texel covering two and a
    // half columns (the middle one split between them) and all three rows.
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 5U, 3U, COOK_IMAGE_RGBA32F, &src), NPH_RESULT_OK);
    p = (f32 *)src.pixels;
    for (u32 i = 0; i < 15U; ++i) {
        const u32 column = i % 5U;
        const u32 row = i / 5U;
        p[(u64)i * 4U] = (f32)column;
        p[(u64)i * 4U + 1U] = (f32)row;
    }
    NTEST_ASSERT_EQ_INT(cook_image_downsample(alloc, &src, &half), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(half.width, 2U);
    NTEST_ASSERT_EQ_UINT(half.height, 1U);
    q = (const f32 *)half.pixels;
    NTEST_ASSERT_NEAR_F32(q[0], (0.0F + 1.0F + 0.5F * 2.0F) / 2.5F, 1e-6F); // 0, 1, half of 2
    NTEST_ASSERT_NEAR_F32(q[4], (0.5F * 2.0F + 3.0F + 4.0F) / 2.5F, 1e-6F); // half of 2, 3, 4
    NTEST_ASSERT_NEAR_F32(q[1], 1.0F, 1e-6F); // the rows 0, 1, 2 averaged
    cook_image_free(&half);
    // Down to 1x1 by repeated halving: the exact area average keeps the
    // image's mean at every level (2, the mean of the columns), where
    // boxes of two and three texels would drift (to 1.75 here).
    NTEST_ASSERT_EQ_INT(cook_image_limit_extent(alloc, &src, 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(src.width, 1U);
    NTEST_ASSERT_EQ_UINT(src.height, 1U);
    NTEST_ASSERT_NEAR_F32(((const f32 *)src.pixels)[0], 2.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(((const f32 *)src.pixels)[1], 1.0F, 1e-6F);
    NTEST_ASSERT_EQ_INT(cook_image_downsample(alloc, &src, &half), NPH_RESULT_INVALID_ARGUMENT);
    cook_image_free(&src);
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 4U, 4U, COOK_IMAGE_RGBA8, &src), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_image_downsample(alloc, &src, &half), NPH_RESULT_INVALID_ARGUMENT);
    cook_image_free(&src);
}

NTEST(image, normal_maps_are_renormalised)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 2U, 1U, COOK_IMAGE_RGBA32F, &img), NPH_RESULT_OK);
    f32 *p = (f32 *)img.pixels;
    // A vector of length 2 along +x, and the zero vector.
    p[0] = 0.5F + 1.0F;
    p[1] = 0.5F;
    p[2] = 0.5F;
    p[4] = 0.5F;
    p[5] = 0.5F;
    p[6] = 0.5F;
    cook_image_renormalize_normals(&img);
    NTEST_ASSERT_NEAR_F32(p[0], 1.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(p[1], 0.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(p[6], 1.0F, 1e-6F); // zero became +Z
    cook_image_free(&img);
}

NTEST(image, blocks_replicate_edges)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 5U, 3U, COOK_IMAGE_RGBA8, &img), NPH_RESULT_OK);
    u8 *p = (u8 *)img.pixels;
    for (u32 i = 0; i < 15U; ++i) {
        p[(u64)i * 4U] = (u8)i;
    }
    CookBlockRgba8 out;
    cook_image_block_rgba8(&img, 4U, 0U, &out);
    u8(*block)[4] = out.texels;
    NTEST_ASSERT_EQ_UINT(block[0][0], 4U);   // (4, 0)
    NTEST_ASSERT_EQ_UINT(block[1][0], 4U);   // (5, 0) clamps to column 4
    NTEST_ASSERT_EQ_UINT(block[4][0], 9U);   // (4, 1)
    NTEST_ASSERT_EQ_UINT(block[12][0], 14U); // (4, 3) clamps to row 2
    NTEST_ASSERT_EQ_UINT(block[15][0], 14U);
    cook_image_free(&img);

    NTEST_ASSERT_EQ_INT(cook_image_create(alloc, 2U, 2U, COOK_IMAGE_RGBA32F, &img), NPH_RESULT_OK);
    f32 *f = (f32 *)img.pixels;
    f[0] = 2.0F;
    f[1] = -1.0F; // negative clamps to zero for BC6H unsigned
    f[2] = 1e6F;  // clamps to the half-float maximum
    CookBlockRgbHalf half_out;
    cook_image_block_half(&img, 0U, 0U, &half_out);
    u16(*half)[3] = half_out.texels;
    NTEST_ASSERT_EQ_UINT(half[0][0], nph_f32_to_f16(2.0F));
    NTEST_ASSERT_EQ_UINT(half[0][1], 0U);
    NTEST_ASSERT_EQ_UINT(half[0][2], nph_f32_to_f16(NPH_F16_MAX));
    NTEST_ASSERT_EQ_UINT(half[15][0], 0U); // (1, 1) is zero
    cook_image_free(&img);
}

#ifdef NPH_FUZZ_CORPUS_DIR
// The fuzz seeds are written from the format descriptions by
// scripts/fuzz_seeds_cook.py, not by stb_image or this cooker: every one
// must decode, with the size its header declares, and a few with known
// content.
NTEST(image, the_fuzz_corpus_decodes)
{
    NphAllocator *alloc = nph_allocator_system();
    static const struct {
        const char *name;
        u32 width;
        u32 height;
        u32 channels;
        CookImageFormat format;
    } seeds[] = {
        {"png_rgba", 4U, 4U, 4U, COOK_IMAGE_RGBA8},    {"png_gray", 5U, 3U, 1U, COOK_IMAGE_RGBA8},
        {"png_palette", 4U, 4U, 4U, COOK_IMAGE_RGBA8}, {"png_rgb16", 3U, 2U, 3U, COOK_IMAGE_RGBA8},
        {"tga", 4U, 4U, 4U, COOK_IMAGE_RGBA8},         {"tga_rle", 6U, 3U, 3U, COOK_IMAGE_RGBA8},
        {"bmp", 5U, 3U, 3U, COOK_IMAGE_RGBA8},         {"hdr", 4U, 2U, 3U, COOK_IMAGE_RGBA32F},
        {"hdr_rle", 8U, 2U, 3U, COOK_IMAGE_RGBA32F},   {"jpeg", 8U, 8U, 1U, COOK_IMAGE_RGBA8},
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(seeds); ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/image/%s", NPH_FUZZ_CORPUS_DIR, seeds[i].name);
        void *bytes = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_all(path, alloc, &bytes, &size), NPH_RESULT_OK);
        u32 w;
        u32 h;
        u32 c;
        NTEST_ASSERT_EQ_INT(cook_image_info(bytes, size, &w, &h, &c), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(w, seeds[i].width);
        NTEST_ASSERT_EQ_UINT(h, seeds[i].height);
        NTEST_ASSERT_EQ_UINT(c, seeds[i].channels);
        CookImage img;
        NTEST_ASSERT_EQ_INT(cook_image_decode(alloc, bytes, size, &img), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(img.width, seeds[i].width);
        NTEST_ASSERT_EQ_UINT(img.height, seeds[i].height);
        NTEST_ASSERT_EQ_INT(img.format, seeds[i].format);
        const u8 *p = (const u8 *)img.pixels;
        if (strcmp(seeds[i].name, "jpeg") == 0) {
            // One flat block: DC 128 under a quantiser of 1 is 128 / 8 + 128.
            for (u32 t = 0; t < 64U; ++t) {
                NTEST_ASSERT_EQ_UINT(p[(u64)t * 4U], 144U);
            }
        } else if (strcmp(seeds[i].name, "png_rgba") == 0) {
            // Texel (1, 2) of the gradient: x * 40, y * 50, (x + y) * 20, 255 - x * 10.
            const u8 *t = p + (u64)(2U * 4U + 1U) * 4U;
            NTEST_ASSERT_EQ_UINT(t[0], 40U);
            NTEST_ASSERT_EQ_UINT(t[1], 100U);
            NTEST_ASSERT_EQ_UINT(t[2], 60U);
            NTEST_ASSERT_EQ_UINT(t[3], 245U);
        } else if (strcmp(seeds[i].name, "png_palette") == 0) {
            // Texel (1, 0) is palette entry 1: red with alpha 128.
            NTEST_ASSERT_EQ_UINT(p[4], 255U);
            NTEST_ASSERT_EQ_UINT(p[7], 128U);
        }
        cook_image_free(&img);
        nph_file_free_all(alloc, bytes, size);
    }
    NTEST_ASSERT_EQ_INT(cook_image_info("nope", 4U, &(u32){0U}, &(u32){0U}, &(u32){0U}),
                        NPH_RESULT_PARSE_ERROR);
}
#endif

// The logger, initialised with a sink that keeps the last error, so a test
// can see what a failure logged. Teardown shuts it down even when an
// assertion fails.
typedef struct LogCaptureFixture {
    char message[NPH_LOG_MESSAGE_MAX];
    u32 count;
    bool initialized;
} LogCaptureFixture;

static void keep_last_error(const NphLogRecord *record, void *user)
{
    LogCaptureFixture *f = (LogCaptureFixture *)user;
    memcpy(f->message, record->message, sizeof f->message);
    f->count += 1U;
}

NTEST_SETUP(LogCaptureFixture)
{
    const NphLogConfig config = {nph_allocator_system(), 64U, 4U};
    NTEST_ASSERT_EQ_INT(nph_log_init(&config), NPH_RESULT_OK);
    f->initialized = true;
    NTEST_ASSERT_EQ_INT(nph_log_add_sink(keep_last_error, f, NPH_LOG_LEVEL_ERROR), NPH_RESULT_OK);
}

NTEST_TEARDOWN(LogCaptureFixture)
{
    (void)nph_log_remove_sink(keep_last_error, f);
    if (f->initialized) {
        nph_log_shutdown();
    }
}

NTEST_F(LogCaptureFixture, decode_failures_log_only_printable_text)
{
    // stb_image splices an unknown critical chunk's type into its failure
    // reason; this one has an escape byte in it. The IDAT before it ends
    // the header scan, so the chunk is reached only by the decode (stb
    // does not check CRCs).
    static const u8 png[] = {
        0x89U, 'P',   'N',   'G',   0x0DU, 0x0AU, 0x1AU, 0x0AU,        // signature
        0x00U, 0x00U, 0x00U, 0x0DU, 'I',   'H',   'D',   'R',          // IHDR
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,        // 1 x 1
        0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, // RGBA8, CRC
        0x00U, 0x00U, 0x00U, 0x01U, 'I',   'D',   'A',   'T',          // one byte of IDAT
        0x78U, 0x00U, 0x00U, 0x00U, 0x00U,                             // and its CRC
        0x00U, 0x00U, 0x00U, 0x00U, 'A',   0x1BU, 'C',   'D',          // empty "A\eCD" chunk
        0x00U, 0x00U, 0x00U, 0x00U,                                    // its CRC
    };
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_decode(nph_allocator_system(), png, sizeof png, &img),
                        NPH_RESULT_PARSE_ERROR);
    (void)nph_log_flush(0U);
    NTEST_ASSERT_EQ_UINT(f->count, 1U);
    NTEST_ASSERT_EQ_STR(f->message, "image decode: A?CD PNG chunk not known");
}

NTEST_F(LogCaptureFixture, a_png_with_an_empty_first_idat_is_refused)
{
    // stb_image would do null-pointer arithmetic on it (see image.c).
    static const u8 png[] = {
        0x89U, 'P',   'N',   'G',   0x0DU, 0x0AU, 0x1AU, 0x0AU,        // signature
        0x00U, 0x00U, 0x00U, 0x0DU, 'I',   'H',   'D',   'R',          // IHDR
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,        // 1 x 1
        0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, // RGBA8, CRC
        0x00U, 0x00U, 0x00U, 0x00U, 'I',   'D',   'A',   'T',          // empty IDAT
        0x00U, 0x00U, 0x00U, 0x00U,                                    // its CRC
        0x00U, 0x00U, 0x00U, 0x00U, 'I',   'E',   'N',   'D',          // IEND
        0x00U, 0x00U, 0x00U, 0x00U,                                    // its CRC
    };
    CookImage img;
    NTEST_ASSERT_EQ_INT(cook_image_decode(nph_allocator_system(), png, sizeof png, &img),
                        NPH_RESULT_PARSE_ERROR);
    (void)nph_log_flush(0U);
    NTEST_ASSERT_EQ_UINT(f->count, 1U);
    NTEST_ASSERT_EQ_STR(f->message, "image decode: PNG whose first IDAT chunk is empty");
    // The header alone is fine: stb's header scan stops at the IDAT.
    u32 w;
    u32 h;
    u32 c;
    NTEST_ASSERT_EQ_INT(cook_image_info(png, sizeof png, &w, &h, &c), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(w, 1U);
    NTEST_ASSERT_EQ_UINT(c, 4U);
}
