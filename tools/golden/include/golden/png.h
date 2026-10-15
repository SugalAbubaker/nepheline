// SPDX-License-Identifier: Apache-2.0
#pragma once

// PNG reading and writing for the golden-image tests (CLAUDE.md §9 "GPU
// golden-image tests ... against approved baselines"; docs/golden.md).
// Baselines are PNG so a reviewer approves them in any image viewer and on
// GitHub; the engine has no image codec of its own (stb_image is the
// cooker's alone, CLAUDE.md §3), so this one is written for the purpose:
//
//   - reading: 8-bit greyscale, greyscale + alpha, RGB, and RGBA images,
//     non-interlaced, every filter type, decompressed by our own inflate
//     (RFC 1950 zlib framing around RFC 1951 deflate: stored, fixed, and
//     dynamic Huffman blocks), CRC-32 and Adler-32 verified; anything else
//     (16-bit, palettes, interlacing) is refused as UNSUPPORTED;
//   - writing: 8-bit RGB or RGBA, filter 0, deflate's stored blocks. The
//     files are larger than a compressor's but exact and simple; Git LFS
//     holds them (.gitattributes).
//
// Every length, offset, and code in a file is checked: malformed input
// yields NPH_RESULT_CORRUPT and never reads or writes out of bounds (the
// reader is fuzzed, tests/fuzz/fuzz_png.c).
//
// Thread-safety: NPH_THREADSAFE (no shared state). Allocation: from the
// allocator passed in.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// The largest image either direction accepts (both sides, and 2^26 texels).
#define GOLDEN_PNG_MAX_EXTENT 16384U
#define GOLDEN_PNG_MAX_TEXELS (1U << 26U)

// An RGBA8 image, rows top-down, tightly packed.
typedef struct GoldenImage {
    NphAllocator *alloc;
    u8 *rgba;
    u32 width;
    u32 height;
} GoldenImage;

void golden_image_free(GoldenImage *image);

// Decodes a PNG held in memory into RGBA8 (greyscale replicated, missing
// alpha set to 255). NPH_RESULT_CORRUPT for malformed data,
// NPH_RESULT_UNSUPPORTED for a valid PNG of a kind not listed above,
// NPH_RESULT_OUT_OF_MEMORY otherwise on failure.
NPH_MUST_USE NphResult golden_png_decode(const void *data, u64 size, NphAllocator *alloc,
                                         GoldenImage *out);

// Reads and decodes a file. NPH_RESULT_NOT_FOUND / NPH_RESULT_IO_ERROR from
// the file system, then as golden_png_decode.
NPH_MUST_USE NphResult golden_png_read(const char *path, NphAllocator *alloc, GoldenImage *out);

// Encodes `width` x `height` RGBA8 texels as an 8-bit PNG with `channels`
// 3 (alpha dropped) or 4. The bytes are allocated from `alloc`
// (NPH_FREE_ARRAY(alloc, u8, *out, *out_size)). NPH_RESULT_INVALID_ARGUMENT
// for an empty or oversized image or another channel count.
NPH_MUST_USE NphResult golden_png_encode(const u8 *rgba, u32 width, u32 height, u32 channels,
                                         NphAllocator *alloc, u8 **out, u64 *out_size);

// Encodes and writes a file (parent directories are not created).
NPH_MUST_USE NphResult golden_png_write(const char *path, const u8 *rgba, u32 width, u32 height,
                                        u32 channels);

// zlib decompression (RFC 1950/1951) into a buffer of known capacity, as
// PNG's IDAT stream needs: `out_written` receives the bytes produced.
// NPH_RESULT_CORRUPT for malformed data, a bad checksum, or output past
// `capacity`.
NPH_MUST_USE NphResult golden_zlib_decompress(const u8 *src, u64 size, u8 *dst, u64 capacity,
                                              u64 *out_written);

// The CRC-32 of PNG chunks and zip (ISO 3309, reflected polynomial
// 0xEDB88320); `crc` is 0 for a new computation.
u32 golden_crc32(u32 crc, const void *data, u64 size);

#ifdef __cplusplus
}
#endif
