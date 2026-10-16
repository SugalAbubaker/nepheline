// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the cooker's fuzz targets ask of every mesh they cook: the written
// file passes the runtime's validation, its blobs unpack, and the mesh view
// resolves. Whatever the cooker writes from hostile input is something the
// engine can load; a failure aborts.

#include <nepheline/asset/format.h>
#include <nepheline/asset/mesh.h>

#include <stdlib.h>
#include <string.h>

// Wraps the written image in an NphAsset the way the loader would, with
// every blob unpacked, and resolves the mesh view.
static inline void fuzz_check_written_mesh(NphAllocator *alloc, NphAssetWriter *w)
{
    void *image = NULL;
    u64 size = 0U;
    if (nph_asset_writer_finish_alloc(w, alloc, &image, &size) != NPH_RESULT_OK) {
        return;
    }
    NphAssetFileInfo info;
    if (nph_asset_file_validate(image, size, &info) != NPH_RESULT_OK) {
        abort(); // the cooker wrote a file its own reader refuses
    }
    NphAssetBlob blobs[16];
    void *owned[16];
    u32 count = 0U;
    for (u32 i = 0; i < info.blob_count && i < 16U; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        void *raw = NPH_ALLOC(alloc, e->raw_size != 0U ? e->raw_size : 1U, e->alignment);
        if (raw == NULL || nph_asset_file_unpack_blob(image, e, raw) != NPH_RESULT_OK) {
            abort(); // a blob the writer compressed does not decode
        }
        blobs[i].tag = e->tag;
        blobs[i].index = e->index;
        blobs[i].codec = e->codec;
        blobs[i].alignment = e->alignment;
        blobs[i].size = e->raw_size;
        blobs[i].data = raw;
        owned[i] = raw;
        ++count;
    }
    NphAsset asset;
    memset(&asset, 0, sizeof asset);
    asset.header = info.header;
    asset.blobs = blobs;
    asset.blob_count = count;
    asset.path = NPH_STR("fuzz");
    NphMeshView view;
    if (nph_mesh_view(&asset, &view) != NPH_RESULT_OK) {
        abort(); // the pipeline wrote a mesh the runtime cannot view
    }
    for (u32 i = 0; i < count; ++i) {
        NPH_FREE(alloc, owned[i], blobs[i].size != 0U ? blobs[i].size : 1U, blobs[i].alignment);
    }
    NPH_FREE(alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
}
