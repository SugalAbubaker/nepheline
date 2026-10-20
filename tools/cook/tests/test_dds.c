// SPDX-License-Identifier: Apache-2.0
// DDS sources (src/dds.c through cook/image.h): files built here, byte by
// byte as the container is documented, in every format the cooker reads,
// against the block decoders' own output; and the malformed and the
// unsupported, refused.
#include <nepheline/math/pack.h>

#include "test_cook_support.h"

#include <cook/bc.h>
#include <cook/image.h>
#include <string.h>

#define DDPF_ALPHAPIXELS 0x1U
#define DDPF_FOURCC      0x4U
#define DDPF_RGB         0x40U

#define FOURCC(a, b, c, d) ((u32)(a) | ((u32)(b) << 8U) | ((u32)(c) << 16U) | ((u32)(d) << 24U))

// Room for the headers and a 16 x 16 RGBA8 level.
#define DDS_MAX_BYTES (4U + 124U + 20U + 16U * 16U * 4U + 64U)

typedef struct TestDds {
    u8 bytes[DDS_MAX_BYTES];
    u64 size;
} TestDds;

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8U);
    p[2] = (u8)(v >> 16U);
    p[3] = (u8)(v >> 24U);
}

// The magic and DDS_HEADER of a `width` x `height` 2D texture with `mips`
// levels; the pixel format's flags, FourCC, bit count, and masks as given.
// Returns the offset after the header.
static u64 dds_header(TestDds *d, u32 width, u32 height, u32 mips, u32 pf_flags, u32 fourcc,
                      u32 bits, const u32 masks[4])
{
    memset(d, 0, sizeof *d);
    u8 *p = d->bytes;
    put_u32(p, FOURCC('D', 'D', 'S', ' '));
    u8 *h = p + 4;
    put_u32(h, 124U);
    put_u32(h + 4, 0x1007U | (mips > 1U ? 0x20000U : 0U)); // caps, height, width, pixel format
    put_u32(h + 8, height);
    put_u32(h + 12, width);
    put_u32(h + 24, mips);
    // Eleven reserved words, then DDS_PIXELFORMAT at 72 and the caps at 104.
    put_u32(h + 72, 32U);
    put_u32(h + 76, pf_flags);
    put_u32(h + 80, fourcc);
    put_u32(h + 84, bits);
    for (u32 i = 0; i < 4U; ++i) {
        put_u32(h + 88 + 4U * (u64)i, masks != NULL ? masks[i] : 0U);
    }
    put_u32(h + 104, 0x1000U); // DDSCAPS_TEXTURE
    return 4U + 124U;
}

// A DX10 file of `dxgi` format; returns the offset of the top level.
static u64 dds_dx10(TestDds *d, u32 width, u32 height, u32 dxgi)
{
    const u64 at =
        dds_header(d, width, height, 1U, DDPF_FOURCC, FOURCC('D', 'X', '1', '0'), 0U, NULL);
    u8 *x = d->bytes + at;
    put_u32(x, dxgi);
    put_u32(x + 4, 3U); // TEXTURE2D
    put_u32(x + 8, 0U);
    put_u32(x + 12, 1U);
    put_u32(x + 16, 0U);
    return at + 20U;
}

static void append(TestDds *d, u64 at, const void *data, u64 size)
{
    NTEST_ASSERT_LE_UINT(at + size, (u64)DDS_MAX_BYTES);
    memcpy(d->bytes + at, data, (size_t)size);
    d->size = at + size;
}

// The texels a 4 x 4 block grid gives a `width` x `height` image: block
// (bx, by) texel (x, y) lands at (4 bx + x, 4 by + y) when that is inside.
static void place(const CookBlockRgba8 *block, u32 bx, u32 by, u32 width, u32 height, u8 *rgba)
{
    for (u32 y = 0; y < 4U; ++y) {
        for (u32 x = 0; x < 4U; ++x) {
            const u32 px = bx * 4U + x;
            const u32 py = by * 4U + y;
            if (px < width && py < height) {
                memcpy(rgba + ((u64)py * width + px) * 4U, block->texels[y * 4U + x], 4U);
            }
        }
    }
}

static void decode_ok(const TestDds *d, u32 width, u32 height, u32 channels, CookImage *img)
{
    NphAllocator *alloc = nph_allocator_system();
    u32 w = 0U;
    u32 h = 0U;
    u32 c = 0U;
    NTEST_ASSERT_EQ_INT(cook_image_info(d->bytes, d->size, &w, &h, &c), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(w, width);
    NTEST_ASSERT_EQ_UINT(h, height);
    NTEST_ASSERT_EQ_UINT(c, channels);
    NTEST_ASSERT_EQ_INT(cook_image_decode(alloc, d->bytes, d->size, img), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(img->width, width);
    NTEST_ASSERT_EQ_UINT(img->height, height);
    NTEST_ASSERT_EQ_UINT(img->source_channels, channels);
}

// A 9 x 6 picture: three blocks across, two down, the last column and row
// partial, so the decode must clip blocks at the edges.
static void test_texels(u32 width, u32 height, u8 *rgba)
{
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u8 *t = rgba + ((u64)y * width + x) * 4U;
            t[0] = (u8)(x * 28U);
            t[1] = (u8)(y * 40U);
            t[2] = (u8)(255U - x * 20U);
            t[3] = (u8)(128U + y * 20U);
        }
    }
}

static CookBlockRgba8 block_of(const u8 *rgba, u32 width, u32 height, u32 bx, u32 by)
{
    CookBlockRgba8 b;
    for (u32 y = 0; y < 4U; ++y) {
        for (u32 x = 0; x < 4U; ++x) {
            const u32 px = nph_min_u32(bx * 4U + x, width - 1U);
            const u32 py = nph_min_u32(by * 4U + y, height - 1U);
            memcpy(b.texels[y * 4U + x], rgba + ((u64)py * width + px) * 4U, 4U);
        }
    }
    return b;
}

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

// BC7 (DX10, sRGB as the scenes ship it): the top level is every block
// decoded and clipped to the image; a second level after it is not read.
NTEST(dds, bc7_decodes_its_top_level)
{
    enum {
        W = 9,
        H = 6
    };
    u8 src[W * H * 4];
    test_texels(W, H, src);
    TestDds d;
    u64 at = dds_dx10(&d, W, H, 99U);
    put_u32(d.bytes + 4 + 24, 2U); // two levels
    u8 expected[W * H * 4];
    for (u32 by = 0; by < 2U; ++by) {
        for (u32 bx = 0; bx < 3U; ++bx) {
            const CookBlockRgba8 b = block_of(src, W, H, bx, by);
            u8 block[16];
            cook_bc7_encode(&b, NULL, block);
            append(&d, at, block, 16U);
            at += 16U;
            CookBlockRgba8 dec;
            NTEST_ASSERT_TRUE(cook_bc7_decode(block, &dec));
            place(&dec, bx, by, W, H, expected);
        }
    }
    // The second level (5 x 3: two blocks by one), which must not matter.
    const u8 junk[32] = {0xFFU};
    append(&d, at, junk, sizeof junk);
    CookImage img;
    decode_ok(&d, W, H, 4U, &img);
    NTEST_ASSERT_EQ_INT(img.format, COOK_IMAGE_RGBA8);
    NTEST_ASSERT_EQ_MEM(img.pixels, expected, sizeof expected);
    cook_image_free(&img);
    NTEST_ASSERT_FALSE(cook_image_data_is_hdr(d.bytes, d.size));
}

// BC1 to BC5 by the DX10 header and by their legacy FourCC codes alike.
NTEST(dds, bc1_to_bc5_decode_by_dxgi_format_and_fourcc)
{
    enum {
        W = 6,
        H = 5
    };
    u8 src[W * H * 4];
    test_texels(W, H, src);
    // Blocks per format: BC1 and BC4 8 bytes, BC2, BC3, BC5 16.
    static const struct {
        u32 bc; // 1 to 5
        u32 dxgi;
        u32 fourcc;
        u32 bytes;
        u32 channels;
    } formats[] = {
        {1U, 71U, FOURCC('D', 'X', 'T', '1'), 8U, 4U},
        {2U, 74U, FOURCC('D', 'X', 'T', '3'), 16U, 4U},
        {3U, 77U, FOURCC('D', 'X', 'T', '5'), 16U, 4U},
        {4U, 80U, FOURCC('A', 'T', 'I', '1'), 8U, 1U},
        {5U, 83U, FOURCC('A', 'T', 'I', '2'), 16U, 2U},
    };
    for (u32 f = 0; f < NPH_ARRAY_COUNT(formats); ++f) {
        u8 blocks[4][16];
        u8 expected[W * H * 4];
        for (u32 i = 0; i < 4U; ++i) {
            const u32 bx = i % 2U;
            const u32 by = i / 2U;
            const CookBlockRgba8 b = block_of(src, W, H, bx, by);
            u8 r[16];
            u8 g[16];
            u8 a[16];
            for (u32 t = 0; t < 16U; ++t) {
                r[t] = b.texels[t][0];
                g[t] = b.texels[t][1];
                a[t] = b.texels[t][3];
            }
            // Colour blocks: endpoints from the block's first and last
            // texels, indices cycling, which is all the decode needs.
            u8 color[8] = {0x1FU ^ (u8)i, 0xF8U, 0x10U, 0x42U, 0x1BU, 0xE4U, 0x39U, 0x8DU};
            CookBlockRgba8 dec;
            memset(&dec, 0, sizeof dec);
            switch (formats[f].bc) {
            case 1U:
                memcpy(blocks[i], color, 8U);
                cook_bc1_decode(color, true, &dec);
                break;
            case 2U: {
                for (u32 t = 0; t < 8U; ++t) {
                    blocks[i][t] = (u8)(a[2U * (u64)t] >> 4U) | (u8)(a[2U * t + 1U] & 0xF0U);
                }
                memcpy(blocks[i] + 8, color, 8U);
                cook_bc1_decode(color, false, &dec);
                for (u32 t = 0; t < 16U; ++t) {
                    dec.texels[t][3] = (u8)((a[t] >> 4U) * 17U);
                }
                break;
            }
            case 3U: {
                cook_bc4_encode(a, blocks[i]);
                memcpy(blocks[i] + 8, color, 8U);
                u8 da[16];
                cook_bc4_decode(blocks[i], da);
                cook_bc1_decode(color, false, &dec);
                for (u32 t = 0; t < 16U; ++t) {
                    dec.texels[t][3] = da[t];
                }
                break;
            }
            case 4U: {
                cook_bc4_encode(r, blocks[i]);
                u8 dr[16];
                cook_bc4_decode(blocks[i], dr);
                for (u32 t = 0; t < 16U; ++t) {
                    dec.texels[t][0] = dr[t];
                    dec.texels[t][1] = dr[t];
                    dec.texels[t][2] = dr[t];
                    dec.texels[t][3] = 255U;
                }
                break;
            }
            default: {
                cook_bc5_encode(r, g, blocks[i]);
                u8 dr[16];
                u8 dg[16];
                cook_bc5_decode(blocks[i], dr, dg);
                for (u32 t = 0; t < 16U; ++t) {
                    dec.texels[t][0] = dr[t];
                    dec.texels[t][1] = dg[t];
                    dec.texels[t][2] = 0U;
                    dec.texels[t][3] = 255U;
                }
                break;
            }
            }
            place(&dec, bx, by, W, H, expected);
        }
        for (u32 legacy = 0; legacy < 2U; ++legacy) {
            TestDds d;
            u64 at = legacy != 0U
                         ? dds_header(&d, W, H, 1U, DDPF_FOURCC, formats[f].fourcc, 0U, NULL)
                         : dds_dx10(&d, W, H, formats[f].dxgi);
            for (u32 i = 0; i < 4U; ++i) {
                append(&d, at, blocks[i], formats[f].bytes);
                at += formats[f].bytes;
            }
            CookImage img;
            decode_ok(&d, W, H, formats[f].channels, &img);
            NTEST_ASSERT_EQ_MEM(img.pixels, expected, sizeof expected);
            cook_image_free(&img);
        }
    }
}

// 8-bit texels: RGBA as stored, BGRA and BGRX swizzled (BGRX opaque), by
// DXGI format and by legacy masks.
NTEST(dds, uncompressed_texels_are_swizzled_to_rgba)
{
    enum {
        W = 3,
        H = 2
    };
    u8 stored[W * H * 4];
    for (u32 i = 0; i < W * H * 4U; ++i) {
        stored[i] = (u8)(i * 9U + 7U);
    }
    static const u32 rgba_masks[4] = {0xFFU, 0xFF00U, 0xFF0000U, 0xFF000000U};
    static const u32 bgra_masks[4] = {0xFF0000U, 0xFF00U, 0xFFU, 0xFF000000U};
    static const u32 bgrx_masks[4] = {0xFF0000U, 0xFF00U, 0xFFU, 0U};
    for (u32 kind = 0; kind < 6U; ++kind) {
        TestDds d;
        u64 at;
        switch (kind) {
        case 0U:
            at = dds_dx10(&d, W, H, 28U);
            break;
        case 1U:
            at = dds_dx10(&d, W, H, 87U);
            break;
        case 2U:
            at = dds_dx10(&d, W, H, 88U);
            break;
        case 3U:
            at = dds_header(&d, W, H, 1U, DDPF_RGB | DDPF_ALPHAPIXELS, 0U, 32U, rgba_masks);
            break;
        case 4U:
            at = dds_header(&d, W, H, 1U, DDPF_RGB | DDPF_ALPHAPIXELS, 0U, 32U, bgra_masks);
            break;
        default:
            at = dds_header(&d, W, H, 1U, DDPF_RGB, 0U, 32U, bgrx_masks);
            break;
        }
        append(&d, at, stored, sizeof stored);
        const bool rgba = kind == 0U || kind == 3U;
        const bool opaque = kind == 2U || kind == 5U;
        CookImage img;
        decode_ok(&d, W, H, opaque ? 3U : 4U, &img);
        const u8 *px = (const u8 *)img.pixels;
        for (u32 i = 0; i < W * H; ++i) {
            const u8 *s = stored + (u64)i * 4U;
            NTEST_ASSERT_EQ_UINT(px[i * 4U + 0U], rgba ? s[0] : s[2]);
            NTEST_ASSERT_EQ_UINT(px[i * 4U + 1U], s[1]);
            NTEST_ASSERT_EQ_UINT(px[i * 4U + 2U], rgba ? s[2] : s[0]);
            NTEST_ASSERT_EQ_UINT(px[i * 4U + 3U], opaque ? 255U : s[3]);
        }
        cook_image_free(&img);
    }
}

// BC6H decodes to float texels, which makes the file HDR to the cooker.
NTEST(dds, bc6h_decodes_to_float)
{
    CookBlockRgbHalf in;
    for (u32 t = 0; t < 16U; ++t) {
        in.texels[t][0] = nph_f32_to_f16(0.5F + (f32)t);
        in.texels[t][1] = nph_f32_to_f16(2.0F);
        in.texels[t][2] = nph_f32_to_f16(0.25F * (f32)t);
    }
    u8 block[16];
    cook_bc6h_encode(&in, block);
    CookBlockRgbHalf dec;
    NTEST_ASSERT_TRUE(cook_bc6h_decode(block, &dec));
    TestDds d;
    const u64 at = dds_dx10(&d, 4U, 4U, 95U);
    append(&d, at, block, 16U);
    NTEST_ASSERT_TRUE(cook_image_data_is_hdr(d.bytes, d.size));
    CookImage img;
    decode_ok(&d, 4U, 4U, 3U, &img);
    NTEST_ASSERT_EQ_INT(img.format, COOK_IMAGE_RGBA32F);
    const f32 *px = (const f32 *)img.pixels;
    for (u32 t = 0; t < 16U; ++t) {
        for (u32 ch = 0; ch < 3U; ++ch) {
            NTEST_ASSERT_NEAR_F32(px[t * 4U + ch], nph_f16_to_f32(dec.texels[t][ch]), 0.0F);
        }
        NTEST_ASSERT_NEAR_F32(px[t * 4U + 3U], 1.0F, 0.0F);
    }
    cook_image_free(&img);
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

static NphResult decode_result(const TestDds *d)
{
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    const NphResult r = cook_image_decode(alloc, d->bytes, d->size, &img);
    if (r == NPH_RESULT_OK) {
        cook_image_free(&img);
    }
    return r;
}

static NphResult info_result(const TestDds *d)
{
    u32 w;
    u32 h;
    u32 c;
    return cook_image_info(d->bytes, d->size, &w, &h, &c);
}

// Headers that do not describe a file, or a file shorter than its top
// level, are parse errors; nothing is read past the bytes given.
NTEST(dds, malformed_files_are_refused)
{
    const u8 block[16] = {0x40U}; // a valid BC7 mode 6 block
    TestDds good;
    const u64 at = dds_dx10(&good, 4U, 4U, 98U);
    append(&good, at, block, 16U);
    NTEST_ASSERT_EQ_INT(decode_result(&good), NPH_RESULT_OK);

    TestDds d = good;
    d.size = at + 15U; // one byte short of the top level
    NTEST_ASSERT_EQ_INT(decode_result(&d), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_OK); // the headers alone are fine
    d.size = 4U + 124U + 10U;                            // the DX10 header cut
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(decode_result(&d), NPH_RESULT_PARSE_ERROR);
    d.size = 100U; // the header cut
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_PARSE_ERROR);

    d = good;
    put_u32(d.bytes + 4, 120U); // the header's size
    NTEST_ASSERT_EQ_INT(decode_result(&d), NPH_RESULT_PARSE_ERROR);
    d = good;
    put_u32(d.bytes + 4 + 72, 28U); // the pixel format's size
    NTEST_ASSERT_EQ_INT(decode_result(&d), NPH_RESULT_PARSE_ERROR);
    d = good;
    put_u32(d.bytes + 4 + 12, 0U); // no width
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_PARSE_ERROR);

    // BC7's reserved mode (no mode bit) is no block an encoder writes.
    d = good;
    memset(d.bytes + at, 0, 16U);
    NTEST_ASSERT_EQ_INT(decode_result(&d), NPH_RESULT_PARSE_ERROR);
}

// What the cooker does not read is refused as unsupported, from the
// headers alone.
NTEST(dds, unsupported_layouts_and_formats_are_refused)
{
    TestDds good;
    const u64 at = dds_dx10(&good, 4U, 4U, 98U);
    const u8 block[16] = {0x40U};
    append(&good, at, block, 16U);
    const u64 dx10 = 4U + 124U;

    TestDds d = good;
    put_u32(d.bytes + 4 + 108, 0x200U | 0xFC00U); // a cube map's caps2
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_EQ_INT(decode_result(&d), NPH_RESULT_UNSUPPORTED);
    d = good;
    put_u32(d.bytes + dx10 + 8, 0x4U); // DX10 TEXTURECUBE
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_UNSUPPORTED);
    d = good;
    put_u32(d.bytes + dx10 + 12, 2U); // an array
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_UNSUPPORTED);
    d = good;
    put_u32(d.bytes + dx10 + 4, 4U); // a volume
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_UNSUPPORTED);
    d = good;
    put_u32(d.bytes + dx10, 2U); // R32G32B32A32_FLOAT
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_UNSUPPORTED);
    d = good;
    put_u32(d.bytes + 4 + 12, 16385U); // past COOK_IMAGE_MAX_EXTENT
    NTEST_ASSERT_EQ_INT(info_result(&d), NPH_RESULT_UNSUPPORTED);

    TestDds legacy;
    (void)dds_header(&legacy, 4U, 4U, 1U, DDPF_FOURCC, FOURCC('R', 'X', 'G', 'B'), 0U, NULL);
    legacy.size = 4U + 124U + 16U;
    NTEST_ASSERT_EQ_INT(info_result(&legacy), NPH_RESULT_UNSUPPORTED);
    static const u32 rgb565[4] = {0xF800U, 0x07E0U, 0x001FU, 0U};
    (void)dds_header(&legacy, 4U, 4U, 1U, DDPF_RGB, 0U, 16U, rgb565);
    legacy.size = 4U + 124U + 32U;
    NTEST_ASSERT_EQ_INT(info_result(&legacy), NPH_RESULT_UNSUPPORTED);
}
