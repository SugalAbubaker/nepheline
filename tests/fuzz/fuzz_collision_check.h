// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the cooker's fuzz targets ask of every collision asset they cook (cook/collision.h;
// ADR-0107): the written file passes the runtime's validation, its blobs unpack, the collision
// view resolves, and every block passes the physics module's validator, so whatever the cooker
// writes from hostile input is something a runtime can make shapes of. A failure aborts.

#include <nepheline/asset/collision.h>
#include <nepheline/asset/format.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>

#include <stdlib.h>
#include <string.h>

// The most blobs the fuzz targets' collision assets hold: a header, the shapes, and a block
// a shape for at most this many less two.
#define FUZZ_COLLISION_BLOBS 18U

static inline void fuzz_check_written_collision(NphAllocator *alloc, NphAssetWriter *w)
{
    void *image = NULL;
    u64 size = 0U;
    if (nph_asset_writer_finish_alloc(w, alloc, &image, &size) != NPH_RESULT_OK) {
        return;
    }
    NphAssetFileInfo info;
    if (nph_asset_file_validate(image, size, &info) != NPH_RESULT_OK ||
        info.blob_count > FUZZ_COLLISION_BLOBS) {
        abort(); // the cooker wrote a file its own reader refuses, or more than it was given
    }
    NphAssetBlob blobs[FUZZ_COLLISION_BLOBS];
    void *owned[FUZZ_COLLISION_BLOBS];
    for (u32 i = 0; i < info.blob_count; ++i) {
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
        blobs[i].offset = e->offset;
        blobs[i].data = raw;
        owned[i] = raw;
    }
    NphAsset asset;
    memset(&asset, 0, sizeof asset);
    asset.header = info.header;
    asset.blobs = blobs;
    asset.blob_count = info.blob_count;
    asset.path = NPH_STR("fuzz");
    NphCollisionView view;
    if (nph_collision_view(&asset, &view) != NPH_RESULT_OK) {
        abort(); // the cooker wrote collision the runtime cannot view
    }
    for (u32 i = 0; i < view.header->shape_count; ++i) {
        const NphCollisionBlock b = nph_collision_shape_block(&asset, &view, i);
        const bool ok = view.shapes[i].kind == (u32)NPH_COLLISION_SHAPE_HULL
                            ? nph_physics_hull_validate(b.data, b.size, 0.0F)
                            : nph_physics_mesh_validate(b.data, b.size);
        if (!ok) {
            abort(); // a block its own module refuses
        }
    }
    for (u32 i = 0; i < info.blob_count; ++i) {
        NPH_FREE(alloc, owned[i], blobs[i].size != 0U ? blobs[i].size : 1U, blobs[i].alignment);
    }
    NPH_FREE(alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
}
