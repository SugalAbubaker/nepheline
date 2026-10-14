// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the cooked asset container (format.h) and the typed
// views: the input, copied to aligned memory, is validated; when it passes,
// every blob is checksummed and unpacked and every typed view is resolved,
// so a table that validation accepted can never lead a view out of bounds.
#include <nepheline/asset/format.h>
#include <nepheline/asset/loader.h>
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/texture.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0U || size > (1U << 22U)) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    u8 *copy = (u8 *)NPH_ALLOC(alloc, size, NPH_ASSET_BLOB_MAX_ALIGN);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data, size);
    NphAssetFileInfo info;
    if (nph_asset_file_validate(copy, size, &info) == NPH_RESULT_OK) {
        NphAssetBlob *blobs = NULL;
        if (info.blob_count != 0U) {
            blobs = NPH_ALLOC_ARRAY_ZEROED(alloc, NphAssetBlob, info.blob_count);
        }
        u64 unpacked_bytes = 0U;
        for (u32 i = 0; i < info.blob_count && (blobs != NULL || info.blob_count == 0U); ++i) {
            const NphAssetBlobEntry *e = &info.blobs[i];
            (void)nph_asset_file_verify_blob(copy, e);
            blobs[i].tag = e->tag;
            blobs[i].index = e->index;
            blobs[i].codec = e->codec;
            blobs[i].alignment = e->alignment;
            blobs[i].size = e->raw_size;
            // Bound the memory a hostile table can ask for.
            if (e->raw_size > (1U << 24U) || unpacked_bytes + e->raw_size > (1U << 26U)) {
                blobs[i].size = 0U;
                blobs[i].data = copy;
                continue;
            }
            void *raw = e->raw_size != 0U ? NPH_ALLOC(alloc, e->raw_size, e->alignment) : NULL;
            if (raw != NULL && nph_asset_file_unpack_blob(copy, e, raw) != NPH_RESULT_OK) {
                NPH_FREE(alloc, raw, e->raw_size, e->alignment);
                raw = NULL;
            }
            if (raw == NULL && e->raw_size != 0U) {
                blobs[i].size = 0U;
                blobs[i].data = copy;
                continue;
            }
            blobs[i].data = raw != NULL ? raw : (const void *)copy;
            unpacked_bytes += e->raw_size;
        }
        if (blobs != NULL || info.blob_count == 0U) {
            NphAsset asset;
            memset(&asset, 0, sizeof asset);
            asset.header = info.header;
            asset.blobs = blobs;
            asset.blob_count = info.blob_count;
            asset.path = NPH_STR("fuzz");
            NphMeshView mesh;
            (void)nph_mesh_view(&asset, &mesh);
            NphTextureView texture;
            (void)nph_texture_view(&asset, &texture);
            NphMaterialView material;
            (void)nph_material_view(&asset, &material);
            NphSceneView scene;
            if (nph_scene_view(&asset, &scene) == NPH_RESULT_OK) {
                for (u32 i = 0; i < scene.header->node_count; ++i) {
                    (void)nph_scene_node_name(&scene, i);
                }
            }
            for (u32 i = 0; i < info.blob_count; ++i) {
                if (blobs[i].data != copy && blobs[i].data != NULL) {
                    NPH_FREE(alloc, (void *)(uptr)blobs[i].data, blobs[i].size, blobs[i].alignment);
                }
            }
        }
        if (blobs != NULL) {
            NPH_FREE_ARRAY(alloc, NphAssetBlob, blobs, info.blob_count);
        }
    }
    NPH_FREE(alloc, copy, size, NPH_ASSET_BLOB_MAX_ALIGN);
    return 0;
}
