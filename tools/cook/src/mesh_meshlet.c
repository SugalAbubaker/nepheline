// SPDX-License-Identifier: Apache-2.0
// Meshlets (mesh.h): the cooker's mesh through the renderer's builder
// (render/meshlet.h), so cooked meshlets and those of meshes made at run
// time come from one implementation (Phase 10 moved it to the renderer).
// The cooked records (asset/mesh.h) and the renderer's are the same bytes.
#include <nepheline/core/assert.h>
#include <nepheline/render/meshlet.h>

#include <cook/mesh.h>
#include <stddef.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphMeshlet) == sizeof(NphRenderMeshlet), "meshlet layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshlet, vertex_offset) == offsetof(NphRenderMeshlet, vertex_offset),
                  "meshlet layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshlet, triangle_offset) ==
                      offsetof(NphRenderMeshlet, triangle_offset),
                  "meshlet layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshlet, vertex_count) == offsetof(NphRenderMeshlet, vertex_count),
                  "meshlet layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshlet, triangle_count) ==
                      offsetof(NphRenderMeshlet, triangle_count),
                  "meshlet layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshlet, submesh) == offsetof(NphRenderMeshlet, submesh),
                  "meshlet layouts agree");
NPH_STATIC_ASSERT(sizeof(NphMeshletBounds) == sizeof(NphRenderMeshletBounds),
                  "meshlet bounds layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshletBounds, center) == offsetof(NphRenderMeshletBounds, center),
                  "meshlet bounds layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshletBounds, radius) == offsetof(NphRenderMeshletBounds, radius),
                  "meshlet bounds layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshletBounds, cone_axis) ==
                      offsetof(NphRenderMeshletBounds, cone_axis),
                  "meshlet bounds layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshletBounds, cone_cutoff) ==
                      offsetof(NphRenderMeshletBounds, cone_cutoff),
                  "meshlet bounds layouts agree");
NPH_STATIC_ASSERT(offsetof(NphMeshletBounds, cone_apex) ==
                      offsetof(NphRenderMeshletBounds, cone_apex),
                  "meshlet bounds layouts agree");
NPH_STATIC_ASSERT(NPH_MESHLET_MAX_VERTICES == NPH_RENDER_MESHLET_MAX_VERTICES &&
                      NPH_MESHLET_MAX_TRIANGLES == NPH_RENDER_MESHLET_MAX_TRIANGLES,
                  "meshlet limits agree");

void cook_meshlets_init(CookMeshlets *m, NphAllocator *alloc)
{
    NPH_ASSERT(m != NULL && alloc != NULL);
    NPH_ARRAY_INIT(&m->meshlets, alloc);
    NPH_ARRAY_INIT(&m->bounds, alloc);
    NPH_ARRAY_INIT(&m->vertices, alloc);
    NPH_ARRAY_INIT(&m->triangles, alloc);
}

void cook_meshlets_free(CookMeshlets *m)
{
    if (m == NULL) {
        return;
    }
    NPH_ARRAY_FREE(&m->meshlets);
    NPH_ARRAY_FREE(&m->bounds);
    NPH_ARRAY_FREE(&m->vertices);
    NPH_ARRAY_FREE(&m->triangles);
}

void cook_meshlet_bounds(const NphVec3 *positions, const u32 *vertices, const u8 *triangles,
                         u32 triangle_count, NphMeshletBounds *out)
{
    NPH_ASSERT(out != NULL);
    NphRenderMeshletBounds b;
    nph_render_meshlet_bounds(positions, vertices, triangles, triangle_count, &b);
    memcpy(out, &b, sizeof *out);
}

// Copies `count` records of the renderer's type over the cooker's (the
// same bytes) at element `at` of `dst`.
static void copy_records(void *dst, u32 at, const void *src, u32 count, u64 elem)
{
    if (count != 0U) {
        memcpy((u8 *)dst + (u64)at * elem, src, (size_t)((u64)count * elem));
    }
}

NphResult cook_mesh_build_meshlets(const CookMesh *mesh, u32 max_vertices, u32 max_triangles,
                                   f32 cone_weight, CookMeshlets *out)
{
    NPH_ASSERT(mesh != NULL && out != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    NphAllocator *alloc = mesh->alloc;
    const u32 vn = mesh->vertices.len;
    const u32 sn = mesh->submeshes.len;
    NphVec3 *positions = NPH_ALLOC_ARRAY(alloc, NphVec3, nph_max_u32(vn, 1U));
    NphRenderIndexRange *ranges = NPH_ALLOC_ARRAY(alloc, NphRenderIndexRange, nph_max_u32(sn, 1U));
    NphResult r = positions != NULL && ranges != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    NphRenderMeshlets built;
    nph_render_meshlets_init(&built, alloc);
    if (r == NPH_RESULT_OK) {
        for (u32 v = 0; v < vn; ++v) {
            positions[v] = mesh->vertices.data[v].position;
        }
        for (u32 s = 0; s < sn; ++s) {
            ranges[s].index_offset = mesh->submeshes.data[s].index_offset;
            ranges[s].index_count = mesh->submeshes.data[s].index_count;
        }
        NphRenderMeshletInput in;
        memset(&in, 0, sizeof in);
        in.positions = positions;
        in.vertex_count = vn;
        in.indices = mesh->indices.data;
        in.index_count = mesh->indices.len;
        in.ranges = ranges;
        in.range_count = sn;
        in.max_vertices = max_vertices;
        in.max_triangles = max_triangles;
        in.cone_weight = cone_weight;
        in.scratch = alloc;
        r = nph_render_meshlets_build(&in, &built);
    }
    // Into the cooker's arrays, after whatever they already hold.
    const u32 m0 = out->meshlets.len;
    const u32 v0 = out->vertices.len;
    const u32 t0 = out->triangles.len;
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&out->meshlets, m0 + built.meshlets.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESIZE(&out->bounds, m0 + built.bounds.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&out->vertices, built.vertices.data, built.vertices.len);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_PUSH_N(&out->triangles, built.triangles.data, built.triangles.len);
    }
    if (r == NPH_RESULT_OK) {
        copy_records(out->meshlets.data, m0, built.meshlets.data, built.meshlets.len,
                     sizeof(NphMeshlet));
        copy_records(out->bounds.data, m0, built.bounds.data, built.bounds.len,
                     sizeof(NphMeshletBounds));
        // Offsets are relative to what this call built.
        for (u32 i = m0; i < out->meshlets.len; ++i) {
            out->meshlets.data[i].vertex_offset += v0;
            out->meshlets.data[i].triangle_offset += t0 / 3U;
        }
    }
    nph_render_meshlets_free(&built);
    if (positions != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, positions, nph_max_u32(vn, 1U));
    }
    if (ranges != NULL) {
        NPH_FREE_ARRAY(alloc, NphRenderIndexRange, ranges, nph_max_u32(sn, 1U));
    }
    return r;
}
