// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the golden toolkit's PNG reader and inflate
// (tools/golden png.h; CLAUDE.md §8.8 "every parser is fuzzed"). The first
// byte selects the mode: even, the rest is a PNG file; odd, a zlib stream
// inflated into an output of (data[1] + 1) * 256 bytes. Neither may fault.
// A PNG that decodes must also survive an encode and decode round trip
// unchanged (the encoder writes only what the decoder accepts).
#include <golden/png.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2U || size > (1U << 20U)) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    if ((data[0] & 1U) != 0U) {
        const u64 cap = ((u64)data[1] + 1U) * 256U;
        u8 *out = NPH_ALLOC_ARRAY(alloc, u8, cap);
        if (out != NULL) {
            u64 written = 0U;
            NPH_IGNORE_RESULT(golden_zlib_decompress(data + 2, size - 2U, out, cap, &written));
            NPH_FREE_ARRAY(alloc, u8, out, cap);
        }
        return 0;
    }
    GoldenImage img;
    if (golden_png_decode(data + 1, size - 1U, alloc, &img) != NPH_RESULT_OK) {
        return 0;
    }
    // Keep the round trip cheap: images the fuzzer grows past a megapixel
    // were already decoded once, which is the point.
    if ((u64)img.width * img.height <= (1U << 20U)) {
        u8 *file = NULL;
        u64 file_size = 0U;
        if (golden_png_encode(img.rgba, img.width, img.height, 4U, alloc, &file, &file_size) ==
            NPH_RESULT_OK) {
            GoldenImage back;
            if (golden_png_decode(file, file_size, alloc, &back) != NPH_RESULT_OK ||
                back.width != img.width || back.height != img.height ||
                memcmp(back.rgba, img.rgba, (u64)img.width * img.height * 4U) != 0) {
                abort(); // the encoder wrote what the decoder refuses or misreads
            }
            golden_image_free(&back);
            NPH_FREE_ARRAY(alloc, u8, file, file_size);
        }
    }
    golden_image_free(&img);
    return 0;
}
