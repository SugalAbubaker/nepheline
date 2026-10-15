// SPDX-License-Identifier: Apache-2.0
// PNG reading and writing (golden/png.h; ISO/IEC 15948, the PNG
// specification, second edition).
#include <nepheline/core/assert.h>
#include <nepheline/platform/file.h>

#include <golden/png.h>
#include <string.h>

static const u8 g_signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
static const u8 g_idat[4] = {'I', 'D', 'A', 'T'};

// ---------------------------------------------------------------------------
// CRC-32 (PNG §5.5; the zip polynomial, reflected)
// ---------------------------------------------------------------------------

u32 golden_crc32(u32 crc, const void *data, u64 size)
{
    const u8 *p = (const u8 *)data;
    crc = ~crc;
    for (u64 i = 0; i < size; ++i) {
        crc ^= p[i];
        for (u32 k = 0; k < 8U; ++k) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24U) | ((u32)p[1] << 16U) | ((u32)p[2] << 8U) | (u32)p[3];
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24U);
    p[1] = (u8)(v >> 16U);
    p[2] = (u8)(v >> 8U);
    p[3] = (u8)v;
}

void golden_image_free(GoldenImage *image)
{
    if (image == NULL || image->rgba == NULL) {
        return;
    }
    NPH_FREE_ARRAY(image->alloc, u8, image->rgba, (u64)image->width * image->height * 4U);
    image->rgba = NULL;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

typedef struct Header {
    u32 width;
    u32 height;
    u32 channels; // 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
} Header;

static NphResult parse_header(const u8 *chunk, u32 length, Header *out)
{
    if (length != 13U) {
        return NPH_RESULT_CORRUPT;
    }
    out->width = be32(chunk);
    out->height = be32(chunk + 4U);
    const u8 depth = chunk[8];
    const u8 color = chunk[9];
    if (out->width == 0U || out->height == 0U || out->width > 0x7FFFFFFFU ||
        out->height > 0x7FFFFFFFU || chunk[10] != 0U || chunk[11] != 0U || chunk[12] > 1U) {
        return NPH_RESULT_CORRUPT; // zero size, unknown compression or filter method
    }
    switch (color) {
    case 0U:
        out->channels = 1U;
        break;
    case 2U:
        out->channels = 3U;
        break;
    case 4U:
        out->channels = 2U;
        break;
    case 6U:
        out->channels = 4U;
        break;
    case 3U:
        return NPH_RESULT_UNSUPPORTED; // palette
    default:
        return NPH_RESULT_CORRUPT;
    }
    if (depth != 8U || chunk[12] != 0U || out->width > GOLDEN_PNG_MAX_EXTENT ||
        out->height > GOLDEN_PNG_MAX_EXTENT ||
        (u64)out->width * out->height > GOLDEN_PNG_MAX_TEXELS) {
        return NPH_RESULT_UNSUPPORTED; // 1/2/4/16-bit, interlaced, or too large
    }
    return NPH_RESULT_OK;
}

static u8 paeth(u8 a, u8 b, u8 c)
{
    const i32 p = (i32)a + (i32)b - (i32)c;
    const i32 pa = p > (i32)a ? p - (i32)a : (i32)a - p;
    const i32 pb = p > (i32)b ? p - (i32)b : (i32)b - p;
    const i32 pc = p > (i32)c ? p - (i32)c : (i32)c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

// Reverses the per-row filters (PNG §9) in place: `rows` holds height rows
// of a filter byte and `stride` bytes.
static NphResult unfilter(u8 *rows, u32 height, u64 stride, u32 bpp)
{
    const u8 *prev = NULL;
    for (u32 y = 0; y < height; ++y) {
        u8 *row = rows + (u64)y * (stride + 1U);
        const u8 type = row[0];
        u8 *cur = row + 1U;
        for (u64 x = 0; x < stride; ++x) {
            const u8 a = x >= bpp ? cur[x - bpp] : 0U;
            const u8 b = prev != NULL ? prev[x] : 0U;
            const u8 c = prev != NULL && x >= bpp ? prev[x - bpp] : 0U;
            switch (type) {
            case 0U:
                break;
            case 1U:
                cur[x] = (u8)(cur[x] + a);
                break;
            case 2U:
                cur[x] = (u8)(cur[x] + b);
                break;
            case 3U:
                cur[x] = (u8)(cur[x] + (u8)(((u32)a + (u32)b) >> 1U));
                break;
            case 4U:
                cur[x] = (u8)(cur[x] + paeth(a, b, c));
                break;
            default:
                return NPH_RESULT_CORRUPT;
            }
        }
        prev = cur;
    }
    return NPH_RESULT_OK;
}

NphResult golden_png_decode(const void *data, u64 size, NphAllocator *alloc, GoldenImage *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    const u8 *p = (const u8 *)data;
    if (p == NULL || size < 8U || memcmp(p, g_signature, 8U) != 0) {
        return NPH_RESULT_CORRUPT;
    }
    // First pass: validate every chunk, read the header, size the IDATs.
    Header h;
    memset(&h, 0, sizeof h);
    bool have_header = false;
    bool have_end = false;
    u64 idat_bytes = 0U;
    for (u64 at = 8U; at < size && !have_end;) {
        if (size - at < 12U) {
            return NPH_RESULT_CORRUPT;
        }
        const u32 length = be32(p + at);
        if (length > 0x7FFFFFFFU || length > size - at - 12U) {
            return NPH_RESULT_CORRUPT;
        }
        const u8 *type = p + at + 4U;
        const u8 *body = p + at + 8U;
        if (golden_crc32(0U, type, (u64)length + 4U) != be32(body + length)) {
            return NPH_RESULT_CORRUPT;
        }
        if (memcmp(type, "IHDR", 4U) == 0) {
            if (have_header || at != 8U) {
                return NPH_RESULT_CORRUPT; // IHDR comes first, once
            }
            NPH_TRY(parse_header(body, length, &h));
            have_header = true;
        } else if (!have_header) {
            return NPH_RESULT_CORRUPT;
        } else if (memcmp(type, "IDAT", 4U) == 0) {
            idat_bytes += length;
        } else if (memcmp(type, "IEND", 4U) == 0) {
            have_end = true;
        } else if ((type[0] & 0x20U) == 0U && memcmp(type, "PLTE", 4U) != 0) {
            return NPH_RESULT_UNSUPPORTED; // an unknown critical chunk
        }
        at += (u64)length + 12U;
    }
    if (!have_header || !have_end || idat_bytes == 0U) {
        return NPH_RESULT_CORRUPT;
    }
    // Second pass: gather the compressed stream, inflate, unfilter.
    const u64 stride = (u64)h.width * h.channels;
    const u64 raw_bytes = (stride + 1U) * h.height;
    u8 *idat = NPH_ALLOC_ARRAY(alloc, u8, idat_bytes);
    u8 *raw = NPH_ALLOC_ARRAY(alloc, u8, raw_bytes);
    NphResult r = idat != NULL && raw != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        u64 filled = 0U;
        for (u64 at = 8U; at < size;) {
            const u32 length = be32(p + at);
            if (memcmp(p + at + 4U, "IDAT", 4U) == 0) {
                memcpy(idat + filled, p + at + 8U, length);
                filled += length;
            } else if (memcmp(p + at + 4U, "IEND", 4U) == 0) {
                break;
            }
            at += (u64)length + 12U;
        }
        u64 written = 0U;
        r = golden_zlib_decompress(idat, idat_bytes, raw, raw_bytes, &written);
        if (r == NPH_RESULT_OK && written != raw_bytes) {
            r = NPH_RESULT_CORRUPT;
        }
    }
    if (r == NPH_RESULT_OK) {
        r = unfilter(raw, h.height, stride, h.channels);
    }
    if (r == NPH_RESULT_OK) {
        out->rgba = NPH_ALLOC_ARRAY(alloc, u8, (u64)h.width * h.height * 4U);
        r = out->rgba != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    if (r == NPH_RESULT_OK) {
        out->alloc = alloc;
        out->width = h.width;
        out->height = h.height;
        for (u32 y = 0; y < h.height; ++y) {
            const u8 *src = raw + (u64)y * (stride + 1U) + 1U;
            u8 *dst = out->rgba + (u64)y * h.width * 4U;
            for (u32 x = 0; x < h.width; ++x) {
                const u8 *s = src + (u64)x * h.channels;
                u8 *d = dst + (u64)x * 4U;
                if (h.channels <= 2U) {
                    d[0] = d[1] = d[2] = s[0];
                    d[3] = h.channels == 2U ? s[1] : 255U;
                } else {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = h.channels == 4U ? s[3] : 255U;
                }
            }
        }
    }
    if (idat != NULL) {
        NPH_FREE_ARRAY(alloc, u8, idat, idat_bytes);
    }
    if (raw != NULL) {
        NPH_FREE_ARRAY(alloc, u8, raw, raw_bytes);
    }
    return r;
}

NphResult golden_png_read(const char *path, NphAllocator *alloc, GoldenImage *out)
{
    NPH_ASSERT(path != NULL && alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, alloc, &data, &size));
    const NphResult r = golden_png_decode(data, size, alloc, out);
    nph_file_free_all(alloc, data, size);
    return r;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

static u8 *put_chunk(u8 *at, const char *type, const u8 *body, u32 length)
{
    put_be32(at, length);
    memcpy(at + 4U, type, 4U);
    if (length != 0U) {
        memcpy(at + 8U, body, length);
    }
    put_be32(at + 8U + length, golden_crc32(0U, at + 4U, (u64)length + 4U));
    return at + 12U + length;
}

NphResult golden_png_encode(const u8 *rgba, u32 width, u32 height, u32 channels,
                            NphAllocator *alloc, u8 **out, u64 *out_size)
{
    NPH_ASSERT(alloc != NULL && out != NULL && out_size != NULL);
    *out = NULL;
    *out_size = 0U;
    if (rgba == NULL || width == 0U || height == 0U || width > GOLDEN_PNG_MAX_EXTENT ||
        height > GOLDEN_PNG_MAX_EXTENT || (u64)width * height > GOLDEN_PNG_MAX_TEXELS ||
        (channels != 3U && channels != 4U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // The filtered rows (filter 0) wrapped in zlib with stored blocks of at
    // most 65535 bytes (RFC 1951 §3.2.4).
    const u64 stride = (u64)width * channels;
    const u64 raw_bytes = (stride + 1U) * height;
    const u64 blocks = (raw_bytes + 65534U) / 65535U;
    const u64 zlib_bytes = 2U + raw_bytes + blocks * 5U + 4U;
    if (zlib_bytes > 0x7FFFFFFFU) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u64 file_bytes = 8U + (12U + 13U) + (12U + zlib_bytes) + 12U;
    u8 *file = NPH_ALLOC_ARRAY(alloc, u8, file_bytes);
    u8 *raw = NPH_ALLOC_ARRAY(alloc, u8, raw_bytes);
    if (file == NULL || raw == NULL) {
        if (file != NULL) {
            NPH_FREE_ARRAY(alloc, u8, file, file_bytes);
        }
        if (raw != NULL) {
            NPH_FREE_ARRAY(alloc, u8, raw, raw_bytes);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 y = 0; y < height; ++y) {
        u8 *row = raw + (u64)y * (stride + 1U);
        row[0] = 0U;
        const u8 *src = rgba + (u64)y * width * 4U;
        for (u32 x = 0; x < width; ++x) {
            memcpy(row + 1U + (u64)x * channels, src + (u64)x * 4U, channels);
        }
    }
    u8 *at = file;
    memcpy(at, g_signature, 8U);
    at += 8U;
    u8 ihdr[13];
    put_be32(ihdr, width);
    put_be32(ihdr + 4U, height);
    ihdr[8] = 8U;                               // bit depth
    ihdr[9] = channels == 4U ? (u8)6U : (u8)2U; // RGBA or RGB
    ihdr[10] = 0U;                              // deflate
    ihdr[11] = 0U;                              // adaptive filtering
    ihdr[12] = 0U;                              // no interlace
    at = put_chunk(at, "IHDR", ihdr, 13U);
    // IDAT, built in place: length, type, zlib stream, CRC.
    put_be32(at, (u32)zlib_bytes);
    memcpy(at + 4U, g_idat, sizeof g_idat);
    u8 *z = at + 8U;
    z[0] = 0x78U; // deflate, 32 KiB window
    z[1] = 0x01U; // no dictionary, fastest; (0x78 << 8 | 0x01) % 31 == 0
    u64 zi = 2U;
    u32 a = 1U;
    u32 b = 0U;
    for (u64 done = 0U; done < raw_bytes;) {
        const u64 n = raw_bytes - done < 65535U ? raw_bytes - done : 65535U;
        z[zi++] = done + n == raw_bytes ? 1U : 0U; // BFINAL, BTYPE 00
        z[zi++] = (u8)n;
        z[zi++] = (u8)(n >> 8U);
        z[zi++] = (u8)~n;
        z[zi++] = (u8)(~n >> 8U);
        memcpy(z + zi, raw + done, n);
        for (u64 i = 0; i < n; ++i) {
            a = (a + raw[done + i]) % 65521U;
            b = (b + a) % 65521U;
        }
        zi += n;
        done += n;
    }
    put_be32(z + zi, (b << 16U) | a);
    zi += 4U;
    NPH_ASSERT(zi == zlib_bytes);
    put_be32(z + zi, golden_crc32(0U, at + 4U, zlib_bytes + 4U));
    at = z + zi + 4U;
    at = put_chunk(at, "IEND", NULL, 0U);
    NPH_ASSERT((u64)(at - file) == file_bytes);
    NPH_FREE_ARRAY(alloc, u8, raw, raw_bytes);
    *out = file;
    *out_size = file_bytes;
    return NPH_RESULT_OK;
}

NphResult golden_png_write(const char *path, const u8 *rgba, u32 width, u32 height, u32 channels)
{
    NPH_ASSERT(path != NULL);
    NphAllocator *alloc = nph_allocator_system();
    u8 *file = NULL;
    u64 size = 0U;
    NPH_TRY(golden_png_encode(rgba, width, height, channels, alloc, &file, &size));
    const NphResult r = nph_file_write_all(path, file, size);
    NPH_FREE_ARRAY(alloc, u8, file, size);
    return r;
}
