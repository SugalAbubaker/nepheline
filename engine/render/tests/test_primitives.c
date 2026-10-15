// SPDX-License-Identifier: Apache-2.0
// Procedural meshes (render/primitives.h): vertex and index counts, outward
// counter-clockwise winding, tangent frames that follow glTF's convention,
// and quantisation within one unorm16 step.
#include <nepheline/math/pack.h>
#include <nepheline/render/primitives.h>

#include <ntest/ntest.h>

#include <string.h>

static NphVec3 position_of(const NphRenderMeshData *m, u32 i)
{
    const NphRenderVertexPosition *p = &m->positions[i];
    const f32 k = 1.0F / 65535.0F;
    return nph_vec3(m->position_offset.x + (f32)p->x * k * m->position_scale.x,
                    m->position_offset.y + (f32)p->y * k * m->position_scale.y,
                    m->position_offset.z + (f32)p->z * k * m->position_scale.z);
}

static NphVec3 normal_of(const NphRenderMeshData *m, u32 i)
{
    return nph_oct_decode_snorm16(m->attribs[i].normal);
}

// Every triangle's geometric normal agrees with its vertices' normals
// (counter-clockwise from outside), and no triangle is degenerate.
static void check_winding(const NphRenderMeshData *m)
{
    for (u32 t = 0; t < m->index_count; t += 3U) {
        const u32 a = m->indices[t];
        const u32 b = m->indices[t + 1U];
        const u32 c = m->indices[t + 2U];
        const NphVec3 pa = position_of(m, a);
        const NphVec3 g = nph_vec3_cross(nph_vec3_sub(position_of(m, b), pa),
                                         nph_vec3_sub(position_of(m, c), pa));
        NTEST_ASSERT_TRUE(nph_vec3_length_sq(g) > 0.0F);
        const NphVec3 n =
            nph_vec3_add(nph_vec3_add(normal_of(m, a), normal_of(m, b)), normal_of(m, c));
        NTEST_EXPECT_TRUE(nph_vec3_dot(g, n) > 0.0F);
    }
}

// Tangents are unit, orthogonal to the normal, and cross(n, t) * w points
// along decreasing v across every triangle with UV area.
static void check_tangents(const NphRenderMeshData *m)
{
    for (u32 i = 0; i < m->vertex_count; ++i) {
        const NphVec4 t = nph_unpack_snorm8x4(m->attribs[i].tangent);
        const NphVec3 n = normal_of(m, i);
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(nph_vec4_xyz(t)), 1.0F, 0.02F);
        NTEST_EXPECT_TRUE(nph_f32_abs(nph_vec3_dot(nph_vec4_xyz(t), n)) < 0.03F);
        NTEST_EXPECT_TRUE(t.w == 1.0F || t.w == -1.0F);
    }
    for (u32 k = 0; k < m->index_count; k += 3U) {
        const u32 a = m->indices[k];
        const u32 b = m->indices[k + 1U];
        const u32 c = m->indices[k + 2U];
        const NphVec2 ua = nph_unpack_f16x2(m->attribs[a].uv0);
        const NphVec2 ub = nph_unpack_f16x2(m->attribs[b].uv0);
        const NphVec2 uc = nph_unpack_f16x2(m->attribs[c].uv0);
        const f32 du1 = ub.x - ua.x;
        const f32 dv1 = ub.y - ua.y;
        const f32 du2 = uc.x - ua.x;
        const f32 dv2 = uc.y - ua.y;
        const f32 det = du1 * dv2 - du2 * dv1;
        if (nph_f32_abs(det) < 1e-6F) {
            continue;
        }
        const NphVec3 e1 = nph_vec3_sub(position_of(m, b), position_of(m, a));
        const NphVec3 e2 = nph_vec3_sub(position_of(m, c), position_of(m, a));
        const NphVec3 dpdv = nph_vec3_scale(
            nph_vec3_sub(nph_vec3_scale(e2, du1), nph_vec3_scale(e1, du2)), 1.0F / det);
        const NphVec4 t = nph_unpack_snorm8x4(m->attribs[a].tangent);
        const NphVec3 bt = nph_vec3_scale(nph_vec3_cross(normal_of(m, a), nph_vec4_xyz(t)), t.w);
        NTEST_EXPECT_TRUE(nph_vec3_dot(bt, dpdv) < 0.0F);
    }
}

static void check_mesh(const NphRenderMeshData *m, u32 vertices, u32 indices)
{
    NTEST_EXPECT_EQ_UINT(m->vertex_count, vertices);
    NTEST_EXPECT_EQ_UINT(m->index_count, indices);
    for (u32 i = 0; i < m->index_count; ++i) {
        NTEST_ASSERT_LT_UINT(m->indices[i], m->vertex_count);
    }
    check_winding(m);
    check_tangents(m);
}

NTEST(primitives, cube)
{
    NphRenderMeshData m;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 2.0F, &m), NPH_RESULT_OK);
    check_mesh(&m, 24U, 36U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.bounds.min, nph_vec3(-1.0F, -1.0F, -1.0F), 1e-6F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.bounds.max, nph_vec3(1.0F, 1.0F, 1.0F), 1e-6F));
    // Every vertex is a corner, exactly representable after quantisation.
    for (u32 i = 0; i < m.vertex_count; ++i) {
        const NphVec3 p = position_of(&m, i);
        NTEST_EXPECT_NEAR_F32(nph_f32_abs(p.x), 1.0F, 1e-6F);
        NTEST_EXPECT_NEAR_F32(nph_f32_abs(p.y), 1.0F, 1e-6F);
        NTEST_EXPECT_NEAR_F32(nph_f32_abs(p.z), 1.0F, 1e-6F);
    }
    nph_render_mesh_data_free(&m);
}

NTEST(primitives, sphere)
{
    NphRenderMeshData m;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_sphere(nph_allocator_system(), 0.5F, 16U, 8U, &m),
                        NPH_RESULT_OK);
    // (segments + 1) x (rings + 1) vertices; the pole rows lose a triangle
    // per quad.
    check_mesh(&m, 17U * 9U, (16U * 8U * 2U - 2U * 16U) * 3U);
    const f32 step = 1.0F / 65535.0F; // one quantisation step of the unit box
    for (u32 i = 0; i < m.vertex_count; ++i) {
        const NphVec3 p = position_of(&m, i);
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(p), 0.5F, 2.0F * step);
        NTEST_EXPECT_TRUE(nph_vec3_near(normal_of(&m, i), nph_vec3_scale(p, 2.0F), 2e-3F));
    }
    nph_render_mesh_data_free(&m);
}

NTEST(primitives, plane)
{
    NphRenderMeshData m;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 4.0F, 3U, 2.0F, &m),
                        NPH_RESULT_OK);
    check_mesh(&m, 16U, 54U);
    for (u32 i = 0; i < m.vertex_count; ++i) {
        NTEST_EXPECT_TRUE(nph_vec3_near(normal_of(&m, i), nph_vec3_unit_y(), 1e-4F));
        // The tangent follows +X (u grows with x); the bitangent is -Z.
        const NphVec4 t = nph_unpack_snorm8x4(m.attribs[i].tangent);
        NTEST_EXPECT_NEAR_F32(t.x, 1.0F, 0.01F);
        const NphVec3 b = nph_vec3_scale(nph_vec3_cross(nph_vec3_unit_y(), nph_vec4_xyz(t)), t.w);
        NTEST_EXPECT_NEAR_F32(b.z, -1.0F, 0.02F);
        const NphVec2 uv = nph_unpack_f16x2(m.attribs[i].uv0);
        NTEST_EXPECT_TRUE(uv.x >= 0.0F && uv.x <= 2.0F && uv.y >= 0.0F && uv.y <= 2.0F);
    }
    // Flat in y: the scale is zero there and every y dequantises to 0.
    NTEST_EXPECT_NEAR_F32(m.position_scale.y, 0.0F, 0.0F);
    nph_render_mesh_data_free(&m);
}

NTEST(primitives, cylinder)
{
    NphRenderMeshData m;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cylinder(nph_allocator_system(), 0.5F, 2.0F, 12U, &m),
                        NPH_RESULT_OK);
    check_mesh(&m, 13U * 2U + 2U * 14U, 12U * 12U);
    NTEST_EXPECT_TRUE(nph_vec3_near(m.bounds.min, nph_vec3(-0.5F, -1.0F, -0.5F), 1e-5F));
    NTEST_EXPECT_TRUE(nph_vec3_near(m.bounds.max, nph_vec3(0.5F, 1.0F, 0.5F), 1e-5F));
    nph_render_mesh_data_free(&m);
}

NTEST(primitives, mesh_data_desc_covers_the_mesh)
{
    NphRenderMeshData m;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_cube(nph_allocator_system(), 1.0F, &m), NPH_RESULT_OK);
    NphRenderSubmesh sub;
    NphRenderMeshDesc d;
    nph_render_mesh_data_desc(&m, "cube", &sub, &d);
    NTEST_EXPECT_EQ_UINT(d.submesh_count, 1U);
    NTEST_EXPECT_EQ_UINT(sub.index_count, 36U);
    NTEST_EXPECT_EQ_UINT(sub.index_offset, 0U);
    NTEST_EXPECT_EQ_UINT(sub.material_slot, 0U);
    NTEST_EXPECT_EQ_PTR(d.positions, m.positions);
    NTEST_EXPECT_EQ_STR(d.name, "cube");
    nph_render_mesh_data_free(&m);
}

NTEST(primitives, rejects_bad_input)
{
    NphAllocator *a = nph_allocator_system();
    NphRenderMeshData m;
    NTEST_EXPECT_EQ_INT(nph_render_primitive_cube(a, 0.0F, &m), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_primitive_sphere(a, 1.0F, 2U, 8U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_primitive_sphere(a, 1.0F, 8U, 1U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_primitive_plane(a, 1.0F, 0U, 1.0F, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_primitive_cylinder(a, 1.0F, -1.0F, 8U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);

    NphRenderVertexInput v[3];
    memset(v, 0, sizeof v);
    v[1].position = nph_vec3(1.0F, 0.0F, 0.0F);
    v[2].position = nph_vec3(0.0F, 1.0F, 0.0F);
    const u32 ok[3] = {0U, 1U, 2U};
    const u32 out_of_range[3] = {0U, 1U, 3U};
    NTEST_EXPECT_EQ_INT(nph_render_mesh_data_build(a, v, 3U, out_of_range, 3U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_mesh_data_build(a, v, 3U, ok, 2U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(nph_render_mesh_data_build(a, v, 0U, ok, 3U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    v[2].position.x = NPH_F32_INFINITY;
    NTEST_EXPECT_EQ_INT(nph_render_mesh_data_build(a, v, 3U, ok, 3U, &m),
                        NPH_RESULT_INVALID_ARGUMENT);
    // A triangle without UV area still gets a valid (arbitrary) tangent.
    v[2].position.x = 0.0F;
    v[0].normal = nph_vec3_unit_z();
    v[1].normal = nph_vec3_unit_z();
    v[2].normal = nph_vec3_unit_z();
    NTEST_ASSERT_EQ_INT(nph_render_mesh_data_build(a, v, 3U, ok, 3U, &m), NPH_RESULT_OK);
    const NphVec4 t = nph_unpack_snorm8x4(m.attribs[0].tangent);
    NTEST_EXPECT_TRUE(nph_f32_abs(t.z) < 0.02F);
    NTEST_EXPECT_NEAR_F32(nph_vec3_length(nph_vec4_xyz(t)), 1.0F, 0.02F);
    nph_render_mesh_data_free(&m);
    nph_render_mesh_data_free(NULL);
}
