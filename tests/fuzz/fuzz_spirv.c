// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the SPIR-V reflection parser (CLAUDE.md §8.8 "every
// parser is fuzzed"). The input is copied to 4-byte-aligned memory and
// truncated to whole words; the parser must return an error for anything
// malformed and never read out of bounds.
#include <nepheline/gpu/shader.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const u64 words = (u64)size / 4U;
    if (words == 0U || words > (1U << 20U)) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    u32 *copy = NPH_ALLOC_ARRAY(alloc, u32, words);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data, words * 4U);
    NphGpuShaderInfo info;
    NPH_IGNORE_RESULT(nph_gpu_spirv_reflect(copy, words * 4U, alloc, &info));
    NPH_FREE_ARRAY(alloc, u32, copy, words);
    return 0;
}
