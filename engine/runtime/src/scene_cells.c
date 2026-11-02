// SPDX-License-Identifier: Apache-2.0
// The scene renderer's world partition: a WorldCell's render cell, its proxy
// and its stream. See runtime_internal.h, components.h and docs/runtime.md
// "World partition".
#include <nepheline/asset/mesh.h>
#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>
#include <nepheline/runtime/proxy_stream.h>

#include "runtime_internal.h"

#include <string.h>

// A cell's box and its proxy's place are axis-aligned world quantities: what
// its entity's world transform may do to them is move them.
#define CELL_LINEAR_SLACK 1e-5F

static bool axis_is(NphVec3 axis, f32 x, f32 y, f32 z)
{
    return nph_f32_abs(axis.x - x) <= CELL_LINEAR_SLACK &&
           nph_f32_abs(axis.y - y) <= CELL_LINEAR_SLACK &&
           nph_f32_abs(axis.z - z) <= CELL_LINEAR_SLACK;
}

static bool translation_only(const NphWorldTransform *wt)
{
    return axis_is(wt->x_axis, 1.0F, 0.0F, 0.0F) && axis_is(wt->y_axis, 0.0F, 1.0F, 0.0F) &&
           axis_is(wt->z_axis, 0.0F, 0.0F, 1.0F);
}

static void log_cell(const SceneCell *c, const char *what, NphResult r)
{
    char guid[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(c->proxy, guid);
    NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                 "world cell %u (proxy %s): %s (%s); its instances draw as themselves", c->id, guid,
                 what, nph_result_string(r));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult scene_cells_init(SceneCells *c, NphAllocator *alloc, NphRenderer *renderer,
                           NphAssetLoader *loader, RenderAssetCache *assets, NphJobSystem *jobs,
                           u32 max_cells, u64 budget_bytes)
{
    memset(c, 0, sizeof *c);
    c->alloc = alloc;
    c->renderer = renderer;
    c->loader = loader;
    c->assets = assets;
    c->jobs = jobs;
    c->max_cells = max_cells;
    c->budget_bytes = budget_bytes;
    c->free_head = SCENE_CELL_NONE;
    NPH_ARRAY_INIT(&c->loading, alloc);
    NphHashMapDesc d;
    memset(&d, 0, sizeof d);
    d.key_size = (u32)sizeof(u32);
    d.val_size = (u32)sizeof(u32);
    return nph_hashmap_init(&c->by_id, alloc, &d);
}

// The cell's render resources and proxy's assets gone. Instances that name it
// are gone first.
static void strip(SceneCells *c, SceneCell *s)
{
    if (s->stream_slot != SCENE_CELL_NONE) {
        nph_proxy_stream_remove(c->stream, s->stream_slot);
        s->stream_slot = SCENE_CELL_NONE;
    }
    if (s->state == SCENE_CELL_READY) {
        c->ready -= 1U;
    }
    nph_render_cell_destroy(c->renderer, s->cell);
    s->cell = NPH_RENDER_CELL_NULL;
    nph_render_mesh_destroy(c->renderer, s->mesh);
    s->mesh = NPH_RENDER_MESH_NULL;
    if (s->head.value != 0U) {
        nph_asset_release(c->loader, s->head); // during a load: unloads when it lands
        s->head = NPH_ASSET_HANDLE_NULL;
    }
    if (s->level.value != 0U) {
        nph_asset_release(c->loader, s->level);
        s->level = NPH_ASSET_HANDLE_NULL;
    }
}

// The cell gone, its slot free.
static void cell_release(SceneCells *c, u32 index, bool retire, NphArrayU32 *retired)
{
    SceneCell *s = &c->items[index];
    strip(c, s);
    if (retire && s->material != RENDER_ASSET_NONE &&
        NPH_ARRAY_PUSH(retired, s->material) == NPH_RESULT_OK) {
        s->material = RENDER_ASSET_NONE;
    }
    render_assets_release(c->assets, s->material);
    if (s->state != SCENE_CELL_FREE) {
        const u32 *mapped = NPH_HASHMAP_FIND(&c->by_id, u32, &s->id);
        // A later cell of the same id, its predecessor going, keeps the map.
        if (mapped != NULL && *mapped == index) {
            NPH_VERIFY(nph_hashmap_remove(&c->by_id, &s->id));
        }
    }
    memset(s, 0, sizeof *s);
    s->state = SCENE_CELL_FREE;
    s->next_free = c->free_head;
    c->free_head = index;
}

void scene_cells_free(SceneCells *c)
{
    if (c->alloc == NULL) {
        return; // never made: creation failed before it
    }
    if (c->items != NULL) {
        for (u32 i = 0; i < c->high; ++i) {
            if (c->items[i].state != SCENE_CELL_FREE) {
                cell_release(c, i, false, NULL);
            }
        }
    }
    nph_proxy_stream_destroy(c->stream);
    c->stream = NULL;
    if (c->items != NULL) {
        NPH_FREE_ARRAY(c->alloc, SceneCell, c->items, c->cap);
        c->items = NULL;
    }
    nph_hashmap_free(&c->by_id);
    NPH_ARRAY_FREE(&c->loading);
}

// ---------------------------------------------------------------------------
// Mirroring
// ---------------------------------------------------------------------------

static NphDVec3 box_center(const NphWorldCell *w, const NphWorldTransform *wt)
{
    return nph_dvec3_add(wt->translation,
                         nph_dvec3(((f64)w->bounds_min.x + (f64)w->bounds_max.x) * 0.5,
                                   ((f64)w->bounds_min.y + (f64)w->bounds_max.y) * 0.5,
                                   ((f64)w->bounds_min.z + (f64)w->bounds_max.z) * 0.5));
}

// Field by field: the struct's padding holds whatever its writer left there.
static bool same_vec3(NphVec3 a, NphVec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool scene_cells_same(const SceneCells *c, u32 index, const NphWorldCell *w,
                      const NphWorldTransform *wt)
{
    const SceneCell *s = &c->items[index];
    const NphWorldCell *o = &s->source;
    return o->id == w->id && same_vec3(o->bounds_min, w->bounds_min) &&
           same_vec3(o->bounds_max, w->bounds_max) && nph_guid_equals(o->proxy, w->proxy) &&
           nph_guid_equals(o->proxy_material, w->proxy_material) &&
           same_vec3(o->proxy_origin, w->proxy_origin) && o->proxy_cell == w->proxy_cell &&
           o->proxy_distance == w->proxy_distance && s->source_translation.x == wt->translation.x &&
           s->source_translation.y == wt->translation.y &&
           s->source_translation.z == wt->translation.z &&
           s->plain == (translation_only(wt) ? 1 : 0);
}

NphResult scene_cells_add(SceneCells *c, NphEntity entity, const NphWorldCell *w,
                          const NphWorldTransform *wt, u32 *out_index)
{
    *out_index = SCENE_CELL_NONE;
    u32 index = c->free_head;
    if (index != SCENE_CELL_NONE) {
        c->free_head = c->items[index].next_free;
    } else {
        if (c->high == c->cap) {
            const u32 cap = nph_max_u32(64U, 2U * c->cap);
            SceneCell *grown = NPH_ALLOC_ARRAY_ZEROED(c->alloc, SceneCell, cap);
            if (grown == NULL) {
                return NPH_RESULT_OUT_OF_MEMORY;
            }
            if (c->items != NULL) {
                memcpy(grown, c->items, (u64)c->high * sizeof *grown);
                NPH_FREE_ARRAY(c->alloc, SceneCell, c->items, c->cap);
            }
            c->items = grown;
            c->cap = cap;
        }
        index = c->high++;
    }
    SceneCell *s = &c->items[index];
    memset(s, 0, sizeof *s);
    s->entity = entity;
    s->id = w->id;
    s->source = *w;
    s->source_translation = wt->translation;
    s->plain = translation_only(wt) ? 1 : 0;
    s->stream_slot = SCENE_CELL_NONE;
    s->material = RENDER_ASSET_NONE;
    s->proxy = w->proxy;
    s->state = SCENE_CELL_BARE;
    *out_index = index;
    if (w->id == 0U) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "a WorldCell without an id: nothing can name it");
        return NPH_RESULT_OK;
    }
    if (NPH_HASHMAP_FIND(&c->by_id, u32, &s->id) != NULL) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "two WorldCells of id %u: the instances name the first",
                     w->id);
        return NPH_RESULT_OK;
    }
    NPH_TRY(nph_hashmap_insert(&c->by_id, &s->id, &index));
    if (nph_guid_is_null(w->proxy)) {
        return NPH_RESULT_OK; // nothing of it stands for it from afar
    }
    if (!s->plain) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                     "world cell %u is turned or scaled, which its box cannot follow: its "
                     "instances draw as themselves",
                     w->id);
        return NPH_RESULT_OK;
    }
    const NphResult r = nph_asset_load_guid_async(c->loader, w->proxy, NULL, &s->head);
    if (r != NPH_RESULT_OK) {
        s->head = NPH_ASSET_HANDLE_NULL;
        log_cell(s, "its proxy does not load", r);
        return NPH_RESULT_OK;
    }
    s->material = render_assets_acquire(c->assets, w->proxy_material, RENDER_ASSET_MATERIAL);
    s->state = SCENE_CELL_LOADING;
    return NPH_ARRAY_PUSH(&c->loading, index);
}

void scene_cells_mark_going(SceneCells *c, u32 index)
{
    SceneCell *s = &c->items[index];
    if (!s->going) {
        s->going = 1;
        c->going += 1U;
    }
    // A new cell of the id may arrive before this one has gone.
    const u32 *mapped = NPH_HASHMAP_FIND(&c->by_id, u32, &s->id);
    if (mapped != NULL && *mapped == index) {
        NPH_VERIFY(nph_hashmap_remove(&c->by_id, &s->id));
    }
}

void scene_cells_finish_going(SceneCells *c, NphArrayU32 *retired)
{
    if (c->going == 0U) {
        return;
    }
    u32 kept = 0U;
    for (u32 i = 0; i < c->loading.len; ++i) {
        const u32 index = c->loading.data[i];
        if (!c->items[index].going) {
            c->loading.data[kept++] = index;
        }
    }
    c->loading.len = kept;
    for (u32 i = 0; i < c->high; ++i) {
        if (c->items[i].state != SCENE_CELL_FREE && c->items[i].going) {
            cell_release(c, i, retired != NULL, retired);
        }
    }
    c->going = 0U;
}

u32 scene_cells_find(const SceneCells *c, u32 id)
{
    if (id == 0U) {
        return SCENE_CELL_NONE;
    }
    const u32 *found = NPH_HASHMAP_FIND(&c->by_id, u32, &id);
    return found != NULL ? *found : SCENE_CELL_NONE;
}

// ---------------------------------------------------------------------------
// The proxy
// ---------------------------------------------------------------------------

static NphResult ensure_stream(SceneCells *c)
{
    if (c->stream != NULL) {
        return NPH_RESULT_OK;
    }
    NphProxyStreamDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = c->alloc;
    d.renderer = c->renderer;
    d.loader = c->loader;
    d.jobs = c->jobs;
    d.max_cells = c->max_cells;
    d.budget_bytes = c->budget_bytes;
    return nph_proxy_stream_create(&d, &c->stream);
}

// The cell made of its mesh, its head's levels being `levels` where it
// streams: its render cell and, where it streams, its place in the stream.
static NphResult make_cell(SceneCells *c, SceneCell *s, const NphMeshLevel *levels, u32 level_count)
{
    const NphWorldCell *w = &s->source;
    const RenderAsset *mat = render_asset(c->assets, s->material);
    const NphRenderMaterial material =
        mat != NULL && mat->state == RENDER_ASSET_READY ? mat->material : NPH_RENDER_MATERIAL_NULL;
    f32 errors[NPH_RENDER_CELL_MAX_LEVELS];
    NphRenderCellDesc d;
    memset(&d, 0, sizeof d);
    NphWorldTransform at = nph_world_transform_identity();
    at.translation = s->source_translation;
    d.center = box_center(w, &at);
    d.extent = nph_vec3_scale(nph_vec3_sub(w->bounds_max, w->bounds_min), 0.5F);
    d.proxy_levels = &s->mesh;
    d.proxy_level_count = 1U;
    d.proxy_materials = &material;
    d.proxy_material_count = 1U;
    d.proxy_transform = nph_render_transform_identity();
    d.proxy_transform.translation =
        nph_dvec3_add(nph_dvec3_from_vec3(w->proxy_origin), s->source_translation);
    d.proxy_error = w->proxy_cell;
    d.distance = w->proxy_distance;
    if (levels != NULL) {
        for (u32 i = 0; i < level_count; ++i) {
            errors[i] = levels[i].error;
        }
        d.proxy_level_errors = errors;
        d.proxy_level_error_count = level_count;
    }
    NPH_TRY(nph_render_cell_create(c->renderer, &d, &s->cell));
    if (levels == NULL) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(ensure_stream(c));
    NphProxyStreamCell pc;
    memset(&pc, 0, sizeof pc);
    pc.cell = s->cell;
    pc.coarsest = s->mesh;
    pc.levels = levels;
    pc.level_count = level_count;
    pc.center = d.center;
    pc.extent = d.extent;
    pc.distance = d.distance;
    return nph_proxy_stream_add(c->stream, &pc, &s->stream_slot);
}

// A failure that says the content is bad (the cell goes bare, its instances
// draw as themselves) as opposed to the engine running out of something.
static bool content_error(NphResult r)
{
    return r != NPH_RESULT_FULL && r != NPH_RESULT_OUT_OF_MEMORY;
}

// One cell's load advanced. *out_done when nothing more will happen to it.
static NphResult advance(SceneCells *c, SceneCell *s, bool *out_done)
{
    *out_done = false;
    if (nph_asset_state(c->loader, s->head) == NPH_ASSET_STATE_LOADING) {
        return NPH_RESULT_OK;
    }
    if (nph_asset_state(c->loader, s->head) != NPH_ASSET_STATE_READY) {
        return nph_asset_error(c->loader, s->head);
    }
    NphMeshView head;
    NPH_TRY(nph_mesh_view(nph_asset_get(c->loader, s->head), &head));
    if (head.levels != NULL) {
        // Cooked apart: the coarsest level alone; the stream brings the rest.
        if (head.level_count == 0U || head.level_count > NPH_RENDER_CELL_MAX_LEVELS) {
            return NPH_RESULT_CORRUPT;
        }
        if (s->mesh.value == 0U) {
            if (s->level.value == 0U) {
                NPH_TRY(nph_asset_load_guid_async(
                    c->loader, head.levels[head.level_count - 1U].mesh, NULL, &s->level));
            }
            if (nph_asset_state(c->loader, s->level) == NPH_ASSET_STATE_LOADING) {
                return NPH_RESULT_OK;
            }
            if (nph_asset_state(c->loader, s->level) != NPH_ASSET_STATE_READY) {
                return nph_asset_error(c->loader, s->level);
            }
            NPH_TRY(nph_proxy_level_mesh_create(c->renderer, nph_asset_get(c->loader, s->level),
                                                &s->mesh));
            nph_asset_release(c->loader, s->level); // the GPU holds it
            s->level = NPH_ASSET_HANDLE_NULL;
        }
    } else if (s->mesh.value == 0U) {
        // Cooked whole: one mesh of voxels alone, whatever the distance.
        NphRenderSubmesh subs[NPH_RENDER_MAX_SUBMESHES];
        NphRenderMeshDesc d;
        NPH_TRY(render_assets_mesh_desc(&head, subs, &d));
        d.name = "cell proxy";
        NPH_TRY(nph_render_mesh_create(c->renderer, &d, &s->mesh));
    }
    if (!render_asset_settled(c->assets, s->material)) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(make_cell(c, s, head.levels, head.levels != NULL ? head.level_count : 0U));
    nph_asset_release(c->loader, s->head); // the stream copied what it needs
    s->head = NPH_ASSET_HANDLE_NULL;
    s->state = SCENE_CELL_READY;
    c->ready += 1U;
    *out_done = true;
    return NPH_RESULT_OK;
}

NphResult scene_cells_poll(SceneCells *c)
{
    NphResult first = NPH_RESULT_OK;
    u32 kept = 0U;
    for (u32 i = 0; i < c->loading.len; ++i) {
        const u32 index = c->loading.data[i];
        SceneCell *s = &c->items[index];
        bool done = false;
        const NphResult r = first == NPH_RESULT_OK ? advance(c, s, &done) : NPH_RESULT_OK;
        if (r != NPH_RESULT_OK && !content_error(r)) {
            first = r; // the renderer is full: it stays loading, and the caller hears
        } else if (r != NPH_RESULT_OK) {
            log_cell(s, "its proxy cannot be used", r);
            // Whatever it got so far goes; its instances draw as themselves.
            strip(c, s);
            s->state = SCENE_CELL_BARE;
            done = true;
        }
        if (!done) {
            c->loading.data[kept++] = index;
        }
    }
    c->loading.len = kept;
    return first;
}

bool scene_cells_wait(SceneCells *c)
{
    bool waited = false;
    for (u32 i = 0; i < c->loading.len; ++i) {
        const SceneCell *s = &c->items[c->loading.data[i]];
        const NphAssetHandle h = s->level.value != 0U ? s->level : s->head;
        if (h.value != 0U && nph_asset_state(c->loader, h) == NPH_ASSET_STATE_LOADING) {
            NPH_IGNORE_RESULT(nph_asset_wait(c->loader, h));
            waited = true;
        }
    }
    return waited;
}
