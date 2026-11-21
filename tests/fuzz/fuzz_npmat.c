// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the physics material source reader (tools/cook/physics_material.h,
// CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never crash"; ADR-0108). A `.npmat`
// document that reads must validate, write back to text that reads again to the same material,
// and make a header the runtime's view takes (asset/physics_material.h).
#include <nepheline/asset/format.h>
#include <nepheline/asset/physics_material.h>
#include <nepheline/core/log.h>

#include <cook/physics_material.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void round_trip(NphAllocator *alloc, const CookPhysicsMaterialSource *m)
{
    NphStrBuilder text;
    nph_sb_init(&text, alloc);
    if (cook_physics_material_source_write(m, &text) != NPH_RESULT_OK) {
        abort(); // a material that read and validated does not write
    }
    CookPhysicsMaterialSource again;
    if (cook_physics_material_source_read(alloc, nph_str(text.data, text.len), &again) !=
            NPH_RESULT_OK ||
        !cook_physics_material_source_equal(&again, m)) {
        abort(); // what was written does not read back as the same material
    }
    nph_sb_free(&text);
}

// The cooker's asset for it (cooker.c cooker_cook_physics_material): its header, which the
// runtime's view must take.
static void check_asset(NphAllocator *alloc, const CookPhysicsMaterialSource *m)
{
    NphPhysicsMaterialHeader h;
    cook_physics_material_source_header(m, &h);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_PHYSICS_MATERIAL,
                          NPH_PHYSICS_MATERIAL_TYPE_VERSION, nph_guid_from_cstr("fuzz"), 0U);
    if (nph_asset_writer_add_blob(&w, NPH_PHYSICS_MATERIAL_TAG_HEADER, 0U, &h, sizeof h,
                                  NPH_ASSET_CODEC_NONE, 0U) != NPH_RESULT_OK) {
        abort();
    }
    void *image = NULL;
    u64 size = 0U;
    if (nph_asset_writer_finish_alloc(&w, alloc, &image, &size) != NPH_RESULT_OK) {
        abort();
    }
    nph_asset_writer_free(&w);
    NphAssetFileInfo info;
    if (nph_asset_file_validate(image, size, &info) != NPH_RESULT_OK || info.blob_count != 1U) {
        abort(); // the writer wrote a file its own reader refuses
    }
    const NphAssetBlobEntry *e = &info.blobs[0];
    void *owned = NPH_ALLOC(alloc, e->raw_size != 0U ? e->raw_size : 1U, e->alignment);
    if (owned == NULL || nph_asset_file_unpack_blob(image, e, owned) != NPH_RESULT_OK) {
        abort();
    }
    NphAssetBlob blob;
    blob.tag = e->tag;
    blob.index = e->index;
    blob.codec = e->codec;
    blob.alignment = e->alignment;
    blob.size = e->raw_size;
    blob.data = owned;
    NphAsset asset;
    memset(&asset, 0, sizeof asset);
    asset.header = info.header;
    asset.blobs = &blob;
    asset.blob_count = 1U;
    asset.path = NPH_STR("fuzz");
    NphPhysicsMaterialView view;
    if (nph_physics_material_view(&asset, &view) != NPH_RESULT_OK ||
        view.header->friction_combine != m->friction_combine ||
        view.header->density != m->density) {
        abort(); // the cooker would write a physics material the runtime cannot view
    }
    NPH_FREE(alloc, owned, blob.size != 0U ? blob.size : 1U, blob.alignment);
    NPH_FREE(alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1U << 20U)) {
        return 0;
    }
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    NphAllocator *alloc = nph_allocator_system();
    CookPhysicsMaterialSource m;
    if (cook_physics_material_source_read(alloc, nph_str((const char *)data, (u64)size), &m) !=
        NPH_RESULT_OK) {
        return 0;
    }
    if (cook_physics_material_source_validate(&m) != NPH_RESULT_OK) {
        abort(); // the reader let through what validation refuses
    }
    round_trip(alloc, &m);
    check_asset(alloc, &m);
    return 0;
}
