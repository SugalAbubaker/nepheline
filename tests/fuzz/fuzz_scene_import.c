// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the reader of scenes' import settings (tools/cook/scene_import.h,
// CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never crash"; ADR-0107). Settings
// that read must write back to text that reads again to the same settings, and give the mesh and
// texture pipelines settings within what they take.
#include <nepheline/core/log.h>

#include <cook/scene_import.h>
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
    CookSceneImport s;
    if (cook_scene_import_read(alloc, nph_str((const char *)data, (u64)size), &s) !=
        NPH_RESULT_OK) {
        return 0;
    }
    NphStrBuilder text;
    nph_sb_init(&text, alloc);
    if (cook_scene_import_write(&s, &text) != NPH_RESULT_OK) {
        abort(); // settings that read do not write
    }
    CookSceneImport again;
    if (cook_scene_import_read(alloc, nph_str(text.data, text.len), &again) != NPH_RESULT_OK ||
        !cook_scene_import_equal(&again, &s)) {
        abort(); // what was written does not read back as the same settings
    }
    // Applied over the defaults, they stay within what the pipelines take.
    CookTextureSettings base;
    cook_texture_settings_default(&base);
    const CookTextureSettings t = cook_scene_import_texture(&s, &base);
    const u32 hull = cook_scene_import_hull_vertices(&s);
    if (t.max_extent > COOK_SCENE_IMPORT_MAX_EXTENT || t.usage != base.usage ||
        hull < COOK_SCENE_HULL_VERTICES_MIN || hull > COOK_SCENE_HULL_VERTICES_MAX ||
        (u32)cook_scene_import_collision(&s) >= (u32)COOK_COLLISION_KIND_COUNT) {
        abort();
    }
    nph_sb_free(&text);
    return 0;
}
