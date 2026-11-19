// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the material preview cache's reader (tools/cook/preview.h, CLAUDE.md
// §8.8 "every parser is fuzzed; malformed input must never crash"; ADR-0105). The input, copied
// to aligned memory as a file read gives it, is read for the key its header names: bytes that
// read must not read for another key, nor from memory off its alignment.
#include <nepheline/asset/format.h>
#include <nepheline/core/log.h>

#include <cook/preview.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1U << 20U)) {
        return 0;
    }
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    NphAllocator *alloc = nph_allocator_system();
    // One byte past the size, so that the copy shifted off its alignment fits too.
    const u64 cap = (u64)size + 1U;
    u8 *copy = (u8 *)NPH_ALLOC(alloc, cap, NPH_ASSET_BLOB_MAX_ALIGN);
    if (copy == NULL) {
        return 0;
    }
    if (size != 0U) {
        memcpy(copy, data, size);
    }
    static u8 pixels[COOK_PREVIEW_BYTES];
    u64 key = 0U;
    NphAssetFileInfo info;
    if (nph_asset_file_validate(copy, size, &info) == NPH_RESULT_OK) {
        key = info.header.content_hash;
    }
    const NphResult r = cook_preview_read(copy, size, key, pixels);
    if (r != NPH_RESULT_OK && r != NPH_RESULT_NOT_FOUND && r != NPH_RESULT_CORRUPT) {
        abort(); // the reader answers with one of its three results
    }
    if (r == NPH_RESULT_OK) {
        if (cook_preview_read(copy, size, key ^ 1U, pixels) != NPH_RESULT_NOT_FOUND) {
            abort(); // a preview read for a key it was not made for
        }
        memmove(copy + 1U, copy, size);
        if (cook_preview_read(copy + 1U, size, key, pixels) != NPH_RESULT_CORRUPT) {
            abort(); // read off its alignment
        }
    }
    NPH_FREE(alloc, copy, cap, NPH_ASSET_BLOB_MAX_ALIGN);
    return 0;
}
