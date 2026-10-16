// SPDX-License-Identifier: Apache-2.0
// Spawning a cooked scene (asset/scene.h) as entities. See scene_render.h.
#include <nepheline/asset/scene.h>
#include <nepheline/core/assert.h>
#include <nepheline/runtime/scene_render.h>
#include <nepheline/scene/transform.h>

#include "runtime_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct Importer {
    NphWorld *world;
    NphRuntimeComponents ids;
    const NphSceneView *view;
    NphEntity *created; // every entity spawned so far, for the rollback
    u32 created_count;
    u32 created_cap;
    NphEntity *node_entities; // per node
    u8 *node_used;            // per node: its entity already holds a MeshRenderer / Light / Camera
} Importer;

enum {
    USED_MESH = 1U,
    USED_LIGHT = 2U,
    USED_CAMERA = 4U,
};

static NphResult spawn(Importer *im, NphStr name, const NphLocalTransform *local, NphEntity parent,
                       NphEntity *out)
{
    if (im->created_count == im->created_cap) {
        return NPH_RESULT_FULL; // sized from the scene's counts: not reachable
    }
    static const NphComponentId comps[3] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                            NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NPH_TRY(nph_world_spawn(im->world, comps, 3U, out));
    im->created[im->created_count++] = *out;
    NphName n;
    (void)nph_text_set(&n.text, name);
    NPH_TRY(nph_world_set(im->world, *out, NPH_COMPONENT_ID_NAME, &n));
    NPH_TRY(nph_world_set(im->world, *out, NPH_COMPONENT_ID_LOCAL_TRANSFORM, local));
    return nph_entity_is_null(parent) ? NPH_RESULT_OK
                                      : nph_world_set_parent(im->world, *out, parent);
}

// The entity that receives a component of kind `used` at `node`: the
// node's own, or a child when the node's own already has one.
static NphResult holder(Importer *im, u32 node, u8 used, const char *suffix, NphEntity *out)
{
    if ((im->node_used[node] & used) == 0U) {
        im->node_used[node] |= used;
        *out = im->node_entities[node];
        return NPH_RESULT_OK;
    }
    char name[NPH_TEXT_CAPACITY];
    const NphStr base = nph_scene_node_name(im->view, node);
    const int n = snprintf(name, sizeof name, "%" NPH_STR_FMT " %s", NPH_STR_ARG(base), suffix);
    const NphLocalTransform identity = nph_local_transform_identity();
    return spawn(im, nph_str(name, n > 0 ? (u64)nph_min_u32((u32)n, NPH_TEXT_CAPACITY - 1U) : 0U),
                 &identity, im->node_entities[node], out);
}

static NphResult import(Importer *im, const NphSceneImportDesc *d, NphEntity *out_root)
{
    const NphSceneView *v = im->view;
    const NphSceneHeader *h = v->header;
    const char *root_name = d->name != NULL ? d->name : "scene";
    NphEntity root;
    NPH_TRY(spawn(im, nph_str_from_cstr(root_name), &d->root_transform, d->parent, &root));
    *out_root = root;
    // Nodes: parents come before their children in a cooked scene.
    for (u32 i = 0; i < h->node_count; ++i) {
        const NphSceneNode *node = &v->nodes[i];
        const NphLocalTransform local = nph_local_transform(nph_dvec3_from_vec3(node->translation),
                                                            node->rotation, node->scale);
        const NphEntity parent =
            node->parent == NPH_SCENE_NO_NODE ? root : im->node_entities[node->parent];
        NPH_TRY(spawn(im, nph_scene_node_name(v, i), &local, parent, &im->node_entities[i]));
    }
    for (u32 i = 0; i < h->instance_count; ++i) {
        const NphSceneMeshInstance *inst = &v->instances[i];
        NphMeshRenderer m;
        nph_component_init_default(NphMeshRenderer_component(), &m);
        m.mesh = inst->mesh;
        const u32 slots = nph_min_u32(inst->material_count, NPH_MESH_RENDERER_MATERIALS);
        if (inst->material_count > NPH_MESH_RENDERER_MATERIALS) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                         "a mesh instance with %u materials: slots past %u use the default",
                         inst->material_count, NPH_MESH_RENDERER_MATERIALS);
        }
        for (u32 s = 0; s < slots; ++s) {
            nph_mesh_renderer_set_material(&m, s, v->materials[inst->material_offset + s]);
        }
        NphEntity e;
        NPH_TRY(holder(im, inst->node, USED_MESH, "mesh", &e));
        NPH_TRY(nph_world_add(im->world, e, im->ids.mesh_renderer, &m));
    }
    for (u32 i = 0; i < h->light_count; ++i) {
        const NphSceneLight *src = &v->lights[i];
        NphLight l;
        nph_component_init_default(NphLight_component(), &l);
        l.type = src->type <= (u32)NPH_SCENE_LIGHT_SPOT ? src->type : (u32)NPH_LIGHT_POINT;
        l.color = src->color;
        l.intensity = src->intensity;
        l.range = src->range;
        l.inner_cone = src->inner_cone;
        l.outer_cone = src->outer_cone;
        NphEntity e;
        NPH_TRY(holder(im, src->node, USED_LIGHT, "light", &e));
        NPH_TRY(nph_world_add(im->world, e, im->ids.light, &l));
    }
    for (u32 i = 0; i < h->camera_count; ++i) {
        const NphSceneCamera *src = &v->cameras[i];
        if (src->type != NPH_SCENE_CAMERA_PERSPECTIVE) {
            continue;
        }
        NphCamera c;
        nph_component_init_default(NphCamera_component(), &c);
        c.yfov = src->yfov;
        c.znear = src->znear > 0.0F ? src->znear : c.znear;
        c.active = 0; // the importer never takes the view
        NphEntity e;
        NPH_TRY(holder(im, src->node, USED_CAMERA, "camera", &e));
        NPH_TRY(nph_world_add(im->world, e, im->ids.camera, &c));
    }
    return NPH_RESULT_OK;
}

NphResult nph_scene_import_cooked(const NphSceneImportDesc *desc, NphEntity *out_root,
                                  u32 *out_count)
{
    NPH_ASSERT(desc != NULL);
    if (out_root != NULL) {
        *out_root = NPH_ENTITY_NULL;
    }
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (desc->world == NULL || desc->loader == NULL || nph_guid_is_null(desc->scene)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Importer im;
    memset(&im, 0, sizeof im);
    im.world = desc->world;
    NPH_TRY(nph_runtime_register_components(desc->world, &im.ids));
    NphAssetHandle handle = NPH_ASSET_HANDLE_NULL;
    NPH_TRY(nph_asset_load_guid(desc->loader, desc->scene, &handle));
    NphSceneView view;
    NphResult r = nph_scene_view(nph_asset_get(desc->loader, handle), &view);
    NphAllocator *a = desc->scratch != NULL ? desc->scratch : nph_allocator_system();
    u32 node_cap = 1U;
    if (r == NPH_RESULT_OK) {
        const NphSceneHeader *h = view.header;
        im.view = &view;
        node_cap = h->node_count != 0U ? h->node_count : 1U;
        // Every node, one extra per instance, light, and camera, the root.
        im.created_cap = 1U + h->node_count + h->instance_count + h->light_count + h->camera_count;
        im.created = NPH_ALLOC_ARRAY(a, NphEntity, im.created_cap);
        im.node_entities = NPH_ALLOC_ARRAY_ZEROED(a, NphEntity, node_cap);
        im.node_used = NPH_ALLOC_ARRAY_ZEROED(a, u8, node_cap);
        if (im.created == NULL || im.node_entities == NULL || im.node_used == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    NphEntity root = NPH_ENTITY_NULL;
    if (r == NPH_RESULT_OK) {
        r = import(&im, desc, &root);
    }
    if (r != NPH_RESULT_OK) {
        // All or nothing: children first does not matter, despawn detaches.
        for (u32 i = im.created_count; i > 0U; --i) {
            NPH_IGNORE_RESULT(nph_world_despawn(desc->world, im.created[i - 1U]));
        }
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "importing a cooked scene failed: %s",
                      nph_result_string(r));
    } else {
        if (out_root != NULL) {
            *out_root = root;
        }
        if (out_count != NULL) {
            *out_count = im.created_count;
        }
    }
    if (im.created != NULL) {
        NPH_FREE_ARRAY(a, NphEntity, im.created, im.created_cap);
    }
    if (im.node_entities != NULL) {
        NPH_FREE_ARRAY(a, NphEntity, im.node_entities, node_cap);
    }
    if (im.node_used != NULL) {
        NPH_FREE_ARRAY(a, u8, im.node_used, node_cap);
    }
    nph_asset_release(desc->loader, handle);
    return r;
}
