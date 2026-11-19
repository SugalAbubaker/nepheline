// SPDX-License-Identifier: Apache-2.0
// Picking (editor/src/picking.h) on a plain world and a mesh made here: the
// nearest entity under the ray, and the material slot of the submesh it hit,
// which a material dropped there sets (ADR-0105).
#include <nepheline/core/alloc_debug.h>
#include <nepheline/runtime/components.h>
#include <nepheline/scene/transform.h>

#include <ntest/ntest.h>

#include "picking.h"

#include <string.h>

// Two squares side by side on the plane z = 0: x from -1 to 0 is submesh 0,
// drawn with slot 0, and x from 0 to 1 is submesh 1, drawn with slot 3.
typedef struct TwoSquares {
    NphMeshHeader header;
    NphMeshPosition positions[8];
    u32 indices[12];
    NphSubmesh submeshes[2];
    NphMeshView view;
} TwoSquares;

static void two_squares(TwoSquares *m)
{
    memset(m, 0, sizeof *m);
    // Quantized over x in [-1, 1] and y in [-0.5, 0.5]; z is 0 throughout.
    m->header.position_offset = nph_vec3(-1.0F, -0.5F, 0.0F);
    m->header.position_scale = nph_vec3(2.0F, 1.0F, 0.0F);
    m->header.bounds.min = nph_vec3(-1.0F, -0.5F, 0.0F);
    m->header.bounds.max = nph_vec3(1.0F, 0.5F, 0.0F);
    m->header.vertex_count = 8U;
    m->header.index_count = 12U;
    m->header.submesh_count = 2U;
    // Each square's corners: x at its edges, y at the bottom and the top.
    const u16 xs[2][2] = {{0U, 32767U}, {32768U, 65535U}};
    for (u32 s = 0; s < 2U; ++s) {
        NphMeshPosition *p = &m->positions[(u64)s * 4U];
        p[0] = (NphMeshPosition){xs[s][0], 0U, 0U, 0U};
        p[1] = (NphMeshPosition){xs[s][1], 0U, 0U, 0U};
        p[2] = (NphMeshPosition){xs[s][1], 65535U, 0U, 0U};
        p[3] = (NphMeshPosition){xs[s][0], 65535U, 0U, 0U};
        const u32 v = s * 4U;
        const u32 tri[6] = {v, v + 1U, v + 2U, v, v + 2U, v + 3U};
        memcpy(&m->indices[(u64)s * 6U], tri, sizeof tri);
        m->submeshes[s].index_offset = s * 6U;
        m->submeshes[s].index_count = 6U;
        m->submeshes[s].material_slot = s == 0U ? 0U : 3U;
    }
    m->view.header = &m->header;
    m->view.positions = m->positions;
    m->view.indices = m->indices;
    m->view.submeshes = m->submeshes;
}

static bool lookup_two_squares(void *user, NphEntity e, NphMeshView *out)
{
    NPH_UNUSED(e);
    *out = ((const TwoSquares *)user)->view;
    return true;
}

typedef struct PickFixture {
    NphDebugAllocator da;
    NphWorld *world;
    NphRuntimeComponents rc;
    TwoSquares mesh;
} PickFixture;

NTEST_SETUP(PickFixture)
{
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_debug_allocator_allocator(&f->da);
    wd.max_entities = 16U;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &f->world), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_runtime_register_components(f->world, &f->rc), NPH_RESULT_OK);
    two_squares(&f->mesh);
}

NTEST_TEARDOWN(PickFixture)
{
    nph_world_destroy(f->world);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

// An entity drawing the two squares at `at`.
static NphEntity squares_at(PickFixture *f, NphDVec3 at)
{
    const NphComponentId ids[2] = {NPH_COMPONENT_ID_WORLD_TRANSFORM, f->rc.mesh_renderer};
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(f->world, ids, 2U, &e), NPH_RESULT_OK);
    NphWorldTransform *t =
        NPH_WORLD_GET_MUT(f->world, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_ASSERT_NOT_NULL(t);
    *t = nph_world_transform_identity();
    t->translation = at;
    return e;
}

// The pick of a ray straight down -Z onto the plane, from 5 m in front of it.
static bool pick_at(PickFixture *f, f64 x, f64 y, EditorPick *out)
{
    return editor_pick(f->world, &f->rc, lookup_two_squares, &f->mesh, nph_dvec3(x, y, 5.0),
                       nph_vec3(0.0F, 0.0F, -1.0F), out);
}

NTEST_F(PickFixture, a_hit_names_the_slot_of_the_submesh_under_the_ray)
{
    const NphEntity e = squares_at(f, nph_dvec3(0.0, 0.0, 0.0));
    EditorPick hit;
    NTEST_ASSERT_TRUE(pick_at(f, 0.5, 0.0, &hit));
    NTEST_EXPECT_TRUE(nph_entity_equals(hit.entity, e));
    NTEST_EXPECT_EQ_UINT(hit.slot, 3U);
    NTEST_EXPECT_NEAR_F64(hit.distance, 5.0, 1e-4);
    NTEST_ASSERT_TRUE(pick_at(f, -0.5, 0.0, &hit));
    NTEST_EXPECT_EQ_UINT(hit.slot, 0U);
    // Off both squares: nothing.
    NTEST_EXPECT_FALSE(pick_at(f, 1.5, 0.0, &hit));
    NTEST_EXPECT_TRUE(nph_entity_is_null(hit.entity));
}

NTEST_F(PickFixture, the_slot_follows_the_entity_where_it_stands)
{
    // Ten metres along x, its right square is under x = 10.5.
    (void)squares_at(f, nph_dvec3(10.0, 0.0, 0.0));
    EditorPick hit;
    NTEST_ASSERT_TRUE(pick_at(f, 10.5, 0.0, &hit));
    NTEST_EXPECT_EQ_UINT(hit.slot, 3U);
    NTEST_ASSERT_TRUE(pick_at(f, 9.5, 0.0, &hit));
    NTEST_EXPECT_EQ_UINT(hit.slot, 0U);
    NTEST_EXPECT_FALSE(pick_at(f, 0.5, 0.0, &hit));
}

NTEST_F(PickFixture, a_mesh_without_submeshes_is_hit_as_slot_zero)
{
    f->mesh.header.submesh_count = 0U;
    f->mesh.view.submeshes = NULL;
    (void)squares_at(f, nph_dvec3(0.0, 0.0, 0.0));
    EditorPick hit;
    NTEST_ASSERT_TRUE(pick_at(f, 0.5, 0.0, &hit));
    NTEST_EXPECT_EQ_UINT(hit.slot, 0U);
}
