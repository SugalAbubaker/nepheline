// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the editable mesh reader (tools/cook/polymesh.h,
// CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never
// crash"). A `.nmesh` document that reads must validate, write back to text
// that reads again to the same mesh, stay valid when its faces are
// extruded, and cook into a mesh the runtime can view, with voxel levels
// where it is aggregate geometry (render/voxel.h; ADR-0061).
#include <nepheline/core/log.h>

#include "fuzz_mesh_check.h"

#include <cook/polymesh.h>
#include <cook/voxel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Work per input stays bounded: extrusion and the pipeline run on meshes of
// at most this many corners.
#define FUZZ_MAX_CORNERS 65536U
#define FUZZ_EXTRUDED    64U

static void round_trip(NphAllocator *alloc, const CookPolyMesh *m)
{
    NphStrBuilder text;
    nph_sb_init(&text, alloc);
    if (cook_polymesh_write(m, &text) != NPH_RESULT_OK) {
        abort(); // a mesh that read and validated does not write
    }
    CookPolyMesh again;
    if (cook_polymesh_read(alloc, nph_str(text.data, text.len), &again) != NPH_RESULT_OK ||
        again.positions.len != m->positions.len || again.faces.len != m->faces.len ||
        again.corners.len != m->corners.len ||
        (m->corners.len != 0U &&
         memcmp(again.corners.data, m->corners.data, sizeof(u32) * (u64)m->corners.len) != 0)) {
        abort(); // what was written does not read back as the same mesh
    }
    cook_polymesh_free(&again);
    nph_sb_free(&text);
}

// The material slots a document's faces name.
#define FUZZ_MAX_SLOTS 64U

static void cook(NphAllocator *alloc, const CookPolyMesh *m)
{
    CookMesh mesh;
    if (cook_polymesh_to_mesh(m, "fuzz", &mesh) == NPH_RESULT_OK && mesh.submeshes.len != 0U) {
        CookMeshSettings settings;
        cook_mesh_settings_default(&settings);
        settings.lz = NPH_LZ_LEVEL_FAST;
        settings.voxels = 1;
        // Every slot the default material, opaque and untextured: the
        // voxelizer runs wherever the mesh is aggregate.
        CookVoxelMaterial slots[FUZZ_MAX_SLOTS];
        NphMaterialHeader h;
        nph_material_header_default(&h);
        for (u32 i = 0; i < FUZZ_MAX_SLOTS; ++i) {
            cook_voxel_material_init(&slots[i], &h);
        }
        CookVoxelMaterials materials = {slots, FUZZ_MAX_SLOTS};
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                              nph_guid_from_cstr("fuzz"), 0U);
        if (cook_mesh_process(&mesh, &settings, &materials, NULL, &w, NULL) == NPH_RESULT_OK) {
            fuzz_check_written_mesh(alloc, &w);
        }
        nph_asset_writer_free(&w);
    }
    cook_mesh_free(&mesh);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1U << 20U)) {
        return 0;
    }
    // Rejecting input is the common case; logging every rejection would
    // dominate the run.
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    if (cook_polymesh_read(alloc, nph_str((const char *)data, size), &m) != NPH_RESULT_OK) {
        cook_polymesh_free(&m);
        return 0;
    }
    if (cook_polymesh_validate(&m) != NPH_RESULT_OK) {
        abort(); // the reader accepted a mesh validation refuses
    }
    round_trip(alloc, &m);
    if (m.corners.len <= FUZZ_MAX_CORNERS) {
        cook(alloc, &m);
        u32 faces[FUZZ_EXTRUDED];
        const u32 n = m.faces.len < FUZZ_EXTRUDED ? m.faces.len : FUZZ_EXTRUDED;
        for (u32 i = 0; i < n; ++i) {
            faces[i] = i;
        }
        NPH_IGNORE_RESULT(cook_polymesh_extrude(&m, faces, n, 0.5F, NULL));
        if (cook_polymesh_validate(&m) != NPH_RESULT_OK) {
            abort(); // extrusion left a mesh validation refuses
        }
        cook(alloc, &m);
    }
    cook_polymesh_free(&m);
    return 0;
}
