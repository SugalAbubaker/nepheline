// SPDX-License-Identifier: Apache-2.0
// The mesh pipeline (mesh.h): validation, cleaning, normals, welding,
// writing, and the driver. Tangents live in mesh_tangent.c, reordering in
// mesh_optimize.c, meshlets in mesh_meshlet.c.
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/time.h>

#include <cook/mesh.h>
#include <string.h>

void cook_mesh_settings_default(CookMeshSettings *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->meshlet_cone_weight = 0.25F;
    out->lz = NPH_LZ_LEVEL_HIGH;
}

void cook_mesh_init(CookMesh *mesh, NphAllocator *alloc)
{
    NPH_ASSERT(mesh != NULL && alloc != NULL);
    memset(mesh, 0, sizeof *mesh);
    mesh->alloc = alloc;
    NPH_ARRAY_INIT(&mesh->vertices, alloc);
    NPH_ARRAY_INIT(&mesh->indices, alloc);
    NPH_ARRAY_INIT(&mesh->submeshes, alloc);
}

void cook_mesh_free(CookMesh *mesh)
{
    if (mesh == NULL || mesh->alloc == NULL) {
        return;
    }
    NPH_ARRAY_FREE(&mesh->vertices);
    NPH_ARRAY_FREE(&mesh->indices);
    NPH_ARRAY_FREE(&mesh->submeshes);
    mesh->alloc = NULL;
}

NphResult cook_mesh_add_submesh(CookMesh *mesh, const CookVertex *vertices, u32 vertex_count,
                                const u32 *indices, u32 index_count, u32 material_slot)
{
    NPH_ASSERT(mesh != NULL);
    if (index_count % 3U != 0U || (vertex_count != 0U && vertices == NULL) ||
        (index_count != 0U && indices == NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 base = mesh->vertices.len;
    if (vertex_count > NPH_U32_MAX - base || index_count > NPH_U32_MAX - mesh->indices.len) {
        return NPH_RESULT_OVERFLOW;
    }
    for (u32 i = 0; i < index_count; ++i) {
        if (indices[i] >= vertex_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    NPH_TRY(NPH_ARRAY_RESERVE(&mesh->vertices, base + vertex_count));
    NPH_TRY(NPH_ARRAY_RESERVE(&mesh->indices, mesh->indices.len + index_count));
    CookSubmesh sub;
    sub.index_offset = mesh->indices.len;
    sub.index_count = index_count;
    sub.material_slot = material_slot;
    NPH_TRY(NPH_ARRAY_PUSH(&mesh->submeshes, sub));
    if (vertex_count != 0U) {
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&mesh->vertices, vertices, vertex_count)); // reserved
    }
    for (u32 i = 0; i < index_count; ++i) {
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&mesh->indices, base + indices[i]));
    }
    return NPH_RESULT_OK;
}

static bool vertex_is_usable(const CookVertex *v)
{
    const f32 *f = (const f32 *)v;
    for (u32 i = 0; i < sizeof *v / sizeof(f32); ++i) {
        if (!nph_f32_is_finite(f[i])) {
            return false;
        }
    }
    return nph_f32_abs(v->position.x) <= COOK_MESH_MAX_COORDINATE &&
           nph_f32_abs(v->position.y) <= COOK_MESH_MAX_COORDINATE &&
           nph_f32_abs(v->position.z) <= COOK_MESH_MAX_COORDINATE;
}

NphResult cook_mesh_validate(const CookMesh *mesh)
{
    NPH_ASSERT(mesh != NULL);
    if (mesh->indices.len % 3U != 0U) {
        return NPH_RESULT_CORRUPT;
    }
    for (u32 i = 0; i < mesh->indices.len; ++i) {
        if (mesh->indices.data[i] >= mesh->vertices.len) {
            return NPH_RESULT_CORRUPT;
        }
    }
    // Non-finite values, and positions so large that their differences
    // overflow, would poison the bounds and turn into undefined
    // float-to-integer conversions when quantised.
    NPH_ARRAY_FOREACH (CookVertex, v, &mesh->vertices) {
        if (!vertex_is_usable(v)) {
            return NPH_RESULT_CORRUPT;
        }
    }
    u32 expected = 0U;
    NPH_ARRAY_FOREACH (CookSubmesh, s, &mesh->submeshes) {
        if (s->index_offset != expected || s->index_count % 3U != 0U ||
            s->index_count > mesh->indices.len - s->index_offset) {
            return NPH_RESULT_CORRUPT;
        }
        expected += s->index_count;
    }
    if (expected != mesh->indices.len) {
        return NPH_RESULT_CORRUPT;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Cleaning and normals
// ---------------------------------------------------------------------------

static NphVec3 face_cross(const CookMesh *mesh, const u32 *tri)
{
    const NphVec3 a = mesh->vertices.data[tri[0]].position;
    const NphVec3 b = mesh->vertices.data[tri[1]].position;
    const NphVec3 c = mesh->vertices.data[tri[2]].position;
    return nph_vec3_cross(nph_vec3_sub(b, a), nph_vec3_sub(c, a));
}

NphResult cook_mesh_remove_degenerate(CookMesh *mesh)
{
    NPH_ASSERT(mesh != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    u32 write = 0U;
    NPH_ARRAY_FOREACH (CookSubmesh, s, &mesh->submeshes) {
        const u32 begin = s->index_offset;
        const u32 end = begin + s->index_count;
        s->index_offset = write;
        for (u32 i = begin; i < end; i += 3U) {
            const u32 *tri = &mesh->indices.data[i];
            // A repeated index, or three positions on a line (or a point).
            if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2] ||
                nph_vec3_length_sq(face_cross(mesh, tri)) == 0.0F) {
                continue;
            }
            const u32 a = tri[0];
            const u32 b = tri[1];
            const u32 c = tri[2];
            mesh->indices.data[write] = a;
            mesh->indices.data[write + 1U] = b;
            mesh->indices.data[write + 2U] = c;
            write += 3U;
        }
        s->index_count = write - s->index_offset;
    }
    mesh->indices.len = write;
    return NPH_RESULT_OK;
}

// Every triangle gets three vertices of its own carrying the face normal.
static NphResult flat_normals(CookMesh *mesh)
{
    const u32 n = mesh->indices.len;
    CookVertexArray corners;
    NPH_ARRAY_INIT(&corners, mesh->alloc);
    NPH_TRY(NPH_ARRAY_RESERVE(&corners, nph_max_u32(n, 1U)));
    for (u32 i = 0; i < n; i += 3U) {
        const u32 *tri = &mesh->indices.data[i];
        const NphVec3 normal = nph_vec3_normalize_or(face_cross(mesh, tri), nph_vec3_unit_z());
        for (u32 k = 0; k < 3U; ++k) {
            CookVertex v = mesh->vertices.data[tri[k]];
            v.normal = normal;
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&corners, v)); // reserved
        }
    }
    for (u32 i = 0; i < n; ++i) {
        mesh->indices.data[i] = i;
    }
    NPH_ARRAY_FREE(&mesh->vertices);
    mesh->vertices = corners;
    return NPH_RESULT_OK;
}

// Area-weighted vertex normals over the existing sharing.
static void smooth_normals(CookMesh *mesh)
{
    NPH_ARRAY_FOREACH (CookVertex, v, &mesh->vertices) {
        v->normal = nph_vec3_zero();
    }
    for (u32 i = 0; i + 2U < mesh->indices.len; i += 3U) {
        const u32 *tri = &mesh->indices.data[i];
        // The cross product's length is twice the area: an area weight for free.
        const NphVec3 n = face_cross(mesh, tri);
        for (u32 k = 0; k < 3U; ++k) {
            CookVertex *v = &mesh->vertices.data[tri[k]];
            v->normal = nph_vec3_add(v->normal, n);
        }
    }
    NPH_ARRAY_FOREACH (CookVertex, v, &mesh->vertices) {
        v->normal = nph_vec3_normalize_or(v->normal, nph_vec3_unit_z());
    }
}

NphResult cook_mesh_generate_normals(CookMesh *mesh, CookNormalMode mode)
{
    NPH_ASSERT(mesh != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    if (mode == COOK_NORMALS_FLAT) {
        NPH_TRY(flat_normals(mesh));
    } else if (mode == COOK_NORMALS_SMOOTH) {
        smooth_normals(mesh);
    } else {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    mesh->flags |= (u32)COOK_MESH_HAS_NORMALS;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Welding
// ---------------------------------------------------------------------------

// Welding compares the bytes of the attributes the mesh carries; the
// others are reset first so stale data cannot keep duplicates apart.
static void canonicalize(CookVertex *v, u32 flags)
{
    if ((flags & (u32)COOK_MESH_HAS_NORMALS) == 0U) {
        v->normal = nph_vec3_zero();
    }
    if ((flags & (u32)COOK_MESH_HAS_TANGENTS) == 0U) {
        v->tangent = nph_vec4_zero();
    }
    if ((flags & (u32)COOK_MESH_HAS_UV0) == 0U) {
        v->uv0 = nph_vec2_zero();
    }
    if ((flags & (u32)COOK_MESH_HAS_COLORS) == 0U) {
        v->color = nph_vec4_one();
    }
    // Negative zero and positive zero must weld together.
    f32 *f = (f32 *)v;
    for (u32 i = 0; i < sizeof *v / sizeof(f32); ++i) {
        if (f[i] == 0.0F) {
            f[i] = 0.0F;
        }
    }
}

NphResult cook_mesh_weld(CookMesh *mesh)
{
    NPH_ASSERT(mesh != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    const u32 n = mesh->vertices.len;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_ARRAY_FOREACH (CookVertex, v, &mesh->vertices) {
        canonicalize(v, mesh->flags);
    }
    NphHashMap map; // vertex bytes -> new index
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = sizeof(CookVertex);
    desc.val_size = sizeof(u32);
    NPH_TRY(nph_hashmap_init(&map, mesh->alloc, &desc));
    u32 *remap = NPH_ALLOC_ARRAY(mesh->alloc, u32, n);
    NphResult r = remap != NULL ? nph_hashmap_reserve(&map, n) : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        for (u32 v = 0; v < n; ++v) {
            remap[v] = NPH_U32_MAX; // unreferenced until an index says otherwise
        }
        for (u32 i = 0; i < mesh->indices.len; ++i) {
            remap[mesh->indices.data[i]] = 0U;
        }
        // Survivors are compacted in place in their original order (the
        // write position never passes the read position).
        u32 count = 0U;
        for (u32 v = 0; v < n && r == NPH_RESULT_OK; ++v) {
            if (remap[v] == NPH_U32_MAX) {
                continue;
            }
            const u32 *found = (const u32 *)nph_hashmap_find(&map, &mesh->vertices.data[v]);
            if (found != NULL) {
                remap[v] = *found;
                continue;
            }
            r = nph_hashmap_insert(&map, &mesh->vertices.data[v], &count);
            mesh->vertices.data[count] = mesh->vertices.data[v];
            remap[v] = count++;
        }
        if (r == NPH_RESULT_OK) {
            mesh->vertices.len = count;
            for (u32 i = 0; i < mesh->indices.len; ++i) {
                mesh->indices.data[i] = remap[mesh->indices.data[i]];
            }
        }
    }
    if (remap != NULL) {
        NPH_FREE_ARRAY(mesh->alloc, u32, remap, n);
    }
    nph_hashmap_free(&map);
    return r;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static u16 quantize_unorm16(f32 v, f32 lo, f32 extent)
{
    if (extent <= 0.0F) {
        return 0U;
    }
    const f32 t = nph_f32_saturate((v - lo) / extent);
    return (u16)(t * 65535.0F + 0.5F);
}

static NphAABB bounds_of(const NphVec3 *positions, const u32 *indices, u32 count, bool indexed)
{
    if (count == 0U) {
        return nph_aabb(nph_vec3_zero(), nph_vec3_zero());
    }
    NphAABB b = nph_aabb_empty();
    for (u32 i = 0; i < count; ++i) {
        b = nph_aabb_expand(b, positions[indexed ? indices[i] : i]);
    }
    return b;
}

NphResult cook_mesh_write(const CookMesh *mesh, const CookMeshlets *meshlets,
                          const CookMeshSettings *settings, const struct CookVoxelMaterials *voxels,
                          NphJobSystem *jobs, NphAssetWriter *writer, CookMeshStats *stats)
{
    NPH_ASSERT(mesh != NULL && meshlets != NULL && settings != NULL && writer != NULL);
    const NphLzLevel lz = settings->lz;
    if (stats != NULL) {
        stats->lod_levels = 1U;
        stats->lods_ns = 0U;
        stats->blobs_ns = 0U;
    }
    NPH_TRY(cook_mesh_validate(mesh));
    if (meshlets->bounds.len != meshlets->meshlets.len || meshlets->triangles.len % 3U != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *alloc = mesh->alloc;
    const u32 vn = mesh->vertices.len;
    const u32 in = mesh->indices.len;
    const u32 sn = mesh->submeshes.len;
    const u32 mn = meshlets->meshlets.len;
    nph_asset_writer_set_lz_level(writer, lz);

    // The quantisation box: the full-precision bounds.
    NphAABB box = nph_aabb_empty();
    NPH_ARRAY_FOREACH (CookVertex, v, &mesh->vertices) {
        box = nph_aabb_expand(box, v->position);
    }
    if (vn == 0U) {
        box = nph_aabb(nph_vec3_zero(), nph_vec3_zero());
    }
    const NphVec3 extent = nph_aabb_size(box);

    NphMeshHeader h;
    memset(&h, 0, sizeof h);
    h.vertex_count = vn;
    h.index_count = in;
    h.submesh_count = sn;
    h.meshlet_count = mn;
    h.meshlet_vertex_count = meshlets->vertices.len;
    h.meshlet_triangle_count = meshlets->triangles.len / 3U;
    h.flags =
        ((mesh->flags & (u32)COOK_MESH_HAS_TANGENTS) != 0U ? (u32)NPH_MESH_FLAG_HAS_TANGENTS : 0U) |
        ((mesh->flags & (u32)COOK_MESH_HAS_UV0) != 0U ? (u32)NPH_MESH_FLAG_HAS_UV0 : 0U) |
        ((mesh->flags & (u32)COOK_MESH_HAS_COLORS) != 0U ? (u32)NPH_MESH_FLAG_HAS_COLORS : 0U);
    h.position_offset = box.min;
    h.position_scale = extent;

    NphVec3 *positions = NPH_ALLOC_ARRAY(alloc, NphVec3, nph_max_u32(vn, 1U));
    NphMeshPosition *posn = NPH_ALLOC_ARRAY(alloc, NphMeshPosition, nph_max_u32(vn, 1U));
    NphMeshAttrib *attr = NPH_ALLOC_ARRAY(alloc, NphMeshAttrib, nph_max_u32(vn, 1U));
    NphSubmesh *subs = NPH_ALLOC_ARRAY_ZEROED(alloc, NphSubmesh, nph_max_u32(sn, 1U));
    NphMeshletBounds *mbounds = NPH_ALLOC_ARRAY(alloc, NphMeshletBounds, nph_max_u32(mn, 1U));
    NphResult r = NPH_RESULT_OK;
    if (positions == NULL || posn == NULL || attr == NULL || subs == NULL || mbounds == NULL) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    }
    if (r == NPH_RESULT_OK) {
        for (u32 i = 0; i < vn; ++i) {
            const CookVertex *v = &mesh->vertices.data[i];
            posn[i].x = quantize_unorm16(v->position.x, box.min.x, extent.x);
            posn[i].y = quantize_unorm16(v->position.y, box.min.y, extent.y);
            posn[i].z = quantize_unorm16(v->position.z, box.min.z, extent.z);
            posn[i].reserved = 0U;
            positions[i] = nph_mesh_dequantize_position(&h, posn[i]);
            attr[i].normal =
                nph_oct_encode_snorm16(nph_vec3_normalize_or(v->normal, nph_vec3_unit_z()));
            const NphVec3 t = nph_vec3_normalize_or(nph_vec4_xyz(v->tangent), nph_vec3_unit_x());
            attr[i].tangent =
                nph_pack_snorm8x4(nph_vec4_from_vec3(t, v->tangent.w < 0.0F ? -1.0F : 1.0F));
            attr[i].uv0 = nph_pack_f16x2(v->uv0);
            attr[i].color = (mesh->flags & (u32)COOK_MESH_HAS_COLORS) != 0U
                                ? nph_pack_unorm8x4(v->color)
                                : 0xFFFFFFFFU;
        }
        // Every bound comes from the dequantised positions, the ones the
        // GPU culls and rasterises; the sphere grows by one quantisation
        // step to absorb the GPU's own rounding of the dequantisation.
        h.bounds = bounds_of(positions, NULL, vn, false);
        h.sphere = nph_sphere_from_points(positions, vn);
        h.sphere.radius += nph_vec3_max_component(extent) * (1.0F / 65535.0F);
        for (u32 m = 0; m < mn; ++m) {
            const NphMeshlet *ml = &meshlets->meshlets.data[m];
            if (ml->submesh >= sn ||
                (u64)ml->vertex_offset + ml->vertex_count > meshlets->vertices.len ||
                ((u64)ml->triangle_offset + ml->triangle_count) * 3U > meshlets->triangles.len) {
                r = NPH_RESULT_INVALID_ARGUMENT;
                break;
            }
            for (u32 k = 0; k < ml->vertex_count; ++k) {
                if (meshlets->vertices.data[ml->vertex_offset + k] >= vn) {
                    r = NPH_RESULT_INVALID_ARGUMENT;
                }
            }
            if (r != NPH_RESULT_OK) {
                break;
            }
            cook_meshlet_bounds(positions, meshlets->vertices.data + ml->vertex_offset,
                                meshlets->triangles.data + (u64)ml->triangle_offset * 3U,
                                ml->triangle_count, &mbounds[m]);
        }
    }
    if (r == NPH_RESULT_OK) {
        // Meshlets were built per submesh in submesh order: one pass finds
        // each submesh's run.
        u32 m = 0U;
        for (u32 s = 0; s < sn; ++s) {
            const CookSubmesh *cs = &mesh->submeshes.data[s];
            subs[s].index_offset = cs->index_offset;
            subs[s].index_count = cs->index_count;
            subs[s].material_slot = cs->material_slot;
            // An empty mesh has no index array at all: no offset into it.
            const u32 *first = cs->index_count != 0U ? mesh->indices.data + cs->index_offset : NULL;
            subs[s].bounds = bounds_of(positions, first, cs->index_count, true);
            subs[s].meshlet_offset = m;
            while (m < mn && meshlets->meshlets.data[m].submesh == s) {
                ++m;
            }
            subs[s].meshlet_count = m - subs[s].meshlet_offset;
        }
        if (m != mn) {
            r = NPH_RESULT_INVALID_ARGUMENT; // meshlets out of submesh order
        }
    }
    // The levels of detail (step 8): every level's clusters replace level
    // 0's in the meshlet blobs, level 0's first and unchanged.
    NphRenderClusterLods lods;
    nph_render_cluster_lods_init(&lods, alloc);
    bool with_lods = false;
    if (r == NPH_RESULT_OK && !settings->no_lods && mn > 1U) {
        const u64 lods_start = nph_time_now_ns();
        r = cook_mesh_build_lods(mesh, meshlets, positions, attr, mbounds, settings, voxels, jobs,
                                 &lods);
        with_lods = r == NPH_RESULT_OK && lods.levels > 1U;
        if (stats != NULL) {
            stats->lods_ns = nph_time_now_ns() - lods_start;
        }
    }
    const u64 blobs_start = nph_time_now_ns();
    // The records written: level 0's, or every level's (the same bytes as
    // the renderer's records, render/cluster_lod.h).
    const void *out_meshlets = meshlets->meshlets.data;
    const void *out_bounds = mbounds;
    const void *out_vertices = meshlets->vertices.data;
    const void *out_triangles = meshlets->triangles.data;
    u64 meshlet_bytes = NPH_ARRAY_BYTES(&meshlets->meshlets);
    u64 vertex_bytes = NPH_ARRAY_BYTES(&meshlets->vertices);
    u64 triangle_bytes = NPH_ARRAY_BYTES(&meshlets->triangles);
    if (with_lods) {
        const NphRenderMeshlets *c = &lods.clusters;
        h.meshlet_count = c->meshlets.len;
        h.meshlet_vertex_count = c->vertices.len;
        h.meshlet_triangle_count = c->triangles.len / 3U;
        out_meshlets = c->meshlets.data;
        out_bounds = c->bounds.data;
        out_vertices = c->vertices.data;
        out_triangles = c->triangles.data;
        meshlet_bytes = NPH_ARRAY_BYTES(&c->meshlets);
        vertex_bytes = NPH_ARRAY_BYTES(&c->vertices);
        triangle_bytes = NPH_ARRAY_BYTES(&c->triangles);
        if (stats != NULL) {
            stats->lod_levels = lods.levels;
            stats->lod_coarsest_triangles = 0U;
            stats->lod_coarsest_clusters = 0U;
            stats->voxel_clusters = 0U;
            for (u32 k = 0; k < lods.lods.len; ++k) {
                const bool voxel = c->meshlets.data[k].vertex_count == 0U;
                stats->voxel_clusters += voxel ? 1U : 0U;
                if (lods.lods.data[k].level == lods.levels - 1U) {
                    stats->lod_coarsest_clusters += 1U;
                    stats->lod_coarsest_triangles +=
                        voxel ? 0U : c->meshlets.data[k].triangle_count;
                }
            }
            stats->voxel_bricks = lods.bricks.len;
            stats->voxels = lods.voxels.len;
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_HEADER, 0U, &h, sizeof h,
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_POSITIONS, 0U, posn,
                                      (u64)vn * sizeof *posn, NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_ATTRIBS, 0U, attr,
                                      (u64)vn * sizeof *attr, NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_INDICES, 0U, mesh->indices.data,
                                      (u64)in * sizeof(u32), NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_SUBMESHES, 0U, subs,
                                      (u64)sn * sizeof *subs, NPH_ASSET_CODEC_NONE, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_MESHLETS, 0U, out_meshlets,
                                      meshlet_bytes, NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_MESHLET_BOUNDS, 0U, out_bounds,
                                      (u64)h.meshlet_count * sizeof *mbounds, NPH_ASSET_CODEC_LZ,
                                      0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_MESHLET_VERTICES, 0U, out_vertices,
                                      vertex_bytes, NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_MESHLET_TRIS, 0U, out_triangles,
                                      triangle_bytes, NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK && with_lods) {
        NphMeshLodHeader lh;
        memset(&lh, 0, sizeof lh);
        lh.levels = lods.levels;
        lh.level0_meshlets = lods.level0_clusters;
        lh.group_count = lods.groups.len;
        lh.group_cluster_count = lods.group_clusters.len;
        lh.node_count = lods.nodes.len;
        lh.brick_count = lods.bricks.len;
        lh.voxel_count = lods.voxels.len;
        r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_HEADER, 0U, &lh, sizeof lh,
                                      NPH_ASSET_CODEC_NONE, 0U);
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_CLUSTERS, 0U, lods.lods.data,
                                          NPH_ARRAY_BYTES(&lods.lods), NPH_ASSET_CODEC_LZ, 0U);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_GROUPS, 0U, lods.groups.data,
                                          NPH_ARRAY_BYTES(&lods.groups), NPH_ASSET_CODEC_LZ, 0U);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(
                writer, NPH_MESH_TAG_LOD_MEMBERS, 0U, lods.group_clusters.data,
                NPH_ARRAY_BYTES(&lods.group_clusters), NPH_ASSET_CODEC_LZ, 0U);
        }
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_LOD_NODES, 0U, lods.nodes.data,
                                          NPH_ARRAY_BYTES(&lods.nodes), NPH_ASSET_CODEC_LZ, 0U);
        }
        if (r == NPH_RESULT_OK && lods.bricks.len != 0U) {
            r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_VOXEL_BRICKS, 0U, lods.bricks.data,
                                          NPH_ARRAY_BYTES(&lods.bricks), NPH_ASSET_CODEC_LZ, 0U);
        }
        if (r == NPH_RESULT_OK && lods.voxels.len != 0U) {
            r = nph_asset_writer_add_blob(writer, NPH_MESH_TAG_VOXELS, 0U, lods.voxels.data,
                                          NPH_ARRAY_BYTES(&lods.voxels), NPH_ASSET_CODEC_LZ, 0U);
        }
    }
    if (stats != NULL) {
        stats->blobs_ns = nph_time_now_ns() - blobs_start;
    }
    nph_render_cluster_lods_free(&lods);
    if (positions != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, positions, nph_max_u32(vn, 1U));
    }
    if (posn != NULL) {
        NPH_FREE_ARRAY(alloc, NphMeshPosition, posn, nph_max_u32(vn, 1U));
    }
    if (attr != NULL) {
        NPH_FREE_ARRAY(alloc, NphMeshAttrib, attr, nph_max_u32(vn, 1U));
    }
    if (subs != NULL) {
        NPH_FREE_ARRAY(alloc, NphSubmesh, subs, nph_max_u32(sn, 1U));
    }
    if (mbounds != NULL) {
        NPH_FREE_ARRAY(alloc, NphMeshletBounds, mbounds, nph_max_u32(mn, 1U));
    }
    return r;
}

// ---------------------------------------------------------------------------
// The pipeline
// ---------------------------------------------------------------------------

NphResult cook_mesh_process(CookMesh *mesh, const CookMeshSettings *settings_in,
                            const struct CookVoxelMaterials *voxels, NphJobSystem *jobs,
                            NphAssetWriter *writer, CookMeshStats *stats)
{
    NPH_ASSERT(mesh != NULL && writer != NULL);
    CookMeshSettings settings;
    if (settings_in != NULL) {
        settings = *settings_in;
    } else {
        cook_mesh_settings_default(&settings);
    }
    CookMeshStats st;
    memset(&st, 0, sizeof st);
    st.source_vertices = mesh->vertices.len;
    st.source_triangles = mesh->indices.len / 3U;
    const u64 start = nph_time_now_ns();
    NPH_TRY(cook_mesh_validate(mesh));
    NPH_TRY(cook_mesh_remove_degenerate(mesh));
    if ((mesh->flags & (u32)COOK_MESH_HAS_NORMALS) == 0U) {
        NPH_TRY(cook_mesh_generate_normals(mesh, COOK_NORMALS_FLAT));
    }
    NPH_TRY(cook_mesh_weld(mesh));
    if ((mesh->flags & (u32)COOK_MESH_HAS_UV0) != 0U &&
        (mesh->flags & (u32)COOK_MESH_HAS_TANGENTS) == 0U) {
        NPH_TRY(cook_mesh_generate_tangents(mesh));
    }
    st.acmr_before = cook_mesh_acmr(mesh->indices.data, mesh->indices.len, 32U);
    const u64 optimize_start = nph_time_now_ns();
    if (!settings.no_optimize) {
        NPH_TRY(cook_mesh_optimize(mesh));
    }
    st.acmr_after = cook_mesh_acmr(mesh->indices.data, mesh->indices.len, 32U);
    const u64 meshlets_start = nph_time_now_ns();
    CookMeshlets meshlets;
    cook_meshlets_init(&meshlets, mesh->alloc);
    NphResult r =
        cook_mesh_build_meshlets(mesh, settings.meshlet_vertices, settings.meshlet_triangles,
                                 settings.meshlet_cone_weight, &meshlets);
    st.prepare_ns = optimize_start - start;
    st.optimize_ns = meshlets_start - optimize_start;
    st.meshlets_ns = nph_time_now_ns() - meshlets_start;
    st.lod_levels = 1U;
    if (r == NPH_RESULT_OK) {
        r = cook_mesh_write(mesh, &meshlets, &settings, voxels, jobs, writer, &st);
    }
    st.total_ns = nph_time_now_ns() - start;
    st.vertices = mesh->vertices.len;
    st.triangles = mesh->indices.len / 3U;
    st.meshlets = meshlets.meshlets.len;
    if (st.lod_levels == 1U) {
        st.lod_coarsest_triangles = st.triangles;
        st.lod_coarsest_clusters = st.meshlets;
    }
    cook_meshlets_free(&meshlets);
    if (stats != NULL) {
        *stats = st;
    }
    return r;
}
