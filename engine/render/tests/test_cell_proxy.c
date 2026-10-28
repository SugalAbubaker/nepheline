// SPDX-License-Identifier: Apache-2.0
// Cell proxies (render/cell_proxy.h; docs/render.md "Cell proxies";
// ADR-0062): a proxy's records are a mesh of voxel levels the renderer can
// walk; what its finest level stops from outside is what its instances stop;
// what no ray from outside reaches (inside a closed box, behind an occluder)
// is left out; the same input gives the same bytes. CPU only.
#include <nepheline/core/alloc_debug.h>
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/cell_proxy.h>
#include <nepheline/render/cluster_lod.h>
#include <nepheline/render/meshlet.h>
#include <nepheline/render/voxel.h>

#include <ntest/ntest.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct ProxyFixture {
    NphDebugAllocator da;
    NphAllocator *a;
} ProxyFixture;

typedef ProxyFixture CellProxy;

NTEST_SETUP(CellProxy)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    f->a = nph_debug_allocator_allocator(&f->da);
}

NTEST_TEARDOWN(CellProxy)
{
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

static NphVec3 random_unit(NphPcg32 *rng)
{
    const f32 z = 2.0F * nph_pcg32_next_f32(rng) - 1.0F;
    const f32 phi = 2.0F * NPH_PI * nph_pcg32_next_f32(rng);
    const f32 r = nph_f32_sqrt(nph_f32_max(1.0F - z * z, 0.0F));
    return nph_vec3(r * nph_f32_cos(phi), r * nph_f32_sin(phi), z);
}

static void tangents(NphVec3 n, NphVec3 *t, NphVec3 *b)
{
    const NphVec3 up = nph_f32_abs(n.z) < 0.9F ? nph_vec3_unit_z() : nph_vec3_unit_x();
    *t = nph_vec3_normalize(nph_vec3_cross(up, n));
    *b = nph_vec3_cross(n, *t);
}

// A mesh, its levels, and its material: one colour, each triangle's own
// normal.
typedef struct TestMesh {
    NPH_ARRAY(NphVec3) positions;
    NphArrayU32 indices;
    NphRenderIndexRange range;
    NphRenderMeshlets meshlets;
    NphRenderClusterLods lods;
    NphRenderVoxelSampler sampler;
    u8 surface[1];
    NphVec3 color;
} TestMesh;

static void mesh_sample(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                        NphRenderVoxelSample *out)
{
    (void)submesh;
    (void)b;
    (void)footprint;
    const TestMesh *m = (const TestMesh *)user;
    const NphVec3 *p = m->positions.data;
    out->base_color = m->color;
    out->normal = nph_vec3_normalize(
        nph_vec3_cross(nph_vec3_sub(p[v[1]], p[v[0]]), nph_vec3_sub(p[v[2]], p[v[0]])));
    out->roughness = 0.5F;
    out->metallic = 0.0F;
    out->occlusion = 1.0F;
}

static void mesh_init(TestMesh *m, NphAllocator *a, NphVec3 color)
{
    memset(m, 0, sizeof *m);
    NPH_ARRAY_INIT(&m->positions, a);
    NPH_ARRAY_INIT(&m->indices, a);
    nph_render_meshlets_init(&m->meshlets, a);
    nph_render_cluster_lods_init(&m->lods, a);
    m->color = color;
    m->surface[0] = NPH_RENDER_VOXEL_SURFACE_OPAQUE;
    m->sampler.user = m;
    m->sampler.submesh_surface = m->surface;
    m->sampler.sample = mesh_sample;
}

static void mesh_free(TestMesh *m)
{
    NPH_ARRAY_FREE(&m->positions);
    NPH_ARRAY_FREE(&m->indices);
    nph_render_meshlets_free(&m->meshlets);
    nph_render_cluster_lods_free(&m->lods);
}

// `count` needles in the cube of `side` from `corner`: thin closed triangular
// prisms, as the landscape's pines' needles are modelled.
static void mesh_needles(TestMesh *m, NphPcg32 *rng, u32 count, NphVec3 corner, f32 side,
                         f32 length, f32 radius)
{
    for (u32 i = 0; i < count; ++i) {
        const NphVec3 c = nph_vec3_add(corner, nph_vec3(side * nph_pcg32_next_f32(rng),
                                                        side * nph_pcg32_next_f32(rng),
                                                        side * nph_pcg32_next_f32(rng)));
        const NphVec3 d = random_unit(rng);
        NphVec3 t;
        NphVec3 b;
        tangents(d, &t, &b);
        const NphVec3 half = nph_vec3_scale(d, 0.5F * length);
        const u32 base = m->positions.len;
        for (u32 end = 0; end < 2U; ++end) {
            const NphVec3 at = end == 0U ? nph_vec3_sub(c, half) : nph_vec3_add(c, half);
            for (u32 k = 0; k < 3U; ++k) {
                const f32 phi = 2.0F * NPH_PI * (f32)k / 3.0F;
                const NphVec3 off = nph_vec3_add(nph_vec3_scale(t, radius * nph_f32_cos(phi)),
                                                 nph_vec3_scale(b, radius * nph_f32_sin(phi)));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->positions, nph_vec3_add(at, off)));
            }
        }
        const u32 q[24] = {base,      base + 2U, base + 1U, base + 3U, base + 4U, base + 5U,
                           base,      base + 1U, base + 4U, base,      base + 4U, base + 3U,
                           base + 1U, base + 2U, base + 5U, base + 1U, base + 5U, base + 4U,
                           base + 2U, base,      base + 3U, base + 2U, base + 3U, base + 5U};
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&m->indices, q, 24U));
    }
}

// A closed box from `lo` to `hi`, each face `n` x `n` quads, wound outward.
static void mesh_box(TestMesh *m, NphVec3 lo, NphVec3 hi, u32 n)
{
    const f32 l[3] = {lo.x, lo.y, lo.z};
    const f32 h[3] = {hi.x, hi.y, hi.z};
    for (u32 axis = 0; axis < 3U; ++axis) {
        const u32 u = (axis + 1U) % 3U;
        const u32 w = (axis + 2U) % 3U;
        for (u32 side = 0; side < 2U; ++side) {
            const u32 base = m->positions.len;
            for (u32 j = 0; j <= n; ++j) {
                for (u32 i = 0; i <= n; ++i) {
                    f32 p[3];
                    p[axis] = side == 0U ? l[axis] : h[axis];
                    p[u] = l[u] + (h[u] - l[u]) * (f32)i / (f32)n;
                    p[w] = l[w] + (h[w] - l[w]) * (f32)j / (f32)n;
                    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&m->positions, nph_vec3(p[0], p[1], p[2])));
                }
            }
            for (u32 j = 0; j < n; ++j) {
                for (u32 i = 0; i < n; ++i) {
                    const u32 a = base + j * (n + 1U) + i;
                    const u32 q[6] = {a, a + 1U, a + n + 2U, a, a + n + 2U, a + n + 1U};
                    const u32 r[6] = {a, a + n + 2U, a + 1U, a, a + n + 1U, a + n + 2U};
                    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&m->indices, side == 0U ? r : q, 6U));
                }
            }
        }
    }
}

// The meshlets and levels of `m` over its one submesh, its voxels made with
// its material.
static NphResult mesh_build(TestMesh *m, NphAllocator *a)
{
    m->range.index_offset = 0U;
    m->range.index_count = m->indices.len;
    NphRenderMeshletInput mi;
    memset(&mi, 0, sizeof mi);
    mi.positions = m->positions.data;
    mi.vertex_count = m->positions.len;
    mi.indices = m->indices.data;
    mi.index_count = m->indices.len;
    mi.ranges = &m->range;
    mi.range_count = 1U;
    mi.cone_weight = 0.25F;
    mi.scratch = a;
    NPH_TRY(nph_render_meshlets_build(&mi, &m->meshlets));
    NphRenderClusterLodInput in;
    memset(&in, 0, sizeof in);
    in.positions = m->positions.data;
    in.vertex_count = m->positions.len;
    in.indices = m->indices.data;
    in.index_count = m->indices.len;
    in.ranges = &m->range;
    in.range_count = 1U;
    in.meshlets = &m->meshlets;
    in.cone_weight = 0.25F;
    in.voxels = &m->sampler;
    in.scratch = a;
    return nph_render_cluster_lods_build(&in, &m->lods);
}

static NphRenderProxyMesh proxy_mesh(const TestMesh *m, bool with_sampler)
{
    NphRenderProxyMesh pm;
    memset(&pm, 0, sizeof pm);
    pm.lods = &m->lods;
    pm.positions = m->positions.data;
    pm.sampler = with_sampler ? &m->sampler : NULL;
    pm.submesh_count = 1U;
    return pm;
}

// A turn about y by `angle`, a uniform `scale`, and a shift.
static NphRenderProxyInstance instance_of(u32 mesh, f32 angle, f32 scale, NphVec3 shift)
{
    NphRenderProxyInstance pi;
    memset(&pi, 0, sizeof pi);
    const f64 c = cos((f64)angle) * (f64)scale;
    const f64 s = sin((f64)angle) * (f64)scale;
    pi.transform[0][0] = c;
    pi.transform[0][2] = s;
    pi.transform[1][1] = (f64)scale;
    pi.transform[2][0] = -s;
    pi.transform[2][2] = c;
    pi.transform[0][3] = (f64)shift.x;
    pi.transform[1][3] = (f64)shift.y;
    pi.transform[2][3] = (f64)shift.z;
    pi.mesh = mesh;
    return pi;
}

static NphVec3 apply(const NphRenderProxyInstance *pi, NphVec3 p)
{
    f64 out[3];
    for (u32 r = 0; r < 3U; ++r) {
        out[r] = pi->transform[r][0] * (f64)p.x + pi->transform[r][1] * (f64)p.y +
                 pi->transform[r][2] * (f64)p.z + pi->transform[r][3];
    }
    return nph_vec3((f32)out[0], (f32)out[1], (f32)out[2]);
}

// ---------------------------------------------------------------------------
// The records
// ---------------------------------------------------------------------------

static bool sphere_holds(NphSphere outer, NphVec3 c, f32 r)
{
    return nph_vec3_distance(outer.center, c) + r <= outer.radius * 1.00001F + 1e-6F;
}

// A proxy's records are a voxel-only mesh's levels: every cluster a voxel
// cluster of its level's cell, its bricks and voxels in order; level 0 first
// and made by no group, every other cluster made by a group of the level
// below whose error is its cell; each cluster consumed by one group of its
// level, listed there once, whose sphere holds what the cluster's maker's
// does (level 0: its bounds); the root group last; every group a leaf of the
// hierarchy once.
static void check_proxy(const NphRenderClusterLods *l, f32 cell)
{
    const u32 n = l->clusters.meshlets.len;
    NTEST_ASSERT_GT_UINT(n, 0U);
    NTEST_ASSERT_EQ_UINT(l->lods.len, n);
    NTEST_ASSERT_GT_UINT(l->levels, 0U);
    NTEST_EXPECT_EQ_UINT(l->clusters.vertices.len, 0U);
    NTEST_EXPECT_EQ_UINT(l->clusters.triangles.len, 0U);
    u32 next_brick = 0U;
    u32 next_voxel = 0U;
    u32 level0 = 0U;
    u32 prev_level = 0U;
    for (u32 c = 0; c < n; ++c) {
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        const NphRenderClusterLod *lod = &l->lods.data[c];
        NTEST_ASSERT_EQ_UINT(m->vertex_count, 0U);
        NTEST_EXPECT_EQ_UINT(m->submesh, 0U);
        NTEST_ASSERT_LT_UINT(lod->level, l->levels);
        NTEST_EXPECT_GE_UINT(lod->level, prev_level);
        prev_level = lod->level;
        level0 += lod->level == 0U ? 1U : 0U;
        const f32 level_cell = cell * (f32)(1U << lod->level);
        NTEST_ASSERT_EQ_UINT(m->vertex_offset, next_brick);
        NTEST_ASSERT_TRUE(m->triangle_count >= 1U &&
                          m->triangle_count <= NPH_RENDER_VOXEL_CLUSTER_BRICKS);
        u32 voxels = 0U;
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
            const u32 count = nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
            NTEST_EXPECT_GT_UINT(count, 0U);
            NTEST_ASSERT_EQ_UINT(br->first_voxel, next_voxel);
            NTEST_EXPECT_EQ_UINT(nph_f32_to_bits(br->cell), nph_f32_to_bits(level_cell));
            next_voxel += count;
            voxels += count;
        }
        NTEST_EXPECT_LE_UINT(voxels, NPH_RENDER_VOXEL_CLUSTER_VOXELS);
        next_brick += m->triangle_count;
        if (lod->level == 0U) {
            NTEST_EXPECT_EQ_UINT(lod->group, NPH_RENDER_LOD_NONE);
        } else {
            NTEST_ASSERT_LT_UINT(lod->group, l->groups.len);
            const NphRenderLodGroup *maker = &l->groups.data[lod->group];
            NTEST_EXPECT_EQ_UINT(maker->level + 1U, lod->level);
            NTEST_EXPECT_EQ_UINT(nph_f32_to_bits(maker->error), nph_f32_to_bits(level_cell));
            // Its maker's sphere, the one its error is projected from, holds it
            // (a coarser cell's footprints may reach past the finer ones').
            const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
            NTEST_EXPECT_TRUE(
                sphere_holds(nph_sphere(maker->center, maker->radius), b->center, b->radius));
        }
        NTEST_ASSERT_LT_UINT(lod->parent, l->groups.len);
        const NphRenderLodGroup *consumer = &l->groups.data[lod->parent];
        NTEST_EXPECT_EQ_UINT(consumer->level, lod->level);
        u32 listed = 0U;
        for (u32 i = 0; i < consumer->cluster_count; ++i) {
            listed += l->group_clusters.data[consumer->cluster_offset + i] == c ? 1U : 0U;
        }
        NTEST_EXPECT_EQ_UINT(listed, 1U);
        const NphSphere outer = nph_sphere(consumer->center, consumer->radius);
        if (lod->group == NPH_RENDER_LOD_NONE) {
            const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
            NTEST_EXPECT_TRUE(sphere_holds(outer, b->center, b->radius));
        } else {
            const NphRenderLodGroup *maker = &l->groups.data[lod->group];
            NTEST_EXPECT_TRUE(sphere_holds(outer, maker->center, maker->radius));
            NTEST_EXPECT_TRUE(consumer->error > maker->error);
        }
    }
    NTEST_EXPECT_EQ_UINT(next_brick, l->bricks.len);
    NTEST_EXPECT_EQ_UINT(next_voxel, l->voxels.len);
    NTEST_EXPECT_EQ_UINT(l->level0_clusters, level0);
    u32 listed = 0U;
    for (u32 g = 0; g < l->groups.len; ++g) {
        listed += l->groups.data[g].cluster_count;
    }
    NTEST_EXPECT_EQ_UINT(listed, n);
    NTEST_EXPECT_EQ_UINT(nph_f32_to_bits(l->groups.data[l->groups.len - 1U].error),
                         nph_f32_to_bits(NPH_F32_MAX));
    // Every group a leaf once under node 0.
    NTEST_ASSERT_GT_UINT(l->nodes.len, 0U);
    u32 leaves = 0U;
    NphArrayU32 stack;
    NPH_ARRAY_INIT(&stack, l->nodes.alloc);
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&stack, 0U));
    while (stack.len != 0U) {
        const u32 node = stack.data[--stack.len];
        NTEST_ASSERT_LT_UINT(node, l->nodes.len);
        for (u32 k = 0; k < NPH_RENDER_LOD_NODE_CHILDREN; ++k) {
            const u32 child = l->nodes.data[node].child[k];
            if (child == NPH_RENDER_LOD_NONE) {
                continue;
            }
            if ((child & NPH_RENDER_LOD_LEAF) != 0U) {
                NTEST_EXPECT_LT_UINT(child & ~NPH_RENDER_LOD_LEAF, l->groups.len);
                leaves += 1U;
            } else {
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&stack, child));
            }
        }
    }
    NPH_ARRAY_FREE(&stack);
    NTEST_EXPECT_EQ_UINT(leaves, l->groups.len);
}

// ---------------------------------------------------------------------------
// What it stops
// ---------------------------------------------------------------------------

// The instances' level-0 triangles in the proxy's space, and their bounds.
// In chunks of SOUP_CHUNK triangles, sorted by the cell of a coarse grid
// their centroids lie in, each chunk with its bounds: a ray tests the chunks
// it meets.
#define SOUP_CHUNK 16U

typedef struct SoupTri {
    NphVec3 p[3];
    u64 key;
} SoupTri;

typedef struct Soup {
    NPH_ARRAY(SoupTri) tris;
    NPH_ARRAY(NphAABB) chunks;
    NphAABB bounds;
} Soup;

static int compare_soup(const void *x, const void *y)
{
    const SoupTri *p = (const SoupTri *)x;
    const SoupTri *q = (const SoupTri *)y;
    return p->key < q->key ? -1 : (p->key > q->key ? 1 : 0);
}

static void soup_build(Soup *s, NphAllocator *a, const TestMesh *const *meshes,
                       const NphRenderProxyInstance *instances, u32 count)
{
    NPH_ARRAY_INIT(&s->tris, a);
    NPH_ARRAY_INIT(&s->chunks, a);
    s->bounds.min = nph_vec3_splat(NPH_F32_MAX);
    s->bounds.max = nph_vec3_splat(-NPH_F32_MAX);
    for (u32 i = 0; i < count; ++i) {
        const TestMesh *m = meshes[instances[i].mesh];
        for (u32 k = 0; k + 2U < m->indices.len; k += 3U) {
            SoupTri t;
            for (u32 c = 0; c < 3U; ++c) {
                t.p[c] = apply(&instances[i], m->positions.data[m->indices.data[k + c]]);
                s->bounds.min = nph_vec3_min(s->bounds.min, t.p[c]);
                s->bounds.max = nph_vec3_max(s->bounds.max, t.p[c]);
            }
            const NphVec3 g = nph_vec3_scale(nph_vec3_add(nph_vec3_add(t.p[0], t.p[1]), t.p[2]),
                                             1.0F / 3.0F / 0.25F);
            t.key = ((u64)(u32)((i32)nph_f32_floor(g.z) + 4096) << 40U) |
                    ((u64)(u32)((i32)nph_f32_floor(g.x) + 4096) << 20U) |
                    (u64)(u32)((i32)nph_f32_floor(g.y) + 4096);
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&s->tris, t));
        }
    }
    qsort(s->tris.data, s->tris.len, sizeof(SoupTri), compare_soup);
    for (u32 k = 0; k < s->tris.len; k += SOUP_CHUNK) {
        NphAABB b;
        b.min = nph_vec3_splat(NPH_F32_MAX);
        b.max = nph_vec3_splat(-NPH_F32_MAX);
        for (u32 j = k; j < k + SOUP_CHUNK && j < s->tris.len; ++j) {
            for (u32 c = 0; c < 3U; ++c) {
                b.min = nph_vec3_min(b.min, s->tris.data[j].p[c]);
                b.max = nph_vec3_max(b.max, s->tris.data[j].p[c]);
            }
        }
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&s->chunks, b));
    }
}

static void soup_free(Soup *s)
{
    NPH_ARRAY_FREE(&s->tris);
    NPH_ARRAY_FREE(&s->chunks);
}

// Whether the ray o + t d (t >= 0) meets the box.
static bool ray_box(NphVec3 o, NphVec3 d, const NphAABB *b)
{
    const f32 oo[3] = {o.x, o.y, o.z};
    const f32 dd[3] = {d.x, d.y, d.z};
    const f32 lo[3] = {b->min.x, b->min.y, b->min.z};
    const f32 hi[3] = {b->max.x, b->max.y, b->max.z};
    f32 t0 = 0.0F;
    f32 t1 = NPH_F32_MAX;
    for (u32 a = 0; a < 3U; ++a) {
        if (nph_f32_abs(dd[a]) < 1e-12F) {
            if (oo[a] < lo[a] || oo[a] > hi[a]) {
                return false;
            }
            continue;
        }
        const f32 ta = (lo[a] - oo[a]) / dd[a];
        const f32 tb = (hi[a] - oo[a]) / dd[a];
        t0 = nph_f32_max(t0, nph_f32_min(ta, tb));
        t1 = nph_f32_min(t1, nph_f32_max(ta, tb));
    }
    return t0 <= t1;
}

static bool ray_triangle(NphVec3 o, NphVec3 d, NphVec3 a, NphVec3 b, NphVec3 c)
{
    const f64 e1[3] = {(f64)b.x - (f64)a.x, (f64)b.y - (f64)a.y, (f64)b.z - (f64)a.z};
    const f64 e2[3] = {(f64)c.x - (f64)a.x, (f64)c.y - (f64)a.y, (f64)c.z - (f64)a.z};
    const f64 dd[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    const f64 p[3] = {dd[1] * e2[2] - dd[2] * e2[1], dd[2] * e2[0] - dd[0] * e2[2],
                      dd[0] * e2[1] - dd[1] * e2[0]};
    const f64 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (nph_f64_abs(det) < 1e-300) {
        return false;
    }
    const f64 inv = 1.0 / det;
    const f64 s[3] = {(f64)o.x - (f64)a.x, (f64)o.y - (f64)a.y, (f64)o.z - (f64)a.z};
    const f64 u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    const f64 q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                      s[0] * e1[1] - s[1] * e1[0]};
    const f64 v = (dd[0] * q[0] + dd[1] * q[1] + dd[2] * q[2]) * inv;
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv >= 0.0;
}

static bool soup_hit(const Soup *s, NphVec3 o, NphVec3 d)
{
    for (u32 c = 0; c < s->chunks.len; ++c) {
        if (!ray_box(o, d, &s->chunks.data[c])) {
            continue;
        }
        for (u32 k = c * SOUP_CHUNK; k < (c + 1U) * SOUP_CHUNK && k < s->tris.len; ++k) {
            const NphVec3 *p = s->tris.data[k].p;
            if (ray_triangle(o, d, p[0], p[1], p[2])) {
                return true;
            }
        }
    }
    return false;
}

// Whether the ray meets a footprint of the proxy's level-`level` clusters.
static bool proxy_hit(const NphRenderClusterLods *l, u32 level, NphVec3 o, NphVec3 d)
{
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        if (l->lods.data[c].level != level) {
            continue;
        }
        // The cluster's sphere against the ray's line.
        const NphRenderMeshletBounds *b = &l->clusters.bounds.data[c];
        const NphVec3 oc = nph_vec3_sub(b->center, o);
        const f32 along = nph_vec3_dot(oc, d);
        const f32 across2 = nph_vec3_dot(oc, oc) - along * along;
        if (across2 > b->radius * b->radius) {
            continue;
        }
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        for (u32 k = 0; k < m->triangle_count; ++k) {
            NphRenderVoxelHit hit;
            if (nph_render_voxel_trace(&l->bricks.data[m->vertex_offset + k], l->voxels.data, o, d,
                                       0.0F, 1e30F, &hit)) {
                return true;
            }
        }
    }
    return false;
}

// The share of a grid of n x n parallel rays along `d` across the bounds'
// sphere that meet the triangles, and that meet the proxy's level.
static void coverage(const Soup *s, const NphRenderClusterLods *l, u32 level, NphVec3 d, u32 n,
                     f64 *out_soup, f64 *out_proxy)
{
    NphVec3 t;
    NphVec3 b;
    tangents(d, &t, &b);
    const NphVec3 centre = nph_vec3_scale(nph_vec3_add(s->bounds.min, s->bounds.max), 0.5F);
    const f32 reach = 0.5F * nph_vec3_distance(s->bounds.min, s->bounds.max);
    u32 hits_soup = 0U;
    u32 hits_proxy = 0U;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f32 u = (((f32)i + 0.5F) / (f32)n * 2.0F - 1.0F) * reach;
            const f32 v = (((f32)j + 0.5F) / (f32)n * 2.0F - 1.0F) * reach;
            const NphVec3 o =
                nph_vec3_add(nph_vec3_sub(centre, nph_vec3_scale(d, 2.0F * reach)),
                             nph_vec3_add(nph_vec3_scale(t, u), nph_vec3_scale(b, v)));
            hits_soup += soup_hit(s, o, d) ? 1U : 0U;
            hits_proxy += proxy_hit(l, level, o, d) ? 1U : 0U;
        }
    }
    *out_soup = (f64)hits_soup / (f64)(n * n);
    *out_proxy = (f64)hits_proxy / (f64)(n * n);
}

// Whether the ray meets instance `pi`'s cut at `error` (proxy units): its
// clusters made at most that error and consumed above it, triangles and
// voxels, the ray taken into its mesh's space (a turn and a uniform scale).
static bool cut_hit(const TestMesh *m, const NphRenderProxyInstance *pi, f64 error, NphVec3 o,
                    NphVec3 d)
{
    const f64 scale =
        sqrt(pi->transform[0][0] * pi->transform[0][0] + pi->transform[1][0] * pi->transform[1][0] +
             pi->transform[2][0] * pi->transform[2][0]);
    const f64 rel[3] = {(f64)o.x - pi->transform[0][3], (f64)o.y - pi->transform[1][3],
                        (f64)o.z - pi->transform[2][3]};
    const f64 dd[3] = {(f64)d.x, (f64)d.y, (f64)d.z};
    f64 om[3];
    f64 dm[3];
    for (u32 c = 0; c < 3U; ++c) {
        // The inverse of s R: R^T / s.
        om[c] = (pi->transform[0][c] * rel[0] + pi->transform[1][c] * rel[1] +
                 pi->transform[2][c] * rel[2]) /
                (scale * scale);
        dm[c] = (pi->transform[0][c] * dd[0] + pi->transform[1][c] * dd[1] +
                 pi->transform[2][c] * dd[2]) /
                (scale * scale);
    }
    const NphVec3 mo = nph_vec3((f32)om[0], (f32)om[1], (f32)om[2]);
    const NphVec3 md = nph_vec3((f32)dm[0], (f32)dm[1], (f32)dm[2]);
    const NphVec3 mdu = nph_vec3_normalize(md);
    const f64 t = error / scale;
    const NphRenderClusterLods *l = &m->lods;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        const NphRenderClusterLod *lod = &l->lods.data[c];
        const f64 made =
            lod->group == NPH_RENDER_LOD_NONE ? 0.0 : (f64)l->groups.data[lod->group].error;
        const f64 consumed = (f64)l->groups.data[lod->parent].error;
        if (made > t || consumed <= t) {
            continue;
        }
        // Its sphere against the ray's line, in the mesh's space.
        const NphRenderMeshletBounds *bd = &l->clusters.bounds.data[c];
        const NphVec3 oc = nph_vec3_sub(bd->center, mo);
        const f32 along = nph_vec3_dot(oc, mdu);
        if (nph_vec3_dot(oc, oc) - along * along > bd->radius * bd->radius) {
            continue;
        }
        const NphRenderMeshlet *ml = &l->clusters.meshlets.data[c];
        if (ml->vertex_count == 0U) {
            for (u32 k = 0; k < ml->triangle_count; ++k) {
                NphRenderVoxelHit hit;
                if (nph_render_voxel_trace(&l->bricks.data[ml->vertex_offset + k], l->voxels.data,
                                           mo, md, 0.0F, 1e30F, &hit)) {
                    return true;
                }
            }
            continue;
        }
        for (u32 k = 0; k < ml->triangle_count; ++k) {
            const u8 *corner = &l->clusters.triangles.data[((u64)ml->triangle_offset + k) * 3U];
            const NphVec3 *p = m->positions.data;
            const u32 *v = &l->clusters.vertices.data[ml->vertex_offset];
            if (ray_triangle(mo, md, p[v[corner[0]]], p[v[corner[1]]], p[v[corner[2]]])) {
                return true;
            }
        }
    }
    return false;
}

// The share of coverage()'s rays that meet the instances' cuts at `error`.
static void cut_coverage(const Soup *s, const TestMesh *const *meshes,
                         const NphRenderProxyInstance *instances, u32 count, f64 error, NphVec3 d,
                         u32 n, f64 *out)
{
    NphVec3 t;
    NphVec3 b;
    tangents(d, &t, &b);
    const NphVec3 centre = nph_vec3_scale(nph_vec3_add(s->bounds.min, s->bounds.max), 0.5F);
    const f32 reach = 0.5F * nph_vec3_distance(s->bounds.min, s->bounds.max);
    u32 hits = 0U;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f32 u = (((f32)i + 0.5F) / (f32)n * 2.0F - 1.0F) * reach;
            const f32 v = (((f32)j + 0.5F) / (f32)n * 2.0F - 1.0F) * reach;
            const NphVec3 o =
                nph_vec3_add(nph_vec3_sub(centre, nph_vec3_scale(d, 2.0F * reach)),
                             nph_vec3_add(nph_vec3_scale(t, u), nph_vec3_scale(b, v)));
            bool hit = false;
            for (u32 k = 0; k < count && !hit; ++k) {
                hit = cut_hit(meshes[instances[k].mesh], &instances[k], error, o, d);
            }
            hits += hit ? 1U : 0U;
        }
    }
    *out = (f64)hits / (f64)(n * n);
}

// The voxels of the proxy's level `level` whose cells lie inside the box.
static u32 voxels_inside(const NphRenderClusterLods *l, u32 level, NphVec3 lo, NphVec3 hi)
{
    u32 inside = 0U;
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        if (l->lods.data[c].level != level) {
            continue;
        }
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
            for (u32 bit = 0; bit < 64U; ++bit) {
                if (((br->mask[bit >> 5U] >> (bit & 31U)) & 1U) == 0U) {
                    continue;
                }
                const NphVec3 cl = nph_vec3_add(
                    br->origin, nph_vec3_scale(nph_vec3((f32)(bit & 3U), (f32)((bit >> 2U) & 3U),
                                                        (f32)(bit >> 4U)),
                                               br->cell));
                const NphVec3 ch = nph_vec3_add(cl, nph_vec3_splat(br->cell));
                inside += cl.x >= lo.x && cl.y >= lo.y && cl.z >= lo.z && ch.x <= hi.x &&
                                  ch.y <= hi.y && ch.z <= hi.z
                              ? 1U
                              : 0U;
            }
        }
    }
    return inside;
}

// A level's cells, by their coordinates.
typedef struct CellAt {
    i32 c[3];
} CellAt;

typedef NPH_ARRAY(CellAt) CellSet;

static void cell_set(const NphRenderClusterLods *l, u32 level, NphAllocator *a, CellSet *out)
{
    NPH_ARRAY_INIT(out, a);
    for (u32 c = 0; c < l->clusters.meshlets.len; ++c) {
        if (l->lods.data[c].level != level) {
            continue;
        }
        const NphRenderMeshlet *m = &l->clusters.meshlets.data[c];
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &l->bricks.data[m->vertex_offset + k];
            for (u32 bit = 0; bit < 64U; ++bit) {
                if (((br->mask[bit >> 5U] >> (bit & 31U)) & 1U) == 0U) {
                    continue;
                }
                CellAt at;
                at.c[0] = (i32)nph_f32_round(br->origin.x / br->cell) + (i32)(bit & 3U);
                at.c[1] = (i32)nph_f32_round(br->origin.y / br->cell) + (i32)((bit >> 2U) & 3U);
                at.c[2] = (i32)nph_f32_round(br->origin.z / br->cell) + (i32)(bit >> 4U);
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(out, at));
            }
        }
    }
}

// Whether `s` holds a cell within `reach` of `c` along every axis.
static bool cell_set_has(const CellSet *s, const i32 c[3], i32 reach)
{
    for (u32 i = 0; i < s->len; ++i) {
        const i32 *q = s->data[i].c;
        if (q[0] - c[0] <= reach && c[0] - q[0] <= reach && q[1] - c[1] <= reach &&
            c[1] - q[1] <= reach && q[2] - c[2] <= reach && c[2] - q[2] <= reach) {
            return true;
        }
    }
    return false;
}

static NphRenderCellProxyInput proxy_input(const NphRenderProxyMesh *meshes, u32 mesh_count,
                                           const NphRenderProxyInstance *instances, u32 count,
                                           f32 cell, NphAllocator *a)
{
    NphRenderCellProxyInput in;
    memset(&in, 0, sizeof in);
    in.meshes = meshes;
    in.mesh_count = mesh_count;
    in.instances = instances;
    in.instance_count = count;
    in.cell = cell;
    in.distance = 100.0F;
    in.scratch = a;
    return in;
}

static void log_stats(const NphRenderCellProxyStats *s)
{
    NTEST_LOG("%u levels; swept %u voxels at level %u, %u seen (%u occluders'); sources %llu "
              "triangles, %llu voxels",
              s->levels, s->swept, s->visibility_level, s->visible, s->occluders,
              (unsigned long long)s->triangles, (unsigned long long)s->footprints);
    for (u32 lvl = 0; lvl < s->levels; ++lvl) {
        NTEST_LOG("  level %u: %u voxels, %u clusters", lvl, s->voxels[lvl], s->clusters[lvl]);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

NTEST_F(CellProxy, a_turned_sheets_voxels_face_where_it_faces)
{
    // A sheet in its mesh's xz plane, turned a quarter about z into the plane
    // x = 1.1 of the proxy.
    TestMesh sheet;
    mesh_init(&sheet, f->a, nph_vec3(0.7F, 0.3F, 0.2F));
    for (u32 j = 0; j <= 8U; ++j) {
        for (u32 i = 0; i <= 8U; ++i) {
            NPH_IGNORE_RESULT(
                NPH_ARRAY_PUSH(&sheet.positions, nph_vec3(0.25F * (f32)i, 0.0F, 0.25F * (f32)j)));
        }
    }
    for (u32 j = 0; j < 8U; ++j) {
        for (u32 i = 0; i < 8U; ++i) {
            const u32 a = j * 9U + i;
            const u32 q[6] = {a, a + 9U, a + 10U, a, a + 10U, a + 1U};
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&sheet.indices, q, 6U));
        }
    }
    NTEST_ASSERT_EQ_INT(mesh_build(&sheet, f->a), NPH_RESULT_OK);
    const NphRenderProxyMesh meshes[1] = {proxy_mesh(&sheet, true)};
    NphRenderProxyInstance turned;
    memset(&turned, 0, sizeof turned);
    turned.transform[0][1] = -1.0;
    turned.transform[1][0] = 1.0;
    turned.transform[2][2] = 1.0;
    turned.transform[0][3] = 1.1;
    turned.transform[1][3] = 0.1;
    turned.transform[2][3] = 0.1;
    NphRenderCellProxyInput in = proxy_input(meshes, 1U, &turned, 1U, 0.25F, f->a);
    NphRenderClusterLods proxy;
    nph_render_cluster_lods_init(&proxy, f->a);
    NphRenderCellProxyStats stats;
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &proxy, &stats), NPH_RESULT_OK);
    log_stats(&stats);
    check_proxy(&proxy, 0.25F);
    // Level 0: the sheet's 9 x 9 cells, each a sheet facing x, the material's.
    u32 cells = 0U;
    u32 facing = 0U;
    for (u32 c = 0; c < proxy.level0_clusters; ++c) {
        const NphRenderMeshlet *m = &proxy.clusters.meshlets.data[c];
        for (u32 k = 0; k < m->triangle_count; ++k) {
            const NphRenderVoxelBrick *br = &proxy.bricks.data[m->vertex_offset + k];
            const u32 n = nph_popcount_u32(br->mask[0]) + nph_popcount_u32(br->mask[1]);
            for (u32 v = 0; v < n; ++v) {
                NphRenderVoxelDesc d;
                nph_render_voxel_unpack(proxy.voxels.data[br->first_voxel + v], &d);
                cells += 1U;
                facing += nph_f32_abs(d.axis.x) > 0.999F && d.kappa > 1000.0F &&
                                  d.shape == NPH_RENDER_VOXEL_SHAPE_BOX
                              ? 1U
                              : 0U;
                NTEST_EXPECT_NEAR_F32(d.albedo.x, 0.7F, 0.01F);
            }
        }
    }
    NTEST_LOG("%u cells at level 0, %u of them sheets facing x", cells, facing);
    NTEST_EXPECT_EQ_UINT(cells, 81U);
    NTEST_EXPECT_EQ_UINT(facing, cells);
    nph_render_cluster_lods_free(&proxy);
    mesh_free(&sheet);
}

NTEST_F(CellProxy, a_grove_cells_proxy_stops_what_its_trees_stop)
{
    TestMesh tree;
    mesh_init(&tree, f->a, nph_vec3(0.1F, 0.5F, 0.1F));
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 7U, 1U);
    mesh_needles(&tree, &rng, 700U, nph_vec3(-1.5F, 0.0F, -1.5F), 3.0F, 0.3F, 0.01F);
    NTEST_ASSERT_EQ_INT(mesh_build(&tree, f->a), NPH_RESULT_OK);
    NTEST_LOG("the tree: %u levels, %u voxels", tree.lods.levels, tree.lods.voxels.len);
    const NphRenderProxyMesh meshes[1] = {proxy_mesh(&tree, true)};
    NphRenderProxyInstance trees[4];
    for (u32 i = 0; i < 4U; ++i) {
        trees[i] =
            instance_of(0U, 0.7F * (f32)i, 0.9F + 0.05F * (f32)i,
                        nph_vec3(2.0F + 3.5F * (f32)(i % 2U), 0.0F, 2.0F + 3.5F * (f32)(i >> 1U)));
    }
    NphRenderCellProxyInput in = proxy_input(meshes, 1U, trees, 4U, 0.25F, f->a);
    NphRenderClusterLods proxy;
    nph_render_cluster_lods_init(&proxy, f->a);
    NphRenderCellProxyStats stats;
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &proxy, &stats), NPH_RESULT_OK);
    log_stats(&stats);
    check_proxy(&proxy, 0.25F);
    NTEST_EXPECT_GE_UINT(proxy.levels, 3U);
    // Its finest level stops, from every side, about what the trees' own cut
    // at its cell does (their voxels, made from their triangles), and some
    // more: voxels made from voxels keep their sources' coverage along the
    // axes but fit each cell's footprint anew, which adds to what the sources
    // themselves add over the needles (ADR-0061: here about 10 %; the proxy
    // about 10 % more along the axes and up to 20 % along slants between
    // them, turned sources the most; ADR-0062, "Measured").
    const TestMesh *mesh_list[1] = {&tree};
    Soup soup;
    soup_build(&soup, f->a, mesh_list, trees, 4U);
    const NphVec3 dirs[] = {
        nph_vec3(1.0F, 0.0F, 0.0F),
        nph_vec3(0.0F, -1.0F, 0.0F),
        nph_vec3(0.0F, 0.0F, -1.0F),
        nph_vec3_normalize(nph_vec3(0.85F, -0.18F, 0.49F)),
        nph_vec3_normalize(nph_vec3(-0.49F, -0.18F, 0.85F)),
        nph_vec3_normalize(nph_vec3(-0.85F, -0.18F, -0.49F)),
        nph_vec3_normalize(nph_vec3(0.49F, -0.18F, -0.85F)),
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(dirs); ++i) {
        f64 tris = 0.0;
        f64 vox = 0.0;
        f64 cut = 0.0;
        // Rays enough for the tolerance: about a thousand hits along the
        // axes (a 3 % deviation), a quarter of that along the slants.
        const u32 rays = i < 3U ? 128U : 64U;
        coverage(&soup, &proxy, 0U, dirs[i], rays, &tris, &vox);
        cut_coverage(&soup, mesh_list, trees, 4U, 0.25, dirs[i], rays, &cut);
        NTEST_LOG("direction (%.2f %.2f %.2f): the needles stop %.3f of the rays, the trees' cut "
                  "at the proxy's cell %.3f, the proxy %.3f",
                  (f64)dirs[i].x, (f64)dirs[i].y, (f64)dirs[i].z, tris, cut, vox);
        if (i < 3U) {
            NTEST_EXPECT_TRUE(vox / cut > 0.95 && vox / cut < 1.15);
        } else {
            NTEST_EXPECT_TRUE(vox / cut > 0.95 && vox / cut < 1.3);
        }
    }
    soup_free(&soup);
    nph_render_cluster_lods_free(&proxy);
    mesh_free(&tree);
}

// Two closed boxes, one inside the other: a closed surface's voxels leave
// slits a 32nd of a cell wide along its edges and corners, where a cell's
// faces meet and its footprint is fitted to their coverage (a ray through one
// sees what is behind, in the renderer as in the sweep), and a second wall's
// slits are elsewhere.
static void mesh_shell(TestMesh *m, NphVec3 outer_lo, NphVec3 outer_hi, NphVec3 inner_lo,
                       NphVec3 inner_hi)
{
    mesh_box(m, outer_lo, outer_hi, 8U);
    mesh_box(m, inner_lo, inner_hi, 8U);
}

NTEST_F(CellProxy, what_a_closed_shell_hides_is_left_out)
{
    TestMesh box;
    mesh_init(&box, f->a, nph_vec3(0.6F, 0.6F, 0.6F));
    mesh_shell(&box, nph_vec3(0.1F, 0.1F, 0.1F), nph_vec3(3.9F, 3.9F, 3.9F),
               nph_vec3(0.65F, 0.8F, 0.7F), nph_vec3(3.35F, 3.2F, 3.3F));
    NTEST_ASSERT_EQ_INT(mesh_build(&box, f->a), NPH_RESULT_OK);
    TestMesh bush;
    mesh_init(&bush, f->a, nph_vec3(0.1F, 0.6F, 0.1F));
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 11U, 1U);
    mesh_needles(&bush, &rng, 600U, nph_vec3(1.2F, 1.2F, 1.2F), 1.6F, 0.2F, 0.01F);
    NTEST_ASSERT_EQ_INT(mesh_build(&bush, f->a), NPH_RESULT_OK);
    const NphRenderProxyMesh meshes[2] = {proxy_mesh(&box, true), proxy_mesh(&bush, true)};
    const NphRenderProxyInstance parts[2] = {instance_of(0U, 0.0F, 1.0F, nph_vec3_zero()),
                                             instance_of(1U, 0.0F, 1.0F, nph_vec3_zero())};
    NphRenderCellProxyInput in = proxy_input(meshes, 2U, parts, 2U, 0.25F, f->a);
    // Without the sweep, the bush is in the proxy.
    in.flags = NPH_RENDER_PROXY_NO_VISIBILITY;
    NphRenderClusterLods all;
    nph_render_cluster_lods_init(&all, f->a);
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &all, NULL), NPH_RESULT_OK);
    check_proxy(&all, 0.25F);
    in.flags = 0U;
    NphRenderClusterLods seen;
    nph_render_cluster_lods_init(&seen, f->a);
    NphRenderCellProxyStats stats;
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &seen, &stats), NPH_RESULT_OK);
    log_stats(&stats);
    check_proxy(&seen, 0.25F);
    const NphVec3 lo = nph_vec3_splat(1.0F);
    const NphVec3 hi = nph_vec3_splat(3.0F);
    const u32 inside_all = voxels_inside(&all, 0U, lo, hi);
    const u32 inside_seen = voxels_inside(&seen, 0U, lo, hi);
    // The cells some ray reaches alone: the bush is left out, and the inner
    // wall but where an outer slit shows it.
    in.flags = NPH_RENDER_PROXY_NO_DILATION;
    NphRenderClusterLods bare;
    nph_render_cluster_lods_init(&bare, f->a);
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &bare, NULL), NPH_RESULT_OK);
    check_proxy(&bare, 0.25F);
    const u32 inside_bare = voxels_inside(&bare, 0U, lo, hi);
    NTEST_LOG("level 0: %u voxels (%u inside) without the sweep; %u (%u inside) seen; %u (%u "
              "inside) with their neighbours",
              all.voxels.len, inside_all, bare.voxels.len, inside_bare, seen.voxels.len,
              inside_seen);
    NTEST_EXPECT_GT_UINT(inside_all, 100U);
    NTEST_EXPECT_EQ_UINT(inside_bare, 0U);
    NTEST_EXPECT_LT_UINT(bare.voxels.len * 10U, all.voxels.len * 7U);
    // The neighbours add the cells next to seen ones, and those alone.
    CellSet seen_cells;
    CellSet bare_cells;
    cell_set(&seen, stats.visibility_level, f->a, &seen_cells);
    cell_set(&bare, stats.visibility_level, f->a, &bare_cells);
    NTEST_EXPECT_GT_UINT(seen_cells.len, bare_cells.len);
    for (u32 i = 0; i < bare_cells.len; ++i) {
        NTEST_EXPECT_TRUE(cell_set_has(&seen_cells, bare_cells.data[i].c, 0));
    }
    for (u32 i = 0; i < seen_cells.len; ++i) {
        NTEST_EXPECT_TRUE(cell_set_has(&bare_cells, seen_cells.data[i].c, 1));
    }
    NPH_ARRAY_FREE(&seen_cells);
    NPH_ARRAY_FREE(&bare_cells);
    nph_render_cluster_lods_free(&bare);
    // The box itself is whole: every ray at it is stopped.
    const TestMesh *mesh_list[1] = {&box};
    Soup soup;
    soup_build(&soup, f->a, mesh_list, parts, 1U);
    const NphVec3 dirs[] = {nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(0.0F, -1.0F, 0.0F),
                            nph_vec3_normalize(nph_vec3(1.0F, 1.0F, 1.0F))};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(dirs); ++i) {
        f64 tris = 0.0;
        f64 vox = 0.0;
        coverage(&soup, &seen, 0U, dirs[i], 32U, &tris, &vox);
        NTEST_LOG("direction (%.2f %.2f %.2f): the box stops %.3f, its proxy %.3f", (f64)dirs[i].x,
                  (f64)dirs[i].y, (f64)dirs[i].z, tris, vox);
        NTEST_EXPECT_NEAR_F64(vox, tris, 0.01);
    }
    soup_free(&soup);
    nph_render_cluster_lods_free(&all);
    nph_render_cluster_lods_free(&seen);
    mesh_free(&box);
    mesh_free(&bush);
}

NTEST_F(CellProxy, what_occluders_hide_on_every_side_is_left_out)
{
    TestMesh bush;
    mesh_init(&bush, f->a, nph_vec3(0.1F, 0.6F, 0.1F));
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 13U, 1U);
    mesh_needles(&bush, &rng, 600U, nph_vec3(1.2F, 1.2F, 1.2F), 1.6F, 0.2F, 0.01F);
    NTEST_ASSERT_EQ_INT(mesh_build(&bush, f->a), NPH_RESULT_OK);
    TestMesh shell;
    mesh_init(&shell, f->a, nph_vec3(0.5F, 0.5F, 0.5F));
    // Its walls in cells of their own at the occluders' cell (twice the
    // visibility level's, a metre).
    mesh_shell(&shell, nph_vec3(-1.1F, -0.9F, -1.0F), nph_vec3(5.1F, 4.9F, 5.0F),
               nph_vec3(0.15F, 0.3F, 0.2F), nph_vec3(3.85F, 3.7F, 3.8F));
    NTEST_ASSERT_EQ_INT(mesh_build(&shell, f->a), NPH_RESULT_OK);
    // The shell an occluder without materials.
    const NphRenderProxyMesh meshes[2] = {proxy_mesh(&bush, true), proxy_mesh(&shell, false)};
    const NphRenderProxyInstance bush_at = instance_of(0U, 0.0F, 1.0F, nph_vec3_zero());
    const NphRenderProxyInstance shell_at = instance_of(1U, 0.0F, 1.0F, nph_vec3_zero());
    NphRenderCellProxyInput in = proxy_input(meshes, 2U, &bush_at, 1U, 0.25F, f->a);
    NphRenderClusterLods open;
    nph_render_cluster_lods_init(&open, f->a);
    NphRenderCellProxyStats stats;
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &open, &stats), NPH_RESULT_OK);
    log_stats(&stats);
    check_proxy(&open, 0.25F);
    const u32 alone = stats.visible;
    in.occluders = &shell_at;
    in.occluder_count = 1U;
    in.occluder_bounds.min = nph_vec3_splat(-2.0F);
    in.occluder_bounds.max = nph_vec3_splat(6.0F);
    NphRenderClusterLods hidden;
    nph_render_cluster_lods_init(&hidden, f->a);
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &hidden, &stats), NPH_RESULT_OK);
    log_stats(&stats);
    NTEST_LOG("seen alone %u voxels at level %u, inside the shell %u", alone,
              stats.visibility_level, stats.visible);
    NTEST_EXPECT_GT_UINT(alone, 50U);
    NTEST_EXPECT_GT_UINT(stats.occluders, 0U);
    NTEST_EXPECT_EQ_UINT(stats.visible, 0U);
    NTEST_EXPECT_EQ_UINT(hidden.clusters.meshlets.len, 0U);
    NTEST_EXPECT_EQ_UINT(stats.levels, 0U);
    // Outside its bounds the shell counts for nothing.
    in.occluder_bounds.min = nph_vec3_splat(10.0F);
    in.occluder_bounds.max = nph_vec3_splat(11.0F);
    NphRenderClusterLods away;
    nph_render_cluster_lods_init(&away, f->a);
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &away, &stats), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(stats.visible, alone);
    nph_render_cluster_lods_free(&open);
    nph_render_cluster_lods_free(&hidden);
    nph_render_cluster_lods_free(&away);
    mesh_free(&bush);
    mesh_free(&shell);
}

static void expect_same_lods(const NphRenderClusterLods *x, const NphRenderClusterLods *y)
{
    NTEST_EXPECT_EQ_UINT(x->levels, y->levels);
    NTEST_EXPECT_EQ_UINT(x->level0_clusters, y->level0_clusters);
#define EXPECT_SAME(field)                                                                         \
    do {                                                                                           \
        NTEST_ASSERT_EQ_UINT(x->field.len, y->field.len);                                          \
        if (x->field.len != 0U) {                                                                  \
            NTEST_EXPECT_EQ_INT(memcmp(x->field.data, y->field.data,                               \
                                       (size_t)x->field.len * sizeof *x->field.data),              \
                                0);                                                                \
        }                                                                                          \
    } while (0)
    EXPECT_SAME(clusters.meshlets);
    EXPECT_SAME(clusters.bounds);
    EXPECT_SAME(lods);
    EXPECT_SAME(groups);
    EXPECT_SAME(group_clusters);
    EXPECT_SAME(nodes);
    EXPECT_SAME(bricks);
    EXPECT_SAME(voxels);
#undef EXPECT_SAME
}

NTEST_F(CellProxy, the_same_cell_gives_the_same_proxy)
{
    TestMesh tree;
    mesh_init(&tree, f->a, nph_vec3(0.1F, 0.5F, 0.1F));
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 17U, 1U);
    mesh_needles(&tree, &rng, 800U, nph_vec3(-1.0F, 0.0F, -1.0F), 2.0F, 0.2F, 0.01F);
    NTEST_ASSERT_EQ_INT(mesh_build(&tree, f->a), NPH_RESULT_OK);
    const NphRenderProxyMesh meshes[1] = {proxy_mesh(&tree, true)};
    NphRenderProxyInstance trees[4];
    for (u32 i = 0; i < 4U; ++i) {
        trees[i] =
            instance_of(0U, 1.1F * (f32)i, 1.0F,
                        nph_vec3(1.5F + 3.0F * (f32)(i % 2U), 0.0F, 1.5F + 3.0F * (f32)(i >> 1U)));
    }
    NphRenderCellProxyInput in = proxy_input(meshes, 1U, trees, 4U, 0.25F, f->a);
    NphRenderClusterLods x;
    NphRenderClusterLods y;
    nph_render_cluster_lods_init(&x, f->a);
    nph_render_cluster_lods_init(&y, f->a);
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &x, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_render_cell_proxy_build(&in, &y, NULL), NPH_RESULT_OK);
    check_proxy(&x, 0.25F);
    expect_same_lods(&x, &y);
    nph_render_cluster_lods_free(&x);
    nph_render_cluster_lods_free(&y);
    mesh_free(&tree);
}

NTEST_F(CellProxy, a_proxys_input_is_checked)
{
    TestMesh tree;
    mesh_init(&tree, f->a, nph_vec3(0.1F, 0.5F, 0.1F));
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 19U, 1U);
    mesh_needles(&tree, &rng, 50U, nph_vec3_zero(), 1.0F, 0.2F, 0.01F);
    NTEST_ASSERT_EQ_INT(mesh_build(&tree, f->a), NPH_RESULT_OK);
    const NphRenderProxyMesh meshes[2] = {proxy_mesh(&tree, true), proxy_mesh(&tree, false)};
    NphRenderProxyInstance one = instance_of(0U, 0.0F, 1.0F, nph_vec3_zero());
    NphRenderClusterLods out;
    nph_render_cluster_lods_init(&out, f->a);
    NphRenderCellProxyInput in = proxy_input(meshes, 2U, &one, 1U, 0.25F, f->a);
    in.cell = 0.3F; // not a power of two
    NTEST_EXPECT_EQ_INT(nph_render_cell_proxy_build(&in, &out, NULL), NPH_RESULT_INVALID_ARGUMENT);
    in.cell = 0.25F;
    in.instance_count = 0U;
    NTEST_EXPECT_EQ_INT(nph_render_cell_proxy_build(&in, &out, NULL), NPH_RESULT_INVALID_ARGUMENT);
    in.instance_count = 1U;
    one.mesh = 2U;
    NTEST_EXPECT_EQ_INT(nph_render_cell_proxy_build(&in, &out, NULL), NPH_RESULT_INVALID_ARGUMENT);
    one.mesh = 1U; // an instance's mesh without materials
    NTEST_EXPECT_EQ_INT(nph_render_cell_proxy_build(&in, &out, NULL), NPH_RESULT_INVALID_ARGUMENT);
    one = instance_of(0U, 0.0F, 0.0F, nph_vec3_zero()); // singular
    NTEST_EXPECT_EQ_INT(nph_render_cell_proxy_build(&in, &out, NULL), NPH_RESULT_INVALID_ARGUMENT);
    one = instance_of(0U, 0.0F, 1.0F, nph_vec3_zero());
    one.transform[0][3] = (f64)NAN;
    NTEST_EXPECT_EQ_INT(nph_render_cell_proxy_build(&in, &out, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(out.clusters.meshlets.len, 0U);
    nph_render_cluster_lods_free(&out);
    mesh_free(&tree);
}
