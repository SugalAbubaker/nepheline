// SPDX-License-Identifier: Apache-2.0
// The viewport's geometry (editor/src/view.h, gizmo.h, picking.h): screen
// projection and rays agree with each other and with the renderer's
// conventions, gizmo drags move, turn, and scale as the mouse says, and
// picking finds the nearest entity under the mouse.
#include <nepheline/math/quat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/scene/transform.h>

#include <ntest/ntest.h>

#include "gizmo.h"
#include "picking.h"
#include "view.h"

#include <string.h>

#define EPS 1e-4F

static EditorView looking_at_origin(NphDVec3 eye)
{
    const NphRenderCamera cam = nph_render_camera_look_at(
        eye, nph_dvec3_zero(), nph_vec3(0.0F, 1.0F, 0.0F), nph_f32_radians(90.0F), 0.1F);
    EditorView v;
    editor_view_init(&v, &cam, 100.0F, 50.0F, 800.0F, 600.0F);
    return v;
}

// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------

NTEST(EditorView, the_middle_of_the_screen_looks_ahead)
{
    const EditorView v = looking_at_origin(nph_dvec3(0.0, 0.0, 10.0));
    f32 x = 0.0F;
    f32 y = 0.0F;
    NTEST_ASSERT_TRUE(editor_view_project(&v, nph_dvec3_zero(), &x, &y));
    NTEST_EXPECT_NEAR_F32(x, 500.0F, EPS); // 100 + 800 / 2
    NTEST_EXPECT_NEAR_F32(y, 350.0F, EPS); // 50 + 600 / 2
    const NphVec3 ahead = editor_view_ray(&v, 500.0F, 350.0F);
    NTEST_EXPECT_NEAR_F32(ahead.z, -1.0F, EPS);
    // +X is to the right on screen, +Y up (the screen's y grows down).
    NTEST_ASSERT_TRUE(editor_view_project(&v, nph_dvec3(1.0, 0.0, 0.0), &x, &y));
    NTEST_EXPECT_TRUE(x > 500.0F);
    NTEST_ASSERT_TRUE(editor_view_project(&v, nph_dvec3(0.0, 1.0, 0.0), &x, &y));
    NTEST_EXPECT_TRUE(y < 350.0F);
    // 90 degrees vertically: the top edge is 45 degrees up, 10 m away.
    NTEST_ASSERT_TRUE(editor_view_project(&v, nph_dvec3(0.0, 10.0, 0.0), &x, &y));
    NTEST_EXPECT_NEAR_F32(y, 50.0F, 1e-3F);
    NTEST_EXPECT_NEAR_F32(editor_view_world_per_unit(&v, 10.0F), 20.0F / 600.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(editor_view_depth(&v, nph_dvec3_zero()), 10.0F, EPS);
    // Behind the eye nothing projects.
    NTEST_EXPECT_FALSE(editor_view_project(&v, nph_dvec3(0.0, 0.0, 20.0), &x, &y));
}

NTEST(EditorView, rays_and_projection_agree_anywhere_in_the_world)
{
    // Ten thousand kilometres out: relative positions keep it exact.
    const NphDVec3 far_eye = nph_dvec3(1.0e7, 2.0, -1.0e7);
    const NphRenderCamera cam =
        nph_render_camera_look_at(far_eye, nph_dvec3(1.0e7 + 3.0, 0.0, -1.0e7 - 4.0),
                                  nph_vec3(0.0F, 1.0F, 0.0F), 1.0F, 0.05F);
    EditorView v;
    editor_view_init(&v, &cam, 0.0F, 0.0F, 1280.0F, 720.0F);
    const f32 points[4][2] = {{10.0F, 10.0F}, {640.0F, 360.0F}, {1200.0F, 30.0F}, {77.0F, 700.0F}};
    for (u32 i = 0; i < 4U; ++i) {
        const NphVec3 dir = editor_view_ray(&v, points[i][0], points[i][1]);
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(dir), 1.0F, EPS);
        const NphDVec3 p = nph_dvec3_add_vec3(far_eye, nph_vec3_scale(dir, 7.5F));
        f32 x = 0.0F;
        f32 y = 0.0F;
        NTEST_ASSERT_TRUE(editor_view_project(&v, p, &x, &y));
        NTEST_EXPECT_NEAR_F32(x, points[i][0], 1e-2F);
        NTEST_EXPECT_NEAR_F32(y, points[i][1], 1e-2F);
    }
}

// ---------------------------------------------------------------------------
// The gizmo
// ---------------------------------------------------------------------------

NTEST(EditorGizmo, the_math_under_the_drags)
{
    f32 s = 0.0F;
    // A ray down -Z through (2, 3): nearest the X axis at x = 2.
    NTEST_ASSERT_TRUE(editor_gizmo_axis_param(
        nph_vec3(2.0F, 3.0F, 10.0F), nph_vec3(0.0F, 0.0F, -1.0F), nph_vec3(1.0F, 0.0F, 0.0F), &s));
    NTEST_EXPECT_NEAR_F32(s, 2.0F, EPS);
    // Along the axis: no answer.
    NTEST_EXPECT_FALSE(editor_gizmo_axis_param(
        nph_vec3(0.0F, 1.0F, 0.0F), nph_vec3(1.0F, 0.0F, 0.0F), nph_vec3(1.0F, 0.0F, 0.0F), &s));
    NphVec3 hit;
    NTEST_ASSERT_TRUE(editor_gizmo_plane_hit(
        nph_vec3(1.0F, 5.0F, 2.0F), nph_vec3(0.0F, -1.0F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F), &hit));
    NTEST_EXPECT_NEAR_F32(hit.x, 1.0F, EPS);
    NTEST_EXPECT_NEAR_F32(hit.y, 0.0F, EPS);
    NTEST_EXPECT_NEAR_F32(hit.z, 2.0F, EPS);
    NTEST_EXPECT_FALSE(editor_gizmo_plane_hit(nph_vec3(1.0F, 5.0F, 2.0F),
                                              nph_vec3(0.0F, 1.0F, 0.0F),
                                              nph_vec3(0.0F, 1.0F, 0.0F), &hit)); // away
    const NphVec3 x = nph_vec3(1.0F, 0.0F, 0.0F);
    const NphVec3 y = nph_vec3(0.0F, 1.0F, 0.0F);
    const NphVec3 z = nph_vec3(0.0F, 0.0F, 1.0F);
    NTEST_EXPECT_NEAR_F32(editor_gizmo_angle(x, y, z), NPH_PI * 0.5F, EPS);
    NTEST_EXPECT_NEAR_F32(editor_gizmo_angle(y, x, z), -NPH_PI * 0.5F, EPS);
    NTEST_EXPECT_NEAR_F32(editor_gizmo_snap(0.74F, 0.5F), 0.5F, EPS);
    NTEST_EXPECT_NEAR_F32(editor_gizmo_snap(-0.26F, 0.5F), -0.5F, EPS);
    NTEST_EXPECT_NEAR_F32(editor_gizmo_snap(0.3F, 0.0F), 0.3F, EPS);
}

NTEST(EditorGizmo, translating_follows_the_axis_or_the_plane)
{
    NphWorldTransform start = nph_world_transform_identity();
    start.translation = nph_dvec3(5.0, 0.0, 0.0);
    // The camera above, rays straight down: relative to the entity's origin.
    const NphVec3 down = nph_vec3(0.0F, -1.0F, 0.0F);
    EditorGizmoDrag d;
    NTEST_ASSERT_TRUE(editor_gizmo_begin(&d, EDITOR_GIZMO_TRANSLATE, EDITOR_GIZMO_X, &start, false,
                                         nph_vec3(1.0F, 10.0F, 0.5F), down, 0.0F));
    NphWorldTransform t = editor_gizmo_drag(&d, nph_vec3(3.7F, 10.0F, 0.9F), down, 0.0F, false);
    NTEST_EXPECT_NEAR_F64(t.translation.x, 7.7, 1e-4); // grabbed at 1, now at 3.7
    NTEST_EXPECT_NEAR_F64(t.translation.z, 0.0, 1e-9); // along X only
    t = editor_gizmo_drag(&d, nph_vec3(3.7F, 10.0F, 0.9F), down, 0.0F, true);
    NTEST_EXPECT_NEAR_F64(t.translation.x, 7.5, 1e-4); // 2.7 snapped to 2.5
    // The ground plane (facing Y): both X and Z follow.
    NTEST_ASSERT_TRUE(editor_gizmo_begin(&d, EDITOR_GIZMO_TRANSLATE, EDITOR_GIZMO_PLANE_Y, &start,
                                         false, nph_vec3(0.2F, 10.0F, 0.2F), down, 0.0F));
    t = editor_gizmo_drag(&d, nph_vec3(1.2F, 10.0F, -2.8F), down, 0.0F, false);
    NTEST_EXPECT_NEAR_F64(t.translation.x, 6.0, 1e-4);
    NTEST_EXPECT_NEAR_F64(t.translation.y, 0.0, 1e-9);
    NTEST_EXPECT_NEAR_F64(t.translation.z, -3.0, 1e-4);
    // Rays that never meet the plane leave the entity where it started.
    t = editor_gizmo_drag(&d, nph_vec3(1.0F, 10.0F, 0.0F), nph_vec3(1.0F, 0.0F, 0.0F), 0.0F, false);
    NTEST_EXPECT_NEAR_F64(t.translation.x, 5.0, 0.0);
}

NTEST(EditorGizmo, rotating_turns_about_the_ring_and_scaling_stretches_the_axes)
{
    const NphWorldTransform start = nph_world_transform_identity();
    const NphVec3 down = nph_vec3(0.0F, -1.0F, 0.0F);
    // Grab the Y ring at +X, drag to -Z: a quarter turn about +Y.
    EditorGizmoDrag d;
    NTEST_ASSERT_TRUE(editor_gizmo_begin(&d, EDITOR_GIZMO_ROTATE, EDITOR_GIZMO_Y, &start, false,
                                         nph_vec3(1.0F, 5.0F, 0.0F), down, 0.0F));
    NphWorldTransform t = editor_gizmo_drag(&d, nph_vec3(0.0F, 5.0F, -1.0F), down, 0.0F, false);
    NTEST_EXPECT_NEAR_F32(t.x_axis.z, -1.0F, EPS);
    NTEST_EXPECT_NEAR_F32(t.z_axis.x, 1.0F, EPS);
    NTEST_EXPECT_NEAR_F32(t.y_axis.y, 1.0F, EPS);
    // Snapped: 80 degrees becomes 75.
    const f32 a = nph_f32_radians(80.0F);
    t = editor_gizmo_drag(&d, nph_vec3(nph_f32_cos(a), 5.0F, -nph_f32_sin(a)), down, 0.0F, true);
    NTEST_EXPECT_NEAR_F32(nph_f32_atan2(-t.x_axis.z, t.x_axis.x), nph_f32_radians(75.0F), 1e-3F);

    // Scale X: grabbed one metre out, dragged to two.
    NTEST_ASSERT_TRUE(editor_gizmo_begin(&d, EDITOR_GIZMO_SCALE, EDITOR_GIZMO_X, &start, false,
                                         nph_vec3(1.0F, 5.0F, 0.0F), down, 0.0F));
    t = editor_gizmo_drag(&d, nph_vec3(2.0F, 5.0F, 0.0F), down, 0.0F, false);
    NTEST_EXPECT_NEAR_F32(t.x_axis.x, 2.0F, EPS);
    NTEST_EXPECT_NEAR_F32(t.y_axis.y, 1.0F, EPS);
    // Never flat.
    t = editor_gizmo_drag(&d, nph_vec3(-3.0F, 5.0F, 0.0F), down, 0.0F, false);
    NTEST_EXPECT_NEAR_F32(t.x_axis.x, 0.01F, EPS);
    // All three: 50 screen units to the right is half again.
    NTEST_ASSERT_TRUE(editor_gizmo_begin(&d, EDITOR_GIZMO_SCALE, EDITOR_GIZMO_ALL, &start, false,
                                         nph_vec3(0.0F, 5.0F, 0.0F), down, 100.0F));
    t = editor_gizmo_drag(&d, nph_vec3(0.0F, 5.0F, 0.0F), down, 150.0F, false);
    NTEST_EXPECT_NEAR_F32(t.x_axis.x, 1.5F, EPS);
    NTEST_EXPECT_NEAR_F32(t.z_axis.z, 1.5F, EPS);
}

NTEST(EditorGizmo, local_axes_follow_the_entity)
{
    NphLocalTransform l = nph_local_transform_identity();
    l.rotation = nph_quat_from_axis_angle(nph_vec3(0.0F, 0.0F, 1.0F), NPH_PI * 0.5F);
    l.scale = nph_vec3(3.0F, 3.0F, 3.0F);
    const NphWorldTransform t = nph_world_transform_from_local(&l);
    NphVec3 axes[3];
    editor_gizmo_axes(&t, true, axes);
    NTEST_EXPECT_NEAR_F32(axes[0].y, 1.0F, EPS); // X turned onto +Y, and unit
    NTEST_EXPECT_NEAR_F32(nph_vec3_length(axes[0]), 1.0F, EPS);
    editor_gizmo_axes(&t, false, axes);
    NTEST_EXPECT_NEAR_F32(axes[0].x, 1.0F, 0.0F);
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

// A two-metre square facing +Z, from -1 to 1 in X and Y.
typedef struct QuadMesh {
    NphMeshHeader header;
    NphMeshPosition positions[4];
    u32 indices[6];
    NphMeshView view;
} QuadMesh;

static void quad_mesh(QuadMesh *m)
{
    memset(m, 0, sizeof *m);
    m->header.vertex_count = 4U;
    m->header.index_count = 6U;
    m->header.position_offset = nph_vec3(-1.0F, -1.0F, 0.0F);
    m->header.position_scale = nph_vec3(2.0F, 2.0F, 0.0F);
    m->header.bounds = nph_aabb(nph_vec3(-1.0F, -1.0F, 0.0F), nph_vec3(1.0F, 1.0F, 0.0F));
    const u16 corners[4][2] = {{0U, 0U}, {65535U, 0U}, {65535U, 65535U}, {0U, 65535U}};
    for (u32 i = 0; i < 4U; ++i) {
        m->positions[i].x = corners[i][0];
        m->positions[i].y = corners[i][1];
    }
    const u32 idx[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    memcpy(m->indices, idx, sizeof idx);
    m->view.header = &m->header;
    m->view.positions = m->positions;
    m->view.indices = m->indices;
}

typedef struct PickWorld {
    QuadMesh quad;
    NphRuntimeComponents rc;
} PickWorld;

static bool lookup(void *user, NphEntity e, NphMeshView *out)
{
    NPH_UNUSED(e);
    *out = ((PickWorld *)user)->quad.view;
    return true;
}

static NphEntity place(NphWorld *w, const PickWorld *pw, bool mesh, bool light, NphDVec3 at,
                       f32 scale)
{
    NphComponentId ids[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, NPH_COMPONENT_ID_WORLD_TRANSFORM};
    u32 n = 2U;
    if (mesh) {
        ids[n++] = pw->rc.mesh_renderer;
    }
    if (light) {
        ids[n++] = pw->rc.light;
    }
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, ids, n, &e), NPH_RESULT_OK);
    NphLocalTransform l = nph_local_transform_identity();
    l.translation = at;
    l.scale = nph_vec3(scale, scale, scale);
    const NphWorldTransform t = nph_world_transform_from_local(&l);
    NTEST_ASSERT_EQ_INT(nph_world_set(w, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, &t), NPH_RESULT_OK);
    return e;
}

NTEST(EditorPicking, the_nearest_entity_under_the_ray_wins)
{
    static PickWorld pw;
    quad_mesh(&pw.quad);
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_allocator_system();
    wd.max_entities = 64U;
    NphWorld *w = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &w), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_runtime_register_components(w, &pw.rc), NPH_RESULT_OK);
    const NphEntity front = place(w, &pw, true, false, nph_dvec3(0.0, 0.0, 0.0), 1.0F);
    const NphEntity back = place(w, &pw, true, false, nph_dvec3(0.0, 0.0, -5.0), 2.0F);
    const NphEntity lamp = place(w, &pw, false, true, nph_dvec3(5.0, 0.0, 0.0), 1.0F);
    const NphVec3 ahead = nph_vec3(0.0F, 0.0F, -1.0F);
    EditorPick hit;
    NTEST_ASSERT_TRUE(editor_pick(w, &pw.rc, lookup, &pw, nph_dvec3(0.5, 0.5, 10.0), ahead, &hit));
    NTEST_EXPECT_TRUE(nph_entity_equals(hit.entity, front));
    NTEST_EXPECT_NEAR_F64(hit.distance, 10.0, 1e-4);
    // Past the front square's edge, the bigger one behind is hit.
    NTEST_ASSERT_TRUE(editor_pick(w, &pw.rc, lookup, &pw, nph_dvec3(1.5, 0.0, 10.0), ahead, &hit));
    NTEST_EXPECT_TRUE(nph_entity_equals(hit.entity, back));
    NTEST_EXPECT_NEAR_F64(hit.distance, 15.0, 1e-4);
    // Hidden meshes are not picked.
    NphMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.hidden = 1;
    NTEST_ASSERT_EQ_INT(nph_world_set(w, front, pw.rc.mesh_renderer, &mr), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(editor_pick(w, &pw.rc, lookup, &pw, nph_dvec3(0.5, 0.5, 10.0), ahead, &hit));
    NTEST_EXPECT_TRUE(nph_entity_equals(hit.entity, back));
    // A light, by its marker.
    NTEST_ASSERT_TRUE(editor_pick(w, &pw.rc, lookup, &pw, nph_dvec3(5.1, 0.0, 10.0), ahead, &hit));
    NTEST_EXPECT_TRUE(nph_entity_equals(hit.entity, lamp));
    // Nothing at all.
    NTEST_EXPECT_FALSE(
        editor_pick(w, &pw.rc, lookup, &pw, nph_dvec3(50.0, 0.0, 10.0), ahead, &hit));
    NTEST_EXPECT_TRUE(nph_entity_is_null(hit.entity));
    // Without meshes loaded, meshes are not hit.
    NTEST_EXPECT_FALSE(editor_pick(w, &pw.rc, NULL, NULL, nph_dvec3(0.5, 0.5, 10.0), ahead, &hit));
    nph_world_destroy(w);
}
