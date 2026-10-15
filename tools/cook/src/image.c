// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>

#include <cook/image.h>
#include <stb_image.h>
#include <string.h>

NphResult cook_image_create(NphAllocator *alloc, u32 width, u32 height, CookImageFormat format,
                            CookImage *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (width == 0U || height == 0U || width > COOK_IMAGE_MAX_EXTENT ||
        height > COOK_IMAGE_MAX_EXTENT) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 size = (u64)width * height * cook_image_bytes_per_texel(format);
    void *pixels = NPH_ALLOC_ZEROED(alloc, size, 16U);
    if (pixels == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    out->alloc = alloc;
    out->width = width;
    out->height = height;
    out->format = format;
    out->source_channels = 4U;
    out->pixels = pixels;
    out->size = size;
    return NPH_RESULT_OK;
}

void cook_image_free(CookImage *img)
{
    if (img == NULL || img->pixels == NULL) {
        return;
    }
    NPH_FREE(img->alloc, img->pixels, img->size, 16U);
    memset(img, 0, sizeof *img);
}

bool cook_image_data_is_hdr(const void *data, u64 size)
{
    if (data == NULL || size == 0U || size > (u64)INT32_MAX) {
        return false;
    }
    return stbi_is_hdr_from_memory((const stbi_uc *)data, (int)size) != 0;
}

// stb_image's failure reasons can carry bytes of the input (an unknown
// PNG chunk's type is spliced into the message), so they are logged with
// everything outside printable ASCII replaced.
static void log_decode_failure(void)
{
    const char *reason = stbi_failure_reason();
    char text[64];
    u32 n = 0U;
    for (; reason != NULL && reason[n] != '\0' && n + 1U < sizeof text; ++n) {
        const char c = reason[n];
        text[n] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    text[n] = '\0';
    NPH_LOG_ERROR(&NPH_LOG_COOK, "image decode: %s", text);
}

// stb_image (v2.30) copies a PNG's first IDAT chunk into a buffer it
// allocates only for a non-empty chunk, so an empty first IDAT makes it
// compute NULL + 0 and call memcpy(NULL, p, 0), both undefined behaviour
// (UBSan reports them). Encoders never write one, so such files are
// refused before stb sees them. The walk mirrors stb's: every chunk is a
// 4-byte big-endian length, a 4-byte type, the data, and a 4-byte CRC.
static bool png_first_idat_is_empty(const u8 *data, u64 size)
{
    static const u8 signature[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (size < sizeof signature || memcmp(data, signature, sizeof signature) != 0) {
        return false;
    }
    u64 at = sizeof signature;
    while (size - at >= 8U) {
        const u32 length = ((u32)data[at] << 24U) | ((u32)data[at + 1U] << 16U) |
                           ((u32)data[at + 2U] << 8U) | (u32)data[at + 3U];
        if (memcmp(data + at + 4U, "IDAT", 4U) == 0) {
            return length == 0U;
        }
        const u64 chunk = (u64)length + 12U;
        if (chunk > size - at) {
            return false;
        }
        at += chunk;
    }
    return false;
}

NphResult cook_image_info(const void *data, u64 size, u32 *out_width, u32 *out_height,
                          u32 *out_channels)
{
    NPH_ASSERT(out_width != NULL && out_height != NULL && out_channels != NULL);
    *out_width = 0U;
    *out_height = 0U;
    *out_channels = 0U;
    if (data == NULL || size == 0U || size > (u64)INT32_MAX) {
        return NPH_RESULT_PARSE_ERROR;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    if (!stbi_info_from_memory((const stbi_uc *)data, (int)size, &w, &h, &channels) || w <= 0 ||
        h <= 0 || channels <= 0) {
        return NPH_RESULT_PARSE_ERROR;
    }
    *out_width = (u32)w;
    *out_height = (u32)h;
    *out_channels = (u32)channels;
    return NPH_RESULT_OK;
}

NphResult cook_image_decode(NphAllocator *alloc, const void *data, u64 size, CookImage *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (data == NULL || size == 0U || size > (u64)INT32_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (png_first_idat_is_empty((const u8 *)data, size)) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "image decode: PNG whose first IDAT chunk is empty");
        return NPH_RESULT_PARSE_ERROR;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    if (!stbi_info_from_memory((const stbi_uc *)data, (int)size, &w, &h, &channels)) {
        log_decode_failure();
        return NPH_RESULT_PARSE_ERROR;
    }
    if (w <= 0 || h <= 0 || w > (int)COOK_IMAGE_MAX_EXTENT || h > (int)COOK_IMAGE_MAX_EXTENT) {
        return NPH_RESULT_UNSUPPORTED;
    }
    const bool hdr = stbi_is_hdr_from_memory((const stbi_uc *)data, (int)size) != 0;
    const CookImageFormat format = hdr ? COOK_IMAGE_RGBA32F : COOK_IMAGE_RGBA8;
    NPH_TRY(cook_image_create(alloc, (u32)w, (u32)h, format, out));
    out->source_channels = (u32)channels;
    void *decoded;
    if (hdr) {
        decoded = stbi_loadf_from_memory((const stbi_uc *)data, (int)size, &w, &h, &channels, 4);
    } else {
        decoded = stbi_load_from_memory((const stbi_uc *)data, (int)size, &w, &h, &channels, 4);
    }
    if (decoded == NULL || (u32)w != out->width || (u32)h != out->height) {
        log_decode_failure();
        if (decoded != NULL) {
            stbi_image_free(decoded);
        }
        cook_image_free(out);
        return NPH_RESULT_PARSE_ERROR;
    }
    memcpy(out->pixels, decoded, (size_t)out->size);
    stbi_image_free(decoded);
    return NPH_RESULT_OK;
}

NphResult cook_image_load(NphAllocator *alloc, const char *path, CookImage *out)
{
    NPH_ASSERT(path != NULL);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, alloc, &data, &size));
    const NphResult r = cook_image_decode(alloc, data, size, out);
    nph_file_free_all(alloc, data, size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: not a supported image (%s)", path, nph_result_string(r));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Transfer functions and conversions
// ---------------------------------------------------------------------------

f32 cook_srgb_to_linear(f32 encoded)
{
    const f32 c = nph_f32_saturate(encoded);
    if (c <= 0.04045F) {
        return c / 12.92F;
    }
    return nph_f32_pow((c + 0.055F) / 1.055F, 2.4F);
}

f32 cook_linear_to_srgb(f32 linear)
{
    const f32 c = nph_f32_saturate(linear);
    if (c <= 0.0031308F) {
        return c * 12.92F;
    }
    return 1.055F * nph_f32_pow(c, 1.0F / 2.4F) - 0.055F;
}

NphResult cook_image_to_f32(NphAllocator *alloc, const CookImage *src, bool srgb, CookImage *out)
{
    NPH_ASSERT(src != NULL && out != NULL);
    NPH_TRY(cook_image_create(alloc, src->width, src->height, COOK_IMAGE_RGBA32F, out));
    out->source_channels = src->source_channels;
    f32 *dst = (f32 *)out->pixels;
    const u64 texels = (u64)src->width * src->height;
    if (src->format == COOK_IMAGE_RGBA32F) {
        memcpy(dst, src->pixels, (size_t)src->size);
        return NPH_RESULT_OK;
    }
    // One table per transfer function: 256 entries beat a pow per texel.
    f32 table[256];
    for (u32 i = 0; i < 256U; ++i) {
        const f32 v = (f32)i / 255.0F;
        table[i] = srgb ? cook_srgb_to_linear(v) : v;
    }
    const u8 *s = (const u8 *)src->pixels;
    for (u64 i = 0; i < texels; ++i) {
        dst[i * 4U] = table[s[i * 4U]];
        dst[i * 4U + 1U] = table[s[i * 4U + 1U]];
        dst[i * 4U + 2U] = table[s[i * 4U + 2U]];
        dst[i * 4U + 3U] = (f32)s[i * 4U + 3U] / 255.0F;
    }
    return NPH_RESULT_OK;
}

static u8 encode_unorm8(f32 v)
{
    return (u8)(nph_f32_saturate(v) * 255.0F + 0.5F);
}

NphResult cook_image_to_rgba8(NphAllocator *alloc, const CookImage *src, bool srgb, CookImage *out)
{
    NPH_ASSERT(src != NULL && out != NULL);
    NPH_TRY(cook_image_create(alloc, src->width, src->height, COOK_IMAGE_RGBA8, out));
    out->source_channels = src->source_channels;
    u8 *dst = (u8 *)out->pixels;
    const u64 texels = (u64)src->width * src->height;
    if (src->format == COOK_IMAGE_RGBA8) {
        memcpy(dst, src->pixels, (size_t)src->size);
        return NPH_RESULT_OK;
    }
    const f32 *s = (const f32 *)src->pixels;
    for (u64 i = 0; i < texels; ++i) {
        for (u32 c = 0; c < 3U; ++c) {
            const f32 v = s[i * 4U + c];
            dst[i * 4U + c] = encode_unorm8(srgb ? cook_linear_to_srgb(v) : v);
        }
        dst[i * 4U + 3U] = encode_unorm8(s[i * 4U + 3U]);
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

// The source texels a destination texel covers along one axis, with the
// fraction of each it covers. Destination texel i of n covers
// [i * src / n, (i + 1) * src / n) in source texels; measured in units of
// 1 / n texel those ends are the integers i * src and (i + 1) * src, so the
// overlaps (and the weights, overlap / src) are exact rationals. Halving
// (n = max(src / 2, 1)) spans at most 3 source texels, so 4 taps suffice.
typedef struct AreaTaps {
    u32 first;
    u32 count;
    f32 weight[4];
} AreaTaps;

static void area_taps(u32 src, u32 n, u32 i, AreaTaps *t)
{
    const u64 a = (u64)i * src;
    const u64 b = (u64)(i + 1U) * src;
    t->first = (u32)(a / n);
    t->count = (u32)((b - 1U) / n) - t->first + 1U;
    NPH_ASSERT(t->count <= 4U);
    for (u32 k = 0; k < t->count; ++k) {
        const u64 lo = nph_max_u64(a, (u64)(t->first + k) * n);
        const u64 hi = nph_min_u64(b, (u64)(t->first + k + 1U) * n);
        t->weight[k] = (f32)((f64)(hi - lo) / (f64)src);
    }
}

NphResult cook_image_downsample(NphAllocator *alloc, const CookImage *src, CookImage *out)
{
    NPH_ASSERT(src != NULL && out != NULL);
    if (src->format != COOK_IMAGE_RGBA32F || (src->width == 1U && src->height == 1U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 ow = nph_max_u32(src->width / 2U, 1U);
    const u32 oh = nph_max_u32(src->height / 2U, 1U);
    AreaTaps *columns = NPH_ALLOC_ARRAY(alloc, AreaTaps, ow);
    if (columns == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const NphResult r = cook_image_create(alloc, ow, oh, COOK_IMAGE_RGBA32F, out);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(alloc, AreaTaps, columns, ow);
        return r;
    }
    out->source_channels = src->source_channels;
    for (u32 x = 0; x < ow; ++x) {
        area_taps(src->width, ow, x, &columns[x]);
    }
    const f32 *s = (const f32 *)src->pixels;
    f32 *d = (f32 *)out->pixels;
    for (u32 y = 0; y < oh; ++y) {
        AreaTaps rows;
        area_taps(src->height, oh, y, &rows);
        for (u32 x = 0; x < ow; ++x) {
            const AreaTaps *cols = &columns[x];
            f32 acc[4] = {0.0F, 0.0F, 0.0F, 0.0F};
            for (u32 j = 0; j < rows.count; ++j) {
                const f32 *row = s + (u64)(rows.first + j) * src->width * 4U;
                for (u32 i = 0; i < cols->count; ++i) {
                    const f32 w = rows.weight[j] * cols->weight[i];
                    const f32 *p = row + (u64)(cols->first + i) * 4U;
                    acc[0] += p[0] * w;
                    acc[1] += p[1] * w;
                    acc[2] += p[2] * w;
                    acc[3] += p[3] * w;
                }
            }
            f32 *q = d + ((u64)y * ow + x) * 4U;
            q[0] = acc[0];
            q[1] = acc[1];
            q[2] = acc[2];
            q[3] = acc[3];
        }
    }
    NPH_FREE_ARRAY(alloc, AreaTaps, columns, ow);
    return NPH_RESULT_OK;
}

NphResult cook_image_limit_extent(NphAllocator *alloc, CookImage *img, u32 max_extent)
{
    NPH_ASSERT(img != NULL);
    if (max_extent == 0U || img->format != COOK_IMAGE_RGBA32F) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    while (img->width > max_extent || img->height > max_extent) {
        CookImage smaller;
        NPH_TRY(cook_image_downsample(alloc, img, &smaller));
        cook_image_free(img);
        *img = smaller;
    }
    return NPH_RESULT_OK;
}

void cook_image_renormalize_normals(CookImage *img)
{
    NPH_ASSERT(img != NULL && img->format == COOK_IMAGE_RGBA32F);
    f32 *p = (f32 *)img->pixels;
    const u64 texels = (u64)img->width * img->height;
    for (u64 i = 0; i < texels; ++i) {
        f32 *t = p + i * 4U;
        NphVec3 n = nph_vec3(t[0] * 2.0F - 1.0F, t[1] * 2.0F - 1.0F, t[2] * 2.0F - 1.0F);
        n = nph_vec3_normalize_or(n, nph_vec3_unit_z());
        t[0] = n.x * 0.5F + 0.5F;
        t[1] = n.y * 0.5F + 0.5F;
        t[2] = n.z * 0.5F + 0.5F;
    }
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------

void cook_image_block_rgba8(const CookImage *img, u32 x, u32 y, CookBlockRgba8 *out)
{
    NPH_ASSERT(img != NULL && img->format == COOK_IMAGE_RGBA8);
    const u8 *p = (const u8 *)img->pixels;
    for (u32 j = 0; j < 4U; ++j) {
        const u32 sy = nph_min_u32(y + j, img->height - 1U);
        for (u32 i = 0; i < 4U; ++i) {
            const u32 sx = nph_min_u32(x + i, img->width - 1U);
            memcpy(out->texels[j * 4U + i], p + ((u64)sy * img->width + sx) * 4U, 4U);
        }
    }
}

void cook_image_block_half(const CookImage *img, u32 x, u32 y, CookBlockRgbHalf *out)
{
    NPH_ASSERT(img != NULL && img->format == COOK_IMAGE_RGBA32F);
    const f32 *p = (const f32 *)img->pixels;
    for (u32 j = 0; j < 4U; ++j) {
        const u32 sy = nph_min_u32(y + j, img->height - 1U);
        for (u32 i = 0; i < 4U; ++i) {
            const u32 sx = nph_min_u32(x + i, img->width - 1U);
            const f32 *t = p + ((u64)sy * img->width + sx) * 4U;
            for (u32 c = 0; c < 3U; ++c) {
                // BC6H unsigned holds [0, 65504] and no negative values.
                out->texels[j * 4U + i][c] = nph_f32_to_f16(nph_f32_clamp(t[c], 0.0F, NPH_F16_MAX));
            }
        }
    }
}
