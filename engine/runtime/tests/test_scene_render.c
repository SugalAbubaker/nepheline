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

    // Removing the Light removes the render light.
    const NphEntity sun = named(&fx, "sun");
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
    const NphQuat want = nph_quat_from_rotation_y(0.5F);
    NTEST_ASSERT_NEAR_F32(nph_f32_abs(nph_quat_dot(view.camera.orientation, want)), 1.0F, 1e-5F);
    // Deactivating it hands the view to the next one.
    c = (NphCamera *)nph_world_get_mut(fx.world, cam, fx.ids.camera);
    c->active = 0;
    (void)sync(&fx);
    NTEST_ASSERT_TRUE(nph_scene_renderer_view(fx.sr, &view));
    NTEST_ASSERT_NEAR_F64(view.camera.position.x, 0.0, 0.0);
    NTEST_ASSERT_NEAR_F32(view.ev100, 15.0F, 0.0F);
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
