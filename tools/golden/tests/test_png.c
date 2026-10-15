// SPDX-License-Identifier: Apache-2.0
// The PNG codec and inflate (golden/png.h) against vectors made by Python's
// zlib (png_vectors.h): every deflate block type, every PNG filter type,
// greyscale and alpha layouts, split IDAT chunks, ancillary chunks, and
// the refusals: corrupted CRCs, truncation, bad checksums, and the
// unsupported variants.
#include <nepheline/platform/file.h>

#include <ntest/ntest.h>

#include "png_vectors.h"

#include <golden/png.h>
#include <stdio.h>
#include <string.h>

static u8 payload_byte(u32 i)
{
    // The payload of make_png_vectors.py: "Nepheline golden images: " x 6,
    // bytes 0..255, "zzzzzzzzzzzzzzzzzzzz" x 3.
    static const char phrase[] = "Nepheline golden images: ";
    const u32 p = (u32)sizeof phrase - 1U;
    if (i < 6U * p) {
        return (u8)phrase[i % p];
    }
    i -= 6U * p;
    if (i < 256U) {
        return (u8)i;
    }
    return (u8)'z';
}

NTEST(png, crc32_check_value)
{
    // The check value of CRC-32/ISO-HDLC.
    NTEST_EXPECT_EQ_UINT(golden_crc32(0U, "123456789", 9U), 0xCBF43926U);
    NTEST_EXPECT_EQ_UINT(golden_crc32(0U, "", 0U), 0U);
    // Incremental equals whole.
    NTEST_EXPECT_EQ_UINT(golden_crc32(golden_crc32(0U, "1234", 4U), "56789", 5U), 0xCBF43926U);
}

static void check_payload(const u8 *stream, u64 size)
{
    u8 out[ZLIB_PAYLOAD_SIZE];
    u64 written = 0U;
    NTEST_ASSERT_EQ_INT(golden_zlib_decompress(stream, size, out, sizeof out, &written),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(written, ZLIB_PAYLOAD_SIZE);
    for (u32 i = 0; i < ZLIB_PAYLOAD_SIZE; ++i) {
        NTEST_ASSERT_EQ_UINT(out[i], payload_byte(i));
    }
}

NTEST(png, inflate_every_block_type)
{
    check_payload(g_zlib_stored, sizeof g_zlib_stored);
    check_payload(g_zlib_fixed, sizeof g_zlib_fixed);
    static u8 skewed[ZLIB_SKEWED_SIZE];
    u64 written = 0U;
    NTEST_ASSERT_EQ_INT(golden_zlib_decompress(g_zlib_dynamic, sizeof g_zlib_dynamic, skewed,
                                               sizeof skewed, &written),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(written, ZLIB_SKEWED_SIZE);
    NTEST_EXPECT_EQ_UINT(golden_crc32(0U, skewed, sizeof skewed), ZLIB_SKEWED_CRC);
}

NTEST(png, inflate_refuses_bad_input)
{
    u8 out[ZLIB_PAYLOAD_SIZE];
    u64 written = 0U;
    // Too small an output buffer.
    NTEST_EXPECT_EQ_INT(golden_zlib_decompress(g_zlib_fixed, sizeof g_zlib_fixed, out,
                                               ZLIB_PAYLOAD_SIZE - 1U, &written),
                        NPH_RESULT_CORRUPT);
    // Every truncation.
    for (u64 n = 0; n < sizeof g_zlib_fixed; n += 7U) {
        NTEST_EXPECT_EQ_INT(golden_zlib_decompress(g_zlib_fixed, n, out, sizeof out, &written),
                            NPH_RESULT_CORRUPT);
    }
    // A wrong Adler-32, a wrong header check, a preset dictionary.
    u8 copy[sizeof g_zlib_stored];
    memcpy(copy, g_zlib_stored, sizeof copy);
    copy[sizeof copy - 1U] ^= 1U;
    NTEST_EXPECT_EQ_INT(golden_zlib_decompress(copy, sizeof copy, out, sizeof out, &written),
                        NPH_RESULT_CORRUPT);
    memcpy(copy, g_zlib_stored, sizeof copy);
    copy[1] ^= 1U;
    NTEST_EXPECT_EQ_INT(golden_zlib_decompress(copy, sizeof copy, out, sizeof out, &written),
                        NPH_RESULT_CORRUPT);
    // A stored block whose length check fails.
    memcpy(copy, g_zlib_stored, sizeof copy);
    copy[5] ^= 0xFFU;
    NTEST_EXPECT_EQ_INT(golden_zlib_decompress(copy, sizeof copy, out, sizeof out, &written),
                        NPH_RESULT_CORRUPT);
    // Block type 3 is reserved.
    u8 reserved[8] = {0x78U, 0x01U, 0x07U, 0U, 0U, 0U, 0U, 1U};
    NTEST_EXPECT_EQ_INT(
        golden_zlib_decompress(reserved, sizeof reserved, out, sizeof out, &written),
        NPH_RESULT_CORRUPT);
}

NTEST(png, decodes_every_filter_type)
{
    GoldenImage img;
    NTEST_ASSERT_EQ_INT(golden_png_decode(g_png_rgba_filters, sizeof g_png_rgba_filters,
                                          nph_allocator_system(), &img),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(img.width, 5U);
    NTEST_ASSERT_EQ_UINT(img.height, 5U);
    for (u32 y = 0; y < 5U; ++y) {
        for (u32 x = 0; x < 5U; ++x) {
            const u8 *p = img.rgba + ((u64)y * 5U + x) * 4U;
            NTEST_EXPECT_EQ_UINT(p[0], (x * 53U + y * 17U) & 0xFFU);
            NTEST_EXPECT_EQ_UINT(p[1], (x * 7U + y * 91U) & 0xFFU);
            NTEST_EXPECT_EQ_UINT(p[2], (x * y * 29U) & 0xFFU);
            NTEST_EXPECT_EQ_UINT(p[3], 255U - x * 10U);
        }
    }
    golden_image_free(&img);
    golden_image_free(&img); // idempotent
}

NTEST(png, decodes_greyscale_layouts)
{
    GoldenImage img;
    NTEST_ASSERT_EQ_INT(
        golden_png_decode(g_png_grey_alpha, sizeof g_png_grey_alpha, nph_allocator_system(), &img),
        NPH_RESULT_OK);
    for (u32 y = 0; y < 5U; ++y) {
        for (u32 x = 0; x < 3U; ++x) {
            const u8 *p = img.rgba + ((u64)y * 3U + x) * 4U;
            const u32 g = (x * 40U + y * 9U) & 0xFFU;
            NTEST_EXPECT_TRUE(p[0] == g && p[1] == g && p[2] == g);
            NTEST_EXPECT_EQ_UINT(p[3], 200U - y * 30U);
        }
    }
    golden_image_free(&img);
    NTEST_ASSERT_EQ_INT(
        golden_png_decode(g_png_grey, sizeof g_png_grey, nph_allocator_system(), &img),
        NPH_RESULT_OK);
    const u64 texel = ((u64)4U * 3U + 2U) * 4U; // (2, 4)
    NTEST_EXPECT_EQ_UINT(img.rgba[texel], (2U * 40U + 4U * 9U) & 0xFFU);
    NTEST_EXPECT_EQ_UINT(img.rgba[texel + 3U], 255U);
    golden_image_free(&img);
}

NTEST(png, refuses_what_it_does_not_support_or_cannot_trust)
{
    NphAllocator *a = nph_allocator_system();
    GoldenImage img;
    NTEST_EXPECT_EQ_INT(golden_png_decode(g_png_16bit, sizeof g_png_16bit, a, &img),
                        NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_EQ_INT(golden_png_decode(g_png_interlaced, sizeof g_png_interlaced, a, &img),
                        NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_EQ_INT(golden_png_decode(NULL, 0U, a, &img), NPH_RESULT_CORRUPT);
    u8 copy[sizeof g_png_rgba_filters];
    // Any single flipped byte past the signature fails a CRC, a check, or a
    // length.
    for (u64 i = 8U; i < sizeof copy; ++i) {
        memcpy(copy, g_png_rgba_filters, sizeof copy);
        copy[i] ^= 0x10U;
        const NphResult r = golden_png_decode(copy, sizeof copy, a, &img);
        NTEST_EXPECT_TRUE(r == NPH_RESULT_CORRUPT || r == NPH_RESULT_UNSUPPORTED);
        if (r == NPH_RESULT_OK) {
            golden_image_free(&img);
        }
    }
    // Every truncation.
    for (u64 n = 0; n < sizeof g_png_rgba_filters; ++n) {
        NTEST_EXPECT_NE_INT(golden_png_decode(g_png_rgba_filters, n, a, &img), NPH_RESULT_OK);
    }
}

NTEST(png, encode_round_trips)
{
    NphAllocator *a = nph_allocator_system();
    // Larger than one stored block (65535 bytes) to cover the block split.
    const u32 w = 181U;
    const u32 h = 127U;
    u8 *rgba = NPH_ALLOC_ARRAY(a, u8, (u64)w * h * 4U);
    NTEST_ASSERT_NOT_NULL(rgba);
    for (u64 i = 0; i < (u64)w * h * 4U; ++i) {
        rgba[i] = (u8)((i * 2654435761ULL) >> 13U);
    }
    for (u32 channels = 3U; channels <= 4U; ++channels) {
        u8 *file = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(golden_png_encode(rgba, w, h, channels, a, &file, &size),
                            NPH_RESULT_OK);
        GoldenImage img;
        NTEST_ASSERT_EQ_INT(golden_png_decode(file, size, a, &img), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(img.width, w);
        NTEST_ASSERT_EQ_UINT(img.height, h);
        for (u64 i = 0; i < (u64)w * h; ++i) {
            NTEST_ASSERT_EQ_MEM(img.rgba + i * 4U, rgba + i * 4U, 3U);
            NTEST_ASSERT_EQ_UINT(img.rgba[i * 4U + 3U], channels == 4U ? rgba[i * 4U + 3U] : 255U);
        }
        golden_image_free(&img);
        NPH_FREE_ARRAY(a, u8, file, size);
    }
    u8 *file = NULL;
    u64 size = 0U;
    NTEST_EXPECT_EQ_INT(golden_png_encode(rgba, w, h, 2U, a, &file, &size),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(golden_png_encode(rgba, 0U, h, 3U, a, &file, &size),
                        NPH_RESULT_INVALID_ARGUMENT);
    NPH_FREE_ARRAY(a, u8, rgba, (u64)w * h * 4U);
}

// The fuzz seeds (scripts/fuzz_seeds.py) describe PNG files independently
// of this codec; the reader must accept every PNG-mode seed (first byte
// even) and inflate every zlib-mode one.
NTEST(png, fuzz_seeds_are_valid)
{
    static const char *const seeds[5] = {"rgba_filters", "rgb_stored", "grey_alpha", "zlib_dynamic",
                                         "zlib_stored"};
    NphAllocator *a = nph_allocator_system();
    for (u32 i = 0; i < 5U; ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/png/%s", NPH_FUZZ_CORPUS_DIR, seeds[i]);
        void *data = NULL;
        u64 size = 0U;
        NTEST_ASSERT_EQ_INT(nph_file_read_all(path, a, &data, &size), NPH_RESULT_OK);
        const u8 *bytes = (const u8 *)data;
        NTEST_ASSERT_GE_UINT(size, 2U);
        if ((bytes[0] & 1U) == 0U) {
            GoldenImage img;
            NTEST_EXPECT_EQ_INT(golden_png_decode(bytes + 1, size - 1U, a, &img), NPH_RESULT_OK);
            golden_image_free(&img);
        } else {
            static u8 out[256U * 256U];
            u64 written = 0U;
            NTEST_EXPECT_EQ_INT(golden_zlib_decompress(bytes + 2, size - 2U, out,
                                                       ((u64)bytes[1] + 1U) * 256U, &written),
                                NPH_RESULT_OK);
        }
        nph_file_free_all(a, data, size);
    }
}

NTEST(png, files)
{
    const u8 texels[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 9, 9, 9, 255};
    char path[512];
    snprintf(path, sizeof path, "%s/golden_png_test.png", NPH_TEST_TEMP_DIR);
    NTEST_ASSERT_EQ_INT(golden_png_write(path, texels, 2U, 2U, 3U), NPH_RESULT_OK);
    GoldenImage img;
    NTEST_ASSERT_EQ_INT(golden_png_read(path, nph_allocator_system(), &img), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_MEM(img.rgba, texels, sizeof texels);
    golden_image_free(&img);
    NPH_IGNORE_RESULT(nph_fs_remove_file(path));
    NTEST_EXPECT_NE_INT(golden_png_read(path, nph_allocator_system(), &img), NPH_RESULT_OK);
}
