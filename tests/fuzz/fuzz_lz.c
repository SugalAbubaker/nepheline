// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the LZ decoder (CLAUDE.md §8.8 "every parser is
// fuzzed"): arbitrary bytes are decoded into every plausible output size
// and must only ever yield a result, never a memory fault. The first byte
// of the input selects the output size so the corpus explores the
// "exactly right", "too small", and "too large" cases.
#include <nepheline/asset/lz.h>
#include <nepheline/core/alloc.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0U || size > (1U << 20U)) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    const u64 dst_size = ((u64)data[0] + 1U) * 64U + (u64)size;
    u8 *dst = (u8 *)NPH_ALLOC(alloc, dst_size, 16U);
    if (dst == NULL) {
        return 0;
    }
    NPH_IGNORE_RESULT(nph_lz_decompress(data + 1, size - 1U, dst, dst_size));
    NPH_IGNORE_RESULT(nph_lz_decompress(data + 1, size - 1U, dst, size - 1U));
    // Whatever the input, compressing it and decompressing the result must
    // reproduce it at both levels.
    const u64 bound = nph_lz_compress_bound(size);
    u8 *packed = (u8 *)NPH_ALLOC(alloc, bound, 16U);
    u8 *back = (u8 *)NPH_ALLOC(alloc, size, 16U);
    void *ws = NPH_ALLOC(alloc, nph_lz_workspace_size(NPH_LZ_LEVEL_HIGH), 16U);
    if (packed != NULL && back != NULL && ws != NULL) {
        for (u32 level = 0; level < 2U; ++level) {
            u64 packed_size = 0U;
            if (nph_lz_compress(data, size, packed, bound, (NphLzLevel)level, ws, &packed_size) !=
                NPH_RESULT_OK) {
                abort();
            }
            if (nph_lz_decompress(packed, packed_size, back, size) != NPH_RESULT_OK ||
                memcmp(back, data, size) != 0) {
                abort();
            }
        }
    }
    if (ws != NULL) {
        NPH_FREE(alloc, ws, nph_lz_workspace_size(NPH_LZ_LEVEL_HIGH), 16U);
    }
    if (back != NULL) {
        NPH_FREE(alloc, back, size, 16U);
    }
    if (packed != NULL) {
        NPH_FREE(alloc, packed, bound, 16U);
    }
    NPH_FREE(alloc, dst, dst_size, 16U);
    return 0;
}
