// SPDX-License-Identifier: Apache-2.0
// The scene renderer: render instances, lights, the sky, and the view from
// an ECS world. See scene_render.h and docs/runtime.md.
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/profile.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/runtime/scene_render.h>
#include <nepheline/scene/query.h>
#include <nepheline/scene/transform.h>

#include "runtime_internal.h"

#include <string.h>

#define DEFAULT_MAX_ASSETS 4096U

typedef enum RecordFlags {
    REC_MESH = 1U << 0U,    // mirrors a MeshRenderer
    REC_LIGHT = 1U << 1U,   // mirrors a Light
    REC_PENDING = 1U << 2U, // in the pending list, waiting for assets
    REC_FAILED = 1U << 3U,  // its mesh failed: no instance until it changes
    REC_HIDDEN = 1U << 4U,  // the instance is hidden (by the component, a bad transform, the host)
    REC_HOST_HIDDEN = 1U << 5U, // the host hides it (nph_scene_renderer_set_hidden)
    REC_RELOAD = 1U << 6U,      // acquire the assets again (nph_scene_renderer_reload)
} RecordFlags;

typedef struct EntityRecord {
    NphEntity entity; // null while the record is free
    u32 flags;
    // A hash of the MeshRenderer's GUIDs as last acquired. Compared with
    // the component rather than the cached assets' GUIDs, so a reference
    // the cache refused (full, or the GUID of another kind) is not retried,
    // and logged, every time the entity moves.
    u64 source;
    u32 mesh; // asset cache index, RENDER_ASSET_NONE
    u32 materials[NPH_MESH_RENDERER_MATERIALS];
    NphRenderInstance instance;
    NphRenderLight light;
    u32 next_free;
} EntityRecord;

struct NphSceneRenderer {
    NphAllocator *alloc;
    NphRenderer *renderer;
    NphAssetLoader *loader;
    NphWorld *world;
    NphRuntimeComponents ids;
    RenderAssetCache assets;
    EntityRecord *records;
    u32 record_cap;
    u32 record_high;
    u32 free_head;
    u32 live_records;
    NphHashMap by_entity; // u64 entity value -> u32 record index
    NphArrayU32 pending;
    // Asset references of the previous world, released after the next sync
    // has acquired the new world's: assets both use are not reloaded.
    NphArrayU32 retired;
    NphQuery *mesh_query;   // MeshRenderer, WorldTransform (both change-filtered)
    NphQuery *light_query;  // Light, WorldTransform (both change-filtered)
    NphQuery *camera_query; // Camera, WorldTransform
    NphQuery *env_query;    // Environment
    u32 last_tick;
    b32 full_pass; // the next sync looks at every chunk
    b32 has_view;
    b32 has_env;        // `env` was applied to the renderer
    NphEnvironment env; // the sky last applied
    NphRenderCamera camera;
    f32 ev100;
    u32 tonemapper;
    f32 shutter;
    u32 instances;
    u32 lights;
    u32 failed;
    NphSceneRenderStats stats;
};

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

static EntityRecord *find_record(NphSceneRenderer *sr, NphEntity e, u32 *out_index)
{
    const u32 *found = (const u32 *)nph_hashmap_find_u64(&sr->by_entity, e.value);
    if (found == NULL) {
        return NULL;
    }
    if (out_index != NULL) {
        *out_index = *found;
    }
    return &sr->records[*found];
}

static const EntityRecord *find_record_const(const NphSceneRenderer *sr, NphEntity e)
{
    const u32 *found = (const u32 *)nph_hashmap_find_u64(&sr->by_entity, e.value);
    return found != NULL ? &sr->records[*found] : NULL;
}

static NphResult record_for(NphSceneRenderer *sr, NphEntity e, EntityRecord **out)
{
    EntityRecord *records = sr->records;
    NPH_ASSERT(records != NULL); // a bound world has its table
    EntityRecord *existing = find_record(sr, e, NULL);
    if (existing != NULL) {
        *out = existing;
        return NPH_RESULT_OK;
    }
    u32 index = sr->free_head;
    if (index != RENDER_ASSET_NONE) {
        sr->free_head = records[index].next_free;
    } else if (sr->record_high < sr->record_cap) {
        index = sr->record_high++;
    } else {
        return NPH_RESULT_FULL; // more entities than the world's capacity: not reachable
    }
    NPH_TRY(nph_hashmap_insert_u64(&sr->by_entity, e.value, &index));
    EntityRecord *r = &records[index];
    memset(r, 0, sizeof *r);
    r->entity = e;
    r->mesh = RENDER_ASSET_NONE;
    for (u32 m = 0; m < NPH_MESH_RENDERER_MATERIALS; ++m) {
        r->materials[m] = RENDER_ASSET_NONE;
    }
    sr->live_records += 1U;
    *out = r;
    return NPH_RESULT_OK;
}

static void drop_instance(NphSceneRenderer *sr, EntityRecord *r)
{
    if (!nph_render_instance_is_valid(sr->renderer, r->instance)) {
        return;
    }
    nph_render_instance_destroy(sr->renderer, r->instance);
    r->instance = NPH_RENDER_INSTANCE_NULL;
    sr->instances -= 1U;
    sr->stats.destroyed += 1U;
}

// Forgets the record's MeshRenderer: its instance and its assets.
static void release_mesh(NphSceneRenderer *sr, EntityRecord *r)
{
    drop_instance(sr, r);
    render_assets_release(&sr->assets, r->mesh);
    r->mesh = RENDER_ASSET_NONE;
    for (u32 m = 0; m < NPH_MESH_RENDERER_MATERIALS; ++m) {
        render_assets_release(&sr->assets, r->materials[m]);
        r->materials[m] = RENDER_ASSET_NONE;
    }
    if ((r->flags & (u32)REC_FAILED) != 0U) {
        sr->failed -= 1U;
    }
    r->flags &= ~(u32)(REC_MESH | REC_FAILED | REC_HIDDEN | REC_PENDING | REC_RELOAD);
}

static void release_light(NphSceneRenderer *sr, EntityRecord *r)
{
    if (nph_render_light_is_valid(sr->renderer, r->light)) {
        nph_render_light_destroy(sr->renderer, r->light);
        sr->lights -= 1U;
    }
    r->light = NPH_RENDER_LIGHT_NULL;
    r->flags &= ~(u32)REC_LIGHT;
}

static void free_record_if_empty(NphSceneRenderer *sr, u32 index)
{
    EntityRecord *r = &sr->records[index];
    if ((r->flags & (u32)(REC_MESH | REC_LIGHT)) != 0U) {
        return;
    }
    NPH_VERIFY(nph_hashmap_remove_u64(&sr->by_entity, r->entity.value));
    r->entity = NPH_ENTITY_NULL;
    r->flags = 0U;
    r->next_free = sr->free_head;
    sr->free_head = index;
    sr->live_records -= 1U;
}

static void release_record(NphSceneRenderer *sr, u32 index)
{
    EntityRecord *r = &sr->records[index];
    release_mesh(sr, r);
    release_light(sr, r);
    free_record_if_empty(sr, index);
}

static void release_all(NphSceneRenderer *sr)
{
    for (u32 i = 0; i < sr->record_high; ++i) {
        if (!nph_entity_is_null(sr->records[i].entity)) {
            release_record(sr, i);
        }
    }
    NPH_ARRAY_CLEAR(&sr->pending);
}

static void release_retired(NphSceneRenderer *sr)
{
    for (u32 i = 0; i < sr->retired.len; ++i) {
        render_assets_release(&sr->assets, sr->retired.data[i]);
    }
    NPH_ARRAY_CLEAR(&sr->retired);
}

// Moves an asset reference to `retired`, or releases it when the list
// cannot grow (the asset then reloads: slower, not wrong).
static void retire_ref(NphSceneRenderer *sr, u32 *index)
{
    if (*index != RENDER_ASSET_NONE && NPH_ARRAY_PUSH(&sr->retired, *index) != NPH_RESULT_OK) {
        render_assets_release(&sr->assets, *index);
    }
    *index = RENDER_ASSET_NONE;
}

// As release_all, except that the asset references are retired instead of
// released.
static void retire_all(NphSceneRenderer *sr)
{
    for (u32 i = 0; i < sr->record_high; ++i) {
        EntityRecord *r = &sr->records[i];
        if (nph_entity_is_null(r->entity)) {
            continue;
        }
        retire_ref(sr, &r->mesh);
        for (u32 m = 0; m < NPH_MESH_RENDERER_MATERIALS; ++m) {
            retire_ref(sr, &r->materials[m]);
        }
        release_record(sr, i);
    }
    NPH_ARRAY_CLEAR(&sr->pending);
}

// ---------------------------------------------------------------------------
// Binding a world
// ---------------------------------------------------------------------------

static void destroy_queries(NphSceneRenderer *sr)
{
    nph_query_destroy(sr->mesh_query);
    nph_query_destroy(sr->light_query);
    nph_query_destroy(sr->camera_query);
    nph_query_destroy(sr->env_query);
    sr->mesh_query = NULL;
    sr->light_query = NULL;
    sr->camera_query = NULL;
    sr->env_query = NULL;
}

static NphResult bind_world(NphSceneRenderer *sr, NphWorld *world)
{
    if (world == NULL || nph_world_events(world, NULL, NULL) == NULL) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the scene renderer needs a world with an event stream");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphRuntimeComponents ids;
    ids.mesh_renderer = nph_world_find_component(world, NPH_STR("MeshRenderer"));
    ids.light = nph_world_find_component(world, NPH_STR("Light"));
    ids.camera = nph_world_find_component(world, NPH_STR("Camera"));
    ids.environment = nph_world_find_component(world, NPH_STR("Environment"));
    if (ids.mesh_renderer == NPH_COMPONENT_NONE || ids.light == NPH_COMPONENT_NONE ||
        ids.camera == NPH_COMPONENT_NONE || ids.environment == NPH_COMPONENT_NONE) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the world lacks the runtime's components");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 capacity = nph_world_entity_capacity(world);
    if (capacity > sr->record_cap) {
        EntityRecord *grown = NPH_ALLOC_ARRAY_ZEROED(sr->alloc, EntityRecord, capacity);
        if (grown == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (sr->records != NULL) {
            NPH_FREE_ARRAY(sr->alloc, EntityRecord, sr->records, sr->record_cap);
        }
        sr->records = grown;
        sr->record_cap = capacity;
    }
    sr->record_high = 0U;
    sr->free_head = RENDER_ASSET_NONE;
    sr->live_records = 0U;
    const NphQueryTerm mesh_terms[2] = {
        nph_term_changed(nph_term_read(ids.mesh_renderer)),
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM))};
    const NphQueryTerm light_terms[2] = {
        nph_term_changed(nph_term_read(ids.light)),
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM))};
    const NphQueryTerm camera_terms[2] = {nph_term_read(ids.camera),
                                          nph_term_read(NPH_COMPONENT_ID_WORLD_TRANSFORM)};
    const NphQueryTerm env_terms[1] = {nph_term_read(ids.environment)};
    const NphQueryDesc mq = {mesh_terms, 2U};
    const NphQueryDesc lq = {light_terms, 2U};
    const NphQueryDesc cq = {camera_terms, 2U};
    const NphQueryDesc eq = {env_terms, 1U};
    NphResult r = nph_query_create(world, &mq, &sr->mesh_query);
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(world, &lq, &sr->light_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(world, &cq, &sr->camera_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(world, &eq, &sr->env_query);
    }
    if (r != NPH_RESULT_OK) {
        destroy_queries(sr);
        return r;
    }
    sr->world = world;
    sr->ids = ids;
    sr->last_tick = nph_world_tick(world);
    sr->full_pass = 1;
    sr->has_view = 0;
    return NPH_RESULT_OK;
}

NphResult nph_scene_renderer_create(const NphSceneRendererDesc *desc, NphSceneRenderer **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL || desc->renderer == NULL || desc->loader == NULL ||
        desc->world == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphSceneRenderer *sr = NPH_ALLOC_TYPE(desc->alloc, NphSceneRenderer);
    if (sr == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    sr->alloc = desc->alloc;
    sr->renderer = desc->renderer;
    sr->loader = desc->loader;
    NPH_ARRAY_INIT(&sr->pending, desc->alloc);
    NPH_ARRAY_INIT(&sr->retired, desc->alloc);
    const NphHashMapDesc map = nph_hashmap_desc_u64((u32)sizeof(u32));
    NphResult r = nph_hashmap_init(&sr->by_entity, desc->alloc, &map);
    if (r == NPH_RESULT_OK) {
        r = render_assets_init(&sr->assets, desc->alloc, desc->renderer, desc->loader,
                               desc->max_assets != 0U ? desc->max_assets : DEFAULT_MAX_ASSETS,
                               desc->emissive_scale);
    }
    if (r == NPH_RESULT_OK) {
        r = bind_world(sr, desc->world);
    }
    if (r != NPH_RESULT_OK) {
        nph_scene_renderer_destroy(sr);
        return r;
    }
    *out = sr;
    return NPH_RESULT_OK;
}

void nph_scene_renderer_destroy(NphSceneRenderer *sr)
{
    if (sr == NULL) {
        return;
    }
    if (sr->records != NULL) {
        release_all(sr);
        NPH_FREE_ARRAY(sr->alloc, EntityRecord, sr->records, sr->record_cap);
    }
    if (sr->assets.items != NULL) {
        release_retired(sr);
    }
    destroy_queries(sr);
    render_assets_free(&sr->assets);
    nph_hashmap_free(&sr->by_entity);
    NPH_ARRAY_FREE(&sr->pending);
    NPH_ARRAY_FREE(&sr->retired);
    NPH_FREE_TYPE(sr->alloc, NphSceneRenderer, sr);
}

NphResult nph_scene_renderer_set_world(NphSceneRenderer *sr, NphWorld *world)
{
    NPH_ASSERT(sr != NULL);
    if (sr->records != NULL) {
        retire_all(sr);
    }
    nph_hashmap_clear(&sr->by_entity);
    destroy_queries(sr);
    sr->world = NULL;
    return bind_world(sr, world);
}

NphWorld *nph_scene_renderer_world(const NphSceneRenderer *sr)
{
    NPH_ASSERT(sr != NULL);
    return sr->world;
}

// ---------------------------------------------------------------------------
// Synchronisation
// ---------------------------------------------------------------------------

static NphRenderTransform render_transform(const NphWorldTransform *wt)
{
    NphRenderTransform t;
    t.linear = nph_world_transform_linear(wt);
    t.translation = wt->translation;
    return t;
}

// A linear part the renderer can draw (it refuses a zero determinant).
static bool drawable(const NphWorldTransform *wt)
{
    const f32 det = nph_mat3_determinant(nph_world_transform_linear(wt));
    return nph_f32_abs(det) > 1e-24F && nph_f32_is_finite(det);
}

static NphResult mark_pending(NphSceneRenderer *sr, EntityRecord *r)
{
    if ((r->flags & (u32)REC_PENDING) != 0U) {
        return NPH_RESULT_OK;
    }
    r->flags |= (u32)REC_PENDING;
    return NPH_ARRAY_PUSH(&sr->pending, (u32)(r - sr->records));
}

static u64 mesh_source(const NphMeshRenderer *m)
{
    u64 h = nph_hash64(&m->mesh, sizeof m->mesh, 0U);
    for (u32 s = 0; s < NPH_MESH_RENDERER_MATERIALS; ++s) {
        const NphGuid g = nph_mesh_renderer_material(m, s);
        h = nph_hash_combine(h, nph_hash64(&g, sizeof g, 0U));
    }
    return h;
}

static bool hidden_now(const EntityRecord *r, const NphMeshRenderer *m, const NphWorldTransform *wt)
{
    return m->hidden != 0 || !drawable(wt) || (r->flags & (u32)REC_HOST_HIDDEN) != 0U;
}

// The instance's transform (unless hidden) and visibility as of now.
static NphResult apply_visibility(NphSceneRenderer *sr, EntityRecord *r, const NphMeshRenderer *m,
                                  const NphWorldTransform *wt)
{
    const bool hide = hidden_now(r, m, wt);
    if (!hide) {
        const NphRenderTransform t = render_transform(wt);
        NPH_TRY(nph_render_instance_set_transform(sr->renderer, r->instance, &t));
        sr->stats.moved += 1U;
    }
    if (hide != ((r->flags & (u32)REC_HIDDEN) != 0U)) {
        nph_render_instance_set_hidden(sr->renderer, r->instance, hide);
        r->flags ^= (u32)REC_HIDDEN;
    }
    return NPH_RESULT_OK;
}

// The entity's MeshRenderer as of now: new references when the GUIDs
// changed (or a reload asks), the transform and visibility of its instance
// otherwise.
static NphResult mirror_mesh(NphSceneRenderer *sr, EntityRecord *r, const NphMeshRenderer *m,
                             const NphWorldTransform *wt)
{
    const u64 source = mesh_source(m);
    if ((r->flags & (u32)(REC_MESH | REC_RELOAD)) != (u32)REC_MESH || source != r->source) {
        // The new references first, then the old ones go: an asset named
        // before and after is neither unloaded nor reloaded.
        const u32 mesh = render_assets_acquire(&sr->assets, m->mesh, RENDER_ASSET_MESH);
        u32 materials[NPH_MESH_RENDERER_MATERIALS];
        for (u32 s = 0; s < NPH_MESH_RENDERER_MATERIALS; ++s) {
            materials[s] = render_assets_acquire(&sr->assets, nph_mesh_renderer_material(m, s),
                                                 RENDER_ASSET_MATERIAL);
        }
        release_mesh(sr, r);
        r->flags |= (u32)REC_MESH;
        r->source = source;
        r->mesh = mesh;
        memcpy(r->materials, materials, sizeof materials);
        return mark_pending(sr, r);
    }
    if (!nph_render_instance_is_valid(sr->renderer, r->instance)) {
        return NPH_RESULT_OK; // pending or failed: placed when created
    }
    return apply_visibility(sr, r, m, wt);
}

// Creates the instances whose assets have settled.
static NphResult resolve_pending(NphSceneRenderer *sr)
{
    u32 kept = 0;
    NphResult first = NPH_RESULT_OK;
    for (u32 i = 0; i < sr->pending.len; ++i) {
        const u32 index = sr->pending.data[i];
        EntityRecord *r = &sr->records[index];
        if ((r->flags & (u32)REC_PENDING) == 0U || nph_entity_is_null(r->entity)) {
            continue; // released meanwhile
        }
        const NphWorldTransform *wt = NPH_WORLD_GET(
            sr->world, r->entity, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        const NphMeshRenderer *m =
            (const NphMeshRenderer *)nph_world_get(sr->world, r->entity, sr->ids.mesh_renderer);
        if (wt == NULL || m == NULL) {
            r->flags &= ~(u32)REC_PENDING; // the removal event releases it
            continue;
        }
        const RenderAsset *mesh = render_asset(&sr->assets, r->mesh);
        if (mesh == NULL || mesh->state == RENDER_ASSET_FAILED) {
            // No mesh named (a MeshRenderer being set up) draws nothing and
            // is no failure; a mesh that failed or was refused is.
            r->flags &= ~(u32)REC_PENDING;
            if (!nph_guid_is_null(m->mesh)) {
                r->flags |= (u32)REC_FAILED;
                sr->failed += 1U;
            }
            continue;
        }
        bool settled = mesh->state == RENDER_ASSET_READY;
        for (u32 s = 0; s < NPH_MESH_RENDERER_MATERIALS && settled; ++s) {
            settled = render_asset_settled(&sr->assets, r->materials[s]);
        }
        if (!settled || !drawable(wt) || first != NPH_RESULT_OK) {
            sr->pending.data[kept++] = index;
            continue;
        }
        NphRenderMaterial table[NPH_MESH_RENDERER_MATERIALS];
        for (u32 s = 0; s < NPH_MESH_RENDERER_MATERIALS; ++s) {
            const RenderAsset *mat = render_asset(&sr->assets, r->materials[s]);
            table[s] = mat != NULL && mat->state == RENDER_ASSET_READY ? mat->material
                                                                       : NPH_RENDER_MATERIAL_NULL;
        }
        NphRenderInstanceDesc d;
        memset(&d, 0, sizeof d);
        d.mesh = mesh->mesh;
        d.materials = table;
        d.material_count = NPH_MESH_RENDERER_MATERIALS;
        d.transform = render_transform(wt);
        d.hidden = hidden_now(r, m, wt) ? 1 : 0;
        const NphResult cr = nph_render_instance_create(sr->renderer, &d, &r->instance);
        if (cr != NPH_RESULT_OK) {
            first = cr;
            sr->pending.data[kept++] = index;
            continue;
        }
        r->flags &= ~(u32)(REC_PENDING | REC_HIDDEN);
        r->flags |= d.hidden != 0 ? (u32)REC_HIDDEN : 0U;
        sr->instances += 1U;
        sr->stats.created += 1U;
    }
    sr->pending.len = kept;
    return first;
}

static NphRenderLightType light_type(u32 type)
{
    switch (type) {
    case NPH_LIGHT_DIRECTIONAL:
        return NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    case NPH_LIGHT_SPOT:
        return NPH_RENDER_LIGHT_TYPE_SPOT;
    default:
        return NPH_RENDER_LIGHT_TYPE_POINT;
    }
}

// The renderer refuses out-of-order cones and negative values; a light
// being edited passes through such states, so they are clamped instead.
static void light_desc(const NphLight *l, const NphWorldTransform *wt, NphRenderLightDesc *d)
{
    memset(d, 0, sizeof *d);
    d->type = light_type(l->type);
    d->color = l->color;
    d->intensity = l->intensity > 0.0F && nph_f32_is_finite(l->intensity) ? l->intensity : 0.0F;
    d->range = l->range > 0.0F && nph_f32_is_finite(l->range) ? l->range : 0.0F;
    d->position = wt->translation;
    d->direction = nph_vec3_normalize_or(nph_vec3_neg(wt->z_axis), nph_vec3(0.0F, -1.0F, 0.0F));
    const f32 half_pi = 1.5707963F;
    const f32 outer = nph_f32_clamp(l->outer_cone, 1e-3F, half_pi);
    d->outer_cone = outer;
    d->inner_cone = nph_f32_clamp(l->inner_cone, 0.0F, outer * 0.999F);
    d->shadows = l->shadows != 0 ? 1 : 0;
}

static NphResult mirror_light(NphSceneRenderer *sr, EntityRecord *r, const NphLight *l,
                              const NphWorldTransform *wt)
{
    r->flags |= (u32)REC_LIGHT;
    NphRenderLightDesc d;
    light_desc(l, wt, &d);
    if (nph_render_light_is_valid(sr->renderer, r->light)) {
        return nph_render_light_update(sr->renderer, r->light, &d);
    }
    NPH_TRY(nph_render_light_create(sr->renderer, &d, &r->light));
    sr->lights += 1U;
    return NPH_RESULT_OK;
}

static void start_iter(const NphSceneRenderer *sr, const NphQuery *q, u32 now, NphQueryIter *it)
{
    if (sr->full_pass) {
        nph_query_iter(q, it);
    } else {
        nph_query_iter_since(q, sr->last_tick, now, it);
    }
}

static NphResult sync_meshes(NphSceneRenderer *sr, u32 now)
{
    NphQueryIter it;
    start_iter(sr, sr->mesh_query, now, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const NphMeshRenderer *m = NPH_CHUNK_COLUMN(&v, 0, NphMeshRenderer);
        const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
        for (u32 i = 0; i < v.count; ++i) {
            EntityRecord *r = NULL;
            NPH_TRY(record_for(sr, v.entities[i], &r));
            NPH_TRY(mirror_mesh(sr, r, &m[i], &wt[i]));
        }
    }
    return NPH_RESULT_OK;
}

static NphResult sync_lights(NphSceneRenderer *sr, u32 now)
{
    NphQueryIter it;
    start_iter(sr, sr->light_query, now, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const NphLight *l = NPH_CHUNK_COLUMN(&v, 0, NphLight);
        const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
        for (u32 i = 0; i < v.count; ++i) {
            EntityRecord *r = NULL;
            NPH_TRY(record_for(sr, v.entities[i], &r));
            NPH_TRY(mirror_light(sr, r, &l[i], &wt[i]));
        }
    }
    return NPH_RESULT_OK;
}

static bool vec3_bits_equal(NphVec3 a, NphVec3 b)
{
    return nph_f32_to_bits(a.x) == nph_f32_to_bits(b.x) &&
           nph_f32_to_bits(a.y) == nph_f32_to_bits(b.y) &&
           nph_f32_to_bits(a.z) == nph_f32_to_bits(b.z);
}

// By bits, so a NaN someone typed in does not make every frame differ.
static bool env_equal(const NphEnvironment *a, const NphEnvironment *b)
{
    return vec3_bits_equal(a->zenith, b->zenith) && vec3_bits_equal(a->horizon, b->horizon) &&
           vec3_bits_equal(a->ground, b->ground) &&
           nph_f32_to_bits(a->luminance) == nph_f32_to_bits(b->luminance) &&
           nph_f32_to_bits(a->intensity) == nph_f32_to_bits(b->intensity);
}

// The sky: the world's first Environment (by chunk order), or the
// component's defaults when it has none, applied when it differs from the
// sky last applied. Comparing values rather than watching for changes
// handles the Environment being added, edited, or removed alike, and a
// world switch to a world with the same sky (play-in-editor) recomputes
// nothing.
static NphResult sync_environment(NphSceneRenderer *sr)
{
    NphQueryIter it;
    nph_query_iter(sr->env_query, &it);
    NphChunkView v;
    const NphEnvironment *e = (const NphEnvironment *)NphEnvironment_component()->defaults;
    if (nph_query_next(&it, &v)) {
        e = NPH_CHUNK_COLUMN(&v, 0, NphEnvironment);
    }
    if (sr->has_env && env_equal(&sr->env, e)) {
        return NPH_RESULT_OK;
    }
    NphRenderEnvironmentDesc d;
    memset(&d, 0, sizeof d);
    d.source = NPH_RENDER_ENV_SKY;
    nph_render_sky_desc_default(&d.sky);
    const f32 lum = e->luminance > 0.0F ? e->luminance : 0.0F;
    d.sky.zenith = nph_vec3_scale(e->zenith, lum);
    d.sky.horizon = nph_vec3_scale(e->horizon, lum);
    d.sky.ground = nph_vec3_scale(e->ground, lum);
    d.intensity = e->intensity > 0.0F ? e->intensity : 1e-6F; // 0 would mean "the default 1"
    NPH_TRY(nph_render_set_environment(sr->renderer, &d));
    sr->env = *e;
    sr->has_env = 1;
    return NPH_RESULT_OK;
}

// The view: the active camera with the lowest entity slot.
static void sync_camera(NphSceneRenderer *sr)
{
    NphQueryIter it;
    nph_query_iter(sr->camera_query, &it);
    NphChunkView v;
    u32 best_slot = NPH_U32_MAX;
    sr->has_view = 0;
    while (nph_query_next(&it, &v)) {
        const NphCamera *c = NPH_CHUNK_COLUMN(&v, 0, NphCamera);
        const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
        for (u32 i = 0; i < v.count; ++i) {
            if (c[i].active == 0) {
                continue;
            }
            const u32 slot = nph_world_entity_index(sr->world, v.entities[i]);
            if (slot >= best_slot) {
                continue;
            }
            best_slot = slot;
            // An orthonormal frame from the entity's axes (scale removed).
            const NphVec3 z = nph_vec3_normalize_or(wt[i].z_axis, nph_vec3_unit_z());
            NphVec3 y =
                nph_vec3_sub(wt[i].y_axis, nph_vec3_scale(z, nph_vec3_dot(wt[i].y_axis, z)));
            y = nph_vec3_normalize_or(y, nph_vec3_perpendicular(z));
            const NphVec3 x = nph_vec3_cross(y, z);
            sr->camera.position = wt[i].translation;
            sr->camera.orientation =
                nph_quat_normalize(nph_quat_from_mat3(nph_mat3_from_cols(x, y, z)));
            sr->camera.yfov = nph_f32_clamp(c[i].yfov, 0.01F, 3.0F);
            sr->camera.znear = c[i].znear > 1e-5F ? c[i].znear : 1e-5F;
            sr->ev100 = c[i].ev100;
            // A second at most: the renderer refuses what is not finite.
            sr->shutter = c[i].shutter > 0.0F ? nph_f32_min(c[i].shutter, 1.0F) : 0.0F;
            sr->tonemapper = c[i].tonemapper <= (u32)NPH_RENDER_TONEMAPPER_AGX
                                 ? c[i].tonemapper
                                 : (u32)NPH_RENDER_TONEMAPPER_ACES;
            sr->has_view = 1;
        }
    }
}

// Despawns and removals since the last clear of the world's events.
static void apply_events(NphSceneRenderer *sr)
{
    u32 count = 0;
    u32 dropped = 0;
    const NphWorldEvent *events = nph_world_events(sr->world, &count, &dropped);
    for (u32 i = 0; i < count; ++i) {
        const NphWorldEvent *e = &events[i];
        u32 index = 0;
        EntityRecord *r = find_record(sr, e->entity, &index);
        if (r == NULL) {
            continue;
        }
        if (e->type == NPH_WORLD_EVENT_DESPAWNED) {
            release_record(sr, index);
        } else if (e->type == NPH_WORLD_EVENT_REMOVED) {
            if (e->component == sr->ids.mesh_renderer ||
                e->component == NPH_COMPONENT_ID_WORLD_TRANSFORM) {
                release_mesh(sr, r);
            }
            if (e->component == sr->ids.light || e->component == NPH_COMPONENT_ID_WORLD_TRANSFORM) {
                release_light(sr, r);
            }
            free_record_if_empty(sr, index);
        }
    }
    if (dropped == 0U) {
        return;
    }
    // The stream overflowed: check every record against the world.
    NPH_ASSERT(sr->records != NULL);
    for (u32 i = 0; i < sr->record_high; ++i) {
        EntityRecord *r = &sr->records[i];
        if (nph_entity_is_null(r->entity)) {
            continue;
        }
        if (!nph_world_is_alive(sr->world, r->entity)) {
            release_record(sr, i);
            continue;
        }
        const bool placed = nph_world_has(sr->world, r->entity, NPH_COMPONENT_ID_WORLD_TRANSFORM);
        if (!placed || !nph_world_has(sr->world, r->entity, sr->ids.mesh_renderer)) {
            release_mesh(sr, r);
        }
        if (!placed || !nph_world_has(sr->world, r->entity, sr->ids.light)) {
            release_light(sr, r);
        }
        free_record_if_empty(sr, i);
    }
}

NphResult nph_scene_renderer_sync(NphSceneRenderer *sr, NphSceneRenderStats *out)
{
    NPH_ASSERT(sr != NULL && sr->world != NULL);
    NPH_PROFILE_BEGIN("runtime.scene_render");
    sr->stats.created = 0U;
    sr->stats.destroyed = 0U;
    sr->stats.moved = 0U;
    apply_events(sr);
    NphResult r = render_assets_poll(&sr->assets);
    // Like a system run: a fresh tick; changes stamped after the previous
    // sync's tick are this sync's (scene/world.h "Change ticks").
    nph_world_clamp_ticks(sr->world, &sr->last_tick, 1U);
    const u32 now = nph_world_advance_tick(sr->world);
    NphResult step = sync_meshes(sr, now);
    r = r != NPH_RESULT_OK ? r : step;
    step = resolve_pending(sr);
    r = r != NPH_RESULT_OK ? r : step;
    release_retired(sr); // the new world holds what it shares with the old one
    step = sync_lights(sr, now);
    r = r != NPH_RESULT_OK ? r : step;
    step = sync_environment(sr);
    r = r != NPH_RESULT_OK ? r : step;
    sync_camera(sr);
    sr->last_tick = now;
    sr->full_pass = r != NPH_RESULT_OK; // after a failure, look at everything again
    const RenderAssetCounts counts = render_assets_counts(&sr->assets);
    sr->stats.entities = sr->live_records;
    sr->stats.instances = sr->instances;
    sr->stats.pending = sr->pending.len;
    sr->stats.failed = sr->failed;
    sr->stats.lights = sr->lights;
    sr->stats.assets_loading = counts.loading;
    sr->stats.assets_ready = counts.ready;
    sr->stats.assets_failed = counts.failed;
    if (out != NULL) {
        *out = sr->stats;
    }
    NPH_PROFILE_END();
    return r;
}

void nph_scene_renderer_wait(NphSceneRenderer *sr)
{
    NPH_ASSERT(sr != NULL);
    (void)render_assets_wait(&sr->assets);
}

bool nph_scene_renderer_view(const NphSceneRenderer *sr, NphRenderView *view)
{
    NPH_ASSERT(sr != NULL && view != NULL);
    if (!sr->has_view) {
        return false;
    }
    view->camera = sr->camera;
    view->ev100 = sr->ev100;
    view->tonemapper = (NphRenderTonemapper)sr->tonemapper;
    view->shutter = sr->shutter;
    return true;
}

const NphAsset *nph_scene_renderer_mesh_asset(const NphSceneRenderer *sr, NphEntity entity)
{
    NPH_ASSERT(sr != NULL);
    const EntityRecord *r = find_record_const(sr, entity);
    if (r == NULL) {
        return NULL;
    }
    const RenderAsset *mesh = render_asset(&sr->assets, r->mesh);
    if (mesh == NULL || mesh->state != RENDER_ASSET_READY) {
        return NULL;
    }
    return nph_asset_get(sr->loader, mesh->handle);
}

NphRenderInstance nph_scene_renderer_instance(const NphSceneRenderer *sr, NphEntity entity)
{
    NPH_ASSERT(sr != NULL);
    const EntityRecord *r = find_record_const(sr, entity);
    return r != NULL ? r->instance : NPH_RENDER_INSTANCE_NULL;
}

bool nph_scene_renderer_set_hidden(NphSceneRenderer *sr, NphEntity entity, bool hidden)
{
    NPH_ASSERT(sr != NULL);
    EntityRecord *r = find_record(sr, entity, NULL);
    if (r == NULL || (r->flags & (u32)REC_MESH) == 0U) {
        return false;
    }
    if (hidden) {
        r->flags |= (u32)REC_HOST_HIDDEN;
    } else {
        r->flags &= ~(u32)REC_HOST_HIDDEN;
    }
    // At once for an instance that exists, with the transform it missed
    // while hidden; one created later starts this way.
    const NphMeshRenderer *m =
        (const NphMeshRenderer *)nph_world_get(sr->world, entity, sr->ids.mesh_renderer);
    const NphWorldTransform *wt =
        NPH_WORLD_GET(sr->world, entity, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    if (m != NULL && wt != NULL && nph_render_instance_is_valid(sr->renderer, r->instance)) {
        const NphResult res = apply_visibility(sr, r, m, wt);
        if (res != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "showing an instance again: %s",
                          nph_result_string(res));
        }
    }
    return true;
}

static bool forgotten(const RenderAssetCache *c, u32 index)
{
    const RenderAsset *a = render_asset(c, index);
    return a != NULL && a->state != RENDER_ASSET_FREE && a->forgotten != 0;
}

u32 nph_scene_renderer_reload(NphSceneRenderer *sr, NphGuid guid)
{
    NPH_ASSERT(sr != NULL);
    RenderAssetCache *c = &sr->assets;
    const u32 index = render_assets_find(c, guid);
    if (index == RENDER_ASSET_NONE) {
        return 0U;
    }
    render_assets_forget(c, index);
    // A texture: the materials sampling it are made again with the new one.
    if (c->items[index].kind == (u32)RENDER_ASSET_TEXTURE) {
        for (u32 i = 0; i < c->high; ++i) {
            const RenderAsset *a = &c->items[i];
            if (a->state == RENDER_ASSET_FREE || a->kind != (u32)RENDER_ASSET_MATERIAL) {
                continue;
            }
            for (u32 t = 0; t < RENDER_MATERIAL_TEXTURES; ++t) {
                if (a->textures[t] == index) {
                    render_assets_forget(c, i);
                    break;
                }
            }
        }
    }
    // Every entity drawing a forgotten entry acquires its assets again at
    // the next sync, which looks at every chunk.
    u32 count = 0U;
    for (u32 i = 0; i < sr->record_high; ++i) {
        EntityRecord *r = &sr->records[i];
        if (nph_entity_is_null(r->entity) || (r->flags & (u32)REC_MESH) == 0U) {
            continue;
        }
        bool uses = forgotten(c, r->mesh);
        for (u32 s = 0; s < NPH_MESH_RENDERER_MATERIALS && !uses; ++s) {
            uses = forgotten(c, r->materials[s]);
        }
        if (uses) {
            r->flags |= (u32)REC_RELOAD;
            ++count;
        }
    }
    if (count != 0U) {
        sr->full_pass = 1;
    }
    return count;
}
