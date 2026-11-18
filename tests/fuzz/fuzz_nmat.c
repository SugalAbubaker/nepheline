// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the material source reader (tools/cook/material.h, CLAUDE.md §8.8 "every
// parser is fuzzed; malformed input must never crash"; ADR-0105). A `.nmat` document that reads
// must validate, write back to text that reads again to the same material, and make a header and
// a transform the runtime's view takes (asset/material.h).
#include <nepheline/asset/format.h>
#include <nepheline/asset/material.h>
#include <nepheline/core/log.h>

#include <cook/material.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void round_trip(NphAllocator *alloc, const CookMaterialSource *m)
{
    NphStrBuilder text;
    nph_sb_init(&text, alloc);
    if (cook_material_source_write(m, &text) != NPH_RESULT_OK) {
        abort(); // a material that read and validated does not write
    }
    CookMaterialSource again;
    if (cook_material_source_read(alloc, nph_str(text.data, text.len), &again) != NPH_RESULT_OK ||
        !cook_material_source_equal(&again, m)) {
        abort(); // what was written does not read back as the same material
    }
    nph_sb_free(&text);
}

// The cooker's asset for it (cooker.c cook_material): a header, and the transform when it has
// one, which the runtime's view must take.
static void check_asset(NphAllocator *alloc, const CookMaterialSource *m)
{
    static const char *const names[NPH_MATERIAL_TEXTURE_COUNT] = {"t0", "t1", "t2", "t3", "t4"};
    NphGuid textures[NPH_MATERIAL_TEXTURE_COUNT];
    for (u32 i = 0; i < NPH_MATERIAL_TEXTURE_COUNT; ++i) {
        textures[i] = nph_guid_from_cstr(names[i]);
    }
    NphMaterialHeader h;
    cook_material_source_header(m, textures, &h);
    const NphMaterialUvTransform uv = cook_material_source_uv(m);
    const bool tiled = !nph_material_uv_is_identity(&uv);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MATERIAL, NPH_MATERIAL_TYPE_VERSION,
                          nph_guid_from_cstr("fuzz"), 0U);
    if (nph_asset_writer_add_blob(&w, NPH_MATERIAL_TAG_HEADER, 0U, &h, sizeof h,
                                  NPH_ASSET_CODEC_NONE, 0U) != NPH_RESULT_OK ||
        (tiled && nph_asset_writer_add_blob(&w, NPH_MATERIAL_TAG_UV, 0U, &uv, sizeof uv,
                                            NPH_ASSET_CODEC_NONE, 0U) != NPH_RESULT_OK)) {
        abort();
    }
    void *image = NULL;
    u64 size = 0U;
    if (nph_asset_writer_finish_alloc(&w, alloc, &image, &size) != NPH_RESULT_OK) {
        abort();
    }
    nph_asset_writer_free(&w);
    NphAssetFileInfo info;
    if (nph_asset_file_validate(image, size, &info) != NPH_RESULT_OK || info.blob_count > 2U) {
        abort(); // the writer wrote a file its own reader refuses
    }
    NphAssetBlob blobs[2];
    void *owned[2] = {NULL, NULL};
    for (u32 i = 0; i < info.blob_count; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        owned[i] = NPH_ALLOC(alloc, e->raw_size != 0U ? e->raw_size : 1U, e->alignment);
        if (owned[i] == NULL || nph_asset_file_unpack_blob(image, e, owned[i]) != NPH_RESULT_OK) {
            abort();
        }
        blobs[i].tag = e->tag;
        blobs[i].index = e->index;
        blobs[i].codec = e->codec;
        blobs[i].alignment = e->alignment;
        blobs[i].size = e->raw_size;
        blobs[i].data = owned[i];
    }
    NphAsset asset;
    memset(&asset, 0, sizeof asset);
    asset.header = info.header;
    asset.blobs = blobs;
    asset.blob_count = info.blob_count;
    asset.path = NPH_STR("fuzz");
    NphMaterialView view;
    if (nph_material_view(&asset, &view) != NPH_RESULT_OK || (view.uv != NULL) != tiled) {
        abort(); // the cooker would write a material the runtime cannot view
    }
    for (u32 i = 0; i < info.blob_count; ++i) {
        NPH_FREE(alloc, owned[i], blobs[i].size != 0U ? blobs[i].size : 1U, blobs[i].alignment);
    }
    NPH_FREE(alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1U << 20U)) {
        return 0;
    }
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    NphAllocator *alloc = nph_allocator_system();
    CookMaterialSource m;
    if (cook_material_source_read(alloc, nph_str((const char *)data, (u64)size), &m) !=
        NPH_RESULT_OK) {
        return 0;
    }
    if (cook_material_source_validate(&m) != NPH_RESULT_OK) {
        abort(); // the reader let through what validation refuses
    }
    round_trip(alloc, &m);
    check_asset(alloc, &m);
    return 0;
}
