// SPDX-License-Identifier: Apache-2.0
// Collision assets in the physics bridge (components.h ASSET Colliders; docs/runtime.md "Physics";
// ADR-0108): an asset's hulls and meshes as shapes, used in place at a scale of one and from
// copies otherwise (shared, turned before they are scaled, mirrored with the entity); meshes left
// out of dynamic bodies; assets missing or damaged making nothing; assets loading during play
// waited for; parts taking their assets' shapes. Headless engines in lockstep: one step a frame.
#include <nepheline/asset/collision.h>
#include <nepheline/asset/format.h>
#include <nepheline/core/alloc_debug.h>
#include <nepheline/core/guid.h>
#include <nepheline/math/quat.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>
#include <nepheline/physics/query.h>
#include <nepheline/physics/shape.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>
#include <nepheline/runtime/engine.h>

#include <ntest/ntest.h>

#include <stdio.h>
#include <string.h>

#define NO_RIGID_BODY NPH_U32_MAX

typedef struct TestAsset {
    NphGuid guid;
    char path[256];
} TestAsset;

typedef struct AssetFixture {
    NphDebugAllocator da;
    NphEngineConfig config;
    NphEngine *engine;
    NphRuntimeComponents ids;
    char dir[1024]; // the content mounted at the VFS root
    TestAsset assets[8];
    u32 asset_count;
} AssetFixture;

static NphResult resolve(void *user, NphGuid guid, char *buf, u64 cap, u64 *out_len)
{
    const AssetFixture *f = (const AssetFixture *)user;
    for (u32 i = 0U; i < f->asset_count; ++i) {
        if (nph_guid_equals(f->assets[i].guid, guid)) {
            const u64 n = strlen(f->assets[i].path);
            if (n + 1U > cap) {
                return NPH_RESULT_OVERFLOW;
            }
            memcpy(buf, f->assets[i].path, (size_t)n + 1U);
            *out_len = n;
            return NPH_RESULT_OK;
        }
    }
    return NPH_RESULT_NOT_FOUND;
}

static void start(AssetFixture *f)
{
    memset(f, 0, sizeof *f);
    nph_debug_allocator_init(&f->da, nph_allocator_system());
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    static u32 counter = 0U;
    (void)snprintf(f->dir, sizeof f->dir, "%s/nph_physics_assets_%llu_%u", tmp,
                   (unsigned long long)nph_platform_process_id(), counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
    f->config.headless = 1;
    f->config.assignments[0] = "rt.lockstep=true";
    f->config.assignment_count = 1U;
    f->config.contents[0] = f->dir;
    f->config.content_count = 1U;
}

// The engine, once the fixture's assets are written.
static void create(AssetFixture *f)
{
    NphEngineDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_debug_allocator_allocator(&f->da);
    d.config = &f->config;
    d.mode = NPH_ENGINE_MODE_HEADLESS;
    d.app_name = "physics asset tests";
    d.resolve = resolve;
    d.resolve_user = f;
    NTEST_ASSERT_EQ_INT(nph_engine_create(&d, &f->engine), NPH_RESULT_OK);
    f->ids = *nph_engine_components(f->engine);
}

static void finish(AssetFixture *f)
{
    nph_engine_destroy(f->engine);
    NTEST_EXPECT_EQ_INT(nph_fs_remove_tree(f->dir), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(nph_debug_allocator_destroy(&f->da, NULL, NULL), 0U);
}

static void frames(AssetFixture *f, u32 n)
{
    for (u32 i = 0; i < n; ++i) {
        NTEST_ASSERT_EQ_INT(nph_engine_frame(f->engine), NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(nph_engine_wait(f->engine), NPH_RESULT_OK);
}

typedef struct TestBlock {
    u32 kind; // NphCollisionShapeKind
    const void *data;
    u32 size;
} TestBlock;

// A collision asset `name` (its GUID's text and its file's) of `blocks`, in the fixture's content.
static NphGuid write_collision(AssetFixture *f, const char *name, const TestBlock *blocks, u32 n)
{
    const NphGuid guid = nph_guid_from_cstr(name);
    NphAssetWriter w;
    nph_asset_writer_init(&w, nph_allocator_system(), NPH_ASSET_TYPE_COLLISION,
                          NPH_COLLISION_TYPE_VERSION, guid, 1U);
    NphCollisionHeader h;
    memset(&h, 0, sizeof h);
    h.shape_count = n;
    h.bounds = nph_aabb(nph_vec3_splat(-10.0F), nph_vec3_splat(10.0F));
    NphCollisionShape shapes[8];
    memset(shapes, 0, sizeof shapes);
    NTEST_ASSERT_LE_UINT(n, 8U);
    u32 next[2] = {0U, 0U};
    for (u32 i = 0U; i < n; ++i) {
        const u32 k = blocks[i].kind == (u32)NPH_COLLISION_SHAPE_HULL ? 0U : 1U;
        shapes[i].kind = blocks[i].kind;
        shapes[i].block = next[k]++;
    }
    NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_COLLISION_TAG_HEADER, 0U, &h, sizeof h,
                                                  NPH_ASSET_CODEC_NONE, 0U),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, NPH_COLLISION_TAG_SHAPES, 0U, shapes,
                                                  (u64)n * sizeof shapes[0], NPH_ASSET_CODEC_NONE,
                                                  0U),
                        NPH_RESULT_OK);
    for (u32 i = 0U; i < n; ++i) {
        const u32 tag = blocks[i].kind == (u32)NPH_COLLISION_SHAPE_HULL ? NPH_COLLISION_TAG_HULL
                                                                        : NPH_COLLISION_TAG_MESH;
        NTEST_ASSERT_EQ_INT(nph_asset_writer_add_blob(&w, tag, shapes[i].block, blocks[i].data,
                                                      blocks[i].size, NPH_ASSET_CODEC_NONE, 16U),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_LT_UINT(f->asset_count, NPH_ARRAY_COUNT(f->assets));
    TestAsset *t = &f->assets[f->asset_count++];
    t->guid = guid;
    (void)snprintf(t->path, sizeof t->path, "%s.nph", name);
    char full[1300];
    (void)snprintf(full, sizeof full, "%s/%s", f->dir, t->path);
    NTEST_ASSERT_EQ_INT(nph_asset_writer_write_file(&w, full), NPH_RESULT_OK);
    nph_asset_writer_free(&w);
    return guid;
}

// A box hull of half extents `half` about the origin, as a block (free with free_hull).
static NphPhysicsHull *box_hull(NphVec3 half)
{
    NphPhysicsHull *h = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_hull_make_box(half, nph_allocator_system(), &h), NPH_RESULT_OK);
    return h;
}

// A one-sided quad facing up at y = 0 over x in [x0, x1], z in [-1, 1].
static NphPhysicsMesh *quad_mesh(f32 x0, f32 x1)
{
    const NphVec3 v[4] = {nph_vec3(x0, 0.0F, -1.0F), nph_vec3(x1, 0.0F, -1.0F),
                          nph_vec3(x1, 0.0F, 1.0F), nph_vec3(x0, 0.0F, 1.0F)};
    // Counter-clockwise seen from above: the normal up.
    static const u32 idx[6] = {0U, 3U, 2U, 0U, 2U, 1U};
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = v;
    d.vertex_count = 4U;
    d.indices = idx;
    d.triangle_count = 2U;
    NphPhysicsMesh *m = NULL;
    NTEST_ASSERT_EQ_INT(nph_physics_mesh_build(&d, nph_allocator_system(), &m), NPH_RESULT_OK);
    return m;
}

static TestBlock hull_block(const NphPhysicsHull *h)
{
    const TestBlock t = {(u32)NPH_COLLISION_SHAPE_HULL, h, h->byte_size};
    return t;
}

static TestBlock mesh_block(const NphPhysicsMesh *m)
{
    const TestBlock t = {(u32)NPH_COLLISION_SHAPE_MESH, m, m->byte_size};
    return t;
}

// An entity at `at` scaled by `scale`, with a RigidBody of `type` (NO_RIGID_BODY: none) and a
// Collider of the collision asset `collision`, in the active world.
static NphEntity spawn_asset(AssetFixture *f, NphDVec3 at, NphVec3 scale, u32 type,
                             NphGuid collision)
{
    NphWorld *w = nph_engine_world(f->engine);
    NphComponentId comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM, NPH_COMPONENT_ID_WORLD_TRANSFORM,
                               f->ids.collider};
    u32 n = 3U;
    if (type != NO_RIGID_BODY) {
        comps[n++] = f->ids.rigid_body;
    }
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, n, &e), NPH_RESULT_OK);
    NphLocalTransform *lt =
        NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    lt->translation = at;
    lt->scale = scale;
    if (type != NO_RIGID_BODY) {
        NPH_WORLD_GET_MUT(w, e, f->ids.rigid_body, NphRigidBody)->type = type;
    }
    NphCollider *c = NPH_WORLD_GET_MUT(w, e, f->ids.collider, NphCollider);
    c->shape = (u32)NPH_COLLIDER_ASSET;
    c->collision = collision;
    return e;
}

static NphEntity spawn_ball(AssetFixture *f, NphDVec3 at)
{
    NphWorld *w = nph_engine_world(f->engine);
    const NphComponentId comps[4] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, f->ids.collider,
                                     f->ids.rigid_body};
    NphEntity e = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 4U, &e), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation = at;
    NphCollider *c = NPH_WORLD_GET_MUT(w, e, f->ids.collider, NphCollider);
    c->shape = (u32)NPH_COLLIDER_SPHERE;
    c->radius = 0.25F;
    NPH_WORLD_GET_MUT(w, e, f->ids.rigid_body, NphRigidBody)->type = NPH_RIGID_BODY_DYNAMIC;
    return e;
}

static u32 shapes_of(AssetFixture *f, NphEntity e, NphPhysicsShape *out, u32 cap)
{
    memset(out, 0, (size_t)cap * sizeof *out);
    const NphPhysicsBody body = nph_engine_physics_body(f->engine, e);
    if (nph_physics_body_is_null(body)) {
        return 0U;
    }
    return nph_physics_body_shapes(nph_engine_physics(f->engine), body, out, cap);
}

static NphPhysicsGeometry geometry_of(AssetFixture *f, NphPhysicsShape s)
{
    return nph_physics_shape_geometry(nph_engine_physics(f->engine), s);
}

// The asset's block of shape `index`, as the engine's loader holds it.
static const void *asset_block(AssetFixture *f, NphGuid guid, u32 index)
{
    NphAssetLoader *loader = nph_engine_loader(f->engine);
    NphAssetHandle h;
    NTEST_ASSERT_EQ_INT(nph_asset_load_guid(loader, guid, &h), NPH_RESULT_OK);
    NphCollisionView v;
    NTEST_ASSERT_EQ_INT(nph_collision_view(nph_asset_get(loader, h), &v), NPH_RESULT_OK);
    const void *data = nph_collision_shape_block(nph_asset_get(loader, h), &v, index).data;
    nph_asset_release(loader, h); // the bridge holds it too
    return data;
}

// A level's piece: a box hull and a floor quad beside it. Used in place, both; a ball dropped
// onto the quad rests on it.
NTEST(PhysicsAssets, an_asset_makes_its_shapes_in_place)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsHull *hull = box_hull(nph_vec3(0.5F, 0.5F, 0.5F));
    NphPhysicsMesh *quad = quad_mesh(1.0F, 3.0F);
    const TestBlock blocks[2] = {hull_block(hull), mesh_block(quad)};
    const NphGuid level = write_collision(&fx, "level.coll", blocks, 2U);
    nph_physics_hull_free(hull, nph_allocator_system());
    nph_physics_mesh_free(quad, nph_allocator_system());
    create(&fx);
    const NphEntity piece =
        spawn_asset(&fx, nph_dvec3_zero(), nph_vec3_one(), NO_RIGID_BODY, level);
    const NphEntity ball = spawn_ball(&fx, nph_dvec3(2.0, 1.0, 0.0));
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NphPhysicsShape s[4];
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, piece, s, 4U), 2U);
    const void *hull_in_asset = asset_block(&fx, level, 0U);
    const void *mesh_in_asset = asset_block(&fx, level, 1U);
    u32 hulls = 0U;
    u32 meshes = 0U;
    for (u32 i = 0U; i < 2U; ++i) {
        const NphPhysicsGeometry g = geometry_of(&fx, s[i]);
        if (g.type == NPH_PHYSICS_SHAPE_HULL) {
            NTEST_EXPECT_EQ_PTR(g.hull, hull_in_asset);
            hulls += 1U;
        } else {
            NTEST_ASSERT_EQ_INT((int)g.type, (int)NPH_PHYSICS_SHAPE_MESH);
            NTEST_EXPECT_EQ_PTR(g.mesh, mesh_in_asset);
            meshes += 1U;
        }
    }
    NTEST_EXPECT_EQ_UINT(hulls, 1U);
    NTEST_EXPECT_EQ_UINT(meshes, 1U);
    frames(&fx, 240U);
    const NphLocalTransform *lt = NPH_WORLD_GET(
        nph_engine_world(fx.engine), ball, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_EXPECT_NEAR_F64(lt->translation.y, 0.25, 0.02);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// A dynamic body takes its asset's hulls and leaves its meshes out (logged): its mass the hull's.
NTEST(PhysicsAssets, dynamic_bodies_take_hulls_not_meshes)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsHull *hull = box_hull(nph_vec3(0.5F, 0.5F, 0.5F));
    NphPhysicsMesh *quad = quad_mesh(1.0F, 3.0F);
    const TestBlock blocks[2] = {hull_block(hull), mesh_block(quad)};
    const NphGuid level = write_collision(&fx, "level.coll", blocks, 2U);
    nph_physics_hull_free(hull, nph_allocator_system());
    nph_physics_mesh_free(quad, nph_allocator_system());
    create(&fx);
    const NphEntity crate =
        spawn_asset(&fx, nph_dvec3(0.0, 5.0, 0.0), nph_vec3_one(), NPH_RIGID_BODY_DYNAMIC, level);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NphPhysicsShape s[4];
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, crate, s, 4U), 1U);
    NTEST_EXPECT_EQ_INT((int)geometry_of(&fx, s[0]).type, (int)NPH_PHYSICS_SHAPE_HULL);
    const NphPhysicsMassData m = nph_physics_body_mass_data(
        nph_engine_physics(fx.engine), nph_engine_physics_body(fx.engine, crate));
    NTEST_EXPECT_NEAR_F32(m.mass, 1000.0F, 0.1F); // a cubic metre of water
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// Colliders of one asset at another scale share one copy of its block, turned by the Collider
// before the scale (a slab turned a quarter about Y, then stretched along X); at a scale of one
// the asset's own block; a copy outlives the first of its users to go.
NTEST(PhysicsAssets, scaled_colliders_share_their_copies)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsHull *slab = box_hull(nph_vec3(1.0F, 0.5F, 0.25F));
    const TestBlock blocks[1] = {hull_block(slab)};
    const NphGuid guid = write_collision(&fx, "slab.coll", blocks, 1U);
    nph_physics_hull_free(slab, nph_allocator_system());
    create(&fx);
    NphWorld *w = nph_engine_world(fx.engine);
    const NphVec3 stretch = nph_vec3(2.0F, 1.0F, 1.0F);
    const NphEntity a = spawn_asset(&fx, nph_dvec3(-5.0, 0.0, 0.0), stretch, NO_RIGID_BODY, guid);
    const NphEntity b = spawn_asset(&fx, nph_dvec3(5.0, 0.0, 0.0), stretch, NO_RIGID_BODY, guid);
    const NphEntity plain =
        spawn_asset(&fx, nph_dvec3(0.0, 0.0, 9.0), nph_vec3_one(), NO_RIGID_BODY, guid);
    const NphQuat quarter = nph_quat_from_axis_angle(nph_vec3_unit_y(), 1.5707963F);
    NPH_WORLD_GET_MUT(w, a, fx.ids.collider, NphCollider)->rotation = quarter;
    NPH_WORLD_GET_MUT(w, b, fx.ids.collider, NphCollider)->rotation = quarter;
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NphPhysicsShape sa[2];
    NphPhysicsShape sb[2];
    NphPhysicsShape sp[2];
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, a, sa, 2U), 1U);
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, b, sb, 2U), 1U);
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, plain, sp, 2U), 1U);
    const NphPhysicsHull *ha = geometry_of(&fx, sa[0]).hull;
    const NphPhysicsHull *hb = geometry_of(&fx, sb[0]).hull;
    const NphPhysicsHull *hp = geometry_of(&fx, sp[0]).hull;
    NTEST_EXPECT_EQ_PTR(ha, hb);
    NTEST_EXPECT_EQ_PTR(hp, asset_block(&fx, guid, 0U));
    NTEST_EXPECT_NE_PTR(ha, hp);
    // Turned, (0.25, 0.5, 1.0); then stretched along X, (0.5, 0.5, 1.0).
    NTEST_EXPECT_NEAR_F32(ha->bounds.max.x, 0.5F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(ha->bounds.max.y, 0.5F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(ha->bounds.max.z, 1.0F, 1e-4F);
    NTEST_EXPECT_TRUE(nph_quat_is_normalized(
        nph_physics_shape_pose(nph_engine_physics(fx.engine), sa[0]).rotation, 1e-5F));
    // The first user gone, the copy holds for the second.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(w, a), NPH_RESULT_OK);
    frames(&fx, 2U);
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, b, sb, 2U), 1U);
    NTEST_EXPECT_NEAR_F32(geometry_of(&fx, sb[0]).hull->bounds.max.z, 1.0F, 1e-4F);
    nph_engine_play_end(fx.engine);
    finish(&fx); // every copy freed: the debug allocator counts
}

static bool ray_down(AssetFixture *f, f64 x)
{
    NphPhysicsQueryHit hit;
    return nph_physics_world_raycast_closest(nph_engine_physics(f->engine), nph_dvec3(x, 5.0, 0.0),
                                             nph_vec3(0.0F, -10.0F, 0.0F), NULL, &hit);
}

// An entity mirrored along X mirrors its mesh: the floor quad over [1, 3] lies over [-3, -1], and
// still faces up (wound again), so a ray from above meets it there and nowhere else.
NTEST(PhysicsAssets, mirrored_entities_mirror_their_meshes)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsMesh *quad = quad_mesh(1.0F, 3.0F);
    const TestBlock blocks[1] = {mesh_block(quad)};
    const NphGuid guid = write_collision(&fx, "floor.coll", blocks, 1U);
    nph_physics_mesh_free(quad, nph_allocator_system());
    create(&fx);
    (void)spawn_asset(&fx, nph_dvec3_zero(), nph_vec3(-1.0F, 1.0F, 1.0F), NO_RIGID_BODY, guid);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(ray_down(&fx, -2.0));
    NTEST_EXPECT_FALSE(ray_down(&fx, 2.0));
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// A Collider given another asset during play takes that asset's shapes; given a sphere, the sphere.
NTEST(PhysicsAssets, a_new_asset_or_shape_makes_the_shapes_again)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsHull *hull = box_hull(nph_vec3(0.5F, 0.5F, 0.5F));
    NphPhysicsMesh *quad = quad_mesh(1.0F, 3.0F);
    const TestBlock one[1] = {hull_block(hull)};
    const TestBlock two[2] = {hull_block(hull), mesh_block(quad)};
    const NphGuid small = write_collision(&fx, "small.coll", one, 1U);
    const NphGuid large = write_collision(&fx, "large.coll", two, 2U);
    nph_physics_hull_free(hull, nph_allocator_system());
    nph_physics_mesh_free(quad, nph_allocator_system());
    create(&fx);
    const NphEntity e = spawn_asset(&fx, nph_dvec3_zero(), nph_vec3_one(), NO_RIGID_BODY, small);
    // Both assets loaded at play begin: the second already in hand when it is named.
    const NphEntity other =
        spawn_asset(&fx, nph_dvec3(9.0, 0.0, 0.0), nph_vec3_one(), NO_RIGID_BODY, large);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NphPhysicsShape s[4];
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, e, s, 4U), 1U);
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, other, s, 4U), 2U);
    NphWorld *w = nph_engine_world(fx.engine);
    NPH_WORLD_GET_MUT(w, e, fx.ids.collider, NphCollider)->collision = large;
    frames(&fx, 1U);
    NTEST_EXPECT_EQ_UINT(shapes_of(&fx, e, s, 4U), 2U);
    NphCollider *c = NPH_WORLD_GET_MUT(w, e, fx.ids.collider, NphCollider);
    c->shape = (u32)NPH_COLLIDER_SPHERE;
    c->radius = 0.5F;
    frames(&fx, 1U);
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, e, s, 4U), 1U);
    NTEST_EXPECT_EQ_INT((int)geometry_of(&fx, s[0]).type, (int)NPH_PHYSICS_SHAPE_SPHERE);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// A Collider whose asset is missing, or whose block the physics module refuses, makes no shapes
// (logged): its body stands empty.
NTEST(PhysicsAssets, missing_and_damaged_assets_make_nothing)
{
    static AssetFixture fx;
    start(&fx);
    static u8 junk[256];
    memset(junk, 0xA5, sizeof junk);
    const TestBlock blocks[1] = {{(u32)NPH_COLLISION_SHAPE_HULL, junk, (u32)sizeof junk}};
    const NphGuid broken = write_collision(&fx, "broken.coll", blocks, 1U);
    create(&fx);
    const NphEntity a = spawn_asset(&fx, nph_dvec3_zero(), nph_vec3_one(), NO_RIGID_BODY,
                                    nph_guid_from_cstr("nowhere.coll"));
    const NphEntity b =
        spawn_asset(&fx, nph_dvec3(3.0, 0.0, 0.0), nph_vec3_one(), NO_RIGID_BODY, broken);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NphPhysicsShape s[2];
    NTEST_EXPECT_EQ_UINT(shapes_of(&fx, a, s, 2U), 0U);
    NTEST_EXPECT_EQ_UINT(shapes_of(&fx, b, s, 2U), 0U);
    frames(&fx, 4U);
    NTEST_EXPECT_EQ_UINT(shapes_of(&fx, b, s, 2U), 0U);
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// A crate whose asset nothing loaded before is spawned during play: it waits asleep where it is
// for its asset, which loads in the background, then falls.
NTEST(PhysicsAssets, assets_loading_in_play_are_waited_for)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsHull *hull = box_hull(nph_vec3(0.5F, 0.5F, 0.5F));
    const TestBlock blocks[1] = {hull_block(hull)};
    const NphGuid crate_coll = write_collision(&fx, "crate.coll", blocks, 1U);
    nph_physics_hull_free(hull, nph_allocator_system());
    create(&fx);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    const NphEntity crate = spawn_asset(&fx, nph_dvec3(0.0, 3.0, 0.0), nph_vec3_one(),
                                        NPH_RIGID_BODY_DYNAMIC, crate_coll);
    NphPhysicsShape s[2];
    u32 waited = 0U;
    bool still = true;
    for (; waited < 600U && shapes_of(&fx, crate, s, 2U) == 0U; ++waited) {
        frames(&fx, 1U);
        const NphLocalTransform *lt =
            NPH_WORLD_GET(nph_engine_world(fx.engine), crate, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                          NphLocalTransform);
        still = still && (shapes_of(&fx, crate, s, 2U) != 0U || lt->translation.y == 3.0);
    }
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, crate, s, 2U), 1U);
    NTEST_EXPECT_TRUE(still); // asleep while it waited
    NTEST_LOG("the crate waited %u frames for its asset", waited);
    frames(&fx, 120U); // half a second: 1.2 m of fall
    const NphLocalTransform *lt = NPH_WORLD_GET(
        nph_engine_world(fx.engine), crate, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_EXPECT_TRUE(lt->translation.y < 2.5); // woken with its shapes, falling
    nph_engine_play_end(fx.engine);
    finish(&fx);
}

// A part of a body (ADR-0108: a Collider under a RigidBody) takes its asset's shapes where its
// entity is in the body.
NTEST(PhysicsAssets, parts_take_their_assets_shapes)
{
    static AssetFixture fx;
    start(&fx);
    NphPhysicsHull *slab = box_hull(nph_vec3(1.0F, 0.5F, 0.25F));
    const TestBlock blocks[1] = {hull_block(slab)};
    const NphGuid guid = write_collision(&fx, "slab.coll", blocks, 1U);
    nph_physics_hull_free(slab, nph_allocator_system());
    create(&fx);
    NphWorld *w = nph_engine_world(fx.engine);
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, fx.ids.rigid_body};
    NphEntity body = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(w, comps, 3U, &body), NPH_RESULT_OK);
    NPH_WORLD_GET_MUT(w, body, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform)->translation =
        nph_dvec3(0.0, 5.0, 0.0);
    NphRigidBody *rb = NPH_WORLD_GET_MUT(w, body, fx.ids.rigid_body, NphRigidBody);
    rb->type = NPH_RIGID_BODY_DYNAMIC;
    rb->gravity_scale = 0.0F;
    const NphEntity piece =
        spawn_asset(&fx, nph_dvec3(1.0, 0.0, 0.0), nph_vec3_one(), NO_RIGID_BODY, guid);
    NTEST_ASSERT_EQ_INT(nph_world_set_parent(w, piece, body), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_engine_play_begin(fx.engine, false), NPH_RESULT_OK);
    NphPhysicsShape s[2];
    NTEST_ASSERT_EQ_UINT(shapes_of(&fx, body, s, 2U), 1U);
    NTEST_EXPECT_EQ_UINT(nph_engine_physics_body(fx.engine, piece).value,
                         nph_engine_physics_body(fx.engine, body).value);
    const NphPhysicsPose pose = nph_physics_shape_pose(nph_engine_physics(fx.engine), s[0]);
    NTEST_EXPECT_NEAR_F32(pose.position.x, 1.0F, 1e-6F);
    NTEST_EXPECT_NEAR_F32(nph_physics_body_mass_data(nph_engine_physics(fx.engine),
                                                     nph_engine_physics_body(fx.engine, body))
                              .mass,
                          1000.0F, 0.1F); // 2 x 1 x 0.5 m of water
    nph_engine_play_end(fx.engine);
    finish(&fx);
}
