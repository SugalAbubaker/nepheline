// SPDX-License-Identifier: Apache-2.0
// BMP capture (capture.h).
#include "capture.h"

#include <nepheline/core/alloc.h>
#include <nepheline/platform/file.h>

static void put_le(u8 *p, u32 v, u32 bytes)
{
    for (u32 i = 0; i < bytes; ++i) {
        p[i] = (u8)(v >> (8U * i));
    }
}

NphResult capture_write_bmp(const char *path, const u8 *texels, u32 width, u32 height, bool bgra)
{
    NphAllocator *alloc = nph_allocator_system();
    const u64 row = ((u64)width * 3U + 3U) & ~(u64)3U; // rows pad to four bytes
    const u64 size = 54U + row * height;
    if (size > NPH_U32_MAX) {
        return NPH_RESULT_OVERFLOW;
    }
    u8 *file = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, size);
    if (file == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    file[0] = 'B';
    file[1] = 'M';
    put_le(file + 2, (u32)size, 4U);
    put_le(file + 10, 54U, 4U); // the pixels' offset
    put_le(file + 14, 40U, 4U); // BITMAPINFOHEADER
    put_le(file + 18, width, 4U);
    put_le(file + 22, height, 4U); // positive: bottom-up
    put_le(file + 26, 1U, 2U);     // planes
    put_le(file + 28, 24U, 2U);    // bits per pixel
    put_le(file + 34, (u32)(row * height), 4U);
    for (u32 y = 0; y < height; ++y) {
        const u8 *src = texels + (u64)(height - 1U - y) * width * 4U;
        u8 *dst = file + 54U + row * y;
        for (u32 x = 0; x < width; ++x) {
            const u8 *t = src + (u64)x * 4U;
            dst[(u64)x * 3U] = bgra ? t[0] : t[2];
            dst[(u64)x * 3U + 1U] = t[1];
            dst[(u64)x * 3U + 2U] = bgra ? t[2] : t[0];
        }
    }
    const NphResult r = nph_file_write_all(path, file, size);
    NPH_FREE_ARRAY(alloc, u8, file, size);
    return r;
}
