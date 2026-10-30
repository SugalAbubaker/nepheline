// SPDX-License-Identifier: Apache-2.0
// Cluster levels of detail (render/cluster_lod.h; docs/render.md "Levels of
// detail"): the simplifier's rules and the levels' structure, on meshes made
// here. CPU only.
//
// The cut tests select clusters as the renderer will (a cluster drawn where
// its maker's projected error is within the threshold and its consumer's is
// not), for cameras that see part of a mesh near and part far, so a cut mixes
// levels across the surface, and check that a closed mesh's cut is closed:
// every edge, welded by position, has exactly two triangles, one each way
// round.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>

#include <ntest/ntest.h>

#include "cluster_lod_internal.h"

#include <string.h>

// A test mesh: positions, an attribute record each (a chart id: vertices at
// one position on two charts are a seam), triangles, and submesh ranges.
typedef struct TestAttrib {
    u32 chart; // a material's or a chart's own vertices
    f32 u;
    f32 v;
} TestAttrib;

typedef struct TestMesh {
    NphAllocator *alloc;
    NPH_ARRAY(NphVec3) positions;
    NPH_ARRAY(TestAttrib) attribs;
    NphArrayU32 indices;
    NPH_ARRAY(NphRenderIndexRange) ranges;
} TestMesh;

static void mesh_init(TestMesh *m, NphAllocator *a)
{
    memset(m, 0, sizeof *m);
    m->alloc = a;
    NPH_ARRAY_INIT(&m->positions, a);
    NPH_ARRAY_INIT(&m->attribs, a);
    NPH_ARRAY_INIT(&m->indices, a);
    NPH_ARRAY_INIT(&m->ranges, a);
}

static void mesh_free(TestMesh *m)
{
    NPH_ARRAY_FREE(&m->positions);
    NPH_ARRAY_FREE(&m->attribs);
    NPH_ARRAY_FREE(&m->indices);
    NPH_ARRAY_FREE(&m->ranges);
}

static u32 mesh_vertex_uv(TestMesh *m, NphVec3 p, u32 chart, f32 u, f32 v)
{
    const TestAttrib at = {chart, u, v};
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->positions, p));
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->attribs, at));
    return m->positions.len - 1U;
}

static u32 mesh_vertex(TestMesh *m, NphVec3 p, u32 chart)
{
    const TestAttrib at = {chart, 0.0F, 0.0F};
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->positions, p));
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->attribs, at));
    return m->positions.len - 1U;
}

static void mesh_tri(TestMesh *m, u32 a, u32 b, u32 c)
{
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->indices, a));
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->indices, b));
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->indices, c));
}

// One submesh over every triangle so far.
static void mesh_one_range(TestMesh *m)
{
    const NphRenderIndexRange r = {0U, m->indices.len};
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->ranges, r));
}

// A torus of `rings` x `sides` quads, closed and seamless (each position
// one vertex), centred on the origin around the y axis.
static void make_torus(TestMesh *m, u32 rings, u32 sides, f32 big, f32 small)
{
    for (u32 i = 0; i < rings; ++i) {
        const f32 a = 2.0F * NPH_PI * (f32)i / (f32)rings;
        for (u32 j = 0; j < sides; ++j) {
            const f32 b = 2.0F * NPH_PI * (f32)j / (f32)sides;
            const f32 rr = big + small * nph_f32_cos(b);
            mesh_vertex(
                m, nph_vec3(rr * nph_f32_cos(a), small * nph_f32_sin(b), rr * nph_f32_sin(a)), 0U);
        }
    }
    for (u32 i = 0; i < rings; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const u32 i1 = (i + 1U) % rings;
            const u32 j1 = (j + 1U) % sides;
            const u32 v00 = i * sides + j;
            const u32 v10 = i1 * sides + j;
            const u32 v01 = i * sides + j1;
            const u32 v11 = i1 * sides + j1;
            mesh_tri(m, v00, v01, v11);
            mesh_tri(m, v00, v11, v10);
        }
    }
    mesh_one_range(m);
}

// The same torus with its UV layout's wrap: (rings + 1) x (sides + 1)
// vertices, u around the tube and v around the ring from 0 to 1, the last
// row and column at the first's positions with u or v 1 (a seam each way).
static void make_seamed_torus(TestMesh *m, u32 rings, u32 sides, f32 big, f32 small)
{
    for (u32 i = 0; i <= rings; ++i) {
        const f32 a = 2.0F * NPH_PI * (f32)(i % rings) / (f32)rings;
        for (u32 j = 0; j <= sides; ++j) {
            const f32 b = 2.0F * NPH_PI * (f32)(j % sides) / (f32)sides;
            const f32 rr = big + small * nph_f32_cos(b);
            mesh_vertex_uv(
                m, nph_vec3(rr * nph_f32_cos(a), small * nph_f32_sin(b), rr * nph_f32_sin(a)), 0U,
                (f32)j / (f32)sides, (f32)i / (f32)rings);
        }
    }
    const u32 row = sides + 1U;
    for (u32 i = 0; i < rings; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const u32 v00 = i * row + j;
            const u32 v10 = (i + 1U) * row + j;
            const u32 v01 = i * row + j + 1U;
            const u32 v11 = (i + 1U) * row + j + 1U;
            mesh_tri(m, v00, v01, v11);
            mesh_tri(m, v00, v11, v10);
        }
    }
    mesh_one_range(m);
}

// A flat n x n grid of quads in z = 0, open at its border.
static void make_grid(TestMesh *m, u32 n, f32 size)
{
    for (u32 y = 0; y <= n; ++y) {
        for (u32 x = 0; x <= n; ++x) {
            mesh_vertex(m, nph_vec3(size * (f32)x / (f32)n, size * (f32)y / (f32)n, 0.0F), 0U);
        }
    }
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const u32 v00 = y * (n + 1U) + x;
            mesh_tri(m, v00, v00 + 1U, v00 + n + 2U);
            mesh_tri(m, v00, v00 + n + 2U, v00 + n + 1U);
        }
    }
    mesh_one_range(m);
}

// A torus of two submeshes, the rings split in half, each half with its own
// vertices (as a cooker gives each primitive its own): the halves meet at
// two rings of shared positions.
static void make_two_material_torus(TestMesh *m, u32 rings, u32 sides, f32 big, f32 small)
{
    const u32 half = rings / 2U;
    for (u32 s = 0; s < 2U; ++s) {
        const u32 base = m->positions.len;
        for (u32 i = 0; i <= half; ++i) {
            const u32 ring = (s * half + i) % rings;
            const f32 a = 2.0F * NPH_PI * (f32)ring / (f32)rings;
            for (u32 j = 0; j < sides; ++j) {
                const f32 b = 2.0F * NPH_PI * (f32)j / (f32)sides;
                const f32 rr = big + small * nph_f32_cos(b);
                mesh_vertex(
                    m, nph_vec3(rr * nph_f32_cos(a), small * nph_f32_sin(b), rr * nph_f32_sin(a)),
                    s);
            }
        }
        const u32 first = m->indices.len;
        for (u32 i = 0; i < half; ++i) {
            for (u32 j = 0; j < sides; ++j) {
                const u32 j1 = (j + 1U) % sides;
                const u32 v00 = base + i * sides + j;
                const u32 v10 = base + (i + 1U) * sides + j;
                const u32 v01 = base + i * sides + j1;
                const u32 v11 = base + (i + 1U) * sides + j1;
                mesh_tri(m, v00, v01, v11);
                mesh_tri(m, v00, v11, v10);
            }
        }
        const NphRenderIndexRange r = {first, m->indices.len - first};
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->ranges, r));
    }
}

// `count` separate small quads scattered in a box (foliage, debris).
static void make_quads(TestMesh *m, u32 count, u64 seed)
{
    NphPcg32 rng;
    nph_pcg32_seed(&rng, seed, 1U);
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c = nph_vec3_scale(nph_pcg32_next_vec3(&rng), 10.0F);
        const f32 s = 0.05F + 0.05F * nph_pcg32_next_f32(&rng);
        const u32 a = mesh_vertex(m, nph_vec3_add(c, nph_vec3(-s, -s, 0.0F)), 0U);
        const u32 b = mesh_vertex(m, nph_vec3_add(c, nph_vec3(s, -s, 0.0F)), 0U);
        const u32 d = mesh_vertex(m, nph_vec3_add(c, nph_vec3(s, s, 0.0F)), 0U);
        const u32 e = mesh_vertex(m, nph_vec3_add(c, nph_vec3(-s, s, 0.0F)), 0U);
        mesh_tri(m, a, b, d);
        mesh_tri(m, a, d, e);
    }
    mesh_one_range(m);
}

// The mesh's meshlets and levels; `uvs` as the input's (NULL: none).
static NphResult build_lods_uvs(const TestMesh *m, NphAllocator *a, NphRenderMeshlets *meshlets,
                                NphRenderClusterLods *lods, const NphVec2 *uvs)
{
    nph_render_meshlets_init(meshlets, a);
    nph_render_cluster_lods_init(lods, a);
    NphRenderMeshletInput mi;
    memset(&mi, 0, sizeof mi);
    mi.positions = m->positions.data;
    mi.vertex_count = m->positions.len;
    mi.indices = m->indices.data;
    mi.index_count = m->indices.len;
    mi.ranges = m->ranges.data;
    mi.range_count = m->ranges.len;
    mi.cone_weight = 0.25F;
    mi.scratch = a;
    NPH_TRY(nph_render_meshlets_build(&mi, meshlets));
    NphRenderClusterLodInput in;
    memset(&in, 0, sizeof in);
    in.positions = m->positions.data;
    in.attribs = m->attribs.data;
    in.attrib_stride = (u32)sizeof(TestAttrib);
    in.uvs = uvs;
    in.vertex_count = m->positions.len;
    in.indices = m->indices.data;
    in.index_count = m->indices.len;
    in.ranges = m->ranges.data;
    in.range_count = m->ranges.len;
    in.meshlets = meshlets;
    in.cone_weight = 0.25F;
    in.scratch = a;
    return nph_render_cluster_lods_build(&in, lods);
}

static NphResult build_lods(const TestMesh *m, NphAllocator *a, NphRenderMeshlets *meshlets,
                            NphRenderClusterLods *lods)
{
    return build_lods_uvs(m, a, meshlets, lods, NULL);
}

static u32 cluster_triangles(const NphRenderClusterLods *l, u32 level)
{
    u32 n = 0U;
    for (u32 i = 0; i < l->lods.len; ++i) {
        if (l->lods.data[i].level == level) {
            n += l->clusters.meshlets.data[i].triangle_count;
        }
    }
    return n;
}

// The renderer's choice, on the CPU: which clusters a camera draws.
static f32 group_pixels(const NphRenderClusterLods *l, u32 g, NphVec3 camera, f32 ppu)
{
    if (g == NPH_RENDER_LOD_NONE) {
        return 0.0F;
    }
    const NphRenderLodGroup *rg = &l->groups.data[g];
    if (rg->error == NPH_F32_MAX) {
        return NPH_F32_MAX;
    }
    return nph_render_lod_projected_error(rg->center, rg->radius, rg->error, 1.0F, camera, ppu,
                                          0.01F);
}

static bool drawn(const NphRenderClusterLods *l, u32 c, NphVec3 camera, f32 ppu, f32 threshold)
{
    const NphRenderClusterLod *lod = &l->lods.data[c];
    return group_pixels(l, lod->group, camera, ppu) <= threshold &&
           group_pixels(l, lod->parent, camera, ppu) > threshold;
}

// The cut's triangles as mesh vertices.
static void cut_triangles(const NphRenderClusterLods *l, NphVec3 camera, f32 ppu, f32 threshold,
                          NphArrayU32 *out, u32 *levels_seen)
{
    NPH_ARRAY_CLEAR(out);
    *levels_seen = 0U;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        if (!drawn(l, c, camera, ppu, threshold)) {
            continue;
        }
        *levels_seen |= 1U << nph_min_u32(l->lods.data[c].level, 31U);
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        for (u32 t = 0; t < (u32)m->triangle_count * 3U; ++t) {
            const u8 corner = l->clusters.triangles.data[(u64)m->triangle_offset * 3U + t];
            NPH_IGNORE_RESULT(
                NPH_ARRAY_PUSH(out, l->clusters.vertices.data[m->vertex_offset + corner]));
        }
    }
}

// Whether triangles (mesh vertices) welded by position close up: every
// edge twice, once each way round. Also the welded Euler characteristic.
static bool closed(NphAllocator *a, const NphVec3 *positions, const NphArrayU32 *tris,
                   i32 *out_euler)
{
    NphHashMapDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.key_size = (u32)sizeof(NphVec3);
    pd.val_size = (u32)sizeof(u32);
    NphHashMap classes;
    NPH_IGNORE_RESULT(nph_hashmap_init(&classes, a, &pd));
    NphArrayU32 cls;
    NPH_ARRAY_INIT(&cls, a);
    for (u32 i = 0; i < tris->len; ++i) {
        const NphVec3 p = positions[tris->data[i]];
        const u32 *found = NPH_HASHMAP_FIND(&classes, u32, &p);
        u32 c = 0U;
        if (found != NULL) {
            c = *found;
        } else {
            c = nph_hashmap_len(&classes);
            NPH_IGNORE_RESULT(nph_hashmap_insert(&classes, &p, &c));
        }
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&cls, c));
    }
    // Directed edges: each must meet its reverse exactly once.
    NphHashMap edges;
    NPH_IGNORE_RESULT(nph_hashmap_init(&edges, a, &(NphHashMapDesc){8U, 4U, NULL, NULL, 0U}));
    bool ok = true;
    for (u32 t = 0; t + 2U < cls.len && ok; t += 3U) {
        for (u32 k = 0; k < 3U; ++k) {
            const u32 x = cls.data[t + k];
            const u32 y = cls.data[t + (k + 1U) % 3U];
            if (x == y) {
                ok = false;
                break;
            }
            const u64 key = ((u64)x << 32U) | y;
            u32 *n = NPH_HASHMAP_FIND(&edges, u32, &key);
            if (n != NULL) {
                ok = false; // the same edge the same way round twice
                break;
            }
            const u32 one = 1U;
            NPH_IGNORE_RESULT(nph_hashmap_insert(&edges, &key, &one));
        }
    }
    const u32 directed = nph_hashmap_len(&edges);
    NphHashMapIter it = nph_hashmap_iter(&edges);
    const void *k = NULL;
    void *v = NULL;
    while (ok && nph_hashmap_next(&it, &k, &v)) {
        const u64 key = *(const u64 *)k;
        const u64 rev = (key << 32U) | (key >> 32U);
        if (!nph_hashmap_contains(&edges, &rev)) {
            ok = false;
        }
    }
    *out_euler = (i32)nph_hashmap_len(&classes) - (i32)(directed / 2U) + (i32)(cls.len / 3U);
    nph_hashmap_free(&edges);
    nph_hashmap_free(&classes);
    NPH_ARRAY_FREE(&cls);
    return ok;
}

// How much wider three coordinates on a wrapping [0, 1] span end to end
// than around the wrap: the end-to-end span less one minus the widest gap
// between them (the arc that holds all three the short way).
static f32 wrap_excess(const f32 x[3])
{
    f32 a = nph_f32_min(nph_f32_min(x[0], x[1]), x[2]);
    f32 c = nph_f32_max(nph_f32_max(x[0], x[1]), x[2]);
    const f32 b = x[0] + x[1] + x[2] - a - c;
    const f32 gap = nph_f32_max(nph_f32_max(b - a, c - b), 1.0F - (c - a));
    return (c - a) - (1.0F - gap);
}

typedef struct LodFixture {
    NphDebugAllocator da;
    NphAllocator *a;
    TestMesh mesh;
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
} LodFixture;

typedef LodFixture ClusterLod;

NTEST_SETUP(ClusterLod)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
    mesh_init(&f->mesh, f->a);
    nph_render_meshlets_init(&f->meshlets, f->a);
    nph_render_cluster_lods_init(&f->lods, f->a);
}

NTEST_TEARDOWN(ClusterLod)
{
    nph_render_cluster_lods_free(&f->lods);
    nph_render_meshlets_free(&f->meshlets);
    mesh_free(&f->mesh);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// ---------------------------------------------------------------------------
// The simplifier
// ---------------------------------------------------------------------------

NTEST(ClusterLod, a_planes_quadric_measures_the_distance_from_it)
{
    LodQuadric q;
    memset(&q, 0, sizeof q);
    lod_quadric_add_plane(&q, nph_vec3(0.0F, 0.0F, 1.0F), -2.0, 3.0);
    NTEST_EXPECT_NEAR_F64(lod_quadric_error(&q, nph_vec3(5.0F, -7.0F, 2.0F)), 0.0, 1e-12);
    NTEST_EXPECT_NEAR_F64(lod_quadric_error(&q, nph_vec3(1.0F, 1.0F, 5.0F)), 9.0, 1e-9);
    LodQuadric o;
    memset(&o, 0, sizeof o);
    lod_quadric_add_plane(&o, nph_vec3(1.0F, 0.0F, 0.0F), 0.0, 1.0);
    lod_quadric_add(&q, &o);
    // (3 * 3^2 + 1 * 1^2) / (3 + 1) at (1, 1, 5).
    NTEST_EXPECT_NEAR_F64(lod_quadric_error(&q, nph_vec3(1.0F, 1.0F, 5.0F)), 7.0, 1e-9);
}

NTEST_F(ClusterLod, a_flat_interior_collapses_to_its_locked_border_with_no_error)
{
    const u32 n = 12U;
    make_grid(&f->mesh, n, 1.0F);
    LodMesh m;
    NTEST_ASSERT_EQ_INT(lod_mesh_init(&m, f->a, f->mesh.positions.data, f->mesh.attribs.data,
                                      (u32)sizeof(TestAttrib), f->mesh.positions.len,
                                      f->mesh.indices.data, f->mesh.indices.len),
                        NPH_RESULT_OK);
    // The border's classes locked, as a group's shared boundary would be.
    u8 *locked = NPH_ALLOC_ARRAY_ZEROED(f->a, u8, m.class_count);
    for (u32 v = 0; v < f->mesh.positions.len; ++v) {
        const NphVec3 p = f->mesh.positions.data[v];
        if (p.x == 0.0F || p.y == 0.0F || p.x == 1.0F || p.y == 1.0F) {
            locked[m.class_of[v]] = 1U;
        }
    }
    const u32 tris = f->mesh.indices.len / 3U;
    u32 *sub = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, tris);
    LodSimplifyInput in;
    memset(&in, 0, sizeof in);
    in.mesh = &m;
    in.indices = f->mesh.indices.data;
    in.submeshes = sub;
    in.triangle_count = tris;
    in.locked = locked;
    in.target_triangles = 1U;
    in.scratch = f->a;
    in.cost_limit = NPH_F64_MAX;
    u32 kept = 0U;
    f32 error = 1.0F;
    NTEST_ASSERT_EQ_INT(lod_simplify(&in, &kept, &error), NPH_RESULT_OK);
    NTEST_LOG("%u triangles left, error %.9f", kept, (f64)error);
    // Every interior vertex gone: a triangulation of the 4n border vertices.
    NTEST_EXPECT_EQ_UINT(kept, 4U * n - 2U);
    NTEST_EXPECT_TRUE(error < 1e-5F); // single precision on a unit grid
    // Still facing +z, every one.
    for (u32 t = 0; t < kept; ++t) {
        const NphVec3 p0 = f->mesh.positions.data[f->mesh.indices.data[(u64)t * 3U]];
        const NphVec3 p1 = f->mesh.positions.data[f->mesh.indices.data[(u64)t * 3U + 1U]];
        const NphVec3 p2 = f->mesh.positions.data[f->mesh.indices.data[(u64)t * 3U + 2U]];
        NTEST_EXPECT_TRUE(nph_vec3_cross(nph_vec3_sub(p1, p0), nph_vec3_sub(p2, p0)).z > 0.0F);
    }
    NPH_FREE_ARRAY(f->a, u32, sub, tris);
    NPH_FREE_ARRAY(f->a, u8, locked, m.class_count);
    lod_mesh_free(&m, f->a);
}

NTEST_F(ClusterLod, a_curved_surface_simplifies_with_its_error_and_without_flips)
{
    make_torus(&f->mesh, 48U, 24U, 1.0F, 0.35F);
    LodMesh m;
    NTEST_ASSERT_EQ_INT(lod_mesh_init(&m, f->a, f->mesh.positions.data, NULL, 0U,
                                      f->mesh.positions.len, f->mesh.indices.data,
                                      f->mesh.indices.len),
                        NPH_RESULT_OK);
    u8 *locked = NPH_ALLOC_ARRAY_ZEROED(f->a, u8, m.class_count);
    const u32 tris = f->mesh.indices.len / 3U;
    u32 *sub = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, tris);
    LodSimplifyInput in;
    memset(&in, 0, sizeof in);
    in.mesh = &m;
    in.indices = f->mesh.indices.data;
    in.submeshes = sub;
    in.triangle_count = tris;
    in.locked = locked;
    in.target_triangles = tris / 8U;
    in.scratch = f->a;
    in.cost_limit = NPH_F64_MAX;
    u32 kept = 0U;
    f32 error = 0.0F;
    NTEST_ASSERT_EQ_INT(lod_simplify(&in, &kept, &error), NPH_RESULT_OK);
    NTEST_EXPECT_LE_UINT(kept, tris / 8U);
    NTEST_EXPECT_TRUE(error > 0.0F && error < 0.1F);
    // Every triangle still faces away from the tube's core circle.
    u32 flipped = 0U;
    for (u32 t = 0; t < kept; ++t) {
        const NphVec3 p0 = f->mesh.positions.data[f->mesh.indices.data[(u64)t * 3U]];
        const NphVec3 p1 = f->mesh.positions.data[f->mesh.indices.data[(u64)t * 3U + 1U]];
        const NphVec3 p2 = f->mesh.positions.data[f->mesh.indices.data[(u64)t * 3U + 2U]];
        const NphVec3 c = nph_vec3_scale(nph_vec3_add(nph_vec3_add(p0, p1), p2), 1.0F / 3.0F);
        const f32 ring = nph_f32_sqrt(c.x * c.x + c.z * c.z);
        const NphVec3 core = ring > 0.0F ? nph_vec3(c.x / ring, 0.0F, c.z / ring) : nph_vec3_zero();
        const NphVec3 out = nph_vec3_sub(c, core);
        const bool faces =
            nph_vec3_dot(nph_vec3_cross(nph_vec3_sub(p1, p0), nph_vec3_sub(p2, p0)), out) > 0.0F;
        if (!faces && flipped < 4U) {
            NTEST_LOG("inward: (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)", (f64)p0.x,
                      (f64)p0.y, (f64)p0.z, (f64)p1.x, (f64)p1.y, (f64)p1.z, (f64)p2.x, (f64)p2.y,
                      (f64)p2.z);
        }
        flipped += faces ? 0U : 1U;
    }
    NTEST_LOG("%u of %u triangles left, error %.5f, %u inward", kept, tris, (f64)error, flipped);
    NTEST_EXPECT_EQ_UINT(flipped, 0U);
    NPH_FREE_ARRAY(f->a, u32, sub, tris);
    NPH_FREE_ARRAY(f->a, u8, locked, m.class_count);
    lod_mesh_free(&m, f->a);
}

// ---------------------------------------------------------------------------
// The levels
// ---------------------------------------------------------------------------

// The triangles of every level of a torus around the y axis of radius 1
// that do not face away from the tube's core circle (into the tube, or
// across its surface).
static u32 torus_inward(const NphRenderClusterLods *l, const TestMesh *mesh)
{
    u32 inward = 0U;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        for (u32 t = 0; t < m->triangle_count; ++t) {
            NphVec3 p[3];
            for (u32 k = 0; k < 3U; ++k) {
                const u8 corner =
                    l->clusters.triangles.data[((u64)m->triangle_offset + t) * 3U + k];
                p[k] = mesh->positions.data[l->clusters.vertices.data[m->vertex_offset + corner]];
            }
            const NphVec3 cen =
                nph_vec3_scale(nph_vec3_add(nph_vec3_add(p[0], p[1]), p[2]), 1.0F / 3.0F);
            const f32 ring = nph_f32_sqrt(cen.x * cen.x + cen.z * cen.z);
            const NphVec3 core = nph_vec3(cen.x / ring, 0.0F, cen.z / ring);
            const NphVec3 n = nph_vec3_cross(nph_vec3_sub(p[1], p[0]), nph_vec3_sub(p[2], p[0]));
            inward += nph_vec3_dot(n, nph_vec3_sub(cen, core)) > 0.0F ? 0U : 1U;
        }
    }
    return inward;
}

NTEST_F(ClusterLod, the_levels_halve_down_to_a_root)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F); // 12,288 triangles
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NTEST_EXPECT_EQ_UINT(l->level0_clusters, f->meshlets.meshlets.len);
    NTEST_EXPECT_GE_UINT(l->levels, 6U);
    u32 prev = cluster_triangles(l, 0U);
    NTEST_EXPECT_EQ_UINT(prev, f->mesh.indices.len / 3U);
    for (u32 level = 1U; level < l->levels; ++level) {
        const u32 tris = cluster_triangles(l, level);
        NTEST_LOG("level %u: %u triangles", level, tris);
        NTEST_EXPECT_LT_UINT(tris, prev);
        NTEST_EXPECT_GE_UINT(tris * 10U, prev * 4U); // at most 60% off a level
        prev = tris;
    }
    // Level 0 is the input's meshlets, unchanged.
    NTEST_EXPECT_EQ_INT(memcmp(l->clusters.meshlets.data, f->meshlets.meshlets.data,
                               f->meshlets.meshlets.len * sizeof(NphRenderMeshlet)),
                        0);
    NTEST_EXPECT_LE_UINT(cluster_triangles(l, l->levels - 1U),
                         2U * NPH_RENDER_MESHLET_MAX_TRIANGLES);
    // Every level's triangles face away from the tube's core circle.
    NTEST_EXPECT_EQ_UINT(torus_inward(l, &f->mesh), 0U);
}

NTEST_F(ClusterLod, every_cluster_has_one_maker_and_one_consumer)
{
    make_torus(&f->mesh, 96U, 32U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    const u32 clusters = l->clusters.meshlets.len;
    NTEST_ASSERT_EQ_UINT(l->lods.len, clusters);
    NTEST_ASSERT_EQ_UINT(l->clusters.bounds.len, clusters);
    // group_clusters lists every cluster once, each in its consumer's run.
    u32 *seen = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, clusters);
    for (u32 g = 0; g < l->groups.len; ++g) {
        const NphRenderLodGroup *rg = &l->groups.data[g];
        NTEST_ASSERT_LE_UINT(rg->cluster_offset + rg->cluster_count, l->group_clusters.len);
        for (u32 i = 0; i < rg->cluster_count; ++i) {
            const u32 c = l->group_clusters.data[rg->cluster_offset + i];
            NTEST_ASSERT_LT_UINT(c, clusters);
            seen[c] += 1U;
            NTEST_EXPECT_EQ_UINT(l->lods.data[c].parent, g);
            NTEST_EXPECT_EQ_UINT(l->lods.data[c].level, rg->level);
        }
    }
    for (u32 c = 0; c < clusters; ++c) {
        NTEST_EXPECT_EQ_UINT(seen[c], 1U);
        const NphRenderClusterLod *lod = &l->lods.data[c];
        if (c < l->level0_clusters) {
            NTEST_EXPECT_EQ_UINT(lod->group, NPH_RENDER_LOD_NONE);
            NTEST_EXPECT_EQ_UINT(lod->level, 0U);
        } else {
            NTEST_ASSERT_LT_UINT(lod->group, l->groups.len);
            NTEST_EXPECT_EQ_UINT(lod->level, l->groups.data[lod->group].level + 1U);
        }
    }
    // The root groups last (error the largest float, the coarsest level),
    // consuming the coarsest clusters, a few each.
    u32 first_root = l->groups.len;
    while (first_root > 0U && l->groups.data[first_root - 1U].error == NPH_F32_MAX) {
        --first_root;
    }
    NTEST_ASSERT_LT_UINT(first_root, l->groups.len);
    for (u32 g = 0; g < l->groups.len; ++g) {
        const NphRenderLodGroup *rg = &l->groups.data[g];
        NTEST_EXPECT_TRUE((rg->error == NPH_F32_MAX) == (g >= first_root));
        if (g >= first_root) {
            NTEST_EXPECT_EQ_UINT(rg->level, l->levels - 1U);
            NTEST_EXPECT_LE_UINT(rg->cluster_count, 2U * 8U);
        }
    }
    for (u32 c = 0; c < clusters; ++c) {
        const bool coarsest = l->lods.data[c].level == l->levels - 1U;
        NTEST_EXPECT_TRUE((l->lods.data[c].parent >= first_root) == coarsest);
    }
    NPH_FREE_ARRAY(f->a, u32, seen, clusters);
}

// Whether sphere (`c`, `r`) holds sphere (`pc`, `pr`), in doubles: the
// hierarchy's spheres hold their parts exactly (the renderer's prunes bound
// the parts by them).
static bool holds(NphVec3 c, f32 r, NphVec3 pc, f32 pr)
{
    const f64 dx = (f64)pc.x - (f64)c.x;
    const f64 dy = (f64)pc.y - (f64)c.y;
    const f64 dz = (f64)pc.z - (f64)c.z;
    return nph_f64_sqrt(dx * dx + dy * dy + dz * dz) + (f64)pr <= (f64)r;
}

// The smallest error of the groups that made a group's clusters (0 for
// level 0's).
static f32 smallest_maker_error(const NphRenderClusterLods *l, u32 g)
{
    const NphRenderLodGroup *rg = &l->groups.data[g];
    f32 e = NPH_F32_MAX;
    for (u32 i = 0; i < rg->cluster_count; ++i) {
        const u32 maker = l->lods.data[l->group_clusters.data[rg->cluster_offset + i]].group;
        e = nph_f32_min(e, maker == NPH_RENDER_LOD_NONE ? 0.0F : l->groups.data[maker].error);
    }
    return e;
}

NTEST_F(ClusterLod, errors_never_fall_and_spheres_hold_their_makers)
{
    make_torus(&f->mesh, 96U, 32U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    for (u32 c = l->level0_clusters; c < l->lods.len; ++c) {
        const NphRenderClusterLod *lod = &l->lods.data[c];
        const NphRenderLodGroup *maker = &l->groups.data[lod->group];
        const NphRenderLodGroup *consumer = &l->groups.data[lod->parent];
        NTEST_EXPECT_TRUE(consumer->error >= maker->error);
        NTEST_EXPECT_TRUE(holds(consumer->center, consumer->radius, maker->center, maker->radius));
        // The cluster's own geometry lies in its maker's sphere.
        const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
        NTEST_EXPECT_TRUE(nph_vec3_distance(b->center, maker->center) <=
                          maker->radius * (1.0F + 1e-4F) + 1e-5F);
    }
    // Level 0's clusters lie in their consumers' spheres.
    for (u32 c = 0; c < l->level0_clusters; ++c) {
        const NphRenderLodGroup *consumer = &l->groups.data[l->lods.data[c].parent];
        const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
        NTEST_EXPECT_TRUE(holds(consumer->center, consumer->radius, b->center, b->radius));
    }
}

// The distance of `p` from the torus around the y axis of radii `big` and
// `small`.
static f32 torus_distance(NphVec3 p, f32 big, f32 small)
{
    const f32 ring = nph_f32_sqrt(p.x * p.x + p.z * p.z) - big;
    return nph_f32_abs(nph_f32_sqrt(ring * ring + p.y * p.y) - small);
}

#define LEVEL0_CHORD 0.0015F

NTEST_F(ClusterLod, a_groups_error_bounds_how_far_its_clusters_stray)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    // Per level: the farthest a point of its triangles (corners, edge
    // midpoints, centroids) lies from the true surface, against the largest
    // error its makers claim.
    for (u32 level = 1U; level < l->levels; ++level) {
        f32 stray = 0.0F;
        f32 claimed = 0.0F;
        f32 worst_ratio = 0.0F;
        for (u32 c = l->level0_clusters; c < l->lods.len; ++c) {
            if (l->lods.data[c].level != level) {
                continue;
            }
            const f32 error = l->groups.data[l->lods.data[c].group].error;
            claimed = nph_f32_max(claimed, error);
            const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
            f32 here = 0.0F;
            for (u32 t = 0; t < m->triangle_count; ++t) {
                NphVec3 p[3];
                for (u32 k = 0; k < 3U; ++k) {
                    const u8 corner =
                        l->clusters.triangles.data[((u64)m->triangle_offset + t) * 3U + k];
                    p[k] = f->mesh.positions
                               .data[l->clusters.vertices.data[m->vertex_offset + corner]];
                }
                const NphVec3 samples[4] = {
                    nph_vec3_scale(nph_vec3_add(p[0], p[1]), 0.5F),
                    nph_vec3_scale(nph_vec3_add(p[1], p[2]), 0.5F),
                    nph_vec3_scale(nph_vec3_add(p[2], p[0]), 0.5F),
                    nph_vec3_scale(nph_vec3_add(nph_vec3_add(p[0], p[1]), p[2]), 1.0F / 3.0F)};
                for (u32 k = 0; k < 4U; ++k) {
                    here = nph_f32_max(here, torus_distance(samples[k], 1.0F, 0.35F));
                }
            }
            stray = nph_f32_max(stray, here);
            // Measured from the level-0 mesh, which itself lies up to its
            // chords' sagitta (0.0015 here) from the true torus.
            if (error > 0.0F) {
                worst_ratio = nph_f32_max(worst_ratio, (here - LEVEL0_CHORD) / error);
            }
        }
        NTEST_LOG("level %u: strays up to %.5f, errors up to %.5f, worst cluster %.2fx its error",
                  level, (f64)stray, (f64)claimed, (f64)worst_ratio);
        NTEST_EXPECT_TRUE(claimed > 0.0F);
        // Sampled at edge midpoints and centroids, where the build measured
        // at vertices: a little over, never the quadrics' threefold.
        NTEST_EXPECT_TRUE(worst_ratio <= 1.25F);
    }
}

NTEST_F(ClusterLod, every_cut_of_a_closed_mesh_is_closed)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    NphArrayU32 tris;
    NPH_ARRAY_INIT(&tris, f->a);
    // Cameras from just off the surface to far away, a pixel's threshold at
    // several resolutions: near ones see the near side fine and the far
    // side coarse.
    const NphVec3 cameras[4] = {nph_vec3(1.4F, 0.0F, 0.0F), nph_vec3(3.0F, 1.0F, 0.5F),
                                nph_vec3(-8.0F, 2.0F, 6.0F), nph_vec3(0.0F, 40.0F, 0.0F)};
    const f32 ppus[6] = {30.0F, 100.0F, 300.0F, 1000.0F, 3000.0F, 10000.0F};
    u32 mixed = 0U;
    for (u32 c = 0; c < 4U; ++c) {
        for (u32 p = 0; p < 6U; ++p) {
            u32 levels = 0U;
            cut_triangles(&f->lods, cameras[c], ppus[p], 1.0F, &tris, &levels);
            i32 euler = 0;
            const bool ok = closed(f->a, f->mesh.positions.data, &tris, &euler);
            NTEST_LOG("camera %u, %.0f px/unit: %u triangles, levels 0x%x, euler %d", c,
                      (f64)ppus[p], tris.len / 3U, levels, euler);
            NTEST_EXPECT_TRUE(ok);
            NTEST_EXPECT_EQ_INT(euler, 0); // a torus
            NTEST_EXPECT_GT_UINT(tris.len, 0U);
            mixed += (levels & (levels - 1U)) != 0U ? 1U : 0U;
        }
    }
    NTEST_EXPECT_GE_UINT(mixed, 3U); // the test saw cuts across levels
    NPH_ARRAY_FREE(&tris);
}

// The cuts at uniform thresholds (nph_render_lod_cuts, the shadow map's):
// each closed on a torus, the first level 0, the last the coarsest level,
// and fewer triangles as the threshold grows.
NTEST_F(ClusterLod, cuts_at_uniform_thresholds_are_closed)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NphArrayU32 table;
    NPH_ARRAY_INIT(&table, f->a);
    i32 exp0 = 0;
    u32 count = 0U;
    NTEST_ASSERT_EQ_INT(nph_render_lod_cuts(l->lods.data, l->lods.len, l->groups.data,
                                            l->groups.len, NPH_RENDER_LOD_MAX_CUT_ENTRIES, &exp0,
                                            &count, &table),
                        NPH_RESULT_OK);
    NTEST_LOG("%u cuts from 2^%d, %u entries for %u clusters", count, exp0, table.len, l->lods.len);
    NTEST_ASSERT_GE_UINT(count, 2U);
    NTEST_ASSERT_LE_UINT(count, NPH_RENDER_LOD_MAX_CUTS);
    NTEST_ASSERT_EQ_UINT(table.data[count], table.len);
    NphArrayU32 tris;
    NPH_ARRAY_INIT(&tris, f->a);
    u32 prev = NPH_U32_MAX;
    for (u32 i = 0; i < count; ++i) {
        NPH_ARRAY_CLEAR(&tris);
        u32 levels = 0U;
        for (u32 k = table.data[i]; k < table.data[i + 1U]; ++k) {
            const u32 c = table.data[k];
            levels |= 1U << nph_min_u32(l->lods.data[c].level, 31U);
            const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
            for (u32 t = 0; t < (u32)m->triangle_count * 3U; ++t) {
                const u8 corner = l->clusters.triangles.data[(u64)m->triangle_offset * 3U + t];
                NPH_IGNORE_RESULT(
                    NPH_ARRAY_PUSH(&tris, l->clusters.vertices.data[m->vertex_offset + corner]));
            }
        }
        i32 euler = 0;
        const bool ok = closed(f->a, f->mesh.positions.data, &tris, &euler);
        NTEST_LOG("cut %u (2^%d): %u triangles, levels 0x%x, euler %d", i, exp0 + (i32)i,
                  tris.len / 3U, levels, euler);
        NTEST_EXPECT_TRUE(ok);
        NTEST_EXPECT_EQ_INT(euler, 0);
        NTEST_EXPECT_LE_UINT(tris.len, prev);
        if (i == 0U) {
            NTEST_EXPECT_EQ_UINT(levels, 1U); // level 0
        }
        if (i == count - 1U) {
            NTEST_EXPECT_EQ_UINT(levels, 1U << (l->levels - 1U)); // the coarsest
        }
        prev = tris.len;
    }
    // Every cut is its definition, cluster for cluster in index order: the
    // clusters whose maker's error is at most T and whose consumer's more.
    u32 mismatches = 0U;
    for (u32 i = 0; i < count; ++i) {
        const f32 t = nph_f32_from_bits((u32)(exp0 + (i32)i + 127) << 23U);
        u32 k = table.data[i];
        for (u32 c = 0; c < l->lods.len; ++c) {
            const NphRenderClusterLod *cl = &l->lods.data[c];
            const f32 made =
                cl->group == NPH_RENDER_LOD_NONE ? 0.0F : l->groups.data[cl->group].error;
            const f32 consumed =
                cl->parent == NPH_RENDER_LOD_NONE ? NPH_F32_MAX : l->groups.data[cl->parent].error;
            const bool listed = k < table.data[i + 1U] && table.data[k] == c;
            mismatches += (made <= t && consumed > t) != listed ? 1U : 0U;
            k += listed ? 1U : 0U;
        }
        mismatches += k != table.data[i + 1U] ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(mismatches, 0U);
    NPH_ARRAY_FREE(&tris);
    NPH_ARRAY_FREE(&table);
}

// A table capped one entry short of its size drops the finest cut and keeps
// the others as they were; one too small for the coarsest cut alone is
// refused.
NTEST_F(ClusterLod, a_full_cut_table_drops_the_finest_cuts_first)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NphArrayU32 full;
    NPH_ARRAY_INIT(&full, f->a);
    NphArrayU32 capped;
    NPH_ARRAY_INIT(&capped, f->a);
    i32 exp0 = 0;
    u32 count = 0U;
    NTEST_ASSERT_EQ_INT(nph_render_lod_cuts(l->lods.data, l->lods.len, l->groups.data,
                                            l->groups.len, NPH_RENDER_LOD_MAX_CUT_ENTRIES, &exp0,
                                            &count, &full),
                        NPH_RESULT_OK);
    NTEST_ASSERT_GE_UINT(count, 3U);
    i32 exp1 = 0;
    u32 count1 = 0U;
    NTEST_ASSERT_EQ_INT(nph_render_lod_cuts(l->lods.data, l->lods.len, l->groups.data,
                                            l->groups.len, full.len - 1U, &exp1, &count1, &capped),
                        NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(exp1, exp0 + 1);
    NTEST_EXPECT_EQ_UINT(count1, count - 1U);
    NTEST_EXPECT_LE_UINT(capped.len, full.len - 1U);
    for (u32 i = 0; i < count1; ++i) {
        const u32 n = capped.data[i + 1U] - capped.data[i];
        NTEST_ASSERT_EQ_UINT(n, full.data[i + 2U] - full.data[i + 1U]);
        NTEST_EXPECT_EQ_MEM(capped.data + capped.data[i], full.data + full.data[i + 1U],
                            (u64)n * sizeof(u32));
    }
    NTEST_EXPECT_EQ_INT(nph_render_lod_cuts(l->lods.data, l->lods.len, l->groups.data,
                                            l->groups.len, 2U, &exp1, &count1, &capped),
                        NPH_RESULT_INVALID_ARGUMENT);
    NPH_ARRAY_FREE(&capped);
    NPH_ARRAY_FREE(&full);
}

NTEST_F(ClusterLod, seams_and_material_borders_are_kept)
{
    make_seamed_torus(&f->mesh, 96U, 32U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    // No collapse crossed a seam: a triangle that took one side's copy of a
    // seam vertex on the other side would span the layout from end to end,
    // far wider than it spans around the wrap (a large triangle that keeps
    // to its side spans the same both ways).
    const NphRenderClusterLods *l = &f->lods;
    NTEST_EXPECT_GE_UINT(l->levels, 4U);
    u32 spanning = 0U;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        for (u32 t = 0; t < m->triangle_count; ++t) {
            f32 us[3];
            f32 vs[3];
            for (u32 k = 0; k < 3U; ++k) {
                const u8 corner =
                    l->clusters.triangles.data[((u64)m->triangle_offset + t) * 3U + k];
                const TestAttrib *at =
                    &f->mesh.attribs.data[l->clusters.vertices.data[m->vertex_offset + corner]];
                us[k] = at->u;
                vs[k] = at->v;
            }
            const f32 du = wrap_excess(us);
            const f32 dv = wrap_excess(vs);
            if ((du > 0.25F || dv > 0.25F) && spanning < 4U) {
                NTEST_LOG("cluster %u level %u triangle %u: u %.3f %.3f %.3f v %.3f %.3f %.3f", c,
                          l->lods.data[c].level, t, (f64)us[0], (f64)us[1], (f64)us[2], (f64)vs[0],
                          (f64)vs[1], (f64)vs[2]);
            }
            spanning += du > 0.25F || dv > 0.25F ? 1U : 0U;
        }
    }
    NTEST_LOG("%u triangles span a wrap", spanning);
    NTEST_EXPECT_EQ_UINT(spanning, 0U);
    NphArrayU32 tris;
    NPH_ARRAY_INIT(&tris, f->a);
    u32 levels = 0U;
    cut_triangles(l, nph_vec3(1.4F, 0.0F, 0.0F), 1000.0F, 1.0F, &tris, &levels);
    i32 euler = 0;
    NTEST_EXPECT_TRUE(closed(f->a, f->mesh.positions.data, &tris, &euler));
    NTEST_EXPECT_EQ_INT(euler, 0);
    NPH_ARRAY_FREE(&tris);

    // Two materials: each cluster keeps its submesh's own vertices.
    nph_render_cluster_lods_free(&f->lods);
    nph_render_meshlets_free(&f->meshlets);
    mesh_free(&f->mesh);
    mesh_init(&f->mesh, f->a);
    make_two_material_torus(&f->mesh, 96U, 32U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    l = &f->lods;
    NTEST_EXPECT_GE_UINT(l->levels, 4U);
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        NTEST_ASSERT_LT_UINT(m->submesh, 2U);
        for (u32 k = 0; k < m->vertex_count; ++k) {
            NTEST_EXPECT_EQ_UINT(
                f->mesh.attribs.data[l->clusters.vertices.data[m->vertex_offset + k]].chart,
                m->submesh);
        }
    }
    NPH_ARRAY_INIT(&tris, f->a);
    cut_triangles(l, nph_vec3(3.0F, 1.0F, 0.5F), 1000.0F, 1.0F, &tris, &levels);
    NTEST_EXPECT_TRUE(closed(f->a, f->mesh.positions.data, &tris, &euler));
    NTEST_EXPECT_EQ_INT(euler, 0);
    NPH_ARRAY_FREE(&tris);
}

NTEST_F(ClusterLod, the_hierarchy_holds_every_group_once)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NTEST_ASSERT_GT_UINT(l->nodes.len, 0U);
    u32 *seen = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, l->groups.len);
    u32 *visited = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, l->nodes.len);
    NphArrayU32 stack;
    NPH_ARRAY_INIT(&stack, f->a);
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&stack, 0U));
    while (stack.len != 0U) {
        const u32 n = NPH_ARRAY_POP(&stack);
        NTEST_ASSERT_LT_UINT(n, l->nodes.len);
        visited[n] += 1U;
        const NphRenderLodNode *node = &l->nodes.data[n];
        for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
            const u32 ch = node->child[i];
            if (ch == NPH_RENDER_LOD_NONE) {
                continue;
            }
            const NphVec3 cc = nph_vec3(node->sphere[i].x, node->sphere[i].y, node->sphere[i].z);
            if ((ch & NPH_RENDER_LOD_LEAF) != 0U) {
                const u32 g = ch & ~NPH_RENDER_LOD_LEAF;
                NTEST_ASSERT_LT_UINT(g, l->groups.len);
                seen[g] += 1U;
                const NphRenderLodGroup *rg = &l->groups.data[g];
                NTEST_EXPECT_TRUE(node->error[i] == rg->error);
                NTEST_EXPECT_TRUE(node->maker_error[i] == smallest_maker_error(l, g));
                NTEST_EXPECT_TRUE(holds(cc, node->sphere[i].w, rg->center, rg->radius));
            } else {
                NTEST_ASSERT_LT_UINT(ch, l->nodes.len);
                // The child's own children within its sphere, under its error.
                const NphRenderLodNode *child = &l->nodes.data[ch];
                for (u32 j = 0; j < NPH_RENDER_LOD_NODE_CHILDREN; ++j) {
                    if (child->child[j] == NPH_RENDER_LOD_NONE) {
                        continue;
                    }
                    const NphVec3 gc =
                        nph_vec3(child->sphere[j].x, child->sphere[j].y, child->sphere[j].z);
                    NTEST_EXPECT_TRUE(holds(cc, node->sphere[i].w, gc, child->sphere[j].w));
                    NTEST_EXPECT_TRUE(child->error[j] <= node->error[i]);
                    NTEST_EXPECT_TRUE(child->maker_error[j] >= node->maker_error[i]);
                }
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&stack, ch));
            }
        }
    }
    for (u32 g = 0; g < l->groups.len; ++g) {
        NTEST_EXPECT_EQ_UINT(seen[g], 1U);
    }
    for (u32 n = 0; n < l->nodes.len; ++n) {
        NTEST_EXPECT_EQ_UINT(visited[n], 1U);
    }
    NPH_ARRAY_FREE(&stack);
    NPH_FREE_ARRAY(f->a, u32, visited, l->nodes.len);
    NPH_FREE_ARRAY(f->a, u32, seen, l->groups.len);
}

NTEST_F(ClusterLod, the_build_is_deterministic)
{
    make_seamed_torus(&f->mesh, 96U, 32U, 1.0F, 0.35F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    NphRenderMeshlets m2;
    NphRenderClusterLods l2;
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &m2, &l2), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NTEST_ASSERT_EQ_UINT(l2.clusters.meshlets.len, l->clusters.meshlets.len);
    NTEST_ASSERT_EQ_UINT(l2.groups.len, l->groups.len);
    NTEST_ASSERT_EQ_UINT(l2.nodes.len, l->nodes.len);
    NTEST_ASSERT_EQ_UINT(l2.clusters.vertices.len, l->clusters.vertices.len);
    NTEST_ASSERT_EQ_UINT(l2.clusters.triangles.len, l->clusters.triangles.len);
    NTEST_EXPECT_EQ_INT(memcmp(l2.clusters.meshlets.data, l->clusters.meshlets.data,
                               l->clusters.meshlets.len * sizeof(NphRenderMeshlet)),
                        0);
    NTEST_EXPECT_EQ_INT(memcmp(l2.clusters.vertices.data, l->clusters.vertices.data,
                               l->clusters.vertices.len * sizeof(u32)),
                        0);
    NTEST_EXPECT_EQ_INT(
        memcmp(l2.clusters.triangles.data, l->clusters.triangles.data, l->clusters.triangles.len),
        0);
    NTEST_EXPECT_EQ_INT(
        memcmp(l2.groups.data, l->groups.data, l->groups.len * sizeof(NphRenderLodGroup)), 0);
    NTEST_EXPECT_EQ_INT(
        memcmp(l2.nodes.data, l->nodes.data, l->nodes.len * sizeof(NphRenderLodNode)), 0);
    nph_render_cluster_lods_free(&l2);
    nph_render_meshlets_free(&m2);
}

NTEST_F(ClusterLod, scattered_pieces_thin_out_and_the_levels_end)
{
    make_quads(&f->mesh, 4000U, 7U);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NTEST_LOG("%u levels, coarsest %u triangles of %u", l->levels,
              cluster_triangles(l, l->levels - 1U), f->mesh.indices.len / 3U);
    NTEST_EXPECT_GE_UINT(l->levels, 2U);
    u32 prev = cluster_triangles(l, 0U);
    for (u32 level = 1U; level < l->levels; ++level) {
        const u32 tris = cluster_triangles(l, level);
        NTEST_EXPECT_LT_UINT(tris, prev);
        prev = tris;
    }
    // A piece taken away counts its size in the error: the coarsest level's
    // groups' errors reach the quads' scale.
    f32 worst = 0.0F;
    for (u32 g = 0; g + 1U < l->groups.len; ++g) {
        worst = nph_f32_max(worst, l->groups.data[g].error);
    }
    NTEST_EXPECT_TRUE(worst >= 0.01F);
}

NTEST(ClusterLod, the_projected_error_follows_distance_and_scale)
{
    const NphVec3 c = nph_vec3(0.0F, 0.0F, -10.0F);
    const f32 e =
        nph_render_lod_projected_error(c, 1.0F, 0.01F, 1.0F, nph_vec3_zero(), 1000.0F, 0.1F);
    NTEST_EXPECT_NEAR_F32(e, 0.01F * 1000.0F / 9.0F, 1e-4F);
    // Twice the scale: twice the error over a nearer nearest point.
    const f32 e2 =
        nph_render_lod_projected_error(c, 1.0F, 0.01F, 2.0F, nph_vec3_zero(), 1000.0F, 0.1F);
    NTEST_EXPECT_NEAR_F32(e2, 0.02F * 1000.0F / 8.0F, 1e-4F);
    // Inside the sphere: the near plane bounds it.
    const f32 e3 =
        nph_render_lod_projected_error(c, 20.0F, 0.01F, 1.0F, nph_vec3_zero(), 1000.0F, 0.1F);
    NTEST_EXPECT_NEAR_F32(e3, 0.01F * 1000.0F / 0.1F, 1e-2F);
}

// A surface the levels cannot simplify (flat shaded: every triangle's
// corners its own, on a chart of its own, so every vertex is a seam corner):
// the levels stop at once, and the coarsest level's many clusters are
// grouped into root groups of a few each, under a tree of their own, so that
// they are culled and handed on a few at a time.
NTEST_F(ClusterLod, a_level_that_stops_shrinking_splits_into_root_groups)
{
    const u32 n = 40U;
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const f32 fx = (f32)x;
            const f32 fz = (f32)z;
            const NphVec3 p00 = nph_vec3(fx, nph_f32_sin(fx * 0.4F) * 0.3F, fz);
            const NphVec3 p10 = nph_vec3(fx + 1.0F, nph_f32_sin((fx + 1.0F) * 0.4F) * 0.3F, fz);
            const NphVec3 p01 = nph_vec3(fx, nph_f32_sin(fx * 0.4F) * 0.3F, fz + 1.0F);
            const NphVec3 p11 =
                nph_vec3(fx + 1.0F, nph_f32_sin((fx + 1.0F) * 0.4F) * 0.3F, fz + 1.0F);
            const u32 chart = (z * n + x) * 2U;
            mesh_tri(&f->mesh, mesh_vertex(&f->mesh, p00, chart), mesh_vertex(&f->mesh, p01, chart),
                     mesh_vertex(&f->mesh, p11, chart));
            mesh_tri(&f->mesh, mesh_vertex(&f->mesh, p00, chart + 1U),
                     mesh_vertex(&f->mesh, p11, chart + 1U),
                     mesh_vertex(&f->mesh, p10, chart + 1U));
        }
    }
    mesh_one_range(&f->mesh);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    u32 roots = 0U;
    u32 largest = 0U;
    for (u32 g = 0; g < l->groups.len; ++g) {
        if (l->groups.data[g].error == NPH_F32_MAX) {
            roots += 1U;
            largest = nph_max_u32(largest, l->groups.data[g].cluster_count);
        }
    }
    u32 coarsest = 0U;
    for (u32 c = 0; c < l->lods.len; ++c) {
        coarsest += l->lods.data[c].level == l->levels - 1U ? 1U : 0U;
    }
    NTEST_LOG("%u levels; the coarsest has %u clusters in %u root groups (at most %u each)",
              l->levels, coarsest, roots, largest);
    NTEST_EXPECT_GT_UINT(coarsest, 8U);
    NTEST_EXPECT_GE_UINT(roots * 8U, coarsest / 2U);
    NTEST_EXPECT_LE_UINT(largest, 2U * 8U);
    // The root node holds no group directly when there are many: the root
    // groups lie under a tree of their own.
    u32 leaves = 0U;
    for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
        const u32 ch = l->nodes.data[0].child[i];
        leaves += ch != NPH_RENDER_LOD_NONE && (ch & NPH_RENDER_LOD_LEAF) != 0U ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(leaves, 0U);
}

NTEST_F(ClusterLod, small_and_empty_meshes_keep_level_zero_under_a_root)
{
    // One quad: one cluster, nothing to simplify.
    make_grid(&f->mesh, 1U, 1.0F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(f->lods.levels, 1U);
    NTEST_EXPECT_EQ_UINT(f->lods.clusters.meshlets.len, 1U);
    NTEST_ASSERT_EQ_UINT(f->lods.groups.len, 1U);
    NTEST_EXPECT_TRUE(f->lods.groups.data[0].error == NPH_F32_MAX);
    NTEST_EXPECT_EQ_UINT(f->lods.lods.data[0].parent, 0U);
    NTEST_ASSERT_GE_UINT(f->lods.nodes.len, 1U);
    NTEST_EXPECT_EQ_UINT(f->lods.nodes.data[0].child[0], 0U | NPH_RENDER_LOD_LEAF);
    // No triangles at all.
    nph_render_cluster_lods_free(&f->lods);
    nph_render_meshlets_free(&f->meshlets);
    mesh_free(&f->mesh);
    mesh_init(&f->mesh, f->a);
    mesh_one_range(&f->mesh);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(f->lods.clusters.meshlets.len, 0U);
    NTEST_EXPECT_EQ_UINT(f->lods.groups.len, 0U);
    NTEST_EXPECT_EQ_UINT(f->lods.nodes.len, 0U);
}

NTEST_F(ClusterLod, degenerate_and_non_manifold_triangles_do_not_break_the_levels)
{
    // A torus with a fin: three triangles on one edge (non-manifold), a
    // triangle repeated, and two of zero area.
    make_torus(&f->mesh, 96U, 32U, 1.0F, 0.35F);
    const u32 tris = f->mesh.indices.len / 3U;
    const u32 a = f->mesh.indices.data[0];
    const u32 b = f->mesh.indices.data[1];
    const u32 fin = mesh_vertex(&f->mesh, nph_vec3(1.5F, 0.6F, 0.0F), 0U);
    mesh_tri(&f->mesh, a, b, fin);
    mesh_tri(&f->mesh, f->mesh.indices.data[3], f->mesh.indices.data[4], f->mesh.indices.data[5]);
    mesh_tri(&f->mesh, a, a, b);
    mesh_tri(&f->mesh, a, b, b);
    f->mesh.ranges.data[0].index_count = f->mesh.indices.len;
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NTEST_EXPECT_GE_UINT(l->levels, 4U);
    NTEST_EXPECT_GE_UINT(cluster_triangles(l, 0U), tris + 4U);
    // The fin's edge stays where it was at every level: neither of its
    // corners is ever simplified away.
    u32 seen_a = 0U;
    for (u32 level = 0; level < l->levels; ++level) {
        bool has_a = false;
        for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
            if (l->lods.data[c].level != level) {
                continue;
            }
            const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
            for (u32 k = 0; k < m->vertex_count; ++k) {
                has_a = has_a || l->clusters.vertices.data[m->vertex_offset + k] == a;
            }
        }
        seen_a += has_a ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(seen_a, l->levels);
}

// Two quads in submeshes of their own, a cluster each, one at z = 0 and the
// other a subnormal float above it (the fuzzer's, through the cooker): the
// clusters' centres span less than a normal float along z, whose reciprocal
// in their Morton order is infinite (0 times it, the lower centre's offset,
// is NaN), and the levels still build, every cluster under a root.
NTEST_F(ClusterLod, centres_a_subnormal_float_apart_still_order)
{
    TestMesh *m = &f->mesh;
    const f32 tiny = nph_f32_from_bits(0x80U); // 1.8e-43
    for (u32 q = 0; q < 2U; ++q) {
        const f32 x = 2.0F * (f32)q;
        const f32 z = q == 0U ? 0.0F : tiny;
        const u32 first = m->indices.len;
        const u32 v0 = mesh_vertex(m, nph_vec3(x, 0.0F, z), q);
        const u32 v1 = mesh_vertex(m, nph_vec3(x + 1.0F, 0.0F, z), q);
        const u32 v2 = mesh_vertex(m, nph_vec3(x + 1.0F, 1.0F, z), q);
        const u32 v3 = mesh_vertex(m, nph_vec3(x, 1.0F, z), q);
        mesh_tri(m, v0, v1, v2);
        mesh_tri(m, v0, v2, v3);
        const NphRenderIndexRange r = {first, m->indices.len - first};
        NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&m->ranges, r), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(build_lods(m, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NTEST_ASSERT_EQ_UINT(l->level0_clusters, 2U);
    NTEST_EXPECT_NE_UINT(nph_f32_to_bits(l->clusters.bounds.data[0].center.z),
                         nph_f32_to_bits(l->clusters.bounds.data[1].center.z));
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        NTEST_EXPECT_LT_UINT(l->lods.data[c].parent, l->groups.len);
    }
}

NTEST_F(ClusterLod, bad_input_is_refused)
{
    make_grid(&f->mesh, 8U, 1.0F);
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    NphRenderClusterLodInput in;
    memset(&in, 0, sizeof in);
    in.positions = f->mesh.positions.data;
    in.vertex_count = f->mesh.positions.len;
    in.indices = f->mesh.indices.data;
    in.index_count = f->mesh.indices.len;
    in.ranges = f->mesh.ranges.data;
    in.range_count = f->mesh.ranges.len;
    in.meshlets = &f->meshlets;
    in.scratch = f->a;
    NphRenderClusterLods out;
    nph_render_cluster_lods_init(&out, f->a);
    // An index past the vertices.
    f->mesh.indices.data[4] = f->mesh.positions.len;
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_build(&in, &out), NPH_RESULT_INVALID_ARGUMENT);
    f->mesh.indices.data[4] = 0U;
    // A position that is not finite.
    const NphVec3 keep = f->mesh.positions.data[3];
    f->mesh.positions.data[3].y = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_build(&in, &out), NPH_RESULT_INVALID_ARGUMENT);
    f->mesh.positions.data[3] = keep;
    // Limits outside the meshlet builder's, and a meshlet past its arrays.
    in.max_vertices = 65U;
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_build(&in, &out), NPH_RESULT_INVALID_ARGUMENT);
    in.max_vertices = 0U;
    in.range_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_build(&in, &out), NPH_RESULT_INVALID_ARGUMENT);
    in.range_count = f->mesh.ranges.len;
    in.attribs = f->mesh.attribs.data;
    in.attrib_stride = 0U;
    NTEST_EXPECT_EQ_INT(nph_render_cluster_lods_build(&in, &out), NPH_RESULT_INVALID_ARGUMENT);
    nph_render_cluster_lods_free(&out);
}

// No triangle of any level turns into the surface or stands across it,
// however the input rounds: the same torus with its coordinates moved by a
// few units in the last place, as another C library's sine and cosine move
// them (glibc's put one triangle inward at level 7 where MSVC's did not,
// before each corner's normals were checked on their own).
NTEST_F(ClusterLod, no_level_turns_into_the_surface_however_the_input_rounds)
{
    for (u32 seed = 1; seed <= 8U; ++seed) {
        mesh_free(&f->mesh);
        mesh_init(&f->mesh, f->a);
        make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
        NphPcg32 rng;
        nph_pcg32_seed(&rng, seed, 7U);
        for (u32 i = 0; i < f->mesh.positions.len; ++i) {
            NphVec3 *p = &f->mesh.positions.data[i];
            p->x *= 1.0F + (f32)((i32)(nph_pcg32_next_u32(&rng) % 5U) - 2) * 1.2e-7F;
            p->y *= 1.0F + (f32)((i32)(nph_pcg32_next_u32(&rng) % 5U) - 2) * 1.2e-7F;
            p->z *= 1.0F + (f32)((i32)(nph_pcg32_next_u32(&rng) % 5U) - 2) * 1.2e-7F;
        }
        nph_render_cluster_lods_free(&f->lods);
        nph_render_meshlets_free(&f->meshlets);
        NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
        NTEST_EXPECT_GE_UINT(f->lods.levels, 6U);
        NTEST_EXPECT_EQ_UINT(torus_inward(&f->lods, &f->mesh), 0U);
    }
}

// ---------------------------------------------------------------------------
// Corners where seams meet (flat-shaded architecture), with input texture
// coordinates: they collapse, each surface a neighbour's position has no vertex
// for taking one added there
// ---------------------------------------------------------------------------

// The six faces' charts: a flat-shaded surface's vertices differ across each
// crease, and each face's texture coordinates are a plane's.
enum {
    STEP_TOP = 0U,
    STEP_BOTTOM,
    STEP_POS_X,
    STEP_NEG_X,
    STEP_POS_Z,
    STEP_NEG_Z,
};

static NphVec2 step_uv(u32 chart, NphVec3 p)
{
    if (chart == STEP_TOP || chart == STEP_BOTTOM) {
        return nph_vec2(p.x, p.z);
    }
    return chart == STEP_POS_X || chart == STEP_NEG_X ? nph_vec2(p.z, p.y) : nph_vec2(p.x, p.y);
}

static void step_quad(TestMesh *m, u32 chart, NphVec3 a, NphVec3 b, NphVec3 c, NphVec3 d)
{
    const NphVec2 ua = step_uv(chart, a);
    const NphVec2 ub = step_uv(chart, b);
    const NphVec2 uc = step_uv(chart, c);
    const NphVec2 ud = step_uv(chart, d);
    const u32 va = mesh_vertex_uv(m, a, chart, ua.x, ua.y);
    const u32 vb = mesh_vertex_uv(m, b, chart, ub.x, ub.y);
    const u32 vc = mesh_vertex_uv(m, c, chart, uc.x, uc.y);
    const u32 vd = mesh_vertex_uv(m, d, chart, ud.x, ud.y);
    mesh_tri(m, va, vb, vc);
    mesh_tri(m, va, vc, vd);
}

// A closed staircase of n x n columns over a floor, each a step higher than the
// last along x and along z, every face flat-shaded with its own chart: most of its
// vertices are corners of three or more seams. Every position is on a grid, and no
// edge has more than two faces.
#define STEP_HEIGHT 0.25F
static void make_steps(TestMesh *m, u32 n)
{
    const f32 size = (f32)n;
    for (u32 i = 0; i < n; ++i) {
        for (u32 j = 0; j < n; ++j) {
            const f32 x0 = (f32)i;
            const f32 x1 = x0 + 1.0F;
            const f32 z0 = (f32)j;
            const f32 z1 = z0 + 1.0F;
            const f32 h = (f32)(i + j + 1U) * STEP_HEIGHT;
            step_quad(m, STEP_TOP, nph_vec3(x0, h, z0), nph_vec3(x0, h, z1), nph_vec3(x1, h, z1),
                      nph_vec3(x1, h, z0));
            step_quad(m, STEP_BOTTOM, nph_vec3(x0, 0.0F, z0), nph_vec3(x1, 0.0F, z0),
                      nph_vec3(x1, 0.0F, z1), nph_vec3(x0, 0.0F, z1));
            // The riser in front of the column along x and along z (the outside
            // walls below): a step high.
            const f32 low = h - STEP_HEIGHT;
            if (i != 0U) {
                step_quad(m, STEP_NEG_X, nph_vec3(x0, low, z0), nph_vec3(x0, low, z1),
                          nph_vec3(x0, h, z1), nph_vec3(x0, h, z0));
            }
            if (j != 0U) {
                step_quad(m, STEP_NEG_Z, nph_vec3(x0, low, z0), nph_vec3(x0, h, z0),
                          nph_vec3(x1, h, z0), nph_vec3(x1, low, z0));
            }
            // The outside walls, a step high at a time.
            const u32 steps = i + j + 1U;
            for (u32 k = 0; k < steps; ++k) {
                const f32 y0 = (f32)k * STEP_HEIGHT;
                const f32 y1 = y0 + STEP_HEIGHT;
                if (i == 0U) {
                    step_quad(m, STEP_NEG_X, nph_vec3(0.0F, y0, z0), nph_vec3(0.0F, y0, z1),
                              nph_vec3(0.0F, y1, z1), nph_vec3(0.0F, y1, z0));
                }
                if (i == n - 1U) {
                    step_quad(m, STEP_POS_X, nph_vec3(size, y0, z0), nph_vec3(size, y1, z0),
                              nph_vec3(size, y1, z1), nph_vec3(size, y0, z1));
                }
                if (j == 0U) {
                    step_quad(m, STEP_NEG_Z, nph_vec3(x0, y0, 0.0F), nph_vec3(x0, y1, 0.0F),
                              nph_vec3(x1, y1, 0.0F), nph_vec3(x1, y0, 0.0F));
                }
                if (j == n - 1U) {
                    step_quad(m, STEP_POS_Z, nph_vec3(x0, y0, size), nph_vec3(x1, y0, size),
                              nph_vec3(x1, y1, size), nph_vec3(x0, y1, size));
                }
            }
        }
    }
    mesh_one_range(m);
}

typedef NPH_ARRAY(NphVec2) StepUvs;

static void step_uvs(const TestMesh *m, NphAllocator *a, StepUvs *out)
{
    NPH_ARRAY_INIT(out, a);
    for (u32 v = 0; v < m->attribs.len; ++v) {
        const NphVec2 uv = nph_vec2(m->attribs.data[v].u, m->attribs.data[v].v);
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(out, uv));
    }
}

static u32 coarsest_triangles(const NphRenderClusterLods *l)
{
    return cluster_triangles(l, l->levels - 1U);
}

NTEST_F(ClusterLod, corners_of_seams_collapse_where_vertices_may_be_added)
{
    make_steps(&f->mesh, 16U);
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    // As before without texture coordinates: the corners never move.
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const u32 level0 = f->mesh.indices.len / 3U;
    const u32 without = coarsest_triangles(&f->lods);
    NTEST_EXPECT_EQ_UINT(f->lods.added.len, 0U);
    NphRenderMeshlets m2;
    NphRenderClusterLods l2;
    NTEST_ASSERT_EQ_INT(build_lods_uvs(&f->mesh, f->a, &m2, &l2, uvs.data), NPH_RESULT_OK);
    const u32 with = coarsest_triangles(&l2);
    NTEST_LOG("%u triangles: at the coarsest %u without added vertices in %u levels, %u with %u "
              "added in %u levels",
              level0, without, f->lods.levels, with, l2.added.len, l2.levels);
    NTEST_EXPECT_GT_UINT(l2.added.len, 0U);
    NTEST_EXPECT_LT_UINT(with * 4U, without);
    NTEST_EXPECT_GT_UINT(l2.levels, f->lods.levels);
    // Level 0 is the same either way, and every cluster names a vertex there is.
    NTEST_ASSERT_GE_UINT(l2.clusters.meshlets.len, l2.level0_clusters);
    NTEST_EXPECT_EQ_UINT(l2.level0_clusters, f->lods.level0_clusters);
    const u32 vertices = f->mesh.positions.len + l2.added.len;
    u32 outside = 0U;
    for (u32 k = 0; k < l2.clusters.vertices.len; ++k) {
        outside += l2.clusters.vertices.data[k] >= vertices ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(outside, 0U);
    nph_render_cluster_lods_free(&l2);
    nph_render_meshlets_free(&m2);
    NPH_ARRAY_FREE(&uvs);
}

// The face a vertex's attributes come from, through the vertices added from it.
static u32 step_vertex_chart(const TestMesh *m, const NphRenderClusterLods *l, u32 v)
{
    const u32 own = m->positions.len;
    while (v >= own) {
        v = l->added.data[v - own].source;
    }
    return m->attribs.data[v].chart;
}

// A surface keeps its own: every triangle of every level lies on one face's chart
// (hard edges stay hard), and every added vertex stands on a position of the mesh
// with texture coordinates that stray from its face's no farther than the levels'
// surface strays from the mesh (textures do not smear): a face a collapse has
// already tilted carries them along its new plane.
NTEST_F(ClusterLod, added_vertices_keep_their_surface)
{
    make_steps(&f->mesh, 16U);
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    NTEST_ASSERT_EQ_INT(build_lods_uvs(&f->mesh, f->a, &f->meshlets, &f->lods, uvs.data),
                        NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    const u32 own = f->mesh.positions.len;
    NTEST_ASSERT_GT_UINT(l->added.len, 0U);
    u32 off_chart = 0U;
    u32 triangles = 0U;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        const NphRenderMeshlet *ml = &l->clusters.meshlets.data[c];
        for (u32 t = 0; t < ml->triangle_count; ++t) {
            const u8 *corner = &l->clusters.triangles.data[((u64)ml->triangle_offset + t) * 3U];
            const u32 c0 = step_vertex_chart(
                &f->mesh, l, l->clusters.vertices.data[ml->vertex_offset + corner[0]]);
            const u32 c1 = step_vertex_chart(
                &f->mesh, l, l->clusters.vertices.data[ml->vertex_offset + corner[1]]);
            const u32 c2 = step_vertex_chart(
                &f->mesh, l, l->clusters.vertices.data[ml->vertex_offset + corner[2]]);
            off_chart += c0 != c1 || c1 != c2 ? 1U : 0U;
            triangles += 1U;
        }
    }
    NTEST_EXPECT_GT_UINT(triangles, 0U);
    NTEST_EXPECT_EQ_UINT(off_chart, 0U);
    f32 largest_error = 0.0F;
    for (u32 g = 0; g < l->groups.len; ++g) {
        if (l->groups.data[g].error != NPH_F32_MAX) {
            largest_error = nph_f32_max(largest_error, l->groups.data[g].error);
        }
    }
    f32 strayed = 0.0F;
    f64 strayed_sum = 0.0;
    for (u32 i = 0; i < l->added.len; ++i) {
        const NphRenderLodAddedVertex *a = &l->added.data[i];
        NTEST_ASSERT_LT_UINT(a->position, own + i);
        NTEST_ASSERT_LT_UINT(a->source, own + i);
        u32 at = a->position;
        while (at >= own) {
            at = l->added.data[at - own].position;
        }
        const u32 chart = step_vertex_chart(&f->mesh, l, a->source);
        const NphVec2 expected = step_uv(chart, f->mesh.positions.data[at]);
        const f32 off =
            nph_f32_max(nph_f32_abs(a->uv.x - expected.x), nph_f32_abs(a->uv.y - expected.y));
        strayed = nph_f32_max(strayed, off);
        strayed_sum += (f64)off;
    }
    const f64 strayed_mean = strayed_sum / (f64)l->added.len;
    NTEST_LOG("%u added vertices: texture coordinates %.3f off at most, %.3f on average; the "
              "largest group error %.3f",
              l->added.len, (f64)strayed, strayed_mean, (f64)largest_error);
    NTEST_EXPECT_TRUE(strayed <= largest_error);
    NTEST_EXPECT_TRUE(strayed_mean <= 0.1 * (f64)largest_error);
    NPH_ARRAY_FREE(&uvs);
}

// What a corner's collapse leaves is still closed, welded by position, in every cut.
NTEST_F(ClusterLod, cuts_with_added_vertices_are_closed)
{
    make_steps(&f->mesh, 16U);
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    NTEST_ASSERT_EQ_INT(build_lods_uvs(&f->mesh, f->a, &f->meshlets, &f->lods, uvs.data),
                        NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NPH_ARRAY(NphVec3) positions;
    NPH_ARRAY_INIT(&positions, f->a);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&positions, f->mesh.positions.data, f->mesh.positions.len),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < l->added.len; ++i) {
        const NphVec3 at = positions.data[l->added.data[i].position];
        NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&positions, at), NPH_RESULT_OK);
    }
    NphArrayU32 tris;
    NPH_ARRAY_INIT(&tris, f->a);
    const NphVec3 cameras[3] = {nph_vec3(8.0F, 20.0F, 8.0F), nph_vec3(-30.0F, 10.0F, -5.0F),
                                nph_vec3(80.0F, 60.0F, 90.0F)};
    const f32 ppus[6] = {10.0F, 30.0F, 100.0F, 300.0F, 1000.0F, 3000.0F};
    u32 mixed = 0U;
    for (u32 c = 0; c < 3U; ++c) {
        for (u32 p = 0; p < 6U; ++p) {
            u32 levels = 0U;
            cut_triangles(l, cameras[c], ppus[p], 1.0F, &tris, &levels);
            i32 euler = 0;
            const bool ok = closed(f->a, positions.data, &tris, &euler);
            NTEST_LOG("camera %u, %.0f px/unit: %u triangles, levels 0x%x, euler %d", c,
                      (f64)ppus[p], tris.len / 3U, levels, euler);
            NTEST_EXPECT_TRUE(ok);
            NTEST_EXPECT_EQ_INT(euler, 2);
            NTEST_EXPECT_GT_UINT(tris.len, 0U);
            mixed += (levels & (levels - 1U)) != 0U ? 1U : 0U;
        }
    }
    NTEST_EXPECT_GE_UINT(mixed, 2U); // the test saw cuts across levels
    NPH_ARRAY_FREE(&tris);
    NPH_ARRAY_FREE(&positions);
    NPH_ARRAY_FREE(&uvs);
}

NTEST_F(ClusterLod, a_build_with_added_vertices_is_deterministic)
{
    make_steps(&f->mesh, 12U);
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    NTEST_ASSERT_EQ_INT(build_lods_uvs(&f->mesh, f->a, &f->meshlets, &f->lods, uvs.data),
                        NPH_RESULT_OK);
    NphRenderMeshlets m2;
    NphRenderClusterLods l2;
    NTEST_ASSERT_EQ_INT(build_lods_uvs(&f->mesh, f->a, &m2, &l2, uvs.data), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(l2.added.len, f->lods.added.len);
    NTEST_ASSERT_EQ_UINT(l2.clusters.vertices.len, f->lods.clusters.vertices.len);
    NTEST_EXPECT_EQ_INT(memcmp(l2.added.data, f->lods.added.data,
                               f->lods.added.len * sizeof(NphRenderLodAddedVertex)),
                        0);
    NTEST_EXPECT_EQ_INT(memcmp(l2.clusters.vertices.data, f->lods.clusters.vertices.data,
                               f->lods.clusters.vertices.len * sizeof(u32)),
                        0);
    nph_render_cluster_lods_free(&l2);
    nph_render_meshlets_free(&m2);
    NPH_ARRAY_FREE(&uvs);
}

// ---------------------------------------------------------------------------
// What a collapse costs beyond its quadrics, and what a level may spend (ADR-0069)
// ---------------------------------------------------------------------------

static void wall_quad(TestMesh *m, u32 chart, f32 x, f32 y)
{
    const f32 x1 = x + 1.0F;
    const f32 y1 = y + 1.0F;
    const u32 a = mesh_vertex_uv(m, nph_vec3(x, y, 0.0F), chart, x, y);
    const u32 b = mesh_vertex_uv(m, nph_vec3(x1, y, 0.0F), chart, x1, y);
    const u32 c = mesh_vertex_uv(m, nph_vec3(x1, y1, 0.0F), chart, x1, y1);
    const u32 d = mesh_vertex_uv(m, nph_vec3(x, y1, 0.0F), chart, x, y1);
    mesh_tri(m, a, b, c);
    mesh_tri(m, a, c, d);
}

#define WALL_WINDOWS 8U // a side
#define WINDOW_CELLS 8U // a window's side, and the wall between two
#define WALL_CELLS   (WALL_WINDOWS * 2U * WINDOW_CELLS)

// A wall of unit cells in the plane z = 0, with windows in it, their cells another
// chart: the quadrics see one plane, whatever becomes of the windows.
static void make_wall(TestMesh *m)
{
    for (u32 y = 0; y < WALL_CELLS; ++y) {
        for (u32 x = 0; x < WALL_CELLS; ++x) {
            const bool window = x % (2U * WINDOW_CELLS) >= WINDOW_CELLS / 2U &&
                                x % (2U * WINDOW_CELLS) < WINDOW_CELLS / 2U + WINDOW_CELLS &&
                                y % (2U * WINDOW_CELLS) >= WINDOW_CELLS / 2U &&
                                y % (2U * WINDOW_CELLS) < WINDOW_CELLS / 2U + WINDOW_CELLS;
            wall_quad(m, window ? 1U : 0U, (f32)x, (f32)y);
        }
    }
    mesh_one_range(m);
}

// The windows at each level stand within the level's error of where they stood: a corner
// that walked along its edge in steps of no error would have turned its rectangle into a
// triangle, with every point still in the plane.
NTEST_F(ClusterLod, a_border_that_moves_counts_in_the_error)
{
    make_wall(&f->mesh);
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    NTEST_ASSERT_EQ_INT(build_lods_uvs(&f->mesh, f->a, &f->meshlets, &f->lods, uvs.data),
                        NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    NPH_ARRAY(NphVec3) positions;
    NPH_ARRAY_INIT(&positions, f->a);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&positions, f->mesh.positions.data, f->mesh.positions.len),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < l->added.len; ++i) {
        const NphVec3 at = positions.data[l->added.data[i].position];
        NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&positions, at), NPH_RESULT_OK);
    }
    const f32 side = (f32)WINDOW_CELLS;
    const u32 windows = WALL_WINDOWS * WALL_WINDOWS;
    const f32 area0 = (f32)windows * side * side;
    const f32 perimeter = (f32)windows * 4.0F * side;
    NPH_ARRAY(NphVec3) window_tris;
    NPH_ARRAY_INIT(&window_tris, f->a);
    u32 levels_moved = 0U;
    for (u32 level = 0; level < l->levels; ++level) {
        f32 error = 0.0F;
        f32 area = 0.0F;
        NPH_ARRAY_CLEAR(&window_tris);
        for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
            const NphRenderClusterLod *lod = &l->lods.data[c];
            if (lod->level != level) {
                continue;
            }
            if (lod->group != NPH_RENDER_LOD_NONE) {
                error = nph_f32_max(error, l->groups.data[lod->group].error);
            }
            const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
            for (u32 t = 0; t < m->triangle_count; ++t) {
                u32 v[3];
                for (u32 k = 0; k < 3U; ++k) {
                    const u8 corner =
                        l->clusters.triangles.data[((u64)m->triangle_offset + t) * 3U + k];
                    v[k] = l->clusters.vertices.data[m->vertex_offset + corner];
                }
                if (step_vertex_chart(&f->mesh, l, v[0]) != 1U) {
                    continue;
                }
                const NphVec3 p0 = positions.data[v[0]];
                const NphVec3 p1 = positions.data[v[1]];
                const NphVec3 p2 = positions.data[v[2]];
                area += 0.5F *
                        nph_vec3_length(nph_vec3_cross(nph_vec3_sub(p1, p0), nph_vec3_sub(p2, p0)));
                NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&window_tris, p0), NPH_RESULT_OK);
                NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&window_tris, p1), NPH_RESULT_OK);
                NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH(&window_tris, p2), NPH_RESULT_OK);
            }
        }
        // How far the windows' corners are from the windows that are left.
        f32 farthest = 0.0F;
        for (u32 wy = 0; wy < WALL_WINDOWS; ++wy) {
            for (u32 wx = 0; wx < WALL_WINDOWS; ++wx) {
                const u32 cell_x = wx * 2U * WINDOW_CELLS + WINDOW_CELLS / 2U;
                const u32 cell_y = wy * 2U * WINDOW_CELLS + WINDOW_CELLS / 2U;
                const f32 x0 = (f32)cell_x;
                const f32 y0 = (f32)cell_y;
                const NphVec3 corners[4] = {nph_vec3(x0, y0, 0.0F), nph_vec3(x0 + side, y0, 0.0F),
                                            nph_vec3(x0 + side, y0 + side, 0.0F),
                                            nph_vec3(x0, y0 + side, 0.0F)};
                for (u32 k = 0; k < 4U; ++k) {
                    f32 nearest = NPH_F32_MAX;
                    for (u32 t = 0; t + 2U < window_tris.len; t += 3U) {
                        const NphVec3 at = nph_closest_point_on_triangle(
                            window_tris.data[t], window_tris.data[t + 1U], window_tris.data[t + 2U],
                            corners[k]);
                        nearest = nph_f32_min(nearest, nph_vec3_distance(at, corners[k]));
                    }
                    farthest = nph_f32_max(farthest, nearest);
                }
            }
        }
        NTEST_LOG("level %u: %u triangles, error %.3f, the windows' area %.1f of %.1f, a corner "
                  "%.3f away",
                  level, cluster_triangles(l, level), (f64)error, (f64)area, (f64)area0,
                  (f64)farthest);
        const f32 slack = 1e-3F;
        levels_moved += nph_f32_abs(area - area0) > slack ? 1U : 0U;
        NTEST_EXPECT_TRUE(nph_f32_abs(area - area0) <= perimeter * error + slack);
        NTEST_EXPECT_TRUE(farthest <= error + slack);
    }
    // The wall does go down, and in the end the windows' corners do move.
    NTEST_EXPECT_LT_UINT(coarsest_triangles(l) * 16U, f->mesh.indices.len / 3U);
    NTEST_EXPECT_GT_UINT(levels_moved, 0U);
    NPH_ARRAY_FREE(&window_tris);
    NPH_ARRAY_FREE(&positions);
    NPH_ARRAY_FREE(&uvs);
}

// Narrow stripes side by side, each a surface of its own, across a wall in one plane: once the
// vertices along their sides have gone (those cost nothing), each side's end can only step along
// the wall's edge to the next side's. Each such step alone moves a border one stripe, so if only
// the step counted, every end along the edge could walk the wall's width in steps within the
// limit, and the stripes fan out from a point. Their sum is what bounds the walk.
NTEST_F(ClusterLod, a_corner_walking_along_its_edge_adds_up_its_steps)
{
    const u32 stripes = 64U;
    const u32 rows = 16U;
    for (u32 y = 0; y < rows; ++y) {
        for (u32 x = 0; x < stripes; ++x) {
            wall_quad(&f->mesh, x, (f32)x, (f32)y);
        }
    }
    mesh_one_range(&f->mesh);
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    LodMesh m;
    NTEST_ASSERT_EQ_INT(lod_mesh_init(&m, f->a, f->mesh.positions.data, f->mesh.attribs.data,
                                      (u32)sizeof(TestAttrib), f->mesh.positions.len,
                                      f->mesh.indices.data, f->mesh.indices.len),
                        NPH_RESULT_OK);
    u8 *locked = NPH_ALLOC_ARRAY_ZEROED(f->a, u8, m.class_count);
    const u32 tris = f->mesh.indices.len / 3U;
    u32 *sub = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, tris);
    NphRenderLodAddedVertexArray added;
    NPH_ARRAY_INIT(&added, f->a);
    const f32 limit = 1.5F;
    LodSimplifyInput in;
    memset(&in, 0, sizeof in);
    in.mesh = &m;
    in.indices = f->mesh.indices.data;
    in.submeshes = sub;
    in.triangle_count = tris;
    in.locked = locked;
    in.target_triangles = 1U;
    in.uvs = uvs.data;
    in.added = &added;
    in.scratch = f->a;
    in.cost_limit = (f64)limit * (f64)limit;
    in.borders = 1;
    u32 kept = 0U;
    f32 error = 0.0F;
    NTEST_ASSERT_EQ_INT(lod_simplify(&in, &kept, &error), NPH_RESULT_OK);
    // How far each end of each stripe still there is from what is left of that stripe.
    f32 farthest = 0.0F;
    u32 present = 0U;
    for (u32 s = 0; s < stripes; ++s) {
        f32 nearest[2] = {NPH_F32_MAX, NPH_F32_MAX};
        const NphVec3 ends[2] = {nph_vec3((f32)s + 0.5F, 0.0F, 0.0F),
                                 nph_vec3((f32)s + 0.5F, (f32)rows, 0.0F)};
        for (u32 t = 0; t < kept; ++t) {
            NphVec3 p[3];
            u32 chart = 0U;
            for (u32 k = 0; k < 3U; ++k) {
                const u32 v = f->mesh.indices.data[(u64)t * 3U + k];
                const bool new_vertex = v >= LOD_ADDED_BASE;
                const u32 place = new_vertex ? added.data[v - LOD_ADDED_BASE].position : v;
                p[k] = f->mesh.positions.data[place];
                chart = f->mesh.attribs.data[new_vertex ? added.data[v - LOD_ADDED_BASE].source : v]
                            .chart;
            }
            if (chart == s) {
                for (u32 e = 0; e < 2U; ++e) {
                    const NphVec3 at = nph_closest_point_on_triangle(p[0], p[1], p[2], ends[e]);
                    nearest[e] = nph_f32_min(nearest[e], nph_vec3_distance(at, ends[e]));
                }
            }
        }
        if (nearest[0] != NPH_F32_MAX) {
            present += 1U;
            farthest = nph_f32_max(farthest, nph_f32_max(nearest[0], nearest[1]));
        }
    }
    NTEST_LOG("%u of %u triangles, %u vertices added, error %.3f, %u of %u stripes left, an end "
              "%.3f from its stripe",
              kept, tris, added.len, (f64)error, present, stripes, (f64)farthest);
    NTEST_EXPECT_LT_UINT(kept, tris / 8U);
    NTEST_EXPECT_TRUE(error <= limit);
    NTEST_EXPECT_GT_UINT(present, stripes / 2U);
    // An end stands half a stripe from where a side of its stood, and that side moved at most the
    // limit.
    NTEST_EXPECT_TRUE(farthest <= limit + 0.5F);
    // No one step moves a border more than a stripe: an error past that is steps added up.
    NTEST_EXPECT_TRUE(error > 1.1F);
    NPH_ARRAY_FREE(&added);
    NPH_FREE_ARRAY(f->a, u32, sub, tris);
    NPH_FREE_ARRAY(f->a, u8, locked, m.class_count);
    lod_mesh_free(&m, f->a);
    NPH_ARRAY_FREE(&uvs);
}

// A group's collapses stop at the limit, short of its target, and it says what it spent;
// the costs read beforehand are of those collapses, and nothing is simplified by reading them.
NTEST_F(ClusterLod, a_cost_limit_stops_a_group_short_of_its_target)
{
    make_torus(&f->mesh, 48U, 24U, 1.0F, 0.35F);
    const u32 tris = f->mesh.indices.len / 3U;
    NphArrayU32 kept_indices;
    NPH_ARRAY_INIT(&kept_indices, f->a);
    u32 *sub = NPH_ALLOC_ARRAY_ZEROED(f->a, u32, tris);
    u32 kept[2] = {0U, 0U};
    f32 error[2] = {0.0F, 0.0F};
    const f32 limit = 0.004F;
    u32 first_positive = 0U;
    for (u32 run = 0; run < 2U; ++run) {
        LodMesh m;
        NTEST_ASSERT_EQ_INT(lod_mesh_init(&m, f->a, f->mesh.positions.data, NULL, 0U,
                                          f->mesh.positions.len, f->mesh.indices.data,
                                          f->mesh.indices.len),
                            NPH_RESULT_OK);
        u8 *locked = NPH_ALLOC_ARRAY_ZEROED(f->a, u8, m.class_count);
        NPH_ARRAY_CLEAR(&kept_indices);
        NTEST_ASSERT_EQ_INT(
            NPH_ARRAY_PUSH_N(&kept_indices, f->mesh.indices.data, f->mesh.indices.len),
            NPH_RESULT_OK);
        LodSimplifyInput in;
        memset(&in, 0, sizeof in);
        in.mesh = &m;
        in.indices = kept_indices.data;
        in.submeshes = sub;
        in.triangle_count = tris;
        in.locked = locked;
        in.target_triangles = tris / 8U;
        in.scratch = f->a;
        in.cost_limit = run == 0U ? (f64)limit * (f64)limit : NPH_F64_MAX;
        if (run == 0U) {
            LodCostArray costs;
            NPH_ARRAY_INIT(&costs, f->a);
            NTEST_ASSERT_EQ_INT(lod_simplify_costs(&in, &costs), NPH_RESULT_OK);
            NTEST_EXPECT_EQ_UINT(costs.len, m.class_count); // a torus: every class has one
            for (u32 i = 0; i < costs.len; ++i) {
                first_positive += costs.data[i] > 0.0 ? 1U : 0U;
            }
            NPH_ARRAY_FREE(&costs);
            NTEST_EXPECT_TRUE(memcmp(kept_indices.data, f->mesh.indices.data,
                                     (size_t)f->mesh.indices.len * sizeof(u32)) == 0);
        }
        NTEST_ASSERT_EQ_INT(lod_simplify(&in, &kept[run], &error[run]), NPH_RESULT_OK);
        NPH_FREE_ARRAY(f->a, u8, locked, m.class_count);
        lod_mesh_free(&m, f->a);
    }
    NTEST_LOG("with the limit %u of %u triangles, error %.5f; without %u, error %.5f", kept[0],
              tris, (f64)error[0], kept[1], (f64)error[1]);
    NTEST_EXPECT_GT_UINT(first_positive, 0U);
    NTEST_EXPECT_GT_UINT(kept[0], tris / 8U);
    NTEST_EXPECT_LT_UINT(kept[0], tris);
    NTEST_EXPECT_TRUE(error[0] <= limit);
    NTEST_EXPECT_LE_UINT(kept[1], tris / 8U);
    NTEST_EXPECT_TRUE(error[1] > limit);
    NPH_FREE_ARRAY(f->a, u32, sub, tris);
    NPH_ARRAY_FREE(&kept_indices);
}

// A flat sheet whose shading normals ripple across it (cloth, a wrinkled awning): its positions
// alone would let every interior vertex go at no error at all, and the shading with them. With
// the normals the build gives, a collapse counts what it does to them: only those along a ripple
// are free, so a level at no error still holds the ripples' strips. A sheet whose normals agree
// costs nothing.
NTEST_F(ClusterLod, a_shading_normal_that_changes_counts_in_the_error)
{
    make_grid(&f->mesh, 64U, 4.0F);
    const u32 vertices = f->mesh.positions.len;
    NphVec3 *rippled = NPH_ALLOC_ARRAY(f->a, NphVec3, vertices);
    NphVec3 *flat = NPH_ALLOC_ARRAY(f->a, NphVec3, vertices);
    for (u32 v = 0; v < vertices; ++v) {
        const f32 a = 0.4F * nph_f32_sin(f->mesh.positions.data[v].x * 12.0F);
        rippled[v] = nph_vec3(nph_f32_sin(a), 0.0F, nph_f32_cos(a));
        flat[v] = nph_vec3(0.0F, 0.0F, 1.0F);
    }
    const NphVec3 *const given[3] = {NULL, flat, rippled};
    u32 free_triangles[3] = {0U, 0U, 0U};
    f32 worst[3] = {0.0F, 0.0F, 0.0F};
    for (u32 run = 0; run < 3U; ++run) {
        NphRenderMeshlets meshlets;
        NphRenderClusterLods lods;
        nph_render_meshlets_init(&meshlets, f->a);
        nph_render_cluster_lods_init(&lods, f->a);
        NphRenderMeshletInput mi;
        memset(&mi, 0, sizeof mi);
        mi.positions = f->mesh.positions.data;
        mi.vertex_count = vertices;
        mi.indices = f->mesh.indices.data;
        mi.index_count = f->mesh.indices.len;
        mi.ranges = f->mesh.ranges.data;
        mi.range_count = f->mesh.ranges.len;
        mi.cone_weight = 0.25F;
        mi.scratch = f->a;
        NTEST_ASSERT_EQ_INT(nph_render_meshlets_build(&mi, &meshlets), NPH_RESULT_OK);
        NphRenderClusterLodInput in;
        memset(&in, 0, sizeof in);
        in.positions = f->mesh.positions.data;
        in.attribs = f->mesh.attribs.data;
        in.attrib_stride = (u32)sizeof(TestAttrib);
        in.normals = given[run];
        in.vertex_count = vertices;
        in.indices = f->mesh.indices.data;
        in.index_count = f->mesh.indices.len;
        in.ranges = f->mesh.ranges.data;
        in.range_count = f->mesh.ranges.len;
        in.meshlets = &meshlets;
        in.cone_weight = 0.25F;
        in.scratch = f->a;
        NTEST_ASSERT_EQ_INT(nph_render_cluster_lods_build(&in, &lods), NPH_RESULT_OK);
        free_triangles[run] = cluster_triangles(&lods, 0U);
        for (u32 level = 1U; level < lods.levels; ++level) {
            f32 error = 0.0F;
            for (u32 c = 0; c < lods.lods.len; ++c) {
                if (lods.lods.data[c].level == level) {
                    error = nph_f32_max(error, lods.groups.data[lods.lods.data[c].group].error);
                }
            }
            worst[run] = nph_f32_max(worst[run], error);
            if (error < 1e-5F) {
                free_triangles[run] = cluster_triangles(&lods, level);
            }
        }
        NTEST_LOG("normals %u: %u levels, %u triangles at no error, errors up to %.5f", run,
                  lods.levels, free_triangles[run], (f64)worst[run]);
        nph_render_cluster_lods_free(&lods);
        nph_render_meshlets_free(&meshlets);
    }
    NTEST_EXPECT_TRUE(worst[0] < 1e-5F);
    NTEST_EXPECT_TRUE(worst[1] < 1e-5F);
    NTEST_EXPECT_TRUE(worst[2] > 1e-3F);
    NTEST_EXPECT_LT_UINT(free_triangles[1] * 4U, free_triangles[2]);
    NPH_FREE_ARRAY(f->a, NphVec3, rippled, vertices);
    NPH_FREE_ARRAY(f->a, NphVec3, flat, vertices);
}

// A true curve's normals vary evenly across it, and come back from the interpolation: giving
// them to the build costs a torus little of what its positions already say.
NTEST_F(ClusterLod, a_true_curves_normals_cost_it_little)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    const u32 vertices = f->mesh.positions.len;
    NphVec3 *normals = NPH_ALLOC_ARRAY(f->a, NphVec3, vertices);
    for (u32 v = 0; v < vertices; ++v) {
        const NphVec3 p = f->mesh.positions.data[v];
        const f32 ring = nph_f32_sqrt(p.x * p.x + p.z * p.z);
        const NphVec3 core = nph_vec3(p.x / ring, 0.0F, p.z / ring);
        normals[v] = nph_vec3_normalize(nph_vec3_sub(p, core));
    }
    f32 error[2] = {0.0F, 0.0F};
    u32 triangles[2] = {0U, 0U};
    for (u32 run = 0; run < 2U; ++run) {
        NphRenderMeshlets meshlets;
        NphRenderClusterLods lods;
        nph_render_meshlets_init(&meshlets, f->a);
        nph_render_cluster_lods_init(&lods, f->a);
        NphRenderMeshletInput mi;
        memset(&mi, 0, sizeof mi);
        mi.positions = f->mesh.positions.data;
        mi.vertex_count = vertices;
        mi.indices = f->mesh.indices.data;
        mi.index_count = f->mesh.indices.len;
        mi.ranges = f->mesh.ranges.data;
        mi.range_count = f->mesh.ranges.len;
        mi.cone_weight = 0.25F;
        mi.scratch = f->a;
        NTEST_ASSERT_EQ_INT(nph_render_meshlets_build(&mi, &meshlets), NPH_RESULT_OK);
        NphRenderClusterLodInput in;
        memset(&in, 0, sizeof in);
        in.positions = f->mesh.positions.data;
        in.attribs = f->mesh.attribs.data;
        in.attrib_stride = (u32)sizeof(TestAttrib);
        in.normals = run == 0U ? NULL : normals;
        in.vertex_count = vertices;
        in.indices = f->mesh.indices.data;
        in.index_count = f->mesh.indices.len;
        in.ranges = f->mesh.ranges.data;
        in.range_count = f->mesh.ranges.len;
        in.meshlets = &meshlets;
        in.cone_weight = 0.25F;
        in.scratch = f->a;
        NTEST_ASSERT_EQ_INT(nph_render_cluster_lods_build(&in, &lods), NPH_RESULT_OK);
        for (u32 c = 0; c < lods.lods.len; ++c) {
            if (lods.lods.data[c].level == 2U) {
                error[run] =
                    nph_f32_max(error[run], lods.groups.data[lods.lods.data[c].group].error);
            }
        }
        triangles[run] = cluster_triangles(&lods, 2U);
        nph_render_cluster_lods_free(&lods);
        nph_render_meshlets_free(&meshlets);
    }
    NTEST_LOG("level 2: %u triangles at errors up to %.5f without normals, %u at %.5f with",
              triangles[0], (f64)error[0], triangles[1], (f64)error[1]);
    NTEST_EXPECT_TRUE(error[1] <= error[0] * 1.5F);
    NTEST_EXPECT_LE_UINT(triangles[1] * 10U, triangles[0] * 12U);
    NPH_FREE_ARRAY(f->a, NphVec3, normals, vertices);
}

// A sampler for surfaces that never voxelize: nothing calls these.
static void never_sampled(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                          NphRenderVoxelSample *out)
{
    (void)user;
    (void)submesh;
    (void)v;
    (void)b;
    (void)footprint;
    memset(out, 0, sizeof *out);
}

static bool never_covers(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint)
{
    (void)user;
    (void)submesh;
    (void)v;
    (void)b;
    (void)footprint;
    return false;
}

// A mesh that may voxelize keeps the simplifier its voxels were measured with: given texture
// coordinates and normals, it adds no vertex and its levels are those it has without them.
NTEST_F(ClusterLod, a_mesh_that_may_voxelize_keeps_its_simplifier)
{
    make_steps(&f->mesh, 8U);
    const u32 vertices = f->mesh.positions.len;
    StepUvs uvs;
    step_uvs(&f->mesh, f->a, &uvs);
    NphVec3 *normals = NPH_ALLOC_ARRAY(f->a, NphVec3, vertices);
    for (u32 v = 0; v < vertices; ++v) {
        normals[v] = nph_vec3_unit_y();
    }
    const u8 surfaces[1] = {NPH_RENDER_VOXEL_SURFACE_NONE};
    NphRenderVoxelSampler sampler;
    memset(&sampler, 0, sizeof sampler);
    sampler.submesh_surface = surfaces;
    sampler.sample = never_sampled;
    sampler.covers = never_covers;
    u32 coarsest[2] = {0U, 0U};
    u32 levels[2] = {0U, 0U};
    for (u32 run = 0; run < 2U; ++run) {
        NphRenderMeshlets meshlets;
        NphRenderClusterLods lods;
        nph_render_meshlets_init(&meshlets, f->a);
        nph_render_cluster_lods_init(&lods, f->a);
        NphRenderMeshletInput mi;
        memset(&mi, 0, sizeof mi);
        mi.positions = f->mesh.positions.data;
        mi.vertex_count = vertices;
        mi.indices = f->mesh.indices.data;
        mi.index_count = f->mesh.indices.len;
        mi.ranges = f->mesh.ranges.data;
        mi.range_count = f->mesh.ranges.len;
        mi.cone_weight = 0.25F;
        mi.scratch = f->a;
        NTEST_ASSERT_EQ_INT(nph_render_meshlets_build(&mi, &meshlets), NPH_RESULT_OK);
        NphRenderClusterLodInput in;
        memset(&in, 0, sizeof in);
        in.positions = f->mesh.positions.data;
        in.attribs = f->mesh.attribs.data;
        in.attrib_stride = (u32)sizeof(TestAttrib);
        in.uvs = run == 0U ? NULL : uvs.data;
        in.normals = run == 0U ? NULL : normals;
        in.voxels = &sampler;
        in.vertex_count = vertices;
        in.indices = f->mesh.indices.data;
        in.index_count = f->mesh.indices.len;
        in.ranges = f->mesh.ranges.data;
        in.range_count = f->mesh.ranges.len;
        in.meshlets = &meshlets;
        in.cone_weight = 0.25F;
        in.scratch = f->a;
        NTEST_ASSERT_EQ_INT(nph_render_cluster_lods_build(&in, &lods), NPH_RESULT_OK);
        NTEST_EXPECT_EQ_UINT(lods.added.len, 0U);
        coarsest[run] = coarsest_triangles(&lods);
        levels[run] = lods.levels;
        nph_render_cluster_lods_free(&lods);
        nph_render_meshlets_free(&meshlets);
    }
    NTEST_EXPECT_EQ_UINT(coarsest[1], coarsest[0]);
    NTEST_EXPECT_EQ_UINT(levels[1], levels[0]);
    NPH_FREE_ARRAY(f->a, NphVec3, normals, vertices);
    NPH_ARRAY_FREE(&uvs);
}

// A big bent card, of its own material, where the surface around it costs little to
// simplify: 8 triangles about a raised centre.
static void make_card(TestMesh *m, NphVec3 c, f32 s, f32 raise)
{
    u32 v[9];
    for (u32 y = 0; y < 3U; ++y) {
        for (u32 x = 0; x < 3U; ++x) {
            const f32 lift = x == 1U && y == 1U ? raise : 0.0F;
            v[y * 3U + x] =
                mesh_vertex(m, nph_vec3_add(c, nph_vec3((f32)x * s - s, (f32)y * s - s, lift)), 1U);
        }
    }
    for (u32 y = 0; y < 2U; ++y) {
        for (u32 x = 0; x < 2U; ++x) {
            const u32 q = y * 3U + x;
            mesh_tri(m, v[q], v[q + 1U], v[q + 4U]);
            mesh_tri(m, v[q], v[q + 4U], v[q + 3U]);
        }
    }
}

// One dear collapse sets a group's whole error, so a level whose groups had to spend it
// would keep all their cheap collapses from being drawn (a tree's bark, its cards in the same
// groups): a level leaves the cards to later levels, and the surface goes on.
NTEST_F(ClusterLod, a_levels_error_leaves_dear_collapses_to_later_levels)
{
    make_torus(&f->mesh, 128U, 48U, 1.0F, 0.35F);
    const u32 first = f->mesh.indices.len;
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 5U, 1U);
    for (u32 i = 0; i < 300U; ++i) {
        const f32 a = 2.0F * NPH_PI * nph_pcg32_next_f32(&rng);
        const f32 h = 0.5F * nph_pcg32_next_f32(&rng) - 0.25F;
        const NphVec3 c = nph_vec3(1.0F * nph_f32_cos(a), h, 1.0F * nph_f32_sin(a));
        make_card(&f->mesh, c, 0.15F, 0.1F);
    }
    const NphRenderIndexRange cards = {first, f->mesh.indices.len - first};
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&f->mesh.ranges, cards));
    NTEST_ASSERT_EQ_INT(build_lods(&f->mesh, f->a, &f->meshlets, &f->lods), NPH_RESULT_OK);
    const NphRenderClusterLods *l = &f->lods;
    f32 error1 = 0.0F;
    for (u32 c = 0; c < l->lods.len; ++c) {
        if (l->lods.data[c].level == 1U) {
            error1 = nph_f32_max(error1, l->groups.data[l->lods.data[c].group].error);
        }
    }
    const u32 level0 = f->mesh.indices.len / 3U;
    NTEST_LOG("%u triangles, level 1 %u at errors up to %.4f, the coarsest level %u of %u", level0,
              cluster_triangles(l, 1U), (f64)error1, coarsest_triangles(l), l->levels);
    NTEST_EXPECT_TRUE(error1 < 0.03F);
    NTEST_EXPECT_LT_UINT(cluster_triangles(l, 1U) * 10U, level0 * 9U);
    NTEST_EXPECT_LT_UINT(coarsest_triangles(l) * 4U, level0);
}
