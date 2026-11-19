// SPDX-License-Identifier: Apache-2.0
// Collision cooked from glTF (cook/collision.h; ADR-0107): proxies found by name and taken out of
// the scene, nodes' extras over the scene's settings, flat hulls made meshes, the assets shared
// by the nodes that draw a mesh the same way, and every block the physics module's validators
// accept.
#include <nepheline/asset/collision.h>
#include <nepheline/asset/scene.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>

#include "test_cook_support.h"

#include <cook/collision.h>
#include <cook/cooker.h>

NTEST(collision, proxies_are_named_by_unreals_prefixes)
{
    const char *rest = NULL;
    NTEST_ASSERT_TRUE(cook_collision_proxy_name("UCX_Crate_01", &rest));
    NTEST_EXPECT_EQ_STR(rest, "Crate_01");
    NTEST_EXPECT_TRUE(cook_collision_proxy_name("UBX_Wall", &rest));
    NTEST_EXPECT_TRUE(cook_collision_proxy_name("USP_Ball", &rest));
    NTEST_EXPECT_TRUE(cook_collision_proxy_name("UCP_Pole", &rest));
    NTEST_EXPECT_FALSE(cook_collision_proxy_name("ucx_Crate", &rest));
    NTEST_EXPECT_FALSE(cook_collision_proxy_name("UCX_", &rest));
    NTEST_EXPECT_FALSE(cook_collision_proxy_name("Crate", &rest));
    NTEST_EXPECT_FALSE(cook_collision_proxy_name("UCX", &rest));
}

NTEST(collision, a_hulls_budget_holds_and_a_flat_cloud_is_a_mesh)
{
    NphAllocator *alloc = nph_allocator_system();
    // A box's corners with a budget of four: the hull of its four most telling points.
    NphVec3 box[8];
    for (u32 i = 0; i < 8U; ++i) {
        box[i] = nph_vec3((i & 1U) != 0U ? 1.0F : -1.0F, (i & 2U) != 0U ? 1.0F : -1.0F,
                          (i & 4U) != 0U ? 1.0F : -1.0F);
    }
    static const u32 box_tris[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                     2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    CookCollisionShapes s;
    cook_collision_shapes_init(&s, alloc);
    bool flat = true;
    NTEST_ASSERT_EQ_INT(cook_collision_add_hull(&s, box, 8U, box_tris, 36U, 4U, &flat),
                        NPH_RESULT_OK);
    NTEST_EXPECT_FALSE(flat);
    NTEST_ASSERT_EQ_UINT(s.blocks.len, 1U);
    const NphPhysicsHull *hull = (const NphPhysicsHull *)s.blocks.data[0].block;
    NTEST_EXPECT_EQ_UINT(hull->vertex_count, 4U);
    NTEST_EXPECT_TRUE(nph_physics_hull_validate(hull, s.blocks.data[0].size, 0.0F));
    // A plane asked for as a hull: its triangles, from both sides.
    static const NphVec3 plane[4] = {
        {-1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 1.0F}, {-1.0F, 0.0F, 1.0F}};
    static const u32 plane_tris[6] = {0, 2, 1, 0, 3, 2};
    NTEST_ASSERT_EQ_INT(cook_collision_add_hull(&s, plane, 4U, plane_tris, 6U, 32U, &flat),
                        NPH_RESULT_OK);
    NTEST_EXPECT_TRUE(flat);
    NTEST_ASSERT_EQ_UINT(s.blocks.len, 2U);
    NTEST_EXPECT_EQ_INT(s.blocks.data[1].kind, NPH_COLLISION_SHAPE_MESH);
    const NphPhysicsMesh *mesh = (const NphPhysicsMesh *)s.blocks.data[1].block;
    NTEST_EXPECT_TRUE((mesh->flags & NPH_PHYSICS_MESH_TWO_SIDED) != 0U);
    NTEST_EXPECT_TRUE(nph_physics_mesh_validate(mesh, s.blocks.data[1].size));
    // Nothing to make: no triangle with an area, or a point beyond the physics range.
    static const NphVec3 line[3] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}};
    static const u32 line_tris[3] = {0, 1, 2};
    NTEST_EXPECT_EQ_INT(cook_collision_add_hull(&s, line, 3U, line_tris, 3U, 32U, &flat),
                        NPH_RESULT_INVALID_ARGUMENT);
    NphVec3 far[8];
    memcpy(far, box, sizeof far);
    far[3].x = 2.0e5F;
    NTEST_EXPECT_EQ_INT(cook_collision_add_hull(&s, far, 8U, box_tris, 36U, 32U, &flat),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_INT(cook_collision_add_mesh(&s, far, 8U, box_tris, 36U, false),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(s.blocks.len, 2U);
    // Written, the view finds each block in its kind's order.
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_COLLISION, NPH_COLLISION_TYPE_VERSION,
                          nph_guid_from_cstr("shapes"), 1U);
    NTEST_ASSERT_EQ_INT(cook_collision_write(&s, NPH_LZ_LEVEL_FAST, &w), NPH_RESULT_OK);
    TestAsset a;
    test_asset_from_writer(alloc, &w, &a);
    NphCollisionView v;
    NTEST_ASSERT_EQ_INT(nph_collision_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->shape_count, 2U);
    NTEST_EXPECT_EQ_UINT(v.shapes[0].kind, (u32)NPH_COLLISION_SHAPE_HULL);
    NTEST_EXPECT_EQ_UINT(v.shapes[1].kind, (u32)NPH_COLLISION_SHAPE_MESH);
    NTEST_EXPECT_NEAR_F32(v.header->bounds.min.x, -1.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(v.header->bounds.max.y, 1.0F, 0.0F);
    const NphCollisionBlock b = nph_collision_shape_block(&a.asset, &v, 1U);
    NTEST_EXPECT_TRUE(nph_physics_mesh_validate(b.data, b.size));
    test_asset_free(&a);
    nph_asset_writer_free(&w);
    cook_collision_shapes_free(&s);
}

// ---------------------------------------------------------------------------
// A scene's collision, cooked
// ---------------------------------------------------------------------------

typedef struct CollisionCook {
    NphAllocator *alloc;
    TempDir dir;
    char out_dir[1200];
    char path[1200];
    Cooker *c;
} CollisionCook;

// A box (8 corners at +-1, 12 triangles facing out) and a plane (y = 0, +-1, facing up), and
// the scene below: a crate with three proxies (a child, a sibling with a Blender duplicate's
// suffix, one with a child of its own) drawn by a mesh nothing else draws, a floor and a sign
// asked by their extras for triangles and a hull, a barrel for a hull, a banner for none, a
// proxy that finds no node, and an extra that names no kind.
static void write_props(CollisionCook *f)
{
    f32 pos[12 * 3];
    for (u32 i = 0; i < 8U; ++i) {
        const u64 at = (u64)i * 3U;
        pos[at] = (i & 1U) != 0U ? 1.0F : -1.0F;
        pos[at + 1U] = (i & 2U) != 0U ? 1.0F : -1.0F;
        pos[at + 2U] = (i & 4U) != 0U ? 1.0F : -1.0F;
    }
    static const f32 plane[12] = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
    memcpy(&pos[24], plane, sizeof plane);
    static const u16 idx[42] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3,
                                3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5, 0, 2, 1, 0, 3, 2};
    u8 buffer[sizeof pos + sizeof idx];
    memcpy(buffer, pos, sizeof pos);
    memcpy(buffer + sizeof pos, idx, sizeof idx);
    char uri[1024];
    strcpy(uri, "data:application/octet-stream;base64,");
    test_base64(buffer, sizeof buffer, uri + strlen(uri));
    NphStrBuilder sb;
    nph_sb_init(&sb, f->alloc);
    NTEST_ASSERT_EQ_INT(
        nph_sb_appendf(
            &sb,
            "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\n"
            "\"scenes\":[{\"nodes\":[0,2,3,4,5,6,7,8,9]}],\n"
            "\"nodes\":[\n"
            " {\"name\":\"Crate\",\"mesh\":0,\"translation\":[5,0,0],\"children\":[1]},\n"
            " {\"name\":\"UCX_Crate_01\",\"mesh\":2,\"translation\":[0,0.5,0],"
            "\"scale\":[0.5,0.5,0.5]},\n"
            " {\"name\":\"UCX_Crate_02.001\",\"mesh\":2,\"translation\":[5,-0.5,0],"
            "\"scale\":[0.5,0.5,0.5]},\n"
            " {\"name\":\"Floor\",\"mesh\":1,\"scale\":[10,1,10],"
            "\"extras\":{\"collision\":\"mesh\"}},\n"
            " {\"name\":\"Sign\",\"mesh\":1,\"translation\":[0,2,0],"
            "\"extras\":{\"collision\":\"hull\"}},\n"
            " {\"name\":\"Barrel\",\"mesh\":0,\"translation\":[-5,0,0],"
            "\"extras\":{\"collision\":\"hull\",\"note\":\"kept\"}},\n"
            " {\"name\":\"Banner\",\"mesh\":1,\"extras\":{\"collision\":\"none\"}},\n"
            " {\"name\":\"UCX_Nothing\",\"mesh\":0,\"translation\":[0,0,9]},\n"
            " {\"name\":\"Odd\",\"mesh\":1,\"translation\":[0,0,-9],\"extras\":{\"collision\":3}},"
            "\n"
            " {\"name\":\"UCX_Crate_03\",\"mesh\":2,\"translation\":[5,0,1],"
            "\"scale\":[0.2,0.2,0.2],\"children\":[10]},\n"
            " {\"name\":\"Handle\",\"mesh\":0,\"scale\":[0.5,0.5,0.5]}\n"
            "],\n"
            "\"meshes\":[\n"
            " {\"name\":\"Box\",\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":2}]},"
            "\n"
            " {\"name\":\"Plane\",\"primitives\":[{\"attributes\":{\"POSITION\":1},\"indices\":3}]}"
            ","
            "\n"
            " {\"name\":\"ProxyBox\",\"primitives\":[{\"attributes\":{\"POSITION\":0},"
            "\"indices\":2}]}\n"
            "],\n"
            "\"accessors\":[\n"
            " {\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\","
            "\"min\":[-1,-1,-1],\"max\":[1,1,1]},\n"
            " {\"bufferView\":0,\"byteOffset\":96,\"componentType\":5126,\"count\":4,"
            "\"type\":\"VEC3\",\"min\":[-1,0,-1],\"max\":[1,0,1]},\n"
            " {\"bufferView\":1,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"},\n"
            " {\"bufferView\":1,\"byteOffset\":72,\"componentType\":5123,\"count\":6,"
            "\"type\":\"SCALAR\"}\n"
            "],\n"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":144},"
            "{\"buffer\":0,\"byteOffset\":144,\"byteLength\":84}],\n"
            "\"buffers\":[{\"byteLength\":%u,\"uri\":\"%s\"}]}\n",
            (u32)sizeof buffer, uri),
        NPH_RESULT_OK);
    test_path_in(&f->dir, "props.gltf", f->path, sizeof f->path);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(f->path, sb.data, sb.len), NPH_RESULT_OK);
    nph_sb_free(&sb);
}

NTEST_SETUP(CollisionCook)
{
    f->alloc = nph_allocator_system();
    test_temp_dir_make(&f->dir, "collision");
    test_path_in(&f->dir, "out", f->out_dir, sizeof f->out_dir);
    write_props(f);
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = f->alloc;
    desc.out_dir = f->out_dir;
    desc.source_root = f->dir.dir;
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    desc.texture.lz = NPH_LZ_LEVEL_FAST;
    desc.mesh.lz = NPH_LZ_LEVEL_FAST;
    f->c = NULL;
    NTEST_ASSERT_EQ_INT(cooker_create(&desc, &f->c), NPH_RESULT_OK);
}

NTEST_TEARDOWN(CollisionCook)
{
    cooker_destroy(f->c);
    test_temp_dir_remove(&f->dir);
}

static void load_output(CollisionCook *f, const CookDbEntry *e, TestAsset *out)
{
    NTEST_ASSERT_NOT_NULL(e);
    char cooked[1500];
    (void)snprintf(cooked, sizeof cooked, "%s/%s", f->out_dir, e->output);
    test_asset_from_file(f->alloc, cooked, out);
}

// What a node's record names: its shapes' count, each kind in turn as letters ('h', 'm'), and
// every block through the physics module's validator.
static void expect_shapes(CollisionCook *f, NphGuid collision, const char *kinds, NphAABB *bounds)
{
    TestAsset a;
    load_output(f, cook_db_find_guid(cooker_db(f->c), collision), &a);
    NphCollisionView v;
    NTEST_ASSERT_EQ_INT(nph_collision_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(v.header->shape_count, (u32)strlen(kinds));
    for (u32 i = 0; i < v.header->shape_count; ++i) {
        const NphCollisionBlock b = nph_collision_shape_block(&a.asset, &v, i);
        if (kinds[i] == 'h') {
            NTEST_EXPECT_EQ_UINT(v.shapes[i].kind, (u32)NPH_COLLISION_SHAPE_HULL);
            NTEST_EXPECT_TRUE(nph_physics_hull_validate(b.data, b.size, 0.0F));
        } else {
            NTEST_EXPECT_EQ_UINT(v.shapes[i].kind, (u32)NPH_COLLISION_SHAPE_MESH);
            NTEST_EXPECT_TRUE(nph_physics_mesh_validate(b.data, b.size));
        }
        if (bounds != NULL) {
            const NphAABB *box = kinds[i] == 'h' ? &((const NphPhysicsHull *)b.data)->bounds
                                                 : &((const NphPhysicsMesh *)b.data)->bounds;
            bounds[i] = *box;
        }
    }
    test_asset_free(&a);
}

static void expect_box(NphAABB b, NphVec3 lo, NphVec3 hi)
{
    NTEST_EXPECT_NEAR_F32(b.min.x, lo.x, 1e-5F);
    NTEST_EXPECT_NEAR_F32(b.min.y, lo.y, 1e-5F);
    NTEST_EXPECT_NEAR_F32(b.min.z, lo.z, 1e-5F);
    NTEST_EXPECT_NEAR_F32(b.max.x, hi.x, 1e-5F);
    NTEST_EXPECT_NEAR_F32(b.max.y, hi.y, 1e-5F);
    NTEST_EXPECT_NEAR_F32(b.max.z, hi.z, 1e-5F);
}

// The cooked scene's node named `name`, NPH_SCENE_NO_NODE when none is.
static u32 node_named(const NphSceneView *v, const char *name)
{
    for (u32 i = 0; i < v->header->node_count; ++i) {
        if (nph_str_equals(nph_scene_node_name(v, i), nph_str_from_cstr(name))) {
            return i;
        }
    }
    return NPH_SCENE_NO_NODE;
}

NTEST_F(CollisionCook, proxies_and_extras_make_the_nodes_collision)
{
    NTEST_ASSERT_EQ_INT(cooker_cook_file(f->c, f->path), NPH_RESULT_OK);
    const CookDb *db = cooker_db(f->c);
    // The mesh only proxies drew is not cooked; the others are.
    NTEST_EXPECT_NULL(cook_db_find(db, NPH_STR("props.gltf"), NPH_STR("mesh/2")));
    NTEST_EXPECT_NOT_NULL(cook_db_find(db, NPH_STR("props.gltf"), NPH_STR("mesh/0")));
    TestAsset a;
    load_output(f, cook_db_find(db, NPH_STR("props.gltf"), NPH_STR("scene")), &a);
    NphSceneView v;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&a.asset, &v), NPH_RESULT_OK);
    // Two proxies left the scene; the one with a child stayed, drawing nothing, its child
    // with it. The proxy that found no node stays as it was, drawn.
    NTEST_EXPECT_EQ_UINT(v.header->node_count, 9U);
    NTEST_EXPECT_EQ_UINT(node_named(&v, "UCX_Crate_01"), NPH_SCENE_NO_NODE);
    NTEST_EXPECT_EQ_UINT(node_named(&v, "UCX_Crate_02.001"), NPH_SCENE_NO_NODE);
    const u32 kept = node_named(&v, "UCX_Crate_03");
    const u32 handle = node_named(&v, "Handle");
    NTEST_ASSERT_NE_UINT(kept, NPH_SCENE_NO_NODE);
    NTEST_ASSERT_NE_UINT(handle, NPH_SCENE_NO_NODE);
    NTEST_EXPECT_EQ_UINT(v.nodes[handle].parent, kept);
    NTEST_EXPECT_EQ_UINT(v.header->instance_count, 8U);
    for (u32 i = 0; i < v.header->instance_count; ++i) {
        NTEST_EXPECT_NE_UINT(v.instances[i].node, kept);
    }
    NTEST_ASSERT_NE_UINT(node_named(&v, "UCX_Nothing"), NPH_SCENE_NO_NODE);
    // Without settings, what the proxies and the extras ask: the crate, the floor, the sign
    // and the barrel, in node order.
    static const char *const colliding[4] = {"Crate", "Floor", "Sign", "Barrel"};
    NTEST_ASSERT_EQ_UINT(v.collision_count, 4U);
    for (u32 k = 0; k < 4U; ++k) {
        NTEST_EXPECT_EQ_UINT(v.collisions[k].node, node_named(&v, colliding[k]));
    }
    // The crate's three hulls, in its frame, in the order its proxies came; the floor's
    // triangles; the flat sign's too; the barrel's hull.
    NphAABB b[3];
    expect_shapes(f, v.collisions[0].collision, "hhh", b);
    expect_box(b[0], nph_vec3(-0.5F, 0.0F, -0.5F), nph_vec3(0.5F, 1.0F, 0.5F));
    expect_box(b[1], nph_vec3(-0.5F, -1.0F, -0.5F), nph_vec3(0.5F, 0.0F, 0.5F));
    expect_box(b[2], nph_vec3(-0.2F, -0.2F, 0.8F), nph_vec3(0.2F, 0.2F, 1.2F));
    expect_shapes(f, v.collisions[1].collision, "m", b);
    expect_box(b[0], nph_vec3(-1.0F, 0.0F, -1.0F), nph_vec3(1.0F, 0.0F, 1.0F));
    expect_shapes(f, v.collisions[2].collision, "m", NULL);
    expect_shapes(f, v.collisions[3].collision, "h", b);
    expect_box(b[0], nph_vec3(-1.0F, -1.0F, -1.0F), nph_vec3(1.0F, 1.0F, 1.0F));
    test_asset_free(&a);
    // Current: nothing cooks again.
    const u32 cooked = cooker_stats(f->c).cooked;
    NTEST_ASSERT_EQ_INT(cooker_cook_file(f->c, f->path), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(cooker_stats(f->c).cooked, cooked);
}

NTEST_F(CollisionCook, the_scenes_settings_ask_the_rest_and_share_the_meshes_assets)
{
    char settings[1300];
    (void)snprintf(settings, sizeof settings, "%s.import.json", f->path);
    static const char json[] = "{\"scene\": 1, \"collision\": \"hull\", \"hull_vertices\": 4}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(settings, json, sizeof json - 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(f->c, f->path), NPH_RESULT_OK);
    const CookDb *db = cooker_db(f->c);
    TestAsset a;
    load_output(f, cook_db_find(db, NPH_STR("props.gltf"), NPH_STR("scene")), &a);
    NphSceneView v;
    NTEST_ASSERT_EQ_INT(nph_scene_view(&a.asset, &v), NPH_RESULT_OK);
    // Every node that draws, but the banner its extras keep out: the unmatched proxy, the odd
    // extra and the handle take the scene's hull, sharing the box's and the plane's.
    static const char *const colliding[7] = {"Crate",       "Floor", "Sign",  "Barrel",
                                             "UCX_Nothing", "Odd",   "Handle"};
    NTEST_ASSERT_EQ_UINT(v.collision_count, 7U);
    for (u32 k = 0; k < 7U; ++k) {
        NTEST_EXPECT_EQ_UINT(v.collisions[k].node, node_named(&v, colliding[k]));
    }
    NTEST_EXPECT_TRUE(nph_guid_equals(v.collisions[4].collision, v.collisions[3].collision));
    NTEST_EXPECT_TRUE(nph_guid_equals(v.collisions[6].collision, v.collisions[3].collision));
    NTEST_EXPECT_TRUE(nph_guid_equals(v.collisions[5].collision, v.collisions[2].collision));
    NTEST_EXPECT_FALSE(nph_guid_equals(v.collisions[1].collision, v.collisions[2].collision));
    // Every hull within the budget of four.
    for (u32 k = 0; k < 7U; ++k) {
        TestAsset c;
        load_output(f, cook_db_find_guid(db, v.collisions[k].collision), &c);
        NphCollisionView cv;
        NTEST_ASSERT_EQ_INT(nph_collision_view(&c.asset, &cv), NPH_RESULT_OK);
        for (u32 i = 0; i < cv.header->shape_count; ++i) {
            const NphCollisionBlock b = nph_collision_shape_block(&c.asset, &cv, i);
            if (cv.shapes[i].kind == (u32)NPH_COLLISION_SHAPE_HULL) {
                NTEST_EXPECT_LE_UINT(((const NphPhysicsHull *)b.data)->vertex_count, 4U);
            }
        }
        test_asset_free(&c);
    }
    test_asset_free(&a);
    // Collision none: only what the extras and proxies ask is left, the rest removed.
    static const char none[] = "{\"scene\": 1}";
    NTEST_ASSERT_EQ_INT(nph_file_write_all(settings, none, sizeof none - 1U), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(cooker_cook_file(f->c, f->path), NPH_RESULT_OK);
    NTEST_EXPECT_NULL(cook_db_find(db, NPH_STR("props.gltf"), NPH_STR("collision/mesh/0/mesh")));
    load_output(f, cook_db_find(db, NPH_STR("props.gltf"), NPH_STR("scene")), &a);
    NTEST_ASSERT_EQ_INT(nph_scene_view(&a.asset, &v), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(v.collision_count, 4U);
    test_asset_free(&a);
}
