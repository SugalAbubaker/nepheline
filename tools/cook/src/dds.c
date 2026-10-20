// SPDX-License-Identifier: Apache-2.0
// DDS sources (dds.h): Microsoft's DirectDraw Surface container, as the
// Direct3D 11 programming guide describes it ("DDS", "DDS_HEADER",
// "DDS_PIXELFORMAT", "DDS_HEADER_DXT10"). Scenes ship textures in it already
// block compressed (glTF's MSFT_texture_dds): the cooker reads the top
// level of a 2D texture, decodes it to texels, and from there treats it as
// any decoded image (mips, normal-map handling, and the runtime's formats
// are its own, as for a PNG).
//
// Read: BC1 to BC5 and BC7 (UNORM or SRGB: the stored values are read as
// stored, the material slot decides their meaning as for a PNG), BC6H
// unsigned (mode 11, as the encoder writes it), and 8-bit RGBA, BGRA, and
// BGRX, by a DXGI format in the DX10 header or the legacy FourCC codes and
// RGB masks. Cube maps, arrays, volumes, and other formats are refused as
// unsupported; the levels after the first are not read.
#include "dds.h"

#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>

#include <cook/bc.h>
#include <string.h>

#define DDS_MAGIC             0x20534444U // "DDS "
#define DDS_HEADER_BYTES      124U
#define DDS_PIXELFORMAT_BYTES 32U
#define DDS_DX10_BYTES        20U
// Offsets within DDS_HEADER (after the magic): the size, flags, height,
// width, pitch, depth, and level count, eleven reserved words, the pixel
// format, then the four caps words.
#define DDS_PIXELFORMAT_AT 72U
#define DDS_CAPS2_AT       108U

#define DDPF_ALPHAPIXELS 0x1U
#define DDPF_FOURCC      0x4U
#define DDPF_RGB         0x40U

#define DDSCAPS2_CUBEMAP 0x200U
#define DDSCAPS2_VOLUME  0x200000U

#define DX10_DIMENSION_TEXTURE2D 3U
#define DX10_MISC_TEXTURECUBE    0x4U

#define FOURCC(a, b, c, d) ((u32)(a) | ((u32)(b) << 8U) | ((u32)(c) << 16U) | ((u32)(d) << 24U))

// The DXGI formats read (dxgiformat.h).
enum {
    DXGI_R8G8B8A8_UNORM = 28,
    DXGI_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_BC1_UNORM = 71,
    DXGI_BC1_UNORM_SRGB = 72,
    DXGI_BC2_UNORM = 74,
    DXGI_BC2_UNORM_SRGB = 75,
    DXGI_BC3_UNORM = 77,
    DXGI_BC3_UNORM_SRGB = 78,
    DXGI_BC4_UNORM = 80,
    DXGI_BC5_UNORM = 83,
    DXGI_B8G8R8A8_UNORM = 87,
    DXGI_B8G8R8X8_UNORM = 88,
    DXGI_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_B8G8R8X8_UNORM_SRGB = 93,
    DXGI_BC6H_UF16 = 95,
    DXGI_BC7_UNORM = 98,
    DXGI_BC7_UNORM_SRGB = 99,
};

typedef enum DdsFormat {
    DDS_UNKNOWN = 0,
    DDS_BC1,
    DDS_BC2,
    DDS_BC3,
    DDS_BC4,
    DDS_BC5,
    DDS_BC6H,
    DDS_BC7,
    DDS_RGBA8,
    DDS_BGRA8,
    DDS_BGRX8,
} DdsFormat;

typedef struct DdsLayout {
    u32 width;
    u32 height;
    DdsFormat format;
    u32 channels;    // what the format holds (1..4)
    u64 data_offset; // of the top level
    u64 top_bytes;   // its size
} DdsLayout;

static u32 read_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8U) | ((u32)p[2] << 16U) | ((u32)p[3] << 24U);
}

static bool is_block_format(DdsFormat f)
{
    return f >= DDS_BC1 && f <= DDS_BC7;
}

static u32 block_bytes(DdsFormat f)
{
    return f == DDS_BC1 || f == DDS_BC4 ? 8U : 16U;
}

static DdsFormat from_dxgi(u32 dxgi)
{
    switch (dxgi) {
    case DXGI_BC1_UNORM:
    case DXGI_BC1_UNORM_SRGB:
        return DDS_BC1;
    case DXGI_BC2_UNORM:
    case DXGI_BC2_UNORM_SRGB:
        return DDS_BC2;
    case DXGI_BC3_UNORM:
    case DXGI_BC3_UNORM_SRGB:
        return DDS_BC3;
    case DXGI_BC4_UNORM:
        return DDS_BC4;
    case DXGI_BC5_UNORM:
        return DDS_BC5;
    case DXGI_BC6H_UF16:
        return DDS_BC6H;
    case DXGI_BC7_UNORM:
    case DXGI_BC7_UNORM_SRGB:
        return DDS_BC7;
    case DXGI_R8G8B8A8_UNORM:
    case DXGI_R8G8B8A8_UNORM_SRGB:
        return DDS_RGBA8;
    case DXGI_B8G8R8A8_UNORM:
    case DXGI_B8G8R8A8_UNORM_SRGB:
        return DDS_BGRA8;
    case DXGI_B8G8R8X8_UNORM:
    case DXGI_B8G8R8X8_UNORM_SRGB:
        return DDS_BGRX8;
    default:
        return DDS_UNKNOWN;
    }
}

// A legacy pixel format: a FourCC code, or 32-bit RGB masks.
static DdsFormat from_legacy(u32 flags, u32 fourcc, u32 bits, const u32 masks[4])
{
    if ((flags & DDPF_FOURCC) != 0U) {
        switch (fourcc) {
        case FOURCC('D', 'X', 'T', '1'):
            return DDS_BC1;
        case FOURCC('D', 'X', 'T', '2'):
        case FOURCC('D', 'X', 'T', '3'):
            return DDS_BC2;
        case FOURCC('D', 'X', 'T', '4'):
        case FOURCC('D', 'X', 'T', '5'):
            return DDS_BC3;
        case FOURCC('A', 'T', 'I', '1'):
        case FOURCC('B', 'C', '4', 'U'):
            return DDS_BC4;
        case FOURCC('A', 'T', 'I', '2'):
        case FOURCC('B', 'C', '5', 'U'):
            return DDS_BC5;
        default:
            return DDS_UNKNOWN;
        }
    }
    if ((flags & DDPF_RGB) == 0U || bits != 32U) {
        return DDS_UNKNOWN;
    }
    const bool alpha = (flags & DDPF_ALPHAPIXELS) != 0U;
    if (masks[0] == 0x000000FFU && masks[1] == 0x0000FF00U && masks[2] == 0x00FF0000U) {
        return alpha && masks[3] == 0xFF000000U ? DDS_RGBA8 : DDS_UNKNOWN;
    }
    if (masks[0] == 0x00FF0000U && masks[1] == 0x0000FF00U && masks[2] == 0x000000FFU) {
        return alpha && masks[3] == 0xFF000000U ? DDS_BGRA8 : DDS_BGRX8;
    }
    return DDS_UNKNOWN;
}

static u32 format_channels(DdsFormat f)
{
    switch (f) {
    case DDS_BC4:
        return 1U;
    case DDS_BC5:
        return 2U;
    case DDS_BC6H:
    case DDS_BGRX8:
        return 3U;
    default:
        return 4U;
    }
}

static NphResult parse(const void *data, u64 size, DdsLayout *out)
{
    memset(out, 0, sizeof *out);
    const u8 *p = (const u8 *)data;
    if (data == NULL || size < 4U + DDS_HEADER_BYTES || read_u32(p) != DDS_MAGIC) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const u8 *h = p + 4;
    if (read_u32(h) != DDS_HEADER_BYTES ||
        read_u32(h + DDS_PIXELFORMAT_AT) != DDS_PIXELFORMAT_BYTES) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const u32 height = read_u32(h + 8);
    const u32 width = read_u32(h + 12);
    const u32 pf_flags = read_u32(h + DDS_PIXELFORMAT_AT + 4);
    const u32 fourcc = read_u32(h + DDS_PIXELFORMAT_AT + 8);
    const u32 bits = read_u32(h + DDS_PIXELFORMAT_AT + 12);
    const u32 masks[4] = {
        read_u32(h + DDS_PIXELFORMAT_AT + 16), read_u32(h + DDS_PIXELFORMAT_AT + 20),
        read_u32(h + DDS_PIXELFORMAT_AT + 24), read_u32(h + DDS_PIXELFORMAT_AT + 28)};
    const u32 caps2 = read_u32(h + DDS_CAPS2_AT);
    if (width == 0U || height == 0U) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u64 offset = 4U + DDS_HEADER_BYTES;
    DdsFormat format;
    if ((pf_flags & DDPF_FOURCC) != 0U && fourcc == FOURCC('D', 'X', '1', '0')) {
        if (size < offset + DDS_DX10_BYTES) {
            return NPH_RESULT_PARSE_ERROR;
        }
        const u8 *x = p + offset;
        const u32 dimension = read_u32(x + 4);
        const u32 misc = read_u32(x + 8);
        const u32 array_size = read_u32(x + 12);
        if (dimension != DX10_DIMENSION_TEXTURE2D || (misc & DX10_MISC_TEXTURECUBE) != 0U ||
            array_size != 1U) {
            return NPH_RESULT_UNSUPPORTED;
        }
        format = from_dxgi(read_u32(x));
        offset += DDS_DX10_BYTES;
    } else {
        format = from_legacy(pf_flags, fourcc, bits, masks);
    }
    if ((caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME)) != 0U || format == DDS_UNKNOWN) {
        return NPH_RESULT_UNSUPPORTED;
    }
    if (width > COOK_IMAGE_MAX_EXTENT || height > COOK_IMAGE_MAX_EXTENT) {
        return NPH_RESULT_UNSUPPORTED;
    }
    out->width = width;
    out->height = height;
    out->format = format;
    out->channels = format_channels(format);
    out->data_offset = offset;
    out->top_bytes = is_block_format(format) ? (u64)((width + 3U) / 4U) *
                                                   (u64)((height + 3U) / 4U) * block_bytes(format)
                                             : (u64)width * (u64)height * 4U;
    return NPH_RESULT_OK;
}

NphResult cook_dds_info(const void *data, u64 size, u32 *out_width, u32 *out_height,
                        u32 *out_channels)
{
    NPH_ASSERT(out_width != NULL && out_height != NULL && out_channels != NULL);
    DdsLayout l;
    NPH_TRY(parse(data, size, &l));
    *out_width = l.width;
    *out_height = l.height;
    *out_channels = l.channels;
    return NPH_RESULT_OK;
}

bool cook_dds_is_hdr(const void *data, u64 size)
{
    DdsLayout l;
    return parse(data, size, &l) == NPH_RESULT_OK && l.format == DDS_BC6H;
}

// Decodes block `src` of format `f` to 16 RGBA8 texels.
static bool decode_block(DdsFormat f, const u8 *src, CookBlockRgba8 *out)
{
    switch (f) {
    case DDS_BC1:
        cook_bc1_decode(src, true, out);
        return true;
    case DDS_BC2: {
        cook_bc1_decode(src + 8, false, out);
        for (u32 i = 0; i < 16U; ++i) {
            const u32 a = (src[i / 2U] >> (4U * (i & 1U))) & 15U;
            out->texels[i][3] = (u8)(a * 17U);
        }
        return true;
    }
    case DDS_BC3: {
        u8 alpha[16];
        cook_bc4_decode(src, alpha);
        cook_bc1_decode(src + 8, false, out);
        for (u32 i = 0; i < 16U; ++i) {
            out->texels[i][3] = alpha[i];
        }
        return true;
    }
    case DDS_BC4: {
        u8 r[16];
        cook_bc4_decode(src, r);
        for (u32 i = 0; i < 16U; ++i) {
            out->texels[i][0] = r[i];
            out->texels[i][1] = r[i];
            out->texels[i][2] = r[i];
            out->texels[i][3] = 255U;
        }
        return true;
    }
    case DDS_BC5: {
        u8 r[16];
        u8 g[16];
        cook_bc5_decode(src, r, g);
        for (u32 i = 0; i < 16U; ++i) {
            out->texels[i][0] = r[i];
            out->texels[i][1] = g[i];
            out->texels[i][2] = 0U;
            out->texels[i][3] = 255U;
        }
        return true;
    }
    case DDS_BC7:
        return cook_bc7_decode(src, out);
    default:
        return false;
    }
}

static NphResult decode_blocks(const DdsLayout *l, const u8 *src, CookImage *img)
{
    const u32 blocks_x = (l->width + 3U) / 4U;
    const u32 blocks_y = (l->height + 3U) / 4U;
    const u32 stride = block_bytes(l->format);
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            const u8 *block = src + ((u64)by * blocks_x + bx) * stride;
            const u32 w = nph_min_u32(4U, l->width - bx * 4U);
            const u32 h = nph_min_u32(4U, l->height - by * 4U);
            if (l->format == DDS_BC6H) {
                CookBlockRgbHalf t;
                if (!cook_bc6h_decode(block, &t)) {
                    NPH_LOG_ERROR(&NPH_LOG_COOK,
                                  "dds: a BC6H block in a mode other than 11 (block %u, %u)", bx,
                                  by);
                    return NPH_RESULT_UNSUPPORTED;
                }
                f32 *px = (f32 *)img->pixels;
                for (u32 y = 0; y < h; ++y) {
                    for (u32 x = 0; x < w; ++x) {
                        f32 *d = px + (((u64)by * 4U + y) * l->width + (u64)bx * 4U + x) * 4U;
                        const u16 *s = t.texels[y * 4U + x];
                        d[0] = nph_f16_to_f32(s[0]);
                        d[1] = nph_f16_to_f32(s[1]);
                        d[2] = nph_f16_to_f32(s[2]);
                        d[3] = 1.0F;
                    }
                }
                continue;
            }
            CookBlockRgba8 t;
            if (!decode_block(l->format, block, &t)) {
                // BC7's reserved mode: what GPUs decode it to, transparent
                // black, is what the specification asks for; but a source
                // holding it is broken, so say so.
                NPH_LOG_ERROR(&NPH_LOG_COOK, "dds: an invalid BC7 block (block %u, %u)", bx, by);
                return NPH_RESULT_PARSE_ERROR;
            }
            u8 *px = (u8 *)img->pixels;
            for (u32 y = 0; y < h; ++y) {
                memcpy(px + (((u64)by * 4U + y) * l->width + (u64)bx * 4U) * 4U,
                       t.texels[(u64)y * 4U], (size_t)w * 4U);
            }
        }
    }
    return NPH_RESULT_OK;
}

static void copy_texels(const DdsLayout *l, const u8 *src, CookImage *img)
{
    u8 *px = (u8 *)img->pixels;
    const u64 count = (u64)l->width * l->height;
    for (u64 i = 0; i < count; ++i) {
        const u8 *s = src + i * 4U;
        u8 *d = px + i * 4U;
        if (l->format == DDS_RGBA8) {
            memcpy(d, s, 4U);
        } else {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = l->format == DDS_BGRX8 ? 255U : s[3];
        }
    }
}

NphResult cook_dds_decode(NphAllocator *alloc, const void *data, u64 size, CookImage *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    DdsLayout l;
    NPH_TRY(parse(data, size, &l));
    if (size - l.data_offset < l.top_bytes) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "dds: %llu bytes after the headers, %llu needed",
                      (unsigned long long)(size - l.data_offset), (unsigned long long)l.top_bytes);
        return NPH_RESULT_PARSE_ERROR;
    }
    const CookImageFormat format = l.format == DDS_BC6H ? COOK_IMAGE_RGBA32F : COOK_IMAGE_RGBA8;
    NPH_TRY(cook_image_create(alloc, l.width, l.height, format, out));
    out->source_channels = l.channels;
    const u8 *src = (const u8 *)data + l.data_offset;
    if (!is_block_format(l.format)) {
        copy_texels(&l, src, out);
        return NPH_RESULT_OK;
    }
    const NphResult r = decode_blocks(&l, src, out);
    if (r != NPH_RESULT_OK) {
        cook_image_free(out);
    }
    return r;
}
