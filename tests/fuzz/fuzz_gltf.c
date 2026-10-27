// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the cooker's glTF importer and mesh pipeline
// (tools/cook, CLAUDE.md §8.8 "every parser is fuzzed; malformed input must
// never crash"). The input is a .gltf (JSON) or .glb document with its
// buffers and images embedded (no directory is given, so no file is ever
// read). When it imports, every mesh goes through the whole pipeline and
// the written asset must pass the runtime's validation and mesh view:
// whatever the cooker writes from hostile input is something the engine
// can load. A small mesh's aggregate geometry becomes voxels, sampled from
// its materials' factors (render/voxel.h; ADR-0061; images are fuzz_image's).
// Embedded images are probed (header only).
#include <nepheline/asset/format.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/core/log.h>

#include "fuzz_mesh_check.h"

#include <cook/gltf.h>
#include <cook/image.h>
#include <cook/voxel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Work per input stays bounded: documents may reference one accessor from
// many meshes, so the pipeline runs on at most this many vertices in all,
// and voxels on meshes of at most this many triangles with this many
// material slots.
#define FUZZ_MAX_VERTICES    65536U
#define FUZZ_VOXEL_TRIANGLES 4096U
#define FUZZ_VOXEL_SLOTS     16U

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
        NPH_IGNORE_RESULT(cook_image_info(scene.images[i].data, scene.images[i].size, &w, &h, &c));
    }
    CookMeshSettings settings;
    cook_mesh_settings_default(&settings);
    settings.lz = NPH_LZ_LEVEL_FAST;
    u32 budget = FUZZ_MAX_VERTICES;
    for (u32 m = 0; m < scene.mesh_count; ++m) {
        const CookGltfMesh *gm = &scene.meshes[m];
        CookMesh *mesh = &scene.meshes[m].mesh;
        if (mesh->vertices.len > budget) {
            break;
        }
        budget -= mesh->vertices.len;
        // Its slots' materials as the voxelizer reads them (a slot past
        // them never voxelizes).
        CookVoxelMaterial slots[FUZZ_VOXEL_SLOTS];
        const u32 slot_count =
            gm->material_count < FUZZ_VOXEL_SLOTS ? gm->material_count : FUZZ_VOXEL_SLOTS;
        for (u32 s = 0; s < slot_count; ++s) {
            const i32 k = gm->materials[s];
            NphMaterialHeader h;
            if (k >= 0 && (u32)k < scene.material_count) {
                h = scene.materials[k].header;
            } else {
                nph_material_header_default(&h);
            }
            cook_voxel_material_init(&slots[s], &h);
        }
        const CookVoxelMaterials materials = {slots, slot_count};
        settings.voxels = mesh->indices.len / 3U <= FUZZ_VOXEL_TRIANGLES;
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                              nph_guid_from_cstr("fuzz"), 0U);
        if (cook_mesh_process(mesh, &settings, &materials, NULL, &w, NULL) == NPH_RESULT_OK) {
            fuzz_check_written_mesh(alloc, &w);
        }
        nph_asset_writer_free(&w);
    }
    cook_gltf_free(&scene);
    return 0;
}
