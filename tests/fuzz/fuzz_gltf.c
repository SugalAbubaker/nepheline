// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the cooker's glTF importer and mesh pipeline
// (tools/cook, CLAUDE.md §8.8 "every parser is fuzzed; malformed input must
// never crash"). The input is a .gltf (JSON) or .glb document with its
// buffers and images embedded (no directory is given, so no file is ever
// read). When it imports, every mesh goes through the whole pipeline and
// the written asset must pass the runtime's validation and mesh view:
// whatever the cooker writes from hostile input is something the engine
// can load. Embedded images are probed (header only).
#include <nepheline/asset/format.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/core/log.h>

#include <cook/gltf.h>
#include <cook/image.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Work per input stays bounded: documents may reference one accessor from
// many meshes, so the pipeline runs on at most this many vertices in all.
#define FUZZ_MAX_VERTICES 65536U

// Wraps the written image in an NphAsset the way the loader would, with
// every blob unpacked, and resolves the mesh view.
static void check_written(NphAllocator *alloc, NphAssetWriter *w)
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0U || size > (1U << 20U)) {
        return 0;
    }
    // Rejecting input is the common case; logging every rejection would
    // dominate the run.
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    NphAllocator *alloc = nph_allocator_system();
    CookGltfScene scene;
    if (cook_gltf_load_memory(alloc, data, size, NULL, &scene) != NPH_RESULT_OK) {
        return 0;
    }
    for (u32 i = 0; i < scene.image_count; ++i) {
        u32 w;
        u32 h;
        u32 c;
        (void)cook_image_info(scene.images[i].data, scene.images[i].size, &w, &h, &c);
    }
    CookMeshSettings settings;
    cook_mesh_settings_default(&settings);
    settings.lz = NPH_LZ_LEVEL_FAST;
    u32 budget = FUZZ_MAX_VERTICES;
    for (u32 m = 0; m < scene.mesh_count; ++m) {
        CookMesh *mesh = &scene.meshes[m].mesh;
        if (mesh->vertices.len > budget) {
            break;
        }
        budget -= mesh->vertices.len;
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                              nph_guid_from_cstr("fuzz"), 0U);
        if (cook_mesh_process(mesh, &settings, &w, NULL) == NPH_RESULT_OK) {
            check_written(alloc, &w);
        }
        nph_asset_writer_free(&w);
    }
    cook_gltf_free(&scene);
    return 0;
}
