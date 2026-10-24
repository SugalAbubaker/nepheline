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
    REC_SEEN = 1U << 7U,        // `seen` holds its world transform as last mirrored
} RecordFlags;

#define MOVING_NONE NPH_U32_MAX

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
    u32 moving; // its entry in `moving`, or MOVING_NONE
    u32 next_free;
} EntityRecord;

// A world transform as translation, rotation, and stretch (what is left of
// the linear part with the rotation taken out, R^T M: scale and shear), so a
// turn interpolates as a turn and a stretch as a stretch rather than a
// matrix's columns shrinking through the middle of a turn (Shoemake and
// Duff, "Matrix Animation and Polar Decomposition", Graphics Interface 1992,
// with the rotation from Gram-Schmidt rather than the polar factor: exact at
// both ends, smooth between).
typedef struct Pose {
    NphDVec3 translation;
    NphQuat rotation;
    NphMat3 stretch;
} Pose;

// What the last capture saw the simulation move: where an entity was at
// the capture before and where it is now.
typedef struct Moving {
    u32 record;
    b32 fresh; // the latest capture saw it move
    Pose from;
    Pose to;
} Moving;

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
    // Per entity slot of the world: its record, or RENDER_ASSET_NONE (the
    // record's handle tells its entity from a later one in the slot).
    u32 *slot_record;
    u32 slot_cap;
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
    // Interpolation (scene_render.h): every record's world transform as last
    // mirrored, and what the last capture saw moving.
    NphWorldTransform *seen; // per record index
    u32 seen_cap;
    Moving *moving;
    u32 moving_count;
    u32 moving_cap;
    NphArrayU32 walk;        // nph_scene_renderer_snap's
    NphEntity camera_entity; // the view's camera as of the last sync
    NphWorldTransform camera_seen;
    b32 camera_has_seen;
    b32 camera_moving;
    b32 camera_cut; // since the last sync: the view does not continue that sync's
    b32 view_cut;   // the last sync's view is a cut
    Moving camera_motion;
    b32 has_view;
    b32 has_env;        // `env` was applied to the renderer
    NphEnvironment env; // the sky last applied
    NphRenderCamera camera;
    f32 ev100;
    u32 tonemapper;
    f32 shutter;
    f32 aperture;
    f32 focus_distance;
    f32 sensor_height;
    u32 instances;
    u32 lights;
    u32 failed;
    NphSceneRenderStats stats;
};

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

// The record index of `e`, or RENDER_ASSET_NONE.
static u32 record_index(const NphSceneRenderer *sr, NphEntity e)
{
    if (sr->world == NULL || nph_entity_is_null(e)) {
        return RENDER_ASSET_NONE;
    }
    const u32 slot = nph_world_handle_slot(sr->world, e);
    if (slot >= sr->slot_cap) {
        return RENDER_ASSET_NONE;
    }
    const u32 index = sr->slot_record[slot];
    return index != RENDER_ASSET_NONE && sr->records[index].entity.value == e.value
               ? index
               : RENDER_ASSET_NONE;
}

static EntityRecord *find_record(NphSceneRenderer *sr, NphEntity e, u32 *out_index)
{
    const u32 index = record_index(sr, e);
    if (index == RENDER_ASSET_NONE) {
        return NULL;
    }
    if (out_index != NULL) {
        *out_index = index;
    }
    return &sr->records[index];
}

static const EntityRecord *find_record_const(const NphSceneRenderer *sr, NphEntity e)
{
    const u32 index = record_index(sr, e);
    return index != RENDER_ASSET_NONE ? &sr->records[index] : NULL;
}

static void release_record(NphSceneRenderer *sr, u32 index);

static NphResult record_for(NphSceneRenderer *sr, NphEntity e, EntityRecord **out)
{
    EntityRecord *records = sr->records;
    NPH_ASSERT(records != NULL); // a bound world has its table
    EntityRecord *existing = find_record(sr, e, NULL);
    if (existing != NULL) {
        *out = existing;
        return NPH_RESULT_OK;
    }
    const u32 slot = nph_world_handle_slot(sr->world, e);
    NPH_ASSERT(slot < sr->slot_cap);
    if (sr->slot_record[slot] != RENDER_ASSET_NONE) {
        // An entity that went from this slot without its events being read
        // (an overflowed stream): its record goes now.
        release_record(sr, sr->slot_record[slot]);
    }
    u32 index = sr->free_head;
    if (index != RENDER_ASSET_NONE) {
        sr->free_head = records[index].next_free;
    } else if (sr->record_high < sr->record_cap) {
        index = sr->record_high++;
    } else {
        return NPH_RESULT_FULL; // more entities than the world's capacity: not reachable
    }
    if (index >= sr->seen_cap) {
        const u32 cap = nph_min_u32(nph_max_u32(64U, 2U * sr->seen_cap), sr->record_cap);
        NphWorldTransform *grown = NPH_ALLOC_ARRAY(sr->alloc, NphWorldTransform, cap);
        if (grown == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (sr->seen != NULL) {
            memcpy(grown, sr->seen, (u64)sr->seen_cap * sizeof *grown);
            NPH_FREE_ARRAY(sr->alloc, NphWorldTransform, sr->seen, sr->seen_cap);
        }
        sr->seen = grown;
        sr->seen_cap = cap;
    }
    sr->slot_record[slot] = index;
    EntityRecord *r = &records[index];
    memset(r, 0, sizeof *r);
    r->entity = e;
    r->moving = MOVING_NONE;
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

static void moving_remove(NphSceneRenderer *sr, u32 i)
{
    NPH_ASSERT(i < sr->moving_count);
    sr->records[sr->moving[i].record].moving = MOVING_NONE;
    const u32 last = sr->moving_count - 1U;
    if (i != last) {
        sr->moving[i] = sr->moving[last];
        sr->records[sr->moving[i].record].moving = i;
    }
    sr->moving_count = last;
}

static void free_record_if_empty(NphSceneRenderer *sr, u32 index)
{
    EntityRecord *r = &sr->records[index];
    if ((r->flags & (u32)(REC_MESH | REC_LIGHT)) != 0U) {
        return;
    }
    if (r->moving != MOVING_NONE) {
        moving_remove(sr, r->moving);
    }
    const u32 slot = sr->world != NULL ? nph_world_handle_slot(sr->world, r->entity) : NPH_U32_MAX;
    if (slot < sr->slot_cap && sr->slot_record[slot] == index) {
        sr->slot_record[slot] = RENDER_ASSET_NONE;
    }
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
    ids.rigid_body = nph_world_find_component(world, NPH_STR("RigidBody"));
    ids.collider = nph_world_find_component(world, NPH_STR("Collider"));
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
    if (capacity > sr->slot_cap) {
        u32 *slots = NPH_ALLOC_ARRAY(sr->alloc, u32, capacity);
        if (slots == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (sr->slot_record != NULL) {
            NPH_FREE_ARRAY(sr->alloc, u32, sr->slot_record, sr->slot_cap);
        }
        sr->slot_record = slots;
        sr->slot_cap = capacity;
    }
    memset(sr->slot_record, 0xFF, (u64)sr->slot_cap * sizeof(u32)); // RENDER_ASSET_NONE
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
    sr->moving_count = 0U;
    sr->camera_entity = NPH_ENTITY_NULL;
    sr->camera_has_seen = 0;
    sr->camera_moving = 0;
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
    NPH_ARRAY_INIT(&sr->walk, desc->alloc);
    NphResult r = render_assets_init(&sr->assets, desc->alloc, desc->renderer, desc->loader,
                                     desc->max_assets != 0U ? desc->max_assets : DEFAULT_MAX_ASSETS,
                                     desc->emissive_scale);
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
    if (sr->slot_record != NULL) {
        NPH_FREE_ARRAY(sr->alloc, u32, sr->slot_record, sr->slot_cap);
    }
    NPH_ARRAY_FREE(&sr->pending);
    NPH_ARRAY_FREE(&sr->retired);
    NPH_ARRAY_FREE(&sr->walk);
    if (sr->seen != NULL) {
        NPH_FREE_ARRAY(sr->alloc, NphWorldTransform, sr->seen, sr->seen_cap);
    }
    if (sr->moving != NULL) {
        NPH_FREE_ARRAY(sr->alloc, Moving, sr->moving, sr->moving_cap);
    }
    NPH_FREE_TYPE(sr->alloc, NphSceneRenderer, sr);
}

NphResult nph_scene_renderer_set_world(NphSceneRenderer *sr, NphWorld *world)
{
    NPH_ASSERT(sr != NULL);
    if (sr->records != NULL) {
        retire_all(sr);
    }
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

// The instance's transform (unless hidden, or `place` is false: it is
// moving, and drawn between its states once the sync has mirrored the rest)
// and visibility as of now.
static NphResult apply_visibility(NphSceneRenderer *sr, EntityRecord *r, const NphMeshRenderer *m,
                                  const NphWorldTransform *wt, bool place)
{
    const bool hide = hidden_now(r, m, wt);
    if (!hide && place) {
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
                             const NphWorldTransform *wt, bool place)
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
    return apply_visibility(sr, r, m, wt, place);
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
        // The table up to the last material named: the slots past it draw
        // with the default material as null ones would, without taking the
        // renderer's material slots (4 per instance by default).
        NphRenderMaterial table[NPH_MESH_RENDERER_MATERIALS];
        u32 used = 0U;
        for (u32 s = 0; s < NPH_MESH_RENDERER_MATERIALS; ++s) {
            const RenderAsset *mat = render_asset(&sr->assets, r->materials[s]);
            table[s] = mat != NULL && mat->state == RENDER_ASSET_READY ? mat->material
                                                                       : NPH_RENDER_MATERIAL_NULL;
            used = table[s].value != 0U ? s + 1U : used;
        }
        NphRenderInstanceDesc d;
        memset(&d, 0, sizeof d);
        d.mesh = mesh->mesh;
        d.materials = table;
        d.material_count = used;
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
    if (r->moving != MOVING_NONE && nph_render_light_is_valid(sr->renderer, r->light)) {
        return NPH_RESULT_OK; // drawn between its states once the sync has mirrored the rest
    }
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

static bool vec3_bits_equal(NphVec3 a, NphVec3 b)
{
    return nph_f32_to_bits(a.x) == nph_f32_to_bits(b.x) &&
           nph_f32_to_bits(a.y) == nph_f32_to_bits(b.y) &&
           nph_f32_to_bits(a.z) == nph_f32_to_bits(b.z);
}

static bool dvec3_bits_equal(NphDVec3 a, NphDVec3 b)
{
    return nph_f64_to_bits(a.x) == nph_f64_to_bits(b.x) &&
           nph_f64_to_bits(a.y) == nph_f64_to_bits(b.y) &&
           nph_f64_to_bits(a.z) == nph_f64_to_bits(b.z);
}

// By fields: the struct's padding holds whatever its writer left there.
static bool transform_bits_equal(const NphWorldTransform *a, const NphWorldTransform *b)
{
    return dvec3_bits_equal(a->translation, b->translation) &&
           vec3_bits_equal(a->x_axis, b->x_axis) && vec3_bits_equal(a->y_axis, b->y_axis) &&
           vec3_bits_equal(a->z_axis, b->z_axis);
}

// A record seen at `wt` by a sync: a change since the capture (made at the
// frame rate: a game's camera rig, an edit) ends its interpolation, and
// `seen` follows. False when it is moving, drawn between its states later.
static bool see(NphSceneRenderer *sr, EntityRecord *r, const NphWorldTransform *wt)
{
    const u32 index = (u32)(r - sr->records);
    if (r->moving != MOVING_NONE && !transform_bits_equal(&sr->seen[index], wt)) {
        moving_remove(sr, r->moving);
    }
    sr->seen[index] = *wt;
    r->flags |= (u32)REC_SEEN;
    return r->moving == MOVING_NONE;
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
            const bool place = see(sr, r, &wt[i]);
            NPH_TRY(mirror_mesh(sr, r, &m[i], &wt[i], place));
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
            (void)see(sr, r, &wt[i]);
            NPH_TRY(mirror_light(sr, r, &l[i], &wt[i]));
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Interpolation
// ---------------------------------------------------------------------------

static Pose pose_of(const NphWorldTransform *wt)
{
    Pose p;
    p.translation = wt->translation;
    const NphVec3 x = nph_vec3_normalize_or(wt->x_axis, nph_vec3_unit_x());
    NphVec3 y = nph_vec3_sub(wt->y_axis, nph_vec3_scale(x, nph_vec3_dot(wt->y_axis, x)));
    y = nph_vec3_normalize_or(y, nph_vec3_perpendicular(x));
    const NphMat3 r = nph_mat3_from_cols(x, y, nph_vec3_cross(x, y));
    p.rotation = nph_quat_normalize(nph_quat_from_mat3(r));
    p.stretch = nph_mat3_mul(nph_mat3_transpose(r), nph_world_transform_linear(wt));
    return p;
}

// The world transform `f` of the way from `m->from` to `m->to`: positions
// in 64 bits, the turn by normalised linear interpolation (the shortest way;
// a step's turn is small, so nlerp's uneven pace is invisible: Blow,
// "Understanding Slerp, Then Not Using It", 2004), the stretch linearly.
static NphWorldTransform interpolate(const Moving *m, f32 f)
{
    const NphQuat q = nph_quat_nlerp(m->from.rotation, m->to.rotation, f);
    NphMat3 s;
    for (u32 c = 0; c < 3U; ++c) {
        s.c[c] = nph_vec3_lerp(m->from.stretch.c[c], m->to.stretch.c[c], f);
    }
    const NphMat3 linear = nph_mat3_mul(nph_mat3_from_quat(q), s);
    NphWorldTransform wt;
    wt.translation = nph_dvec3_lerp(m->from.translation, m->to.translation, (f64)f);
    wt.x_axis = linear.c[0];
    wt.y_axis = linear.c[1];
    wt.z_axis = linear.c[2];
    return wt;
}

// Draws a record at `wt`: its instance (when visible) and its light.
static NphResult place_record(NphSceneRenderer *sr, EntityRecord *r, const NphWorldTransform *wt)
{
    if ((r->flags & (u32)REC_MESH) != 0U && (r->flags & (u32)REC_HIDDEN) == 0U &&
        nph_render_instance_is_valid(sr->renderer, r->instance) && drawable(wt)) {
        const NphRenderTransform t = render_transform(wt);
        NPH_TRY(nph_render_instance_set_transform(sr->renderer, r->instance, &t));
        sr->stats.moved += 1U;
    }
    if ((r->flags & (u32)REC_LIGHT) != 0U && nph_render_light_is_valid(sr->renderer, r->light)) {
        const NphLight *l = (const NphLight *)nph_world_get(sr->world, r->entity, sr->ids.light);
        if (l != NULL) {
            NphRenderLightDesc d;
            light_desc(l, wt, &d);
            NPH_TRY(nph_render_light_update(sr->renderer, r->light, &d));
        }
    }
    return NPH_RESULT_OK;
}

static NphResult moving_push(NphSceneRenderer *sr, u32 record, u32 *out)
{
    if (sr->moving_count == sr->moving_cap) {
        const u32 cap = nph_max_u32(64U, 2U * sr->moving_cap);
        Moving *grown = NPH_ALLOC_ARRAY(sr->alloc, Moving, cap);
        if (grown == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (sr->moving != NULL) {
            memcpy(grown, sr->moving, (u64)sr->moving_count * sizeof *grown);
            NPH_FREE_ARRAY(sr->alloc, Moving, sr->moving, sr->moving_cap);
        }
        sr->moving = grown;
        sr->moving_cap = cap;
    }
    *out = sr->moving_count++;
    sr->moving[*out].record = record;
    sr->records[record].moving = *out;
    return NPH_RESULT_OK;
}

// The records of a query's changed chunks that moved since the last sync.
static NphResult capture_query(NphSceneRenderer *sr, const NphQuery *q)
{
    NphQueryIter it;
    nph_query_iter_since(q, sr->last_tick, sr->last_tick, &it);
    NphChunkView v;
    while (nph_query_next(&it, &v)) {
        const NphWorldTransform *wt = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
        for (u32 i = 0; i < v.count; ++i) {
            u32 index = 0U;
            EntityRecord *r = find_record(sr, v.entities[i], &index);
            // A record first seen by the coming sync is drawn where it is.
            if (r == NULL || (r->flags & (u32)REC_SEEN) == 0U ||
                transform_bits_equal(&sr->seen[index], &wt[i])) {
                continue;
            }
            u32 m = r->moving;
            if (m == MOVING_NONE) {
                NPH_TRY(moving_push(sr, index, &m));
                sr->moving[m].from = pose_of(&sr->seen[index]);
            } else {
                sr->moving[m].from = sr->moving[m].to; // it moved on from there
            }
            sr->moving[m].to = pose_of(&wt[i]);
            sr->moving[m].fresh = 1;
            sr->seen[index] = wt[i];
        }
    }
    return NPH_RESULT_OK;
}

static const NphWorldTransform *camera_transform(const NphSceneRenderer *sr)
{
    if (nph_entity_is_null(sr->camera_entity)) {
        return NULL;
    }
    return NPH_WORLD_GET(sr->world, sr->camera_entity, NPH_COMPONENT_ID_WORLD_TRANSFORM,
                         NphWorldTransform);
}

NphResult nph_scene_renderer_capture(NphSceneRenderer *sr)
{
    NPH_ASSERT(sr != NULL && sr->world != NULL);
    if (sr->full_pass) {
        return NPH_RESULT_OK; // the next sync looks at everything anew
    }
    NPH_PROFILE_BEGIN("runtime.scene_capture");
    for (u32 i = 0; i < sr->moving_count; ++i) {
        sr->moving[i].fresh = 0;
    }
    NphResult r = capture_query(sr, sr->mesh_query);
    if (r == NPH_RESULT_OK) {
        r = capture_query(sr, sr->light_query);
    }
    // What moved before and not since came to rest where it went.
    for (u32 i = sr->moving_count; i-- > 0U;) {
        if (!sr->moving[i].fresh) {
            const u32 index = sr->moving[i].record;
            moving_remove(sr, i);
            const NphResult pr = place_record(sr, &sr->records[index], &sr->seen[index]);
            r = r != NPH_RESULT_OK ? r : pr;
        }
    }
    const NphWorldTransform *cam = camera_transform(sr);
    if (cam != NULL && sr->camera_has_seen && !transform_bits_equal(cam, &sr->camera_seen)) {
        sr->camera_motion.from =
            sr->camera_moving ? sr->camera_motion.to : pose_of(&sr->camera_seen);
        sr->camera_motion.to = pose_of(cam);
        sr->camera_moving = 1;
        sr->camera_seen = *cam;
    } else {
        sr->camera_moving = 0;
    }
    if (r != NPH_RESULT_OK) {
        sr->full_pass = 1;
    }
    NPH_PROFILE_END();
    return r;
}

// Draws what the capture saw moving at `fraction` of the way (at rest, and
// done moving, from 1 up).
static NphResult place_moving(NphSceneRenderer *sr, f32 fraction)
{
    NphResult first = NPH_RESULT_OK;
    if (fraction >= 1.0F) {
        while (sr->moving_count > 0U) {
            const u32 index = sr->moving[sr->moving_count - 1U].record;
            moving_remove(sr, sr->moving_count - 1U);
            const NphResult pr = place_record(sr, &sr->records[index], &sr->seen[index]);
            first = first != NPH_RESULT_OK ? first : pr;
        }
        sr->camera_moving = 0;
        return first;
    }
    for (u32 i = 0; i < sr->moving_count; ++i) {
        const u32 index = sr->moving[i].record;
        NphWorldTransform wt = interpolate(&sr->moving[i], fraction);
        if (!drawable(&wt)) {
            wt = sr->seen[index]; // a stretch through nothing: drawn where it ends
        }
        const NphResult pr = place_record(sr, &sr->records[index], &wt);
        first = first != NPH_RESULT_OK ? first : pr;
    }
    sr->stats.interpolating = sr->moving_count;
    return first;
}

void nph_scene_renderer_snap(NphSceneRenderer *sr, NphEntity entity)
{
    NPH_ASSERT(sr != NULL);
    if (sr->world == NULL || !nph_world_is_alive(sr->world, entity)) {
        return;
    }
    NPH_ARRAY_CLEAR(&sr->walk);
    if (NPH_ARRAY_PUSH(&sr->walk, entity.value) != NPH_RESULT_OK) {
        return;
    }
    while (sr->walk.len > 0U) {
        const NphEntity e = {sr->walk.data[--sr->walk.len]};
        u32 index = 0U;
        EntityRecord *r = find_record(sr, e, &index);
        if (r != NULL && r->moving != MOVING_NONE) {
            moving_remove(sr, r->moving);
            const NphResult pr = place_record(sr, r, &sr->seen[index]);
            if (pr != NPH_RESULT_OK) {
                sr->full_pass = 1;
            }
        }
        if (e.value == sr->camera_entity.value) {
            sr->camera_moving = 0;
            sr->camera_cut = 1;
        }
        for (NphEntity c = nph_world_first_child(sr->world, e); !nph_entity_is_null(c);
             c = nph_world_next_sibling(sr->world, c)) {
            if (NPH_ARRAY_PUSH(&sr->walk, c.value) != NPH_RESULT_OK) {
                sr->full_pass = 1; // out of memory: the rest are drawn as they are next time
                break;
            }
        }
    }
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

// Places the view's camera at `wt`: an orthonormal frame from the entity's
// axes (scale removed).
static void place_camera(NphSceneRenderer *sr, const NphWorldTransform *wt)
{
    const NphVec3 z = nph_vec3_normalize_or(wt->z_axis, nph_vec3_unit_z());
    NphVec3 y = nph_vec3_sub(wt->y_axis, nph_vec3_scale(z, nph_vec3_dot(wt->y_axis, z)));
    y = nph_vec3_normalize_or(y, nph_vec3_perpendicular(z));
    const NphVec3 x = nph_vec3_cross(y, z);
    sr->camera.position = wt->translation;
    sr->camera.orientation = nph_quat_normalize(nph_quat_from_mat3(nph_mat3_from_cols(x, y, z)));
}

// The view: the active camera with the lowest entity slot, between its
// states while the capture saw it moving (a camera on a character), as it
// is otherwise. Another camera than the last sync's is a cut.
static void sync_camera(NphSceneRenderer *sr, f32 fraction)
{
    NphQueryIter it;
    nph_query_iter(sr->camera_query, &it);
    NphChunkView v;
    u32 best_slot = NPH_U32_MAX;
    sr->has_view = 0;
    NphEntity chosen = NPH_ENTITY_NULL;
    NphWorldTransform chosen_wt = nph_world_transform_identity();
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
            chosen = v.entities[i];
            chosen_wt = wt[i];
            sr->camera.yfov = nph_f32_clamp(c[i].yfov, 0.01F, 3.0F);
            sr->camera.znear = c[i].znear > 1e-5F ? c[i].znear : 1e-5F;
            sr->ev100 = c[i].ev100;
            // A second at most: the renderer refuses what is not finite.
            sr->shutter = c[i].shutter > 0.0F ? nph_f32_min(c[i].shutter, 1.0F) : 0.0F;
            // A lens's f-number and sensor finite and positive, else a
            // pinhole and full frame (its focus is checked with the view).
            sr->aperture =
                c[i].aperture > 0.0F && nph_f32_is_finite(c[i].aperture) ? c[i].aperture : 0.0F;
            sr->focus_distance = c[i].focus_distance;
            sr->sensor_height = c[i].sensor_height > 0.0F && nph_f32_is_finite(c[i].sensor_height)
                                    ? c[i].sensor_height
                                    : 0.0F;
            sr->tonemapper = c[i].tonemapper <= (u32)NPH_RENDER_TONEMAPPER_AGX
                                 ? c[i].tonemapper
                                 : (u32)NPH_RENDER_TONEMAPPER_ACES;
            sr->has_view = 1;
        }
    }
    if (chosen.value != sr->camera_entity.value) {
        if (sr->camera_has_seen) {
            sr->camera_cut = 1;
        }
        sr->camera_moving = 0;
    } else if (sr->camera_has_seen && !transform_bits_equal(&chosen_wt, &sr->camera_seen)) {
        sr->camera_moving = 0; // moved at the frame rate
    }
    sr->camera_entity = chosen;
    sr->camera_seen = chosen_wt;
    sr->camera_has_seen = sr->has_view;
    if (!sr->has_view) {
        return;
    }
    if (sr->camera_moving && fraction < 1.0F) {
        const NphWorldTransform wt = interpolate(&sr->camera_motion, fraction);
        place_camera(sr, &wt);
    } else {
        sr->camera_moving = 0;
        place_camera(sr, &chosen_wt);
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
    return nph_scene_renderer_sync_at(sr, 1.0F, out);
}

NphResult nph_scene_renderer_sync_at(NphSceneRenderer *sr, f32 fraction, NphSceneRenderStats *out)
{
    NPH_ASSERT(sr != NULL && sr->world != NULL);
    NPH_PROFILE_BEGIN("runtime.scene_render");
    // Not finite or out of range: as it is.
    fraction = fraction >= 0.0F && fraction < 1.0F ? fraction : 1.0F;
    sr->stats.created = 0U;
    sr->stats.destroyed = 0U;
    sr->stats.moved = 0U;
    sr->stats.interpolating = 0U;
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
    step = place_moving(sr, fraction);
    r = r != NPH_RESULT_OK ? r : step;
    sync_camera(sr, fraction);
    sr->view_cut = sr->camera_cut;
    sr->camera_cut = 0;
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
    view->camera_cut = sr->view_cut;
    view->ev100 = sr->ev100;
    view->tonemapper = (NphRenderTonemapper)sr->tonemapper;
    view->shutter = sr->shutter;
    view->aperture = sr->aperture;
    view->focus_distance = sr->focus_distance;
    view->sensor_height = sr->sensor_height;
    // A lens that cannot focus there, not beyond its focal length or not
    // finite, is a pinhole (the renderer refuses the lens).
    if (!(nph_f32_is_finite(view->focus_distance) &&
          view->focus_distance > nph_render_view_focal_length(view))) {
        view->aperture = 0.0F;
    }
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

NphRenderLight nph_scene_renderer_light(const NphSceneRenderer *sr, NphEntity entity)
{
    NPH_ASSERT(sr != NULL);
    const EntityRecord *r = find_record_const(sr, entity);
    return r != NULL ? r->light : NPH_RENDER_LIGHT_NULL;
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
        const NphResult res = apply_visibility(sr, r, m, wt, true);
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
