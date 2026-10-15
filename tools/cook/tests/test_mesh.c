// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/mesh.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/pack.h>

#include "test_cook_support.h"

#include <cook/mesh.h>

// A cube with four vertices per face (normals and UVs, no tangents).
static void make_cube(CookMesh *mesh, bool with_normals)
{
    static const f32 normals[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                      {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
    static const f32 tangents[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1},
                                       {0, 0, 1}, {1, 0, 0},  {1, 0, 0}};
    CookVertex verts[24];
    u32 indices[36];
    for (u32 face = 0; face < 6U; ++face) {
        const f32 *n = normals[face];
        const f32 *t = tangents[face];
        const f32 b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2],
                          n[0] * t[1] - n[1] * t[0]};
        for (u32 corner = 0; corner < 4U; ++corner) {
            const f32 u = (corner == 1U || corner == 2U) ? 1.0F : 0.0F;
            const f32 v = corner >= 2U ? 1.0F : 0.0F;
            const f32 su = u * 2.0F - 1.0F;
            const f32 sv = v * 2.0F - 1.0F;
            CookVertex *out = &verts[face * 4U + corner];
            memset(out, 0, sizeof *out);
            out->position = nph_vec3(n[0] + t[0] * su + b[0] * sv, n[1] + t[1] * su + b[1] * sv,
                                     n[2] + t[2] * su + b[2] * sv);
            out->normal = with_normals ? nph_vec3(n[0], n[1], n[2]) : nph_vec3_zero();
            out->uv0 = nph_vec2(u, v);
            out->color = nph_vec4_one();
        }
        const u32 base = face * 4U;
        u32 *idx = &indices[(u64)face * 6U];
        idx[0] = base;
        idx[1] = base + 1U;
        idx[2] = base + 2U;
        idx[3] = base;
        idx[4] = base + 2U;
        idx[5] = base + 3U;
    }
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(mesh, verts, 24U, indices, 36U, 0U), NPH_RESULT_OK);
    mesh->flags = (u32)COOK_MESH_HAS_UV0 | (with_normals ? (u32)COOK_MESH_HAS_NORMALS : 0U);
    strcpy(mesh->name, "cube");
}

// A grid of (n x n) quads over a bumpy height field, triangles in a
// cache-hostile (shuffled) order.
static void make_grid(CookMesh *mesh, u32 n, u64 seed)
{
    NphAllocator *alloc = nph_allocator_system();
    const u32 vcount = (n + 1U) * (n + 1U);
    CookVertex *verts = NPH_ALLOC_ARRAY_ZEROED(alloc, CookVertex, vcount);
    u32 *indices = NPH_ALLOC_ARRAY(alloc, u32, (u64)n * n * 6U);
    NTEST_ASSERT_NOT_NULL(verts);
    NTEST_ASSERT_NOT_NULL(indices);
    for (u32 y = 0; y <= n; ++y) {
        for (u32 x = 0; x <= n; ++x) {
            CookVertex *v = &verts[y * (n + 1U) + x];
            v->position =
                nph_vec3((f32)x, nph_f32_sin((f32)x * 0.3F) * nph_f32_cos((f32)y * 0.2F), (f32)y);
            v->uv0 = nph_vec2((f32)x / (f32)n, (f32)y / (f32)n);
            v->color = nph_vec4_one();
        }
    }
    u32 t = 0U;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = y * (n + 1U) + x;
            indices[t++] = a;
            indices[t++] = a + n + 1U;
            indices[t++] = a + 1U;
            indices[t++] = a + 1U;
            indices[t++] = a + n + 1U;
            indices[t++] = a + n + 2U;
        }
    }
    u64 rng = seed;
    const u32 tris = n * n * 2U;
    for (u32 i = tris; i > 1U; --i) {
        const u32 j = (u32)(test_rng_next(&rng) % i);
        for (u32 k = 0; k < 3U; ++k) {
            const u32 tmp = indices[(i - 1U) * 3U + k];
            indices[(i - 1U) * 3U + k] = indices[j * 3U + k];
            indices[j * 3U + k] = tmp;
        }
    }
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(mesh, verts, vcount, indices, n * n * 6U, 0U),
                        NPH_RESULT_OK);
    mesh->flags = (u32)COOK_MESH_HAS_UV0;
    NPH_FREE_ARRAY(alloc, CookVertex, verts, vcount);
    NPH_FREE_ARRAY(alloc, u32, indices, (u64)n * n * 6U);
}

static CookVertex vertex_at(f32 x, f32 y, f32 z, f32 u, f32 v)
{
    CookVertex out;
    memset(&out, 0, sizeof out);
    out.position = nph_vec3(x, y, z);
    out.normal = nph_vec3(0.0F, 0.0F, 1.0F);
    out.uv0 = nph_vec2(u, v);
    out.color = nph_vec4_one();
    return out;
}

static f32 quiet_nan(void)
{
    const u32 bits = 0x7FC00000U;
    f32 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

// The bitangent the format defines: cross(n, t) * w.
static NphVec3 bitangent_of(const CookVertex *v)
{
    return nph_vec3_scale(nph_vec3_cross(v->normal, nph_vec4_xyz(v->tangent)), v->tangent.w);
}

// ---------------------------------------------------------------------------
// Validation and cleaning
// ---------------------------------------------------------------------------

NTEST(mesh, add_validate_and_degenerate_removal)
{
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    CookVertex v[4];
    memset(v, 0, sizeof v);
    v[1].position = nph_vec3(1.0F, 0.0F, 0.0F);
    v[2].position = nph_vec3(1.0F, 1.0F, 0.0F);
    v[3].position = nph_vec3(2.0F, 2.0F, 0.0F); // collinear with 0 and 2
    const u32 idx[12] = {0, 1, 2, 0, 0, 2, 0, 2, 3, 2, 1, 0};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 4U, idx, 12U, 7U), NPH_RESULT_OK);
    const u32 bad[3] = {0, 1, 4};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 4U, bad, 3U, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 4U, idx, 4U, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, NULL, 4U, idx, 3U, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_remove_degenerate(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.indices.len, 6U); // the repeated-index and collinear triangles went
    NTEST_ASSERT_EQ_UINT(m.submeshes.data[0].index_count, 6U);
    NTEST_ASSERT_EQ_UINT(m.submeshes.data[0].material_slot, 7U);
    m.indices.data[0] = 9U;
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_CORRUPT);
    m.indices.data[0] = 0U;
    // Non-finite attributes are refused before they reach the bounds and
    // the quantisers.
    m.vertices.data[1].uv0.x = quiet_nan();
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_CORRUPT);
    m.vertices.data[1].uv0.x = 0.0F;
    m.vertices.data[2].position.y = NPH_F32_INFINITY;
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_CORRUPT);
    cook_mesh_free(&m);
}

NTEST(mesh, flat_and_smooth_normals)
{
    // A tetrahedron on four shared vertices.
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    CookVertex v[4];
    memset(v, 0, sizeof v);
    v[0].position = nph_vec3(0, 0, 0);
    v[1].position = nph_vec3(1, 0, 0);
    v[2].position = nph_vec3(0, 1, 0);
    v[3].position = nph_vec3(0, 0, 1);
    const u32 idx[12] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 4U, idx, 12U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_normals(&m, COOK_NORMALS_SMOOTH), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 4U);
    // The apex at the origin averages three axis-aligned faces.
    const f32 k = -1.0F / nph_f32_sqrt(3.0F);
    NTEST_ASSERT_TRUE(nph_vec3_near(m.vertices.data[0].normal, nph_vec3(k, k, k), 1e-5F));
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_normals(&m, COOK_NORMALS_FLAT), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 12U); // every triangle owns its corners
    NTEST_ASSERT_TRUE(nph_vec3_near(m.vertices.data[0].normal, nph_vec3(0, 0, -1), 1e-6F));
    NTEST_ASSERT_TRUE(nph_vec3_near(m.vertices.data[11].normal,
                                    nph_vec3_splat(1.0F / nph_f32_sqrt(3.0F)), 1e-5F));
    NTEST_ASSERT_TRUE((m.flags & (u32)COOK_MESH_HAS_NORMALS) != 0U);
    NTEST_ASSERT_EQ_INT(cook_mesh_weld(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 12U); // no two faces are coplanar
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const CookNormalMode bad_mode = (CookNormalMode)7;
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_normals(&m, bad_mode), NPH_RESULT_INVALID_ARGUMENT);
    cook_mesh_free(&m);
}

NTEST(mesh, welding_merges_duplicates_and_drops_unreferenced_vertices)
{
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    // A quad as two triangles with six vertices, two of them duplicates,
    // plus a vertex no triangle uses.
    CookVertex v[7];
    const f32 p[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const u32 corners[6] = {0, 1, 2, 0, 2, 3};
    for (u32 i = 0; i < 6U; ++i) {
        v[i] = vertex_at(p[corners[i]][0], p[corners[i]][1], i == 3U ? -0.0F : 0.0F,
                         p[corners[i]][0], p[corners[i]][1]);
        v[i].tangent = nph_vec4((f32)i, 0, 0, 0); // ignored: not claimed by the flags
    }
    v[6] = vertex_at(5, 5, 5, 0, 0);
    const u32 idx[6] = {0, 1, 2, 3, 4, 5};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 7U, idx, 6U, 0U), NPH_RESULT_OK);
    m.flags = (u32)COOK_MESH_HAS_UV0 | (u32)COOK_MESH_HAS_NORMALS;
    NTEST_ASSERT_EQ_INT(cook_mesh_weld(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 4U); // -0 welded with +0, the stray vertex gone
    NTEST_ASSERT_EQ_UINT(m.indices.len, 6U);
    NTEST_ASSERT_EQ_UINT(m.indices.data[3], 0U);
    NTEST_ASSERT_EQ_UINT(m.indices.data[4], 2U);
    // Survivors keep their relative order.
    NTEST_ASSERT_TRUE(nph_vec3_near(m.vertices.data[1].position, nph_vec3(1, 0, 0), 0.0F));
    NTEST_ASSERT_TRUE(nph_vec3_near(m.vertices.data[3].position, nph_vec3(0, 1, 0), 0.0F));
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_OK);
    cook_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// Tangents
// ---------------------------------------------------------------------------

NTEST(mesh, tangents_follow_the_gltf_handedness)
{
    // A quad facing +Z with the image upright in glTF terms: u along +X,
    // v growing downwards (towards -Y). The bitangent cross(n, t) * w must
    // point up the image (+Y, decreasing v), which makes w = +1.
    for (u32 mirrored = 0; mirrored < 2U; ++mirrored) {
        CookMesh m;
        cook_mesh_init(&m, nph_allocator_system());
        const f32 u_flip = mirrored != 0U ? -1.0F : 1.0F;
        CookVertex v[4];
        v[0] = vertex_at(0, 0, 0, 0, 1);
        v[1] = vertex_at(1, 0, 0, u_flip, 1);
        v[2] = vertex_at(1, 1, 0, u_flip, 0);
        v[3] = vertex_at(0, 1, 0, 0, 0);
        const u32 idx[6] = {0, 1, 2, 0, 2, 3};
        NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 4U, idx, 6U, 0U), NPH_RESULT_OK);
        m.flags = (u32)COOK_MESH_HAS_UV0 | (u32)COOK_MESH_HAS_NORMALS;
        NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_OK);
        NTEST_ASSERT_TRUE((m.flags & (u32)COOK_MESH_HAS_TANGENTS) != 0U);
        NTEST_ASSERT_EQ_UINT(m.vertices.len, 4U);
        for (u32 i = 0; i < 4U; ++i) {
            const CookVertex *c = &m.vertices.data[i];
            // dP/du is +X, or -X where the image is mirrored in u; the
            // bitangent points up the image either way.
            NTEST_ASSERT_TRUE(
                nph_vec3_near(nph_vec4_xyz(c->tangent), nph_vec3(u_flip, 0, 0), 1e-5F));
            NTEST_ASSERT_NEAR_F32(c->tangent.w, mirrored != 0U ? -1.0F : 1.0F, 0.0F);
            NTEST_ASSERT_TRUE(nph_vec3_near(bitangent_of(c), nph_vec3(0, 1, 0), 1e-5F));
        }
        cook_mesh_free(&m);
    }
    // Without UVs there is nothing to derive tangents from.
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    CookVertex v[3] = {vertex_at(0, 0, 0, 0, 0), vertex_at(1, 0, 0, 0, 0),
                       vertex_at(0, 1, 0, 0, 0)};
    const u32 idx[3] = {0, 1, 2};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 3U, idx, 3U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_INVALID_ARGUMENT);
    cook_mesh_free(&m);
}

NTEST(mesh, tangents_split_a_mirrored_seam_and_weight_by_angle)
{
    // Two quads side by side sharing the middle column of vertices
    // (positions, normals, and UVs equal there): the left one maps u
    // forwards, the right one mirrors it back, as a symmetric character's
    // UVs do. The shared vertices sit on two orientation groups and must
    // be split, each side keeping its own frame.
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    CookVertex v[6];
    v[0] = vertex_at(0, 0, 0, 0, 1);
    v[1] = vertex_at(1, 0, 0, 1, 1); // the seam
    v[2] = vertex_at(1, 1, 0, 1, 0); // the seam
    v[3] = vertex_at(0, 1, 0, 0, 0);
    v[4] = vertex_at(2, 0, 0, 0, 1);
    v[5] = vertex_at(2, 1, 0, 0, 0);
    const u32 idx[12] = {0, 1, 2, 0, 2, 3, 1, 4, 5, 1, 5, 2};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 6U, idx, 12U, 0U), NPH_RESULT_OK);
    m.flags = (u32)COOK_MESH_HAS_UV0 | (u32)COOK_MESH_HAS_NORMALS;
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 8U); // both seam vertices doubled
    for (u32 t = 0; t < 4U; ++t) {
        const bool right = t >= 2U;
        for (u32 k = 0; k < 3U; ++k) {
            const CookVertex *c = &m.vertices.data[m.indices.data[t * 3U + k]];
            NTEST_ASSERT_TRUE(nph_vec3_near(nph_vec4_xyz(c->tangent),
                                            nph_vec3(right ? -1.0F : 1.0F, 0, 0), 1e-5F));
            NTEST_ASSERT_NEAR_F32(c->tangent.w, right ? -1.0F : 1.0F, 0.0F);
            NTEST_ASSERT_TRUE(nph_vec3_near(bitangent_of(c), nph_vec3(0, 1, 0), 1e-5F));
        }
    }
    cook_mesh_free(&m);

    // Angle weighting. Two triangles share the edge O-P1 (and the UVs on
    // it) at the origin O, facing +Z with the same UV orientation:
    //   A = (O, P1, P2), P1 = (1, 0), P2 = (0, 1), UVs (0, 0) (1, 0.5) (0, -1):
    //       dP/du = (1, 0.5) / |..| = (0.894427, 0.447214), 90 degrees at O;
    //   B = (O, P3, P1), P3 = (1, -0.2), UV (0.8, 1):
    //       dP/du = (0.5, 0.1) / |..| = (0.980581, 0.196116), atan(0.2) at O.
    // At O the tangent is normalize(pi/2 dir_A + 0.197396 dir_B) =
    // (0.907222, 0.420654); an unweighted mean would be (0.946, 0.324).
    cook_mesh_init(&m, nph_allocator_system());
    CookVertex w[4];
    w[0] = vertex_at(0, 0, 0, 0, 0);
    w[1] = vertex_at(1, 0, 0, 1, 0.5F);
    w[2] = vertex_at(0, 1, 0, 0, -1);
    w[3] = vertex_at(1, -0.2F, 0, 0.8F, 1);
    const u32 widx[6] = {0, 1, 2, 0, 3, 1};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, w, 4U, widx, 6U, 0U), NPH_RESULT_OK);
    m.flags = (u32)COOK_MESH_HAS_UV0 | (u32)COOK_MESH_HAS_NORMALS;
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 4U); // one group per vertex: nothing split
    const CookVertex *origin = &m.vertices.data[m.indices.data[0]];
    NTEST_ASSERT_TRUE(
        nph_vec3_near(nph_vec4_xyz(origin->tangent), nph_vec3(0.907222F, 0.420654F, 0), 1e-4F));
    NTEST_ASSERT_NEAR_F32(origin->tangent.w, 1.0F, 0.0F);
    // P2 belongs to A alone: A's own direction.
    const CookVertex *p2 = &m.vertices.data[m.indices.data[2]];
    NTEST_ASSERT_TRUE(
        nph_vec3_near(nph_vec4_xyz(p2->tangent), nph_vec3(0.894427F, 0.447214F, 0), 1e-4F));
    cook_mesh_free(&m);
}

NTEST(mesh, tangents_without_normals_make_flat_ones_first)
{
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    make_cube(&m, false);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE((m.flags & (u32)COOK_MESH_HAS_NORMALS) != 0U);
    NTEST_ASSERT_EQ_UINT(m.vertices.len, 24U);
    for (u32 i = 0; i < m.vertices.len; ++i) {
        const CookVertex *c = &m.vertices.data[i];
        NTEST_ASSERT_NEAR_F32(nph_vec3_dot(c->normal, nph_vec4_xyz(c->tangent)), 0.0F, 1e-5F);
        NTEST_ASSERT_NEAR_F32(nph_vec3_length(nph_vec4_xyz(c->tangent)), 1.0F, 1e-5F);
    }
    cook_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// Reordering
// ---------------------------------------------------------------------------

NTEST(mesh, cache_optimisation_lowers_the_miss_ratio_and_keeps_the_triangles)
{
    CookMesh m;
    cook_mesh_init(&m, nph_allocator_system());
    make_grid(&m, 40U, 3U);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_normals(&m, COOK_NORMALS_SMOOTH), NPH_RESULT_OK);
    const f32 before = cook_mesh_acmr(m.indices.data, m.indices.len, 32U);
    // The set of triangles (as hashed position triples) must not change.
    u64 sum_before = 0U;
    for (u32 t = 0; t < m.indices.len / 3U; ++t) {
        const NphVec3 a = m.vertices.data[m.indices.data[(u64)t * 3U]].position;
        const NphVec3 b = m.vertices.data[m.indices.data[t * 3U + 1U]].position;
        const NphVec3 c = m.vertices.data[m.indices.data[t * 3U + 2U]].position;
        sum_before += nph_hash64(&a, sizeof a, 1U) ^ nph_hash64(&b, sizeof b, 2U) ^
                      nph_hash64(&c, sizeof c, 3U);
    }
    NTEST_ASSERT_EQ_INT(cook_mesh_optimize(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_OK);
    const f32 after = cook_mesh_acmr(m.indices.data, m.indices.len, 32U);
    NTEST_LOG("ACMR %.2f -> %.2f", (f64)before, (f64)after);
    NTEST_ASSERT_GT_INT((i32)(before * 100.0F), 150); // shuffled: well over 1.5 misses per triangle
    NTEST_ASSERT_LT_INT((i32)(after * 100.0F), 80);   // optimised: under 0.8
    u64 sum_after = 0U;
    for (u32 t = 0; t < m.indices.len / 3U; ++t) {
        const NphVec3 a = m.vertices.data[m.indices.data[(u64)t * 3U]].position;
        const NphVec3 b = m.vertices.data[m.indices.data[t * 3U + 1U]].position;
        const NphVec3 c = m.vertices.data[m.indices.data[t * 3U + 2U]].position;
        sum_after += nph_hash64(&a, sizeof a, 1U) ^ nph_hash64(&b, sizeof b, 2U) ^
                     nph_hash64(&c, sizeof c, 3U);
    }
    NTEST_ASSERT_EQ_UINT(sum_after, sum_before); // winding and triangle set preserved
    // Vertex fetch: the first triangle references the first vertices.
    NTEST_ASSERT_EQ_UINT(m.indices.data[0], 0U);
    NTEST_ASSERT_LT_UINT(m.indices.data[1], 3U);
    NTEST_ASSERT_LT_UINT(m.indices.data[2], 3U);
    cook_mesh_free(&m);
}

NTEST(mesh, acmr_of_known_orders)
{
    const u32 strip[12] = {0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4}; // a strip: one new vertex each
    NTEST_ASSERT_NEAR_F32(cook_mesh_acmr(strip, 12U, 32U), 6.0F / 4.0F, 1e-6F);
    const u32 worst[6] = {0, 1, 2, 3, 4, 5};
    NTEST_ASSERT_NEAR_F32(cook_mesh_acmr(worst, 6U, 32U), 3.0F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(cook_mesh_acmr(worst, 0U, 32U), 0.0F, 0.0F);
    // A strip needs only the two newest vertices: a 2-entry FIFO already
    // reaches the ideal 1.5. With one entry it forgets what the next
    // triangle shares: 3 + 3 + 2 + 3 misses over four triangles.
    NTEST_ASSERT_NEAR_F32(cook_mesh_acmr(strip, 12U, 2U), 1.5F, 1e-6F);
    NTEST_ASSERT_NEAR_F32(cook_mesh_acmr(strip, 12U, 1U), 11.0F / 4.0F, 1e-6F);
}

// ---------------------------------------------------------------------------
// Meshlets
// ---------------------------------------------------------------------------

// Checks the structural invariants of a meshlet set over `mesh`: limits,
// local indices in range, every triangle of every submesh exactly once
// (as a hashed set of global triangles), and submesh order.
static void check_meshlets(const CookMesh *mesh, const CookMeshlets *ml, u32 max_v, u32 max_t)
{
    NTEST_ASSERT_EQ_UINT(ml->bounds.len, ml->meshlets.len);
    u64 sum_mesh = 0U;
    for (u32 t = 0; t < mesh->indices.len / 3U; ++t) {
        const u32 *tri = &mesh->indices.data[(u64)t * 3U];
        sum_mesh += nph_hash64(tri, 12U, 7U);
    }
    u64 sum_meshlets = 0U;
    u32 total = 0U;
    u32 last_submesh = 0U;
    for (u32 i = 0; i < ml->meshlets.len; ++i) {
        const NphMeshlet *mt = &ml->meshlets.data[i];
        NTEST_ASSERT_LE_UINT(mt->vertex_count, max_v);
        NTEST_ASSERT_LE_UINT(mt->triangle_count, max_t);
        NTEST_ASSERT_GT_UINT(mt->triangle_count, 0U);
        NTEST_ASSERT_GE_UINT(mt->submesh, last_submesh);
        last_submesh = mt->submesh;
        const CookSubmesh *sub = &mesh->submeshes.data[mt->submesh];
        for (u32 t = 0; t < mt->triangle_count; ++t) {
            u32 tri[3];
            for (u32 k = 0; k < 3U; ++k) {
                const u8 local = ml->triangles.data[((u64)mt->triangle_offset + t) * 3U + k];
                NTEST_ASSERT_LT_UINT(local, mt->vertex_count);
                tri[k] = ml->vertices.data[mt->vertex_offset + local];
            }
            sum_meshlets += nph_hash64(tri, 12U, 7U);
            // The triangle belongs to its meshlet's submesh.
            bool found = false;
            for (u32 j = sub->index_offset; j < sub->index_offset + sub->index_count && !found;
                 j += 3U) {
                found = memcmp(&mesh->indices.data[j], tri, sizeof tri) == 0;
            }
            NTEST_ASSERT_TRUE(found);
        }
        total += mt->triangle_count;
    }
    NTEST_ASSERT_EQ_UINT(total, mesh->indices.len / 3U);
    NTEST_ASSERT_EQ_UINT(sum_meshlets, sum_mesh);
}

// Samples camera positions around each meshlet: whenever the stored cone
// says "cull" (the GPU's test on the stored snorm8 values), every
// triangle must face away from the camera. Returns how many samples were
// culled.
static u32 check_cones(const NphVec3 *positions, const CookMeshlets *ml)
{
    u32 culled_total = 0U;
    for (u32 i = 0; i < ml->meshlets.len; ++i) {
        const NphMeshlet *mt = &ml->meshlets.data[i];
        const NphMeshletBounds *b = &ml->bounds.data[i];
        // Every vertex lies inside the sphere.
        for (u32 t = 0; t < (u32)mt->triangle_count * 3U; ++t) {
            const u8 local = ml->triangles.data[(u64)mt->triangle_offset * 3U + t];
            const NphVec3 p = positions[ml->vertices.data[mt->vertex_offset + local]];
            NTEST_ASSERT_TRUE(nph_vec3_distance(p, b->center) <= b->radius * 1.0001F + 1e-6F);
        }
        const NphVec3 axis =
            nph_vec3(nph_snorm8_to_f32(b->cone_axis[0]), nph_snorm8_to_f32(b->cone_axis[1]),
                     nph_snorm8_to_f32(b->cone_axis[2]));
        const f32 cutoff = nph_snorm8_to_f32(b->cone_cutoff);
        if (b->cone_cutoff == 127) {
            NTEST_ASSERT_TRUE(nph_vec3_length_sq(axis) == 0.0F); // "never" has no axis
        }
        u64 rng = 100U + i;
        for (u32 s = 0; s < 300U; ++s) {
            const f32 r = (f32)(test_rng_next(&rng) % 1000U) / 1000.0F * 30.0F + 0.5F;
            const NphVec3 dir =
                nph_vec3_normalize_or(nph_vec3((f32)(test_rng_next(&rng) % 2001U) / 1000.0F - 1.0F,
                                               (f32)(test_rng_next(&rng) % 2001U) / 1000.0F - 1.0F,
                                               (f32)(test_rng_next(&rng) % 2001U) / 1000.0F - 1.0F),
                                      nph_vec3_unit_y());
            const NphVec3 camera = nph_vec3_add(b->center, nph_vec3_scale(dir, r));
            const NphVec3 to_apex = nph_vec3_normalize_or_zero(nph_vec3_sub(b->cone_apex, camera));
            if (nph_vec3_dot(to_apex, axis) < cutoff) {
                continue; // not culled
            }
            ++culled_total;
            for (u32 t = 0; t < mt->triangle_count; ++t) {
                const u8 *tri = &ml->triangles.data[((u64)mt->triangle_offset + t) * 3U];
                const NphVec3 p0 = positions[ml->vertices.data[mt->vertex_offset + tri[0]]];
                const NphVec3 p1 = positions[ml->vertices.data[mt->vertex_offset + tri[1]]];
                const NphVec3 p2 = positions[ml->vertices.data[mt->vertex_offset + tri[2]]];
                const NphVec3 n = nph_vec3_cross(nph_vec3_sub(p1, p0), nph_vec3_sub(p2, p0));
                NTEST_ASSERT_TRUE(nph_vec3_dot(nph_vec3_sub(camera, p0), n) <= 1e-4F);
            }
        }
    }
    return culled_total;
}

static void mesh_positions(const CookMesh *m, NphVec3 *out)
{
    for (u32 i = 0; i < m->vertices.len; ++i) {
        out[i] = m->vertices.data[i].position;
    }
}

NTEST(mesh, meshlets_respect_limits_cover_everything_and_cones_are_conservative)
{
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    make_grid(&m, 12U, 9U);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_normals(&m, COOK_NORMALS_SMOOTH), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_optimize(&m), NPH_RESULT_OK);
    NphVec3 *positions = NPH_ALLOC_ARRAY(alloc, NphVec3, m.vertices.len);
    NTEST_ASSERT_NOT_NULL(positions);
    mesh_positions(&m, positions);
    for (u32 weight = 0; weight < 2U; ++weight) {
        CookMeshlets ml;
        cook_meshlets_init(&ml, alloc);
        NTEST_ASSERT_EQ_INT(cook_mesh_build_meshlets(&m, 16U, 10U, weight != 0U ? 0.5F : 0.0F, &ml),
                            NPH_RESULT_OK);
        NTEST_ASSERT_GT_UINT(ml.meshlets.len, 28U); // 288 triangles, at most 10 per meshlet
        check_meshlets(&m, &ml, 16U, 10U);
        const u32 culled = check_cones(positions, &ml);
        NTEST_LOG("cone weight %u: %u meshlets, %u culled samples", weight, ml.meshlets.len,
                  culled);
        NTEST_ASSERT_GT_UINT(culled, 0U); // gently curved meshlets do cull
        cook_meshlets_free(&ml);
    }
    CookMeshlets ml;
    cook_meshlets_init(&ml, alloc);
    NTEST_ASSERT_EQ_INT(cook_mesh_build_meshlets(&m, 2U, 10U, 0.0F, &ml),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(cook_mesh_build_meshlets(&m, 0U, 125U, 0.0F, &ml),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(cook_mesh_build_meshlets(&m, 0U, 0U, 1.5F, &ml),
                        NPH_RESULT_INVALID_ARGUMENT);
    cook_meshlets_free(&ml);
    NPH_FREE_ARRAY(alloc, NphVec3, positions, m.vertices.len);
    cook_mesh_free(&m);
}

NTEST(mesh, meshlets_are_compact_on_a_connected_surface)
{
    // On a 64 x 64 grid the 64-vertex limit binds first: the best meshlet
    // is an 8 x 8 patch of vertices, 7 x 7 quads or 98 triangles, with a
    // sphere radius near 5 (half its diagonal; more over the bumps). A
    // greedy scan over the shuffled order would be several times that.
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    make_grid(&m, 64U, 21U);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_normals(&m, COOK_NORMALS_SMOOTH), NPH_RESULT_OK);
    CookMeshlets ml;
    cook_meshlets_init(&ml, alloc);
    NTEST_ASSERT_EQ_INT(cook_mesh_build_meshlets(&m, 0U, 0U, 0.25F, &ml), NPH_RESULT_OK);
    check_meshlets(&m, &ml, NPH_MESHLET_MAX_VERTICES, NPH_MESHLET_MAX_TRIANGLES);
    f64 radius = 0.0;
    u32 vertices = 0U;
    for (u32 i = 0; i < ml.meshlets.len; ++i) {
        radius += (f64)ml.bounds.data[i].radius;
        vertices += ml.meshlets.data[i].vertex_count;
    }
    radius /= (f64)ml.meshlets.len;
    const u32 triangles = m.indices.len / 3U;
    const f64 reuse = (f64)triangles / (f64)vertices;
    NTEST_LOG("%u meshlets for %u triangles: mean radius %.2f, %.2f triangles per vertex",
              ml.meshlets.len, m.indices.len / 3U, radius, reuse);
    NTEST_ASSERT_LE_UINT(ml.meshlets.len, (m.indices.len / 3U) / 80U); // well filled
    NTEST_ASSERT_LE_F64(radius, 8.0);
    NTEST_ASSERT_GE_F64(reuse, 1.3); // the 8 x 8 patch: 98 triangles on 64 vertices, 1.53
    cook_meshlets_free(&ml);
    cook_mesh_free(&m);
}

NTEST(mesh, meshlets_gather_scattered_triangles_by_distance)
{
    // 600 disconnected triangles at random places: no adjacency to follow,
    // so every step goes through the nearest-unused search. Meshlets must
    // still be spatially tight: much smaller than the 100-unit field.
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    u64 rng = 4U;
    CookVertex *v = NPH_ALLOC_ARRAY(alloc, CookVertex, 1800U);
    u32 *idx = NPH_ALLOC_ARRAY(alloc, u32, 1800U);
    NTEST_ASSERT_NOT_NULL(v);
    NTEST_ASSERT_NOT_NULL(idx);
    for (u32 t = 0; t < 600U; ++t) {
        const f32 x = (f32)(test_rng_next(&rng) % 10000U) / 100.0F;
        const f32 y = (f32)(test_rng_next(&rng) % 10000U) / 100.0F;
        v[(u64)t * 3U] = vertex_at(x, y, 0, 0, 0);
        v[t * 3U + 1U] = vertex_at(x + 0.1F, y, 0, 0, 0);
        v[t * 3U + 2U] = vertex_at(x, y + 0.1F, 0, 0, 0);
        idx[(u64)t * 3U] = t * 3U;
        idx[t * 3U + 1U] = t * 3U + 1U;
        idx[t * 3U + 2U] = t * 3U + 2U;
    }
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 1800U, idx, 1800U, 0U), NPH_RESULT_OK);
    m.flags = (u32)COOK_MESH_HAS_NORMALS;
    CookMeshlets ml;
    cook_meshlets_init(&ml, alloc);
    NTEST_ASSERT_EQ_INT(cook_mesh_build_meshlets(&m, 0U, 0U, 0.0F, &ml), NPH_RESULT_OK);
    check_meshlets(&m, &ml, NPH_MESHLET_MAX_VERTICES, NPH_MESHLET_MAX_TRIANGLES);
    f64 radius = 0.0;
    for (u32 i = 0; i < ml.meshlets.len; ++i) {
        radius += (f64)ml.bounds.data[i].radius;
    }
    radius /= (f64)ml.meshlets.len;
    // 21 triangles per meshlet (64 vertices / 3) out of 600 over 100 x 100:
    // a tight group spans about a fifth of the field.
    NTEST_LOG("%u meshlets, mean radius %.1f", ml.meshlets.len, radius);
    NTEST_ASSERT_EQ_UINT(ml.meshlets.len, (600U + 20U) / 21U);
    NTEST_ASSERT_LE_F64(radius, 25.0);
    cook_meshlets_free(&ml);
    NPH_FREE_ARRAY(alloc, CookVertex, v, 1800U);
    NPH_FREE_ARRAY(alloc, u32, idx, 1800U);
    cook_mesh_free(&m);
}

// ---------------------------------------------------------------------------
// The whole pipeline
// ---------------------------------------------------------------------------

// A mesh left without triangles (a source whose every triangle is
// degenerate, or an index accessor of count 0) cooks to a valid empty
// asset. Under UBSan this also pins the absence of arithmetic on the null
// index array the fuzzer found.
NTEST(mesh, a_mesh_without_triangles_writes_an_empty_asset)
{
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    CookVertex v[3];
    memset(v, 0, sizeof v);
    v[1].position = nph_vec3(1.0F, 0.0F, 0.0F);
    v[2].position = nph_vec3(0.0F, 1.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 3U, NULL, 0U, 0U), NPH_RESULT_OK);
    CookMeshSettings s;
    cook_mesh_settings_default(&s);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                          nph_guid_from_cstr("empty"), 1U);
    CookMeshStats st;
    NTEST_ASSERT_EQ_INT(cook_mesh_process(&m, &s, &w, &st), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(st.triangles, 0U);
    NTEST_ASSERT_EQ_UINT(st.meshlets, 0U);
    TestAsset ta;
    test_asset_from_writer(alloc, &w, &ta);
    nph_asset_writer_free(&w);
    NphMeshView view;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &view), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(view.header->index_count, 0U);
    NTEST_ASSERT_EQ_UINT(view.header->submesh_count, 1U);
    test_asset_free(&ta);
    cook_mesh_free(&m);
}

// A source normal of zero is invalid glTF but finite, so it reaches the
// pipeline; tangent generation treats it as +Z instead of asserting (a
// mutation of the fuzz corpus found the assertion).
NTEST(mesh, tangents_survive_a_zero_normal)
{
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    CookVertex v[3];
    memset(v, 0, sizeof v);
    v[1].position = nph_vec3(1.0F, 0.0F, 0.0F);
    v[2].position = nph_vec3(0.0F, 1.0F, 0.0F);
    v[1].uv0 = nph_vec2(1.0F, 0.0F);
    v[2].uv0 = nph_vec2(0.0F, 1.0F);
    const u32 tri[3] = {0U, 1U, 2U};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 3U, tri, 3U, 0U), NPH_RESULT_OK);
    m.flags = (u32)COOK_MESH_HAS_NORMALS | (u32)COOK_MESH_HAS_UV0; // all three normals are zero
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_OK);
    for (u32 i = 0; i < m.vertices.len; ++i) {
        const NphVec4 t = m.vertices.data[i].tangent;
        NTEST_ASSERT_TRUE(nph_f32_is_finite(t.x) && nph_f32_is_finite(t.y) &&
                          nph_f32_is_finite(t.z));
        NTEST_ASSERT_NEAR_F32(nph_vec3_length(nph_vec3(t.x, t.y, t.z)), 1.0F, 1e-5F);
    }
    cook_mesh_free(&m);
}

// Normals and positions at the edge of the float range: a normal whose
// squared length overflows keeps its direction (the fuzzer's (0, 1.8e19, 0)
// once came out as the zero vector), and positions beyond
// COOK_MESH_MAX_COORDINATE are refused rather than overflowing later.
NTEST(mesh, extreme_values_are_normalised_or_refused)
{
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    CookVertex v[3];
    memset(v, 0, sizeof v);
    v[1].position = nph_vec3(1.0F, 0.0F, 0.0F);
    v[2].position = nph_vec3(0.0F, 1.0F, 0.0F);
    v[1].uv0 = nph_vec2(1.0F, 0.0F);
    v[2].uv0 = nph_vec2(0.0F, 1.0F);
    for (u32 i = 0; i < 3U; ++i) {
        v[i].normal = nph_vec3(0.0F, 0.0F, i == 0U ? 1.8446744e19F : 1.0F);
    }
    const u32 tri[3] = {0U, 1U, 2U};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 3U, tri, 3U, 0U), NPH_RESULT_OK);
    m.flags = (u32)COOK_MESH_HAS_NORMALS | (u32)COOK_MESH_HAS_UV0;
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_generate_tangents(&m), NPH_RESULT_OK);
    NTEST_ASSERT_NEAR_F32(m.vertices.data[0].tangent.x, 1.0F, 1e-5F);
    cook_mesh_free(&m);

    cook_mesh_init(&m, alloc);
    v[2].position = nph_vec3(0.0F, 2e9F, 0.0F);
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, v, 3U, tri, 3U, 0U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_mesh_validate(&m), NPH_RESULT_CORRUPT);
    cook_mesh_free(&m);
}

NTEST(mesh, the_pipeline_writes_a_valid_asset)
{
    NphAllocator *alloc = nph_allocator_system();
    for (u32 variant = 0; variant < 2U; ++variant) {
        CookMesh m;
        cook_mesh_init(&m, alloc);
        make_cube(&m, variant == 0U);
        CookMeshSettings s;
        cook_mesh_settings_default(&s);
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                              nph_guid_from_cstr("cube"), 1U);
        CookMeshStats st;
        NTEST_ASSERT_EQ_INT(cook_mesh_process(&m, &s, &w, &st), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(st.source_vertices, 24U);
        NTEST_ASSERT_EQ_UINT(st.source_triangles, 12U);
        NTEST_ASSERT_EQ_UINT(st.triangles, 12U);
        NTEST_ASSERT_EQ_UINT(st.vertices, 24U); // per-face normals keep the corners apart
        NTEST_ASSERT_EQ_UINT(st.meshlets, 1U);
        TestAsset ta;
        test_asset_from_writer(alloc, &w, &ta);
        nph_asset_writer_free(&w);
        NphMeshView v;
        NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &v), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_UINT(v.header->vertex_count, 24U);
        NTEST_ASSERT_EQ_UINT(v.header->index_count, 36U);
        NTEST_ASSERT_EQ_UINT(v.header->submesh_count, 1U);
        NTEST_ASSERT_EQ_UINT(v.header->meshlet_count, 1U);
        NTEST_ASSERT_TRUE((v.header->flags & (u32)NPH_MESH_FLAG_HAS_TANGENTS) != 0U);
        NTEST_ASSERT_TRUE((v.header->flags & (u32)NPH_MESH_FLAG_HAS_UV0) != 0U);
        NTEST_ASSERT_TRUE(nph_aabb_near(
            v.header->bounds, nph_aabb(nph_vec3_splat(-1.0F), nph_vec3_splat(1.0F)), 1e-6F));
        NTEST_ASSERT_TRUE(v.header->sphere.radius >= nph_f32_sqrt(3.0F));
        // Dequantised positions are on the cube's corners, normals unit
        // and axis-aligned, tangents perpendicular to normals.
        for (u32 i = 0; i < 24U; ++i) {
            const NphVec3 p = nph_mesh_dequantize_position(v.header, v.positions[i]);
            NTEST_ASSERT_NEAR_F32(nph_f32_abs(p.x), 1.0F, 1e-4F);
            NTEST_ASSERT_NEAR_F32(nph_f32_abs(p.y), 1.0F, 1e-4F);
            NTEST_ASSERT_NEAR_F32(nph_f32_abs(p.z), 1.0F, 1e-4F);
            const NphVec3 n = nph_oct_decode_snorm16(v.attribs[i].normal);
            NTEST_ASSERT_NEAR_F32(nph_vec3_length(n), 1.0F, 1e-3F);
            NTEST_ASSERT_NEAR_F32(nph_vec3_max_component(nph_vec3_abs(n)), 1.0F, 1e-3F);
            const NphVec4 t = nph_unpack_snorm8x4(v.attribs[i].tangent);
            NTEST_ASSERT_NEAR_F32(nph_vec3_dot(n, nph_vec4_xyz(t)), 0.0F, 0.05F);
            NTEST_ASSERT_NEAR_F32(nph_f32_abs(t.w), 1.0F, 0.0F);
            NTEST_ASSERT_EQ_UINT(v.attribs[i].color, 0xFFFFFFFFU);
        }
        NTEST_ASSERT_EQ_UINT(v.meshlets[0].triangle_count, 12U);
        NTEST_ASSERT_EQ_UINT(v.meshlets[0].vertex_count, 24U);
        NTEST_ASSERT_EQ_INT(v.meshlet_bounds[0].cone_cutoff, 127); // a closed cube is never culled
        NTEST_ASSERT_EQ_INT(v.meshlet_bounds[0].cone_axis[0], 0);
        NTEST_ASSERT_EQ_UINT(v.submeshes[0].meshlet_count, 1U);
        test_asset_free(&ta);
        cook_mesh_free(&m);
    }
}

NTEST(mesh, written_bounds_hold_for_the_dequantised_positions)
{
    // The bounds the renderer culls with must contain the positions it
    // draws: the dequantised ones, not the source's.
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    make_grid(&m, 24U, 5U);
    CookMeshSettings s;
    cook_mesh_settings_default(&s);
    s.lz = NPH_LZ_LEVEL_FAST;
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                          nph_guid_from_cstr("grid"), 1U);
    NTEST_ASSERT_EQ_INT(cook_mesh_process(&m, &s, &w, NULL), NPH_RESULT_OK);
    TestAsset ta;
    test_asset_from_writer(alloc, &w, &ta);
    nph_asset_writer_free(&w);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &v), NPH_RESULT_OK);
    const u32 vn = v.header->vertex_count;
    NphVec3 *positions = NPH_ALLOC_ARRAY(alloc, NphVec3, vn);
    NTEST_ASSERT_NOT_NULL(positions);
    for (u32 i = 0; i < vn; ++i) {
        positions[i] = nph_mesh_dequantize_position(v.header, v.positions[i]);
        NTEST_ASSERT_TRUE(nph_aabb_contains_point(v.header->bounds, positions[i]));
        NTEST_ASSERT_TRUE(nph_sphere_contains_point(v.header->sphere, positions[i]));
    }
    for (u32 s2 = 0; s2 < v.header->submesh_count; ++s2) {
        const NphSubmesh *sub = &v.submeshes[s2];
        for (u32 i = sub->index_offset; i < sub->index_offset + sub->index_count; ++i) {
            NTEST_ASSERT_TRUE(nph_aabb_contains_point(sub->bounds, positions[v.indices[i]]));
        }
    }
    // The meshlets as written, against the dequantised positions.
    CookMeshlets ml;
    cook_meshlets_init(&ml, alloc);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&ml.meshlets, v.meshlets, v.header->meshlet_count),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(NPH_ARRAY_PUSH_N(&ml.bounds, v.meshlet_bounds, v.header->meshlet_count),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        NPH_ARRAY_PUSH_N(&ml.vertices, v.meshlet_vertices, v.header->meshlet_vertex_count),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        NPH_ARRAY_PUSH_N(&ml.triangles, v.meshlet_triangles, v.header->meshlet_triangle_count * 3U),
        NPH_RESULT_OK);
    NTEST_ASSERT_GT_UINT(check_cones(positions, &ml), 0U);
    cook_meshlets_free(&ml);
    NPH_FREE_ARRAY(alloc, NphVec3, positions, vn);
    test_asset_free(&ta);
    cook_mesh_free(&m);
}

NTEST(mesh, several_submeshes_and_no_optimize)
{
    NphAllocator *alloc = nph_allocator_system();
    CookMesh m;
    cook_mesh_init(&m, alloc);
    make_grid(&m, 6U, 1U);
    CookVertex tri[3];
    memset(tri, 0, sizeof tri);
    tri[1].position = nph_vec3(1, 0, 0);
    tri[2].position = nph_vec3(0, 1, 0);
    const u32 idx[3] = {0, 1, 2};
    NTEST_ASSERT_EQ_INT(cook_mesh_add_submesh(&m, tri, 3U, idx, 3U, 1U), NPH_RESULT_OK);
    CookMeshSettings s;
    cook_mesh_settings_default(&s);
    s.no_optimize = 1;
    s.meshlet_triangles = 8U;
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                          nph_guid_from_cstr("grid"), 1U);
    NTEST_ASSERT_EQ_INT(cook_mesh_process(&m, &s, &w, NULL), NPH_RESULT_OK);
    TestAsset ta;
    test_asset_from_writer(alloc, &w, &ta);
    nph_asset_writer_free(&w);
    NphMeshView v;
    NTEST_ASSERT_EQ_INT(nph_mesh_view(&ta.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->submesh_count, 2U);
    NTEST_ASSERT_EQ_UINT(v.submeshes[1].material_slot, 1U);
    NTEST_ASSERT_EQ_UINT(v.submeshes[1].index_count, 3U);
    NTEST_ASSERT_EQ_UINT(v.submeshes[1].meshlet_count, 1U);
    NTEST_ASSERT_EQ_UINT(v.submeshes[0].meshlet_count + v.submeshes[1].meshlet_count,
                         v.header->meshlet_count);
    NTEST_ASSERT_EQ_UINT(v.meshlets[v.submeshes[1].meshlet_offset].submesh, 1U);
    for (u32 i = 0; i < v.header->meshlet_count; ++i) {
        NTEST_ASSERT_LE_UINT(v.meshlets[i].triangle_count, 8U);
    }
    test_asset_free(&ta);
    cook_mesh_free(&m);
}
