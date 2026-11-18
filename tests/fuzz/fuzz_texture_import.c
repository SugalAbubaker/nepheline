// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the reader of images' import settings (tools/cook/texture_import.h,
// CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never crash"; ADR-0105). Settings
// that read must write back to text that reads again to the same settings.
#include <nepheline/core/log.h>

#include <cook/texture_import.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1U << 16U)) {
        return 0;
    }
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    NphAllocator *alloc = nph_allocator_system();
    CookTextureImport s;
    if (cook_texture_import_read(alloc, nph_str((const char *)data, (u64)size), &s) !=
        NPH_RESULT_OK) {
        return 0;
    }
    NphStrBuilder text;
    nph_sb_init(&text, alloc);
    if (cook_texture_import_write(&s, &text) != NPH_RESULT_OK) {
        abort(); // settings that read do not write
    }
    CookTextureImport again;
    if (cook_texture_import_read(alloc, nph_str(text.data, text.len), &again) != NPH_RESULT_OK ||
        !cook_texture_import_equal(&again, &s)) {
        abort(); // what was written does not read back as the same settings
    }
    // Applied over any settings, they stay within what the texture pipeline takes.
    CookTextureSettings base;
    cook_texture_settings_default(&base);
    const CookTextureSettings t = cook_texture_import_apply(&s, &base, true);
    if ((u32)t.usage > (u32)COOK_TEXTURE_HDR || t.max_extent > COOK_TEXTURE_IMPORT_MAX_EXTENT) {
        abort();
    }
    nph_sb_free(&text);
    return 0;
}
