// SPDX-License-Identifier: Apache-2.0
// Editable meshes (cook/polymesh.h): the primitives are closed and face out
// (their signed volumes are the solids'), faces triangulate into their own
// area whatever their shape, normals follow the smoothing angle, the
// `.nmesh` file reads back what was written and refuses what it must, and
// extrusion keeps a closed mesh closed.
#include <nepheline/core/hashmap.h>
#include <nepheline/math/scalar.h>

#include "test_cook_support.h"

#include <cook/polymesh.h>

// True when every directed edge appears once and its reverse once: a closed
// surface whose faces agree on their orientation.
static bool closed(const CookPolyMesh *m)
{
    NphHashMap edges;
    const NphHashMapDesc d = nph_hashmap_desc_u64(0U);
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&edges, m->alloc, &d), NPH_RESULT_OK);
    bool ok = true;
    for (u32 f = 0; f < m->faces.len && ok; ++f) {
        const CookPolyFace *face = &m->faces.data[f];
        for (u32 i = 0; i < face->count && ok; ++i) {
            const u64 a = m->corners.data[face->first + i];
            const u64 b = m->corners.data[face->first + (i + 1U) % face->count];
            ok = !nph_hashmap_contains_u64(&edges, (a << 32U) | b) &&
                 nph_hashmap_insert_u64(&edges, (a << 32U) | b, NULL) == NPH_RESULT_OK;
        }
    }
    for (u32 f = 0; f < m->faces.len && ok; ++f) {
        const CookPolyFace *face = &m->faces.data[f];
        for (u32 i = 0; i < face->count && ok; ++i) {
            const u64 a = m->corners.data[face->first + i];
            const u64 b = m->corners.data[face->first + (i + 1U) % face->count];
            ok = nph_hashmap_contains_u64(&edges, (b << 32U) | a);
        }
    }
    nph_hashmap_free(&edges);
    return ok;
}

// The signed volume the triangulated faces enclose (divergence theorem):
// positive when they face out.
static f64 volume(const CookPolyMesh *m)
{
    f64 v = 0.0;
    u32 tri[(COOK_POLYMESH_MAX_FACE_CORNERS - 2U) * 3U];
    for (u32 f = 0; f < m->faces.len; ++f) {
        const CookPolyFace *face = &m->faces.data[f];
        const u32 n = cook_polymesh_triangulate_face(m, f, tri);
        NTEST_ASSERT_EQ_UINT(n, face->count - 2U);
        for (u32 t = 0; t < n; ++t) {
            const NphVec3 a = m->positions.data[m->corners.data[face->first + tri[(u64)t * 3U]]];
            const NphVec3 b =
                m->positions.data[m->corners.data[face->first + tri[(u64)t * 3U + 1U]]];
            const NphVec3 c =
                m->positions.data[m->corners.data[face->first + tri[(u64)t * 3U + 2U]]];
            v += (f64)nph_vec3_dot(a, nph_vec3_cross(b, c)) / 6.0;
        }
    }
    return v;
}

static bool uvs_in_unit_square(const CookPolyMesh *m)
{
    for (u32 c = 0; c < m->uvs.len; ++c) {
        const NphVec2 uv = m->uvs.data[c];
        if (uv.x < 0.0F || uv.x > 1.0F || uv.y < 0.0F || uv.y > 1.0F) {
            return false;
        }
    }
    return true;
}

// Every face of a solid centred on the origin faces away from it.
static bool faces_out(const CookPolyMesh *m)
{
    for (u32 f = 0; f < m->faces.len; ++f) {
        if (nph_vec3_dot(cook_polymesh_face_normal(m, f), cook_polymesh_face_center(m, f)) <=
            0.0F) {
            return false;
        }
    }
    return true;
}

NTEST(polymesh, the_primitives_are_closed_and_face_out)
{
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    cook_polymesh_init(&m, alloc);

    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 2.0F), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(m.positions.len, 8U);
    NTEST_EXPECT_EQ_UINT(m.faces.len, 6U);
    NTEST_EXPECT_EQ_UINT(m.corners.len, 24U);
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_TRUE(faces_out(&m));
    NTEST_EXPECT_NEAR_F64(volume(&m), 8.0, 1e-5);
    NTEST_EXPECT_TRUE(uvs_in_unit_square(&m));
    NTEST_EXPECT_EQ_INT(cook_polymesh_validate(&m), NPH_RESULT_OK);

    NTEST_ASSERT_EQ_INT(cook_polymesh_cylinder(&m, 1.0F, 2.0F, 12U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(m.positions.len, 24U);
    NTEST_EXPECT_EQ_UINT(m.faces.len, 14U);
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_TRUE(faces_out(&m));
    // The prism on the inscribed 12-gon: (n / 2) r^2 sin(2 pi / n) h.
    NTEST_EXPECT_NEAR_F64(volume(&m), 6.0 * 1.0 * 0.5 * 2.0, 1e-4);
    NTEST_EXPECT_TRUE(uvs_in_unit_square(&m));

    NTEST_ASSERT_EQ_INT(cook_polymesh_sphere(&m, 1.0F, 24U, 12U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(m.positions.len, 2U + 11U * 24U);
    NTEST_EXPECT_EQ_UINT(m.faces.len, 24U * 12U);
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_TRUE(faces_out(&m));
    const f64 sphere = 4.0 / 3.0 * (f64)NPH_PI;
    NTEST_EXPECT_TRUE(volume(&m) < sphere && volume(&m) > sphere * 0.95); // inscribed
    NTEST_EXPECT_TRUE(uvs_in_unit_square(&m));
    NTEST_ASSERT_EQ_INT(cook_polymesh_sphere(&m, 1.0F, 3U, 2U), NPH_RESULT_OK); // the least
    NTEST_EXPECT_EQ_UINT(m.faces.len, 6U);
    NTEST_EXPECT_TRUE(closed(&m));

    // The plane is open: its rim is the only edge without a twin.
    NTEST_ASSERT_EQ_INT(cook_polymesh_plane(&m, 2.0F, 3U), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(m.positions.len, 16U);
    NTEST_EXPECT_EQ_UINT(m.faces.len, 9U);
    NTEST_EXPECT_FALSE(closed(&m));
    for (u32 f = 0; f < m.faces.len; ++f) {
        NTEST_EXPECT_TRUE(
            nph_vec3_near(cook_polymesh_face_normal(&m, f), nph_vec3_unit_y(), 1e-6F));
    }
    NTEST_EXPECT_TRUE(uvs_in_unit_square(&m));

    NTEST_EXPECT_EQ_INT(cook_polymesh_plane(&m, 1.0F, 0U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_polymesh_cylinder(&m, 1.0F, 1.0F, 2U), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_polymesh_sphere(&m, 1.0F, 8U, 1U), NPH_RESULT_INVALID_ARGUMENT);
    cook_polymesh_free(&m);
}

// A face of the given 2D outline (x, z pairs in the XZ plane, facing +Y).
static void outline(CookPolyMesh *m, const f32 *xz, u32 n)
{
    cook_polymesh_clear(m);
    u32 corners[64];
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(
            cook_polymesh_add_position(m, nph_vec3(xz[(u64)i * 2U], 0.0F, xz[(u64)i * 2U + 1U]),
                                       &corners[i]),
            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(cook_polymesh_add_face(m, corners, NULL, n, 0U, NULL), NPH_RESULT_OK);
}

// The face's triangles: their total area, and whether each faces as the face.
static f64 triangulated_area(const CookPolyMesh *m, bool *all_front)
{
    u32 tri[(COOK_POLYMESH_MAX_FACE_CORNERS - 2U) * 3U];
    const u32 n = cook_polymesh_triangulate_face(m, 0U, tri);
    NTEST_ASSERT_EQ_UINT(n, m->faces.data[0].count - 2U);
    const NphVec3 normal = cook_polymesh_face_normal(m, 0U);
    f64 area = 0.0;
    *all_front = true;
    for (u32 t = 0; t < n; ++t) {
        const NphVec3 a = m->positions.data[m->corners.data[tri[(u64)t * 3U]]];
        const NphVec3 b = m->positions.data[m->corners.data[tri[(u64)t * 3U + 1U]]];
        const NphVec3 c = m->positions.data[m->corners.data[tri[(u64)t * 3U + 2U]]];
        const f32 twice =
            nph_vec3_dot(nph_vec3_cross(nph_vec3_sub(b, a), nph_vec3_sub(c, a)), normal);
        *all_front = *all_front && twice > 0.0F;
        area += (f64)twice * 0.5;
    }
    return area;
}

NTEST(polymesh, faces_of_any_shape_triangulate_into_their_own_area)
{
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    cook_polymesh_init(&m, alloc);
    bool front = false;
    // Counter-clockwise seen from +Y means clockwise in (x, z) drawn with z
    // down: these outlines list x, z with that in mind.
    static const f32 square[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    outline(&m, &square[0][0], 4U);
    NTEST_EXPECT_TRUE(nph_vec3_near(cook_polymesh_face_normal(&m, 0U), nph_vec3_unit_y(), 1e-6F));
    NTEST_EXPECT_NEAR_F64(triangulated_area(&m, &front), 1.0, 1e-6);
    NTEST_EXPECT_TRUE(front);
    // An L: one reflex corner, which a fan from the wrong corner gets wrong.
    static const f32 ell[6][2] = {{0, 0}, {0, 2}, {2, 2}, {2, 1}, {1, 1}, {1, 0}};
    outline(&m, &ell[0][0], 6U);
    NTEST_EXPECT_NEAR_F64(triangulated_area(&m, &front), 3.0, 1e-6);
    NTEST_EXPECT_TRUE(front);
    // A comb: teeth of reflex corners.
    static const f32 comb[12][2] = {{0, 0}, {0, 3}, {5, 3}, {5, 0}, {4, 0}, {4, 2},
                                    {3, 2}, {3, 0}, {2, 0}, {2, 2}, {1, 2}, {1, 0}};
    outline(&m, &comb[0][0], 12U);
    NTEST_EXPECT_NEAR_F64(triangulated_area(&m, &front), 15.0 - 2.0 * 2.0, 1e-5); // two notches
    NTEST_EXPECT_TRUE(front);
    // A star with its points first in the list.
    f32 star[16][2];
    for (u32 i = 0; i < 16U; ++i) {
        const f32 a = -2.0F * NPH_PI * (f32)i / 16.0F;
        const f32 r = (i % 2U) == 0U ? 2.0F : 0.7F;
        star[i][0] = r * nph_f32_cos(a);
        star[i][1] = r * nph_f32_sin(a);
    }
    outline(&m, &star[0][0], 16U);
    f64 expected = 0.0;
    for (u32 i = 0; i < 16U; ++i) {
        const f32 *p = star[i];
        const f32 *q = star[(i + 1U) % 16U];
        expected += 0.5 * ((f64)p[1] * (f64)q[0] - (f64)p[0] * (f64)q[1]);
    }
    NTEST_EXPECT_NEAR_F64(triangulated_area(&m, &front), expected, 1e-4);
    NTEST_EXPECT_TRUE(front);
    // Degenerate: every corner on a line. It still gives count - 2
    // triangles (of no area), and nothing breaks.
    static const f32 line[5][2] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}};
    outline(&m, &line[0][0], 5U);
    NTEST_EXPECT_TRUE(nph_vec3_eq(cook_polymesh_face_normal(&m, 0U), nph_vec3_zero()));
    u32 tri[9];
    NTEST_EXPECT_EQ_UINT(cook_polymesh_triangulate_face(&m, 0U, tri), 3U);
    cook_polymesh_free(&m);
}

NTEST(polymesh, normals_follow_the_smoothing_angle)
{
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    cook_polymesh_init(&m, alloc);
    CookMesh mesh;
    // A cube's edges are sharper than 60 degrees: every corner keeps its
    // face's normal.
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 1.0F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_polymesh_to_mesh(&m, "cube", &mesh), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(mesh.vertices.len, 24U);
    NTEST_EXPECT_EQ_UINT(mesh.indices.len, 36U);
    NTEST_EXPECT_EQ_UINT(mesh.submeshes.len, 1U);
    NTEST_EXPECT_EQ_STR(mesh.name, "cube");
    for (u32 v = 0; v < mesh.vertices.len; ++v) {
        const NphVec3 n = mesh.vertices.data[v].normal;
        const f32 axis = nph_f32_max3(nph_f32_abs(n.x), nph_f32_abs(n.y), nph_f32_abs(n.z));
        NTEST_EXPECT_NEAR_F32(axis, 1.0F, 1e-6F);
    }
    cook_mesh_free(&mesh);
    // A sphere's are not: corners point out from the centre.
    NTEST_ASSERT_EQ_INT(cook_polymesh_sphere(&m, 2.0F, 32U, 16U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_polymesh_to_mesh(&m, "sphere", &mesh), NPH_RESULT_OK);
    for (u32 v = 0; v < mesh.vertices.len; ++v) {
        const CookVertex *x = &mesh.vertices.data[v];
        NTEST_EXPECT_TRUE(nph_vec3_dot(x->normal, nph_vec3_normalize(x->position)) > 0.99F);
    }
    cook_mesh_free(&mesh);
    // A cylinder's sides are smooth and its caps flat.
    NTEST_ASSERT_EQ_INT(cook_polymesh_cylinder(&m, 1.0F, 2.0F, 16U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_polymesh_to_mesh(&m, "cylinder", &mesh), NPH_RESULT_OK);
    for (u32 v = 0; v < mesh.vertices.len; ++v) {
        const CookVertex *x = &mesh.vertices.data[v];
        const bool cap = nph_f32_abs(x->normal.y) > 0.5F;
        if (cap) {
            NTEST_EXPECT_NEAR_F32(nph_f32_abs(x->normal.y), 1.0F, 1e-6F);
        } else {
            NTEST_EXPECT_NEAR_F32(x->normal.y, 0.0F, 1e-6F);
            const NphVec3 radial = nph_vec3_normalize(nph_vec3(x->position.x, 0.0F, x->position.z));
            NTEST_EXPECT_TRUE(nph_vec3_dot(x->normal, radial) > 0.999F);
        }
    }
    cook_mesh_free(&mesh);
    // Smoothing 180: everything around a position averages, even across a
    // cube's edges.
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 1.0F), NPH_RESULT_OK);
    m.smoothing = 180.0F;
    NTEST_ASSERT_EQ_INT(cook_polymesh_to_mesh(&m, "round", &mesh), NPH_RESULT_OK);
    for (u32 v = 0; v < mesh.vertices.len; ++v) {
        const CookVertex *x = &mesh.vertices.data[v];
        NTEST_EXPECT_TRUE(nph_vec3_near(x->normal, nph_vec3_normalize(x->position), 1e-5F));
    }
    cook_mesh_free(&mesh);
    // Faces of two materials become two submeshes, in slot order.
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 1.0F), NPH_RESULT_OK);
    m.faces.data[4].material = 2U;
    NTEST_ASSERT_EQ_INT(cook_polymesh_to_mesh(&m, "two", &mesh), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(mesh.submeshes.len, 2U);
    NTEST_EXPECT_EQ_UINT(mesh.submeshes.data[0].material_slot, 0U);
    NTEST_EXPECT_EQ_UINT(mesh.submeshes.data[0].index_count, 30U);
    NTEST_EXPECT_EQ_UINT(mesh.submeshes.data[1].material_slot, 2U);
    NTEST_EXPECT_EQ_UINT(mesh.submeshes.data[1].index_count, 6U);
    cook_mesh_free(&mesh);
    cook_polymesh_free(&m);
}

NTEST(polymesh, the_file_reads_back_what_was_written)
{
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    cook_polymesh_init(&m, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_cylinder(&m, 0.5F, 1.25F, 7U), NPH_RESULT_OK);
    m.smoothing = 42.5F;
    m.faces.data[0].material = 3U;
    NphStrBuilder text;
    nph_sb_init(&text, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_write(&m, &text), NPH_RESULT_OK);
    CookPolyMesh back;
    NTEST_ASSERT_EQ_INT(cook_polymesh_read(alloc, nph_str(text.data, text.len), &back),
                        NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(back.smoothing, 42.5F, 0.0F);
    NTEST_ASSERT_EQ_UINT(back.positions.len, m.positions.len);
    NTEST_ASSERT_EQ_UINT(back.faces.len, m.faces.len);
    NTEST_ASSERT_EQ_UINT(back.corners.len, m.corners.len);
    // Equal values (the file writes -0 as 0), and the same topology.
    for (u32 i = 0; i < m.positions.len; ++i) {
        NTEST_EXPECT_TRUE(nph_vec3_eq(back.positions.data[i], m.positions.data[i]));
    }
    for (u32 i = 0; i < m.uvs.len; ++i) {
        NTEST_EXPECT_TRUE(nph_vec2_eq(back.uvs.data[i], m.uvs.data[i]));
    }
    NTEST_EXPECT_EQ_MEM(back.faces.data, m.faces.data, sizeof(CookPolyFace) * (u64)m.faces.len);
    NTEST_EXPECT_EQ_MEM(back.corners.data, m.corners.data, sizeof(u32) * (u64)m.corners.len);
    // Written again, the same text: one canonical form.
    NphStrBuilder again;
    nph_sb_init(&again, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_write(&back, &again), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(nph_str_equals(nph_str(text.data, text.len), nph_str(again.data, again.len)));
    nph_sb_free(&again);
    nph_sb_free(&text);
    cook_polymesh_free(&back);

    // Saved and loaded, nothing left beside it.
    TempDir dir;
    test_temp_dir_make(&dir, "polymesh");
    char path[1200];
    char part[1300];
    test_path_in(&dir, "thing.nmesh", path, sizeof path);
    (void)snprintf(part, sizeof part, "%s.part", path);
    NTEST_ASSERT_EQ_INT(cook_polymesh_save(&m, path), NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(nph_fs_exists(part));
    NTEST_ASSERT_EQ_INT(cook_polymesh_load(alloc, path, &back), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(back.faces.len, m.faces.len);
    cook_polymesh_free(&back);
    test_temp_dir_remove(&dir);

    // The optional fields have their defaults.
    static const char minimal[] =
        "{\"nmesh\": 1, \"positions\": [[0,0,0],[1,0,0],[0,0,-1]], \"faces\": [{\"v\": [0,1,2]}]}";
    NTEST_ASSERT_EQ_INT(cook_polymesh_read(alloc, nph_str_from_cstr(minimal), &back),
                        NPH_RESULT_OK);
    NTEST_EXPECT_NEAR_F32(back.smoothing, COOK_POLYMESH_SMOOTHING, 0.0F);
    NTEST_EXPECT_EQ_UINT(back.faces.data[0].material, 0U);
    NTEST_EXPECT_TRUE(nph_vec2_eq(back.uvs.data[2], nph_vec2_zero()));
    cook_polymesh_free(&back);
    cook_polymesh_free(&m);
}

NTEST(polymesh, the_reader_refuses_what_it_must)
{
    NphAllocator *alloc = nph_allocator_system();
    // Three positions, which most of the faces below name.
#define TRIANGLE "\"positions\": [[0,0,0],[1,0,0],[0,1,0]]"
    static const char *const bad[] = {
        "",
        "[]",
        "{\"nmesh\": 1}",
        "{\"nmesh\": 1, \"positions\": [], \"faces\": {}}",
        "{\"nmesh\": 1, \"positions\": [[0,0]], \"faces\": []}",
        "{\"nmesh\": 1, \"positions\": [[0,0,1e30]], \"faces\": []}",
        "{\"nmesh\": 1, \"positions\": [[0,0,\"x\"]], \"faces\": []}",
        "{\"nmesh\": 1, \"smoothing\": 181, \"positions\": [], \"faces\": []}",
        "{\"nmesh\": 1.5, \"positions\": [], \"faces\": []}",
        "{\"nmesh\": 1, \"positions\": [[0,0,0],[1,0,0]], \"faces\": [{\"v\": [0,1]}]}",
        "{\"nmesh\": 1, " TRIANGLE ", \"faces\": [{\"v\": [0,1,3]}]}",
        "{\"nmesh\": 1, " TRIANGLE ", \"faces\": [{\"v\": [0,1,-1]}]}",
        "{\"nmesh\": 1, " TRIANGLE ", \"faces\": [{\"v\": [0,1,2.5]}]}",
        "{\"nmesh\": 1, " TRIANGLE ", \"faces\": [{\"v\": [0,1,2], \"uv\": [[0,0]]}]}",
        "{\"nmesh\": 1, " TRIANGLE ", \"faces\": [{\"v\": [0,1,2], \"m\": 8}]}",
        "{\"nmesh\": 1, " TRIANGLE ", \"faces\": [[0,1,2]]}",
    };
#undef TRIANGLE
    for (u32 i = 0; i < NPH_ARRAY_COUNT(bad); ++i) {
        CookPolyMesh m;
        NTEST_EXPECT_EQ_INT(cook_polymesh_read(alloc, nph_str_from_cstr(bad[i]), &m),
                            NPH_RESULT_PARSE_ERROR);
        NTEST_EXPECT_EQ_UINT(m.faces.len, 0U); // left empty
        cook_polymesh_free(&m);
    }
    CookPolyMesh m;
    NTEST_EXPECT_EQ_INT(cook_polymesh_read(alloc,
                                           nph_str_from_cstr("{\"nmesh\": 2, \"positions\": [], "
                                                             "\"faces\": []}"),
                                           &m),
                        NPH_RESULT_VERSION_MISMATCH);
    cook_polymesh_free(&m);

    // In memory: what validation catches.
    cook_polymesh_init(&m, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 1.0F), NPH_RESULT_OK);
    m.corners.data[5] = 99U;
    NTEST_EXPECT_EQ_INT(cook_polymesh_validate(&m), NPH_RESULT_CORRUPT);
    m.corners.data[5] = 1U;
    m.faces.data[2].first += 1U;
    NTEST_EXPECT_EQ_INT(cook_polymesh_validate(&m), NPH_RESULT_CORRUPT);
    m.faces.data[2].first -= 1U;
    m.positions.data[0].x = nph_f32_from_bits(0x7FC00000U); // NaN
    NTEST_EXPECT_EQ_INT(cook_polymesh_validate(&m), NPH_RESULT_CORRUPT);
    m.positions.data[0].x = -0.5F;
    NTEST_EXPECT_EQ_INT(cook_polymesh_validate(&m), NPH_RESULT_OK);
    // Adding refuses as validation would.
    const u32 two[2] = {0U, 1U};
    const u32 far[3] = {0U, 1U, 8U};
    const u32 ok[3] = {0U, 1U, 2U};
    NTEST_EXPECT_EQ_INT(cook_polymesh_add_face(&m, two, NULL, 2U, 0U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_polymesh_add_face(&m, far, NULL, 3U, 0U, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_polymesh_add_face(&m, ok, NULL, 3U, COOK_POLYMESH_MATERIALS, NULL),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(m.faces.len, 6U);
    cook_polymesh_free(&m);
}

NTEST(polymesh, extruding_keeps_a_closed_mesh_closed)
{
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    cook_polymesh_init(&m, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 2.0F), NPH_RESULT_OK);
    // The top face (+Y, face 4), up by one: four new positions, four sides,
    // and a box one taller.
    const u32 top = 4U;
    u32 sides = 0U;
    NTEST_ASSERT_EQ_INT(cook_polymesh_extrude(&m, &top, 1U, 1.0F, &sides), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(sides, 4U);
    NTEST_EXPECT_EQ_UINT(m.positions.len, 12U);
    NTEST_EXPECT_EQ_UINT(m.faces.len, 10U);
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_NEAR_F64(volume(&m), 8.0 + 4.0, 1e-4);
    NTEST_EXPECT_NEAR_F32(cook_polymesh_face_center(&m, top).y, 2.0F, 1e-6F);
    NTEST_EXPECT_EQ_INT(cook_polymesh_validate(&m), NPH_RESULT_OK);
    // Two faces sharing an edge, as one region: the shared edge gets no side.
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 2.0F), NPH_RESULT_OK);
    const u32 corner[3] = {4U, 2U, 4U}; // +Y and +X, one listed twice
    NTEST_ASSERT_EQ_INT(cook_polymesh_extrude(&m, corner, 3U, 0.5F, &sides), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(sides, 6U);
    NTEST_EXPECT_EQ_UINT(m.positions.len, 8U + 6U);
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_TRUE(volume(&m) > 8.0);
    // Every face of a closed mesh: the whole of it grows, and stays closed.
    NTEST_ASSERT_EQ_INT(cook_polymesh_sphere(&m, 1.0F, 8U, 6U), NPH_RESULT_OK);
    const f64 before = volume(&m);
    u32 all[48];
    for (u32 f = 0; f < m.faces.len; ++f) {
        all[f] = f;
    }
    NTEST_ASSERT_EQ_INT(cook_polymesh_extrude(&m, all, m.faces.len, 0.25F, &sides), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(sides, 0U); // no boundary
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_TRUE(volume(&m) > before);
    // A distance of zero leaves sides of no area, still closed; nothing
    // selected changes nothing; a face out of range is refused.
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 2.0F), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cook_polymesh_extrude(&m, &top, 1U, 0.0F, &sides), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(closed(&m));
    NTEST_EXPECT_NEAR_F64(volume(&m), 8.0, 1e-4);
    NTEST_ASSERT_EQ_INT(cook_polymesh_extrude(&m, NULL, 0U, 1.0F, &sides), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(sides, 0U);
    const u32 beyond = 99U;
    NTEST_EXPECT_EQ_INT(cook_polymesh_extrude(&m, &beyond, 1U, 1.0F, &sides),
                        NPH_RESULT_INVALID_ARGUMENT);
    cook_polymesh_free(&m);
}

NTEST(polymesh, a_copy_is_its_own)
{
    NphAllocator *alloc = nph_allocator_system();
    CookPolyMesh m;
    cook_polymesh_init(&m, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_cube(&m, 1.0F), NPH_RESULT_OK);
    CookPolyMesh c;
    NTEST_ASSERT_EQ_INT(cook_polymesh_copy(&m, alloc, &c), NPH_RESULT_OK);
    c.positions.data[0] = nph_vec3(9.0F, 9.0F, 9.0F);
    NTEST_EXPECT_NEAR_F32(m.positions.data[0].x, -0.5F, 0.0F);
    NTEST_EXPECT_EQ_UINT(c.faces.len, 6U);
    cook_polymesh_free(&c);
    CookPolyMesh empty;
    cook_polymesh_init(&empty, alloc);
    NTEST_ASSERT_EQ_INT(cook_polymesh_copy(&empty, alloc, &c), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(c.positions.len, 0U);
    cook_polymesh_free(&c);
    cook_polymesh_free(&empty);
    cook_polymesh_free(&m);
}
