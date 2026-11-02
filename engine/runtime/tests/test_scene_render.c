// SPDX-License-Identifier: Apache-2.0
// The scene renderer and the cooked-scene importer against the cooked
// showcase scene (samples/asset_viewer/content/showcase.gltf, cooked into
// a pack at build time): entities become instances and lights once their
// assets load, follow their transforms, and go when they go. GPU tests
// skip without a Vulkan device; the import test needs none.
#include <nepheline/asset/vfs.h>
#include <nepheline/core/guid.h>
#include <nepheline/runtime/scene_render.h>
#include <nepheline/scene/scene_io.h>
#include <nepheline/scene/transform.h>

#include "test_render_support.h"

#ifndef NPH_TEST_PACK
#error "NPH_TEST_PACK must name the cooked showcase pack"
#endif

// showcase.gltf: a "stage" root holding a textured "torus" and a "ground"
// plane, and a directional "sun" root: two meshes, two materials, one
// texture.
#define SCENE_KEY      "showcase.gltf#scene"
#define SCENE_ASSETS   5U
#define SCENE_ENTITIES 5U // the import root and four nodes

typedef struct SceneFixture {
    RenderFixture rf;
    NphJobSystem *jobs;
    NphVfs *vfs;
    NphAssetLoader *loader;
    NphWorld *world;
    NphRuntimeComponents ids;
    NphTransformSystem *transforms;
    NphSceneRenderer *sr;
} SceneFixture;

static void scene_setup(SceneFixture *f, bool gpu, u32 event_capacity)
{
    memset(f, 0, sizeof *f);
    if (gpu) {
        test_render_setup(&f->rf, NULL);
    }
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = nph_allocator_system();
    jd.worker_count = 2U;
    NTEST_ASSERT_EQ_INT(nph_job_system_create(&jd, &f->jobs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_create(nph_allocator_system(), &f->vfs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_vfs_mount_pack(f->vfs, nph_str_empty(), NPH_TEST_PACK, NULL),
                        NPH_RESULT_OK);
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = nph_allocator_system();
    ld.vfs = f->vfs;
    ld.jobs = f->jobs;
    NTEST_ASSERT_EQ_INT(nph_asset_loader_create(&ld, &f->loader), NPH_RESULT_OK);
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_allocator_system();
    wd.max_entities = 1024U;
    wd.event_capacity = event_capacity;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &f->world), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_runtime_register_components(f->world, &f->ids), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_transform_system_create(f->world, nph_allocator_system(), &f->transforms),
        NPH_RESULT_OK);
}

static void scene_teardown(SceneFixture *f)
{
    nph_scene_renderer_destroy(f->sr);
    nph_transform_system_destroy(f->transforms);
    nph_world_destroy(f->world);
    nph_asset_loader_destroy(f->loader);
    nph_vfs_destroy(f->vfs);
    nph_job_system_destroy(f->jobs);
    test_render_teardown(&f->rf);
}

static NphEntity import_showcase(SceneFixture *f, u32 *out_count)
{
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = f->world;
    d.loader = f->loader;
    d.scene = nph_guid_from_cstr(SCENE_KEY);
    d.root_transform = nph_local_transform_identity();
    d.name = "showcase";
    NphEntity root = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, &root, out_count), NPH_RESULT_OK);
    return root;
}

static NphEntity named_in(const NphWorld *world, const char *name)
{
    for (u32 i = 0; i < nph_world_entity_slot_count(world); ++i) {
        const NphEntity e = nph_world_entity_at(world, i);
        const NphName *n = NPH_WORLD_GET(world, e, NPH_COMPONENT_ID_NAME, NphName);
        if (n != NULL && nph_str_equals(nph_text_str(&n->text), nph_str_from_cstr(name))) {
            return e;
        }
    }
    return NPH_ENTITY_NULL;
}

static NphEntity named(const SceneFixture *f, const char *name)
{
    return named_in(f->world, name);
}

// One frame of a world: its transforms, then the scene renderer, then the
// world's events cleared (the engine's order).
static NphSceneRenderStats sync_world(SceneFixture *f, NphWorld *world, NphTransformSystem *ts)
{
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(ts, f->jobs), NPH_RESULT_OK);
    NphSceneRenderStats st;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_sync(f->sr, &st), NPH_RESULT_OK);
    nph_world_clear_events(world);
    return st;
}

static NphSceneRenderStats sync(SceneFixture *f)
{
    return sync_world(f, f->world, f->transforms);
}

static void make_scene_renderer(SceneFixture *f)
{
    NphSceneRendererDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.renderer = f->rf.renderer;
    d.loader = f->loader;
    d.world = f->world;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_create(&d, &f->sr), NPH_RESULT_OK);
}

// Imports the showcase and syncs until its assets are in.
static void load_showcase(SceneFixture *f)
{
    (void)import_showcase(f, NULL);
    (void)sync(f);
    nph_scene_renderer_wait(f->sr);
    const NphSceneRenderStats st = sync(f);
    NTEST_ASSERT_EQ_UINT(st.instances, 2U);
    NTEST_ASSERT_EQ_UINT(st.lights, 1U);
}

static NphMeshRenderer *mesh_renderer(SceneFixture *f, NphEntity e)
{
    NphMeshRenderer *m = (NphMeshRenderer *)nph_world_get_mut(f->world, e, f->ids.mesh_renderer);
    NTEST_ASSERT_NOT_NULL(m);
    return m;
}

static NphLocalTransform *local(SceneFixture *f, NphEntity e)
{
    NphLocalTransform *lt =
        NPH_WORLD_GET_MUT(f->world, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_ASSERT_NOT_NULL(lt);
    return lt;
}

// A camera on +Z, above, looking at the stage.
static NphRenderView test_view(void)
{
    NphRenderView view;
    nph_render_view_default(&view,
                            nph_render_camera_look_at(nph_dvec3(0.0, 3.0, 8.0), nph_dvec3_zero(),
                                                      nph_vec3_unit_y(), 0.9F, 0.05F));
    return view;
}

NTEST(SceneImport, a_cooked_scene_becomes_entities)
{
    static SceneFixture fx;
    scene_setup(&fx, false, 256U);
    u32 count = 0;
    const NphEntity root = import_showcase(&fx, &count);
    NTEST_ASSERT_EQ_UINT(count, SCENE_ENTITIES);
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(fx.world), SCENE_ENTITIES);
    const NphName *root_name = NPH_WORLD_GET(fx.world, root, NPH_COMPONENT_ID_NAME, NphName);
    NTEST_ASSERT_NOT_NULL(root_name);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_text_str(&root_name->text), NPH_STR("showcase")));
    const NphEntity stage = named(&fx, "stage");
    const NphEntity torus = named(&fx, "torus");
    const NphEntity ground = named(&fx, "ground");
    const NphEntity sun = named(&fx, "sun");
    NTEST_ASSERT_FALSE(nph_entity_is_null(stage));
    NTEST_ASSERT_FALSE(nph_entity_is_null(torus));
    NTEST_ASSERT_FALSE(nph_entity_is_null(ground));
    NTEST_ASSERT_FALSE(nph_entity_is_null(sun));
    NTEST_ASSERT_TRUE(nph_entity_is_null(nph_world_parent(fx.world, root)));
    NTEST_ASSERT_TRUE(nph_entity_equals(nph_world_parent(fx.world, stage), root));
    NTEST_ASSERT_TRUE(nph_entity_equals(nph_world_parent(fx.world, sun), root));
    NTEST_ASSERT_TRUE(nph_entity_equals(nph_world_parent(fx.world, torus), stage));
    NTEST_ASSERT_TRUE(nph_entity_equals(nph_world_parent(fx.world, ground), stage));
    // The node's own transform.
    const NphLocalTransform *tl =
        NPH_WORLD_GET(fx.world, torus, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    NTEST_ASSERT_NOT_NULL(tl);
    NTEST_ASSERT_NEAR_F64(tl->translation.y, 1.1, 1e-6);
    // Meshes and materials by GUID; the stage draws nothing.
    const NphMeshRenderer *m =
        (const NphMeshRenderer *)nph_world_get(fx.world, torus, fx.ids.mesh_renderer);
    NTEST_ASSERT_NOT_NULL(m);
    NTEST_ASSERT_FALSE(nph_guid_is_null(m->mesh));
    NTEST_ASSERT_FALSE(nph_guid_is_null(m->material0));
    NTEST_ASSERT_TRUE(nph_guid_is_null(m->material1));
    const NphMeshRenderer *g =
        (const NphMeshRenderer *)nph_world_get(fx.world, ground, fx.ids.mesh_renderer);
    NTEST_ASSERT_NOT_NULL(g);
    NTEST_ASSERT_FALSE(nph_guid_equals(g->mesh, m->mesh));
    NTEST_ASSERT_FALSE(nph_guid_equals(g->material0, m->material0));
    NTEST_ASSERT_FALSE(nph_world_has(fx.world, stage, fx.ids.mesh_renderer));
    const NphLight *l = (const NphLight *)nph_world_get(fx.world, sun, fx.ids.light);
    NTEST_ASSERT_NOT_NULL(l);
    NTEST_ASSERT_EQ_UINT(l->type, (u32)NPH_LIGHT_DIRECTIONAL);
    NTEST_ASSERT_TRUE(l->intensity > 0.0F);
    NTEST_ASSERT_FALSE(nph_world_has(fx.world, sun, fx.ids.mesh_renderer));

    // An unknown scene, or none, changes nothing.
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = fx.world;
    d.loader = fx.loader;
    d.scene = nph_guid_from_cstr("missing.gltf#scene");
    d.root_transform = nph_local_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_NOT_FOUND);
    d.scene = NPH_GUID_NULL;
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_INVALID_ARGUMENT);
    // Neither does a world too small for it (all or nothing).
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_allocator_system();
    wd.max_entities = 3U;
    wd.registry_from = fx.world;
    NphWorld *small = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &small), NPH_RESULT_OK);
    d.world = small;
    d.scene = nph_guid_from_cstr(SCENE_KEY);
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(small), 0U);
    nph_world_destroy(small);
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(fx.world), SCENE_ENTITIES);
    // Imported again, under a parent: a second copy.
    d.world = fx.world;
    d.parent = root;
    d.name = NULL;
    NphEntity second = NPH_ENTITY_NULL;
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, &second, &count), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(count, SCENE_ENTITIES);
    NTEST_ASSERT_TRUE(nph_entity_equals(nph_world_parent(fx.world, second), root));
    const NphName *second_name = NPH_WORLD_GET(fx.world, second, NPH_COMPONENT_ID_NAME, NphName);
    NTEST_ASSERT_NOT_NULL(second_name);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_text_str(&second_name->text), NPH_STR("scene")));
    NTEST_ASSERT_EQ_UINT(nph_world_entity_count(fx.world), 2U * SCENE_ENTITIES);
    scene_teardown(&fx);
}

// A further tile of a world brings its meshes, not another sun or camera.
NTEST(SceneImport, instances_only_leaves_the_lights_and_cameras)
{
    static SceneFixture fx;
    scene_setup(&fx, false, 256U);
    (void)import_showcase(&fx, NULL);
    NphSceneImportDesc d;
    memset(&d, 0, sizeof d);
    d.world = fx.world;
    d.loader = fx.loader;
    d.scene = nph_guid_from_cstr(SCENE_KEY);
    d.root_transform = nph_local_transform_identity();
    d.instances_only = 1;
    NTEST_ASSERT_EQ_INT(nph_scene_import_cooked(&d, NULL, NULL), NPH_RESULT_OK);
    u32 meshes = 0U;
    u32 lights = 0U;
    u32 cameras = 0U;
    for (u32 i = 0; i < nph_world_entity_slot_count(fx.world); ++i) {
        const NphEntity e = nph_world_entity_at(fx.world, i);
        if (nph_entity_is_null(e)) {
            continue;
        }
        meshes += nph_world_get(fx.world, e, fx.ids.mesh_renderer) != NULL ? 1U : 0U;
        lights += nph_world_get(fx.world, e, fx.ids.light) != NULL ? 1U : 0U;
        cameras += nph_world_get(fx.world, e, fx.ids.camera) != NULL ? 1U : 0U;
    }
    NTEST_EXPECT_EQ_UINT(meshes, 4U); // twice the torus and the ground
    NTEST_EXPECT_EQ_UINT(lights, 1U); // the first's sun alone
    NTEST_EXPECT_EQ_UINT(cameras, 0U);
    scene_teardown(&fx);
}

NTEST(SceneRender, entities_become_instances_and_lights)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    (void)import_showcase(&fx, NULL);
    NphSceneRenderStats st = sync(&fx);
    // The assets are loading: the light is there, the meshes wait.
    NTEST_ASSERT_EQ_UINT(st.entities, 3U);
    NTEST_ASSERT_EQ_UINT(st.lights, 1U);
    NTEST_ASSERT_EQ_UINT(st.instances, 0U);
    NTEST_ASSERT_EQ_UINT(st.pending, 2U);
    NTEST_ASSERT_EQ_UINT(st.failed, 0U);
    nph_scene_renderer_wait(fx.sr);
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 2U);
    NTEST_ASSERT_EQ_UINT(st.created, 2U);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    NTEST_ASSERT_EQ_UINT(st.failed, 0U);
    NTEST_ASSERT_EQ_UINT(st.assets_loading, 0U);
    NTEST_ASSERT_EQ_UINT(st.assets_ready, SCENE_ASSETS);
    NTEST_ASSERT_EQ_UINT(st.assets_failed, 0U);
    const NphEntity torus = named(&fx, "torus");
    NTEST_ASSERT_TRUE(
        nph_render_instance_is_valid(fx.rf.renderer, nph_scene_renderer_instance(fx.sr, torus)));
    NTEST_ASSERT_NOT_NULL(nph_scene_renderer_mesh_asset(fx.sr, torus));
    // A static world touches nothing.
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.created, 0U);
    NTEST_ASSERT_EQ_UINT(st.destroyed, 0U);
    NTEST_ASSERT_EQ_UINT(st.moved, 0U);

    // The renderer draws them.
    const NphRenderView view = test_view();
    NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_UINT(fs.instances, 2U);
    NTEST_ASSERT_EQ_UINT(fs.culled, 0U);
    NTEST_ASSERT_EQ_UINT(fs.draws, 2U);
    NTEST_ASSERT_EQ_UINT(fs.lights, 1U);

    // Moving the parent moves both children.
    const NphEntity stage = named(&fx, "stage");
    local(&fx, stage)->translation.x += 2.0;
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.moved, 2U);
    NTEST_ASSERT_EQ_UINT(st.created, 0U);

    // Hidden by the component, then by a degenerate scale (hidden until it
    // can be drawn again).
    mesh_renderer(&fx, torus)->hidden = 1;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_UINT(fs.culled, 1U);
    mesh_renderer(&fx, torus)->hidden = 0;
    local(&fx, torus)->scale = nph_vec3(0.0F, 1.0F, 1.0F);
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_UINT(fs.culled, 1U);
    local(&fx, torus)->scale = nph_vec3_one();
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_UINT(fs.culled, 0U);

    // A Light that casts shadows: the frame traces shadow rays where the
    // renderer can.
    const NphEntity sun = named(&fx, "sun");
    NphLight *sun_light = (NphLight *)nph_world_get_mut(fx.world, sun, fx.ids.light);
    NTEST_ASSERT_NOT_NULL(sun_light);
    NTEST_ASSERT_FALSE(sun_light->shadows);
    sun_light->shadows = 1;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_INT(fs.shadow_rays ? 1 : 0,
                        nph_renderer_ray_tracing_supported(fx.rf.renderer) ? 1 : 0);
    sun_light = (NphLight *)nph_world_get_mut(fx.world, sun, fx.ids.light);
    sun_light->shadows = 0;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_FALSE(fs.shadow_rays);

    // Removing the Light removes the render light.
    NTEST_ASSERT_EQ_INT(nph_world_remove(fx.world, sun, fx.ids.light), NPH_RESULT_OK);
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.lights, 0U);
    NTEST_ASSERT_EQ_UINT(st.entities, 2U);
    // Despawning destroys the instance and releases what only it used.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(fx.world, torus), NPH_RESULT_OK);
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 1U);
    NTEST_ASSERT_EQ_UINT(st.destroyed, 1U);
    NTEST_ASSERT_EQ_UINT(st.entities, 1U);
    NTEST_ASSERT_EQ_UINT(st.assets_ready, 2U); // the ground's mesh and material
    // A MeshRenderer that names no mesh draws nothing, and is no failure.
    const NphEntity ground = named(&fx, "ground");
    mesh_renderer(&fx, ground)->mesh = NPH_GUID_NULL;
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 0U);
    NTEST_ASSERT_EQ_UINT(st.destroyed, 1U);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    NTEST_ASSERT_EQ_UINT(st.failed, 0U);
    NTEST_ASSERT_EQ_UINT(st.entities, 1U);
    NTEST_ASSERT_EQ_UINT(st.assets_ready, 1U); // its material stays referenced
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_UINT(fs.instances, 0U);
    scene_teardown(&fx);
}

NTEST(SceneRender, a_missing_mesh_fails_and_a_missing_material_defaults)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    (void)import_showcase(&fx, NULL);
    const NphEntity torus = named(&fx, "torus");
    const NphEntity ground = named(&fx, "ground");
    mesh_renderer(&fx, torus)->mesh = nph_guid_from_cstr("nowhere#mesh");
    mesh_renderer(&fx, ground)->material0 = nph_guid_from_cstr("nowhere#material");
    (void)sync(&fx);
    nph_scene_renderer_wait(fx.sr);
    NphSceneRenderStats st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 1U); // the ground, with the default material
    NTEST_ASSERT_EQ_UINT(st.failed, 1U);    // the torus
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    NTEST_ASSERT_EQ_UINT(st.assets_failed, 2U);
    NTEST_ASSERT_NULL(nph_scene_renderer_mesh_asset(fx.sr, torus));
    NTEST_ASSERT_NOT_NULL(nph_scene_renderer_mesh_asset(fx.sr, ground));
    NTEST_ASSERT_FALSE(
        nph_render_instance_is_valid(fx.rf.renderer, nph_scene_renderer_instance(fx.sr, torus)));
    NTEST_ASSERT_TRUE(
        nph_render_instance_is_valid(fx.rf.renderer, nph_scene_renderer_instance(fx.sr, ground)));
    // A failed entity that moves stays failed, without retrying.
    local(&fx, torus)->translation.z += 1.0;
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.failed, 1U);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    // A mesh GUID used as a material is refused: the default material.
    const NphGuid ground_mesh = mesh_renderer(&fx, ground)->mesh;
    NphMeshRenderer *m = mesh_renderer(&fx, torus);
    m->mesh = ground_mesh;
    m->material0 = ground_mesh;
    (void)sync(&fx);
    nph_scene_renderer_wait(fx.sr);
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 2U);
    NTEST_ASSERT_EQ_UINT(st.failed, 0U);
    // ... and moving it does not ask again. (Change detection is per
    // chunk: the ground, in the torus's chunk, is written too.)
    local(&fx, torus)->translation.z -= 1.0;
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.moved, 2U);
    NTEST_ASSERT_EQ_UINT(st.created, 0U);
    NTEST_ASSERT_EQ_UINT(st.destroyed, 0U);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    const NphRenderView view = test_view();
    const NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_EQ_UINT(fs.instances, 2U);
    scene_teardown(&fx);
}

// An instance takes the renderer's material slots up to the last material
// its entity names (the rest draw with the default material): the test
// renderer's 64 instances fit its 256 slots when most name none, where
// eight a piece would fill them at 32.
NTEST(SceneRender, instances_take_only_the_material_slots_they_name)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx); // the torus and the ground, each with its material
    const NphGuid torus_mesh = mesh_renderer(&fx, named(&fx, "torus"))->mesh;
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, fx.ids.mesh_renderer};
    for (u32 i = 0; i < 62U; ++i) {
        NphEntity e;
        NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &e), NPH_RESULT_OK);
        mesh_renderer(&fx, e)->mesh = torus_mesh;
        const u32 row = i / 8U;
        local(&fx, e)->translation = nph_dvec3(0.5 * (f64)(i % 8U), 0.0, -0.5 * (f64)row);
    }
    const NphSceneRenderStats st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 64U);
    NTEST_EXPECT_EQ_UINT(st.pending, 0U);
    const NphRenderView view = test_view();
    const NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.instances, 64U);
    scene_teardown(&fx);
}

// A Light of a near range is one of the many: created, changed only when it changes,
// and an ordinary light again when its near range goes (ADR-0073).
NTEST(SceneRender, a_light_of_a_near_range_is_one_of_the_many)
{
    static SceneFixture fx;
    scene_setup(&fx, false, 256U);
    NphRendererDesc rd;
    test_renderer_desc(&rd, NULL);
    rd.max_many_lights = 4U;
    test_render_setup(&fx.rf, &rd);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    if (!nph_renderer_production_supported(fx.rf.renderer)) {
        scene_teardown(&fx);
        NTEST_SKIP("no production path on this device");
    }
    NphRenderView view = test_view();
    view.pipeline = NPH_RENDER_PIPELINE_PRODUCTION;
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, fx.ids.light};
    NphEntity e;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &e), NPH_RESULT_OK);
    NphLight *l = NPH_WORLD_GET_MUT(fx.world, e, fx.ids.light, NphLight);
    NTEST_ASSERT_NOT_NULL(l);
    l->near_range = 0.5F;
    NphSceneRenderStats st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.lights, 1U);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_light(fx.sr, e).value, 0U);
    NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.many_lights, 1U);
    NTEST_EXPECT_EQ_UINT(fs.lights, 0U);
    NTEST_EXPECT_TRUE(fs.many_lights_rebuilt);

    // A sync that changed nothing about it leaves the set as built, though its chunk
    // was written.
    NPH_WORLD_GET_MUT(fx.world, e, fx.ids.light, NphLight)->intensity = l->intensity;
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.lights, 1U);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_FALSE(fs.many_lights_rebuilt);
    // A change does make it rebuild.
    local(&fx, e)->translation.x += 1.0;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.many_lights, 1U);
    NTEST_EXPECT_TRUE(fs.many_lights_rebuilt);

    // Its near range goes: an ordinary light.
    NPH_WORLD_GET_MUT(fx.world, e, fx.ids.light, NphLight)->near_range = 0.0F;
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.lights, 1U);
    NTEST_EXPECT_TRUE(
        nph_render_light_is_valid(fx.rf.renderer, nph_scene_renderer_light(fx.sr, e)));
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.many_lights, 0U);
    NTEST_EXPECT_EQ_UINT(fs.lights, 1U);
    // And back; then it goes.
    NPH_WORLD_GET_MUT(fx.world, e, fx.ids.light, NphLight)->near_range = 0.5F;
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.lights, 1U);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_light(fx.sr, e).value, 0U);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.many_lights, 1U);
    NTEST_EXPECT_EQ_UINT(fs.lights, 0U);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(fx.world, e), NPH_RESULT_OK);
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.lights, 0U);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.many_lights, 0U);
    scene_teardown(&fx);
}

NTEST(SceneRender, the_camera_and_the_environment)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    NphRenderView view = test_view();
    (void)sync(&fx);
    NTEST_ASSERT_FALSE(nph_scene_renderer_view(fx.sr, &view));
    // Three cameras: the active one with the lowest slot is the view.
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, fx.ids.camera};
    NphEntity inactive;
    NphEntity cam;
    NphEntity other;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &inactive), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &cam), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &other), NPH_RESULT_OK);
    NphCamera *c = (NphCamera *)nph_world_get_mut(fx.world, inactive, fx.ids.camera);
    c->active = 0;
    c = (NphCamera *)nph_world_get_mut(fx.world, cam, fx.ids.camera);
    c->ev100 = 9.0F;
    c->tonemapper = (u32)NPH_RENDER_TONEMAPPER_AGX;
    c->yfov = 1.2F;
    c->znear = 0.25F;
    c->shutter = 1.0F / 480.0F;
    c->aperture = 2.8F;
    c->focus_distance = 4.0F;
    c->sensor_height = 0.036F;
    NphLocalTransform *lt = local(&fx, cam);
    lt->translation = nph_dvec3(1.0, 2.0, 3.0);
    lt->rotation = nph_quat_from_rotation_y(0.5F);
    lt->scale = nph_vec3(2.0F, 2.0F, 2.0F); // the view ignores scale
    (void)sync(&fx);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(fx.sr, &view));
    NTEST_ASSERT_NEAR_F64(view.camera.position.x, 1.0, 1e-12);
    NTEST_ASSERT_NEAR_F64(view.camera.position.y, 2.0, 1e-12);
    NTEST_ASSERT_NEAR_F64(view.camera.position.z, 3.0, 1e-12);
    NTEST_ASSERT_NEAR_F32(view.camera.yfov, 1.2F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.camera.znear, 0.25F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.ev100, 9.0F, 0.0F);
    NTEST_ASSERT_EQ_UINT((u32)view.tonemapper, (u32)NPH_RENDER_TONEMAPPER_AGX);
    NTEST_ASSERT_NEAR_F32(view.shutter, 1.0F / 480.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.aperture, 2.8F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.focus_distance, 4.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.sensor_height, 0.036F, 0.0F);
    // A focus no further than the focal length (36 mm over 2 tan(0.6): 26
    // mm) makes the lens a pinhole, as does one that is not finite.
    c = (NphCamera *)nph_world_get_mut(fx.world, cam, fx.ids.camera);
    c->focus_distance = 0.02F;
    (void)sync(&fx);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(fx.sr, &view));
    NTEST_ASSERT_NEAR_F32(view.aperture, 0.0F, 0.0F);
    c = (NphCamera *)nph_world_get_mut(fx.world, cam, fx.ids.camera);
    c->focus_distance = NPH_F32_INFINITY;
    (void)sync(&fx);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(fx.sr, &view));
    NTEST_ASSERT_NEAR_F32(view.aperture, 0.0F, 0.0F);
    const NphQuat want = nph_quat_from_rotation_y(0.5F);
    NTEST_ASSERT_NEAR_F32(nph_f32_abs(nph_quat_dot(view.camera.orientation, want)), 1.0F, 1e-5F);
    // Deactivating it hands the view to the next one.
    c = (NphCamera *)nph_world_get_mut(fx.world, cam, fx.ids.camera);
    c->active = 0;
    (void)sync(&fx);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(fx.sr, &view));
    NTEST_ASSERT_NEAR_F64(view.camera.position.x, 0.0, 0.0);
    NTEST_ASSERT_NEAR_F32(view.ev100, 15.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.shutter, 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.aperture, 0.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.focus_distance, 10.0F, 0.0F);
    NTEST_ASSERT_NEAR_F32(view.sensor_height, 0.0F, 0.0F);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(fx.world, other), NPH_RESULT_OK);
    (void)sync(&fx);
    NTEST_ASSERT_FALSE(nph_scene_renderer_view(fx.sr, &view));
    view = test_view();

    // A world without an Environment has the default sky, set by the first
    // sync: the first frame computes its lighting, the next ones do not.
    NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_TRUE(fs.environment_updated);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_FALSE(fs.environment_updated);
    // An Environment with the default values changes nothing.
    const NphComponentId env_comps[1] = {fx.ids.environment};
    NphEntity env;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, env_comps, 1U, &env), NPH_RESULT_OK);
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_FALSE(fs.environment_updated);
    // Edited, it does; unchanged, it does not.
    NphEnvironment *e = (NphEnvironment *)nph_world_get_mut(fx.world, env, fx.ids.environment);
    e->luminance = 2000.0F;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_TRUE(fs.environment_updated);
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_FALSE(fs.environment_updated);
    // Removed, the default sky comes back.
    NTEST_ASSERT_EQ_INT(nph_world_despawn(fx.world, env), NPH_RESULT_OK);
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_ASSERT_TRUE(fs.environment_updated);
    scene_teardown(&fx);
}

NTEST(SceneRender, an_overflowing_event_stream_is_recovered)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 2U); // an import overflows two events
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx);
    // Remove the light, then despawn everything else, with the stream
    // full: the sync re-validates what it mirrors.
    const NphEntity sun = named(&fx, "sun");
    NTEST_ASSERT_EQ_INT(nph_world_remove(fx.world, sun, fx.ids.light), NPH_RESULT_OK);
    for (u32 i = 0; i < nph_world_entity_slot_count(fx.world); ++i) {
        const NphEntity e = nph_world_entity_at(fx.world, i);
        if (!nph_entity_is_null(e) && !nph_entity_equals(e, sun)) {
            NTEST_ASSERT_EQ_INT(nph_world_despawn(fx.world, e), NPH_RESULT_OK);
        }
    }
    u32 count = 0;
    u32 dropped = 0;
    (void)nph_world_events(fx.world, &count, &dropped);
    NTEST_ASSERT_GT_UINT(dropped, 0U);
    const NphSceneRenderStats st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 0U);
    NTEST_ASSERT_EQ_UINT(st.lights, 0U);
    NTEST_ASSERT_EQ_UINT(st.entities, 0U);
    NTEST_ASSERT_EQ_UINT(st.destroyed, 2U);
    NTEST_ASSERT_EQ_UINT(st.assets_ready, 0U);
    scene_teardown(&fx);
}

NTEST(SceneRender, a_world_switch_keeps_the_shared_assets)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx);
    // The play copy (play-in-editor): the same entities in a second world.
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_allocator_system();
    wd.max_entities = 1024U;
    wd.event_capacity = 64U;
    wd.registry_from = fx.world;
    NphWorld *play = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &play), NPH_RESULT_OK);
    NphBinWriter w;
    nph_bin_writer_init(&w, nph_allocator_system());
    NTEST_ASSERT_EQ_INT(nph_scene_save_binary(fx.world, NULL, &w, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_scene_load_binary(play, nph_bin_writer_data(&w), nph_bin_writer_len(&w), NULL, NULL),
        NPH_RESULT_OK);
    nph_bin_writer_free(&w);
    NphTransformSystem *play_transforms = NULL;
    NTEST_ASSERT_EQ_INT(nph_transform_system_create(play, nph_allocator_system(), &play_transforms),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_set_world(fx.sr, play), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_scene_renderer_world(fx.sr) == play);
    // Nothing reloads: the first sync draws the copy.
    NphSceneRenderStats st = sync_world(&fx, play, play_transforms);
    NTEST_ASSERT_EQ_UINT(st.instances, 2U);
    NTEST_ASSERT_EQ_UINT(st.created, 2U);
    NTEST_ASSERT_EQ_UINT(st.lights, 1U);
    NTEST_ASSERT_EQ_UINT(st.pending, 0U);
    NTEST_ASSERT_EQ_UINT(st.assets_loading, 0U);
    NTEST_ASSERT_EQ_UINT(st.assets_ready, SCENE_ASSETS);
    const NphEntity play_torus = named_in(play, "torus");
    NTEST_ASSERT_TRUE(nph_render_instance_is_valid(fx.rf.renderer,
                                                   nph_scene_renderer_instance(fx.sr, play_torus)));
    // Back to the edited world, the same.
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_set_world(fx.sr, fx.world), NPH_RESULT_OK);
    st = sync(&fx);
    NTEST_ASSERT_EQ_UINT(st.instances, 2U);
    NTEST_ASSERT_EQ_UINT(st.assets_loading, 0U);
    // An empty world lets them go.
    NphWorld *empty = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &empty), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_set_world(fx.sr, empty), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_sync(fx.sr, &st), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(st.instances, 0U);
    NTEST_ASSERT_EQ_UINT(st.lights, 0U);
    NTEST_ASSERT_EQ_UINT(st.assets_ready, 0U);
    // A world without an event stream is refused, and leaves none bound.
    wd.event_capacity = 0U;
    NphWorld *silent = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &silent), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_set_world(fx.sr, silent), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(nph_scene_renderer_world(fx.sr));
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_set_world(fx.sr, fx.world), NPH_RESULT_OK);
    nph_world_destroy(silent);
    nph_world_destroy(empty);
    nph_transform_system_destroy(play_transforms);
    nph_world_destroy(play);
    scene_teardown(&fx);
}

NTEST(SceneRender, the_host_hides_an_instance_whatever_its_component_says)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx);
    const NphEntity torus = named(&fx, "torus");
    const NphEntity ground = named(&fx, "ground");
    const NphRenderView view = test_view();
    NTEST_EXPECT_FALSE(nph_scene_renderer_set_hidden(fx.sr, named(&fx, "sun"), true)); // a light
    NTEST_ASSERT_TRUE(nph_scene_renderer_set_hidden(fx.sr, torus, true));
    NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 1U);
    // Moved while hidden, it stays hidden, and shows where it is now.
    local(&fx, named(&fx, "stage"))->translation.x += 100.0; // out of the view
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 2U); // the torus hidden, the ground out of view
    NTEST_ASSERT_TRUE(nph_scene_renderer_set_hidden(fx.sr, torus, false));
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 2U); // both out of view: the torus moved with the stage
    local(&fx, named(&fx, "stage"))->translation.x -= 100.0;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 0U);
    // The component and the host both hide: either is enough.
    NTEST_ASSERT_TRUE(nph_scene_renderer_set_hidden(fx.sr, torus, true));
    mesh_renderer(&fx, torus)->hidden = 1;
    (void)sync(&fx);
    mesh_renderer(&fx, torus)->hidden = 0;
    (void)sync(&fx);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 1U);
    // It holds across a new mesh, whose instance starts hidden.
    mesh_renderer(&fx, torus)->mesh = mesh_renderer(&fx, ground)->mesh;
    (void)sync(&fx);
    nph_scene_renderer_wait(fx.sr);
    const NphSceneRenderStats st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 2U);
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 1U);
    NTEST_ASSERT_TRUE(nph_scene_renderer_set_hidden(fx.sr, torus, false));
    fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.culled, 0U);
    scene_teardown(&fx);
}

NTEST(SceneRender, a_reloaded_asset_is_loaded_again_for_whoever_draws_it)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx);
    const u32 live = nph_asset_loader_live(fx.loader);
    const NphEntity torus = named(&fx, "torus");
    const NphRenderInstance before = nph_scene_renderer_instance(fx.sr, torus);
    const NphAsset *old_mesh = nph_scene_renderer_mesh_asset(fx.sr, torus);
    NTEST_ASSERT_NOT_NULL(old_mesh);

    // The torus's mesh: the torus alone loses its instance until it is back.
    NTEST_EXPECT_EQ_UINT(
        nph_scene_renderer_reload(fx.sr, nph_guid_from_cstr("showcase.gltf#mesh/0")), 1U);
    NphSceneRenderStats st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 1U);
    NTEST_EXPECT_EQ_UINT(st.pending, 1U);
    NTEST_EXPECT_FALSE(
        nph_render_instance_is_valid(fx.rf.renderer, nph_scene_renderer_instance(fx.sr, torus)));
    nph_scene_renderer_wait(fx.sr);
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 2U);
    NTEST_EXPECT_EQ_UINT(st.created, 1U);
    NTEST_EXPECT_EQ_UINT(st.assets_ready, SCENE_ASSETS);
    NTEST_EXPECT_NE_UINT(nph_scene_renderer_instance(fx.sr, torus).value, before.value);
    NTEST_ASSERT_NOT_NULL(nph_scene_renderer_mesh_asset(fx.sr, torus));
    NTEST_EXPECT_EQ_UINT(nph_asset_loader_live(fx.loader), live); // the old one went

    // The checker texture: its material is made again, so the torus, which
    // uses it, is drawn again; the ground is untouched.
    NTEST_EXPECT_EQ_UINT(
        nph_scene_renderer_reload(fx.sr, nph_guid_from_cstr("showcase.gltf#image/0/color")), 1U);
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 1U);
    nph_scene_renderer_wait(fx.sr);
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 2U);
    NTEST_EXPECT_EQ_UINT(st.assets_ready, SCENE_ASSETS);
    NTEST_EXPECT_EQ_UINT(st.failed, 0U);
    NTEST_EXPECT_EQ_UINT(nph_asset_loader_live(fx.loader), live);

    // Twice before a sync is once; an asset nothing holds reloads nothing.
    NTEST_EXPECT_EQ_UINT(
        nph_scene_renderer_reload(fx.sr, nph_guid_from_cstr("showcase.gltf#material/1")), 1U);
    NTEST_EXPECT_EQ_UINT(
        nph_scene_renderer_reload(fx.sr, nph_guid_from_cstr("showcase.gltf#material/1")), 0U);
    NTEST_EXPECT_EQ_UINT(nph_scene_renderer_reload(fx.sr, nph_guid_from_cstr("nothing")), 0U);
    (void)sync(&fx);
    nph_scene_renderer_wait(fx.sr);
    st = sync(&fx);
    NTEST_EXPECT_EQ_UINT(st.instances, 2U);
    NTEST_EXPECT_EQ_UINT(st.assets_ready, SCENE_ASSETS);
    const NphRenderView view = test_view();
    const NphRenderFrameStats fs = test_render_frame(&fx.rf, &view, fx.rf.target);
    NTEST_EXPECT_EQ_UINT(fs.draws, 2U);
    scene_teardown(&fx);
}

// ---------------------------------------------------------------------------
// Interpolation
// ---------------------------------------------------------------------------

// A simulation step's end: the world transforms composed, as the step's
// transform system does, and the scene renderer's capture.
static void step_and_capture(SceneFixture *f)
{
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(f->transforms, f->jobs), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_capture(f->sr), NPH_RESULT_OK);
}

// The rest of a frame: its own changes composed, the sync at `fraction`, the
// world's events cleared.
static NphSceneRenderStats draw_at(SceneFixture *f, f32 fraction)
{
    NTEST_ASSERT_EQ_INT(nph_transform_system_update(f->transforms, f->jobs), NPH_RESULT_OK);
    NphSceneRenderStats st;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_sync_at(f->sr, fraction, &st), NPH_RESULT_OK);
    nph_world_clear_events(f->world);
    return st;
}

static NphRenderTransform drawn(const SceneFixture *f, NphEntity e)
{
    NphRenderTransform t;
    NTEST_ASSERT_EQ_INT(
        nph_render_instance_transform(f->rf.renderer, nph_scene_renderer_instance(f->sr, e), &t),
        NPH_RESULT_OK);
    return t;
}

static NphWorldTransform world_of(const SceneFixture *f, NphEntity e)
{
    const NphWorldTransform *wt =
        NPH_WORLD_GET(f->world, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    NTEST_ASSERT_NOT_NULL(wt);
    return *wt;
}

NTEST(SceneRender, what_the_simulation_moved_is_drawn_between_its_states)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx);
    const NphEntity torus = named(&fx, "torus");
    const NphEntity stage = named(&fx, "stage");
    const f64 x0 = world_of(&fx, torus).translation.x;

    // A step moves it a metre: drawn a quarter and three quarters of the way
    // by two frames, the second without a step of its own.
    local(&fx, torus)->translation.x += 1.0;
    step_and_capture(&fx);
    NphSceneRenderStats st = draw_at(&fx, 0.25F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 1U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, x0 + 0.25, 1e-6);
    st = draw_at(&fx, 0.75F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 1U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, x0 + 0.75, 1e-6);
    // The next step moves it on, from where the last one left it.
    local(&fx, torus)->translation.x += 1.0;
    step_and_capture(&fx);
    (void)draw_at(&fx, 0.5F);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, x0 + 1.5, 1e-6);
    // A step that does not move it: at rest where it went.
    step_and_capture(&fx);
    st = draw_at(&fx, 0.5F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 0U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, x0 + 2.0, 1e-9);

    // A turn of 90 degrees is drawn half turned, its size kept (not the
    // columns' average, 0.71 long).
    const NphWorldTransform before = world_of(&fx, torus);
    local(&fx, torus)->rotation =
        nph_quat_mul(nph_quat_from_rotation_y(1.5707964F), local(&fx, torus)->rotation);
    step_and_capture(&fx);
    (void)draw_at(&fx, 0.5F);
    const NphRenderTransform half = drawn(&fx, torus);
    const NphWorldTransform after = world_of(&fx, torus);
    for (u32 c = 0; c < 3U; ++c) {
        const NphVec3 axis = c == 0U ? before.x_axis : c == 1U ? before.y_axis : before.z_axis;
        NTEST_EXPECT_NEAR_F32(nph_vec3_length(half.linear.c[c]), nph_vec3_length(axis), 1e-5F);
    }
    const NphVec3 x_from = nph_vec3_normalize(before.x_axis);
    const NphVec3 x_to = nph_vec3_normalize(after.x_axis);
    const NphVec3 x_half = nph_vec3_normalize(half.linear.c[0]);
    NTEST_EXPECT_NEAR_F32(nph_vec3_dot(x_half, x_from), 0.70710678F, 1e-4F);
    NTEST_EXPECT_NEAR_F32(nph_vec3_dot(x_half, x_to), 0.70710678F, 1e-4F);
    step_and_capture(&fx); // at rest

    // A change after the capture (at the frame rate) is drawn as it is.
    local(&fx, torus)->translation.x += 1.0;
    step_and_capture(&fx);
    local(&fx, torus)->translation.x += 5.0;
    st = draw_at(&fx, 0.5F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 0U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, x0 + 8.0, 1e-9);

    // A snapped entity is drawn where it is, its descendants with it: the
    // stage teleported carries the torus.
    local(&fx, stage)->translation.x += 1.0;
    step_and_capture(&fx);
    nph_scene_renderer_snap(fx.sr, stage);
    st = draw_at(&fx, 0.5F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 0U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, world_of(&fx, torus).translation.x,
                          1e-9);
    // A fraction of 1 draws everything as it is and ends the interpolation.
    local(&fx, torus)->translation.x += 1.0;
    step_and_capture(&fx);
    (void)draw_at(&fx, 0.5F);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, world_of(&fx, torus).translation.x - 0.5,
                          1e-6);
    st = draw_at(&fx, 1.0F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 0U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, world_of(&fx, torus).translation.x,
                          1e-9);
    st = draw_at(&fx, 0.5F); // and stays so
    NTEST_EXPECT_EQ_UINT(st.interpolating, 0U);
    NTEST_EXPECT_NEAR_F64(drawn(&fx, torus).translation.x, world_of(&fx, torus).translation.x,
                          1e-9);
    // Despawned while moving: gone from the interpolation too.
    local(&fx, torus)->translation.x += 1.0;
    step_and_capture(&fx);
    NTEST_ASSERT_EQ_INT(nph_world_despawn(fx.world, torus), NPH_RESULT_OK);
    st = draw_at(&fx, 0.5F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 0U);
    NTEST_EXPECT_EQ_UINT(st.instances, 1U);
    scene_teardown(&fx);
}

NTEST(SceneRender, lights_and_the_camera_move_between_their_states_too)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    make_scene_renderer(&fx);
    load_showcase(&fx);
    // The sun turns 60 degrees about Z in a step: half way, 30.
    const NphEntity sun = named(&fx, "sun");
    const NphVec3 dir_from = nph_vec3_neg(nph_vec3_normalize(world_of(&fx, sun).z_axis));
    local(&fx, sun)->rotation =
        nph_quat_mul(nph_quat_from_rotation_z(1.0471976F), local(&fx, sun)->rotation);
    step_and_capture(&fx);
    const NphSceneRenderStats st = draw_at(&fx, 0.5F);
    NTEST_EXPECT_EQ_UINT(st.interpolating, 1U);
    NphSceneRenderer *sr = fx.sr;
    NphRenderLightDesc ld;
    NTEST_ASSERT_EQ_INT(
        nph_render_light_desc(fx.rf.renderer, nph_scene_renderer_light(sr, sun), &ld),
        NPH_RESULT_OK);
    // Turned half way about Z: 30 degrees.
    const NphVec3 want = nph_quat_rotate(nph_quat_from_rotation_z(0.5235988F), dir_from);
    NTEST_EXPECT_NEAR_F32(nph_vec3_dot(nph_vec3_normalize(ld.direction), want), 1.0F, 1e-5F);
    NTEST_EXPECT_TRUE(nph_render_light_is_valid(fx.rf.renderer, nph_scene_renderer_light(sr, sun)));
    NTEST_EXPECT_FALSE(nph_render_light_is_valid(
        fx.rf.renderer, nph_scene_renderer_light(sr, named(&fx, "torus"))));

    // A camera carried by the simulation is drawn between its states; one
    // moved at the frame rate where it is; another camera is a cut, and so
    // is a snapped one.
    const NphComponentId comps[3] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                     NPH_COMPONENT_ID_WORLD_TRANSFORM, fx.ids.camera};
    NphEntity cam;
    NphEntity other;
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &cam), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_world_spawn(fx.world, comps, 3U, &other), NPH_RESULT_OK);
    ((NphCamera *)nph_world_get_mut(fx.world, other, fx.ids.camera))->active = 0;
    local(&fx, other)->translation.x = 50.0;
    (void)draw_at(&fx, 1.0F);
    NphRenderView view = test_view();
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_FALSE(view.camera_cut); // the first camera there is
    local(&fx, cam)->translation.x = 2.0;
    step_and_capture(&fx);
    (void)draw_at(&fx, 0.25F);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_NEAR_F64(view.camera.position.x, 0.5, 1e-6);
    NTEST_EXPECT_FALSE(view.camera_cut);
    (void)draw_at(&fx, 0.75F);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_NEAR_F64(view.camera.position.x, 1.5, 1e-6);
    local(&fx, cam)->translation.x = 3.0; // the frame's own move
    (void)draw_at(&fx, 0.75F);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_NEAR_F64(view.camera.position.x, 3.0, 1e-9);
    // Another camera.
    ((NphCamera *)nph_world_get_mut(fx.world, cam, fx.ids.camera))->active = 0;
    ((NphCamera *)nph_world_get_mut(fx.world, other, fx.ids.camera))->active = 1;
    (void)draw_at(&fx, 1.0F);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_NEAR_F64(view.camera.position.x, 50.0, 1e-9);
    NTEST_EXPECT_TRUE(view.camera_cut);
    (void)draw_at(&fx, 1.0F);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_FALSE(view.camera_cut);
    // Snapped: a cut, drawn where it is.
    local(&fx, other)->translation.x = 60.0;
    step_and_capture(&fx);
    nph_scene_renderer_snap(sr, other);
    (void)draw_at(&fx, 0.5F);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(sr, &view));
    NTEST_EXPECT_NEAR_F64(view.camera.position.x, 60.0, 1e-9);
    NTEST_EXPECT_TRUE(view.camera_cut);
    scene_teardown(&fx);
}

NTEST(SceneRender, create_refuses_an_unprepared_world)
{
    static SceneFixture fx;
    scene_setup(&fx, true, 256U);
    if (fx.rf.dev == NULL) {
        scene_teardown(&fx);
        NTEST_SKIP("no Vulkan device available");
    }
    NphSceneRendererDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.renderer = fx.rf.renderer;
    d.loader = fx.loader;
    NphSceneRenderer *sr = NULL;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_create(&d, &sr), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(sr);
    // A world without the runtime's components.
    NphWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    wd.alloc = nph_allocator_system();
    wd.event_capacity = 16U;
    NphWorld *bare = NULL;
    NTEST_ASSERT_EQ_INT(nph_world_create(&wd, &bare), NPH_RESULT_OK);
    d.world = bare;
    NTEST_ASSERT_EQ_INT(nph_scene_renderer_create(&d, &sr), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NULL(sr);
    nph_world_destroy(bare);
    scene_teardown(&fx);
}
