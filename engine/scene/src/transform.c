// SPDX-License-Identifier: Apache-2.0
// Built-in components, the hierarchy, transform math, and the parallel
// transform system (transform.h; docs/scene.md "Transforms").

#include <nepheline/core/assert.h>
#include <nepheline/scene/prefab.h>
#include <nepheline/scene/transform.h>

#include "scene_internal.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Built-in components
// ---------------------------------------------------------------------------

NPH_COMPONENT_DEFINE(NphName, NPH_NAME_FIELDS, "Name", 0U, NULL)

static const NphLocalTransform g_local_default = {
    {0.0, 0.0, 0.0}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}};
NPH_COMPONENT_DEFINE(NphLocalTransform, NPH_LOCAL_TRANSFORM_FIELDS, "LocalTransform", 0U,
                     &g_local_default)

static const NphWorldTransform g_world_default = {
    {0.0, 0.0, 0.0}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
NPH_COMPONENT_DEFINE(NphWorldTransform, NPH_WORLD_TRANSFORM_FIELDS, "WorldTransform",
                     NPH_COMPONENT_FLAG_TRANSIENT, &g_world_default)

NPH_COMPONENT_DEFINE(NphHierarchy, NPH_HIERARCHY_FIELDS, "Hierarchy",
                     NPH_COMPONENT_FLAG_INTERNAL | NPH_COMPONENT_FLAG_TRANSIENT, NULL)

const NphComponentDesc *nph_builtin_component_desc_(u32 index)
{
    switch (index) {
    case NPH_COMPONENT_ID_NAME:
        return NphName_component();
    case NPH_COMPONENT_ID_LOCAL_TRANSFORM:
        return NphLocalTransform_component();
    case NPH_COMPONENT_ID_WORLD_TRANSFORM:
        return NphWorldTransform_component();
    case NPH_COMPONENT_ID_HIERARCHY:
        return NphHierarchy_component();
    case NPH_COMPONENT_ID_PREFAB_INSTANCE:
        return NphPrefabInstance_component();
    default:
        NPH_UNREACHABLE();
        return NULL;
    }
}

// ---------------------------------------------------------------------------
// Transform math
// ---------------------------------------------------------------------------

NphWorldTransform nph_world_transform_from_local(const NphLocalTransform *local)
{
    const NphMat3 r = nph_mat3_from_quat(local->rotation);
    NphWorldTransform t;
    t.translation = local->translation;
    t.x_axis = nph_vec3_scale(r.c[0], local->scale.x);
    t.y_axis = nph_vec3_scale(r.c[1], local->scale.y);
    t.z_axis = nph_vec3_scale(r.c[2], local->scale.z);
    return t;
}

NphWorldTransform nph_world_transform_compose(const NphWorldTransform *parent,
                                              const NphLocalTransform *local)
{
    const NphMat3 r = nph_mat3_from_quat(local->rotation);
    const NphVec3 lx = nph_vec3_scale(r.c[0], local->scale.x);
    const NphVec3 ly = nph_vec3_scale(r.c[1], local->scale.y);
    const NphVec3 lz = nph_vec3_scale(r.c[2], local->scale.z);
    const NphVec3 px = parent->x_axis;
    const NphVec3 py = parent->y_axis;
    const NphVec3 pz = parent->z_axis;
    NphWorldTransform t;
    // world.linear = parent.linear * local.linear, column by column.
    t.x_axis = nph_vec3_add(nph_vec3_add(nph_vec3_scale(px, lx.x), nph_vec3_scale(py, lx.y)),
                            nph_vec3_scale(pz, lx.z));
    t.y_axis = nph_vec3_add(nph_vec3_add(nph_vec3_scale(px, ly.x), nph_vec3_scale(py, ly.y)),
                            nph_vec3_scale(pz, ly.z));
    t.z_axis = nph_vec3_add(nph_vec3_add(nph_vec3_scale(px, lz.x), nph_vec3_scale(py, lz.y)),
                            nph_vec3_scale(pz, lz.z));
    // The translation in f64: the parent's linear part is f32, but the
    // local offset and the sum keep 64-bit precision far from the origin.
    const NphDVec3 lt = local->translation;
    t.translation.x =
        parent->translation.x + (f64)px.x * lt.x + (f64)py.x * lt.y + (f64)pz.x * lt.z;
    t.translation.y =
        parent->translation.y + (f64)px.y * lt.x + (f64)py.y * lt.y + (f64)pz.y * lt.z;
    t.translation.z =
        parent->translation.z + (f64)px.z * lt.x + (f64)py.z * lt.y + (f64)pz.z * lt.z;
    return t;
}

NphAffine nph_world_transform_relative(const NphWorldTransform *t, NphDVec3 origin)
{
    const f32 tx = (f32)(t->translation.x - origin.x);
    const f32 ty = (f32)(t->translation.y - origin.y);
    const f32 tz = (f32)(t->translation.z - origin.z);
    return nph_affine_from_rows(nph_vec4(t->x_axis.x, t->y_axis.x, t->z_axis.x, tx),
                                nph_vec4(t->x_axis.y, t->y_axis.y, t->z_axis.y, ty),
                                nph_vec4(t->x_axis.z, t->y_axis.z, t->z_axis.z, tz));
}

NphDVec3 nph_world_transform_point(const NphWorldTransform *t, NphVec3 p)
{
    NphDVec3 out;
    out.x = t->translation.x + (f64)t->x_axis.x * (f64)p.x + (f64)t->y_axis.x * (f64)p.y +
            (f64)t->z_axis.x * (f64)p.z;
    out.y = t->translation.y + (f64)t->x_axis.y * (f64)p.x + (f64)t->y_axis.y * (f64)p.y +
            (f64)t->z_axis.y * (f64)p.z;
    out.z = t->translation.z + (f64)t->x_axis.z * (f64)p.x + (f64)t->y_axis.z * (f64)p.y +
            (f64)t->z_axis.z * (f64)p.z;
    return out;
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

static const NphHierarchy *hier(const NphWorld *w, NphEntity e)
{
    return NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_HIERARCHY, NphHierarchy);
}

// A writable Hierarchy cell (the column is stamped as changed).
static NphHierarchy *hier_mut(NphWorld *w, NphEntity e)
{
    NphEntityRecord *rec = nph_world_record_(w, e);
    NPH_ASSERT(rec != NULL);
    NphHierarchy *h = (NphHierarchy *)nph_world_cell_mut_(w, rec, NPH_COMPONENT_ID_HIERARCHY);
    NPH_ASSERT(h != NULL);
    return h;
}

NphEntity nph_world_parent(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    const NphHierarchy *h = hier(w, e);
    return h != NULL ? h->parent : NPH_ENTITY_NULL;
}

NphEntity nph_world_first_child(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    const NphHierarchy *h = hier(w, e);
    return h != NULL ? h->first_child : NPH_ENTITY_NULL;
}

NphEntity nph_world_next_sibling(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    const NphHierarchy *h = hier(w, e);
    return h != NULL ? h->next_sibling : NPH_ENTITY_NULL;
}

u32 nph_world_depth(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    const NphHierarchy *h = hier(w, e);
    return h != NULL ? h->depth : 0U;
}

// Removes `e` from its parent's child list.
static void hier_unlink(NphWorld *w, NphEntity e, NphHierarchy *h)
{
    NPH_ASSERT(!nph_entity_is_null(h->parent));
    NphHierarchy *p = hier_mut(w, h->parent);
    if (nph_entity_equals(p->first_child, e)) {
        p->first_child = h->next_sibling;
    }
    if (nph_entity_equals(p->last_child, e)) {
        p->last_child = h->prev_sibling;
    }
    if (!nph_entity_is_null(h->prev_sibling)) {
        hier_mut(w, h->prev_sibling)->next_sibling = h->next_sibling;
    }
    if (!nph_entity_is_null(h->next_sibling)) {
        hier_mut(w, h->next_sibling)->prev_sibling = h->prev_sibling;
    }
    h->parent = NPH_ENTITY_NULL;
    h->next_sibling = NPH_ENTITY_NULL;
    h->prev_sibling = NPH_ENTITY_NULL;
}

// Appends `e` to the children of `parent`.
static void hier_link_last(NphWorld *w, NphEntity e, NphHierarchy *h, NphEntity parent)
{
    NphHierarchy *p = hier_mut(w, parent);
    h->parent = parent;
    h->prev_sibling = p->last_child;
    h->next_sibling = NPH_ENTITY_NULL;
    if (!nph_entity_is_null(p->last_child)) {
        hier_mut(w, p->last_child)->next_sibling = e;
    } else {
        p->first_child = e;
    }
    p->last_child = e;
}

// The next entity of the subtree of `root` in pre-order after `node`, or
// null when the walk is done. Needs no stack: the links lead back up.
static NphEntity subtree_next(const NphWorld *w, NphEntity root, NphEntity node)
{
    const NphHierarchy *h = hier(w, node);
    if (!nph_entity_is_null(h->first_child)) {
        return h->first_child;
    }
    while (!nph_entity_equals(node, root)) {
        h = hier(w, node);
        if (!nph_entity_is_null(h->next_sibling)) {
            return h->next_sibling;
        }
        node = h->parent;
    }
    return NPH_ENTITY_NULL;
}

// The depth of the deepest descendant of `root`, relative to it.
static u32 subtree_height(const NphWorld *w, NphEntity root)
{
    const NphHierarchy *rh = hier(w, root);
    if (rh == NULL) {
        return 0U;
    }
    u32 height = 0U;
    for (NphEntity n = subtree_next(w, root, root); !nph_entity_is_null(n);
         n = subtree_next(w, root, n)) {
        height = nph_max_u32(height, hier(w, n)->depth - rh->depth);
    }
    return height;
}

// Sets the depth of `root` and renumbers its subtree.
static void set_depths(NphWorld *w, NphEntity root, u32 depth)
{
    hier_mut(w, root)->depth = depth;
    w->max_depth = nph_max_u32(w->max_depth, depth);
    for (NphEntity n = subtree_next(w, root, root); !nph_entity_is_null(n);
         n = subtree_next(w, root, n)) {
        NphHierarchy *h = hier_mut(w, n);
        h->depth = hier(w, h->parent)->depth + 1U;
        w->max_depth = nph_max_u32(w->max_depth, h->depth);
    }
}

void nph_hierarchy_on_despawn_(NphWorld *w, NphEntity e)
{
    NphHierarchy *h = hier_mut(w, e);
    if (!nph_entity_is_null(h->parent)) {
        hier_unlink(w, e, h);
    }
    NphEntity c = h->first_child;
    while (!nph_entity_is_null(c)) {
        NphHierarchy *ch = hier_mut(w, c);
        const NphEntity next = ch->next_sibling;
        ch->parent = NPH_ENTITY_NULL;
        ch->next_sibling = NPH_ENTITY_NULL;
        ch->prev_sibling = NPH_ENTITY_NULL;
        set_depths(w, c, 0U);
        nph_world_event_(w, NPH_WORLD_EVENT_PARENT_CHANGED, c, NPH_COMPONENT_NONE);
        c = next;
    }
    h->first_child = NPH_ENTITY_NULL;
    h->last_child = NPH_ENTITY_NULL;
    h->depth = 0U;
}

NphResult nph_world_set_parent(NphWorld *w, NphEntity child, NphEntity parent)
{
    NPH_ASSERT(w != NULL);
    nph_world_structural_begin_(w);
    if (!nph_world_is_alive(w, child) ||
        (!nph_entity_is_null(parent) && !nph_world_is_alive(w, parent))) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (nph_entity_equals(child, parent)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 new_depth = 0U;
    if (!nph_entity_is_null(parent)) {
        // A cycle would make the child its own ancestor.
        for (NphEntity a = parent; !nph_entity_is_null(a); a = nph_world_parent(w, a)) {
            if (nph_entity_equals(a, child)) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
        new_depth = nph_world_depth(w, parent) + 1U;
    }
    if (new_depth + subtree_height(w, child) > NPH_HIERARCHY_MAX_DEPTH) {
        return NPH_RESULT_OVERFLOW;
    }
    if (nph_world_parent(w, child).value == parent.value &&
        nph_world_has(w, child, NPH_COMPONENT_ID_HIERARCHY)) {
        return NPH_RESULT_OK;
    }

    const bool added_child = !nph_world_has(w, child, NPH_COMPONENT_ID_HIERARCHY);
    if (added_child) {
        NPH_TRY(nph_world_add(w, child, NPH_COMPONENT_ID_HIERARCHY, NULL));
    }
    if (!nph_entity_is_null(parent) && !nph_world_has(w, parent, NPH_COMPONENT_ID_HIERARCHY)) {
        const NphResult r = nph_world_add(w, parent, NPH_COMPONENT_ID_HIERARCHY, NULL);
        if (r != NPH_RESULT_OK) {
            if (added_child) {
                NPH_IGNORE_RESULT(nph_world_remove(w, child, NPH_COMPONENT_ID_HIERARCHY));
            }
            return r;
        }
    }
    NphHierarchy *h = hier_mut(w, child);
    if (!nph_entity_is_null(h->parent)) {
        hier_unlink(w, child, h);
    }
    if (!nph_entity_is_null(parent)) {
        hier_link_last(w, child, hier_mut(w, child), parent);
    }
    set_depths(w, child, new_depth);
    nph_world_event_(w, NPH_WORLD_EVENT_PARENT_CHANGED, child, NPH_COMPONENT_NONE);
    return NPH_RESULT_OK;
}

NphResult nph_world_despawn_recursive(NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    if (!nph_world_is_alive(w, e)) {
        return NPH_RESULT_NOT_FOUND;
    }
    // Remove leaves until the root has no children: each despawn detaches
    // the leaf, so no list of the subtree is needed.
    for (;;) {
        NphEntity leaf = e;
        for (NphEntity c = nph_world_first_child(w, leaf); !nph_entity_is_null(c);
             c = nph_world_first_child(w, leaf)) {
            leaf = c;
        }
        NPH_TRY(nph_world_despawn(w, leaf));
        if (nph_entity_equals(leaf, e)) {
            return NPH_RESULT_OK;
        }
    }
}

// ---------------------------------------------------------------------------
// The transform system
// ---------------------------------------------------------------------------

// Chunks per block in the gather passes, and entities per job in a level.
#define GATHER_BLOCK_CHUNKS 16U
#define LEVEL_BATCH         512U
#define ROOT_BATCH_CHUNKS   4U

struct NphTransformSystem {
    NphWorld *world;
    NphAllocator *alloc;
    NphQuery *roots;    // Local (read, changed), World (write), Hierarchy (optional, changed)
    NphQuery *children; // Hierarchy (read), with Local and World
    u32 *stamps;        // per entity slot: the run that last recomputed its world transform
    u32 capacity;
    u32 run_id;
    u32 last_tick; // for nph_transform_system_update

    // Growable scratch (amortised; docs/scene.md).
    void **chunks;
    u32 chunk_cap;
    u32 *hist; // block x (depth + 1) counts, then running offsets
    u32 hist_cap;
    u32 *bucket; // entity slots grouped by depth
    u32 bucket_cap;
    u32 *level_start; // depth -> first bucket index; [max_depth + 2]
    u32 level_cap;

    NphAtomicU32 roots_updated;
    NphAtomicU32 children_updated;
    NphTransformStats stats;
};

typedef struct RunState {
    NphTransformSystem *ts;
    NphWorld *world;
    u32 tick;      // stamped on written WorldTransform columns
    u32 last_tick; // chunks changed after this are recomputed
    u32 run_id;
    u32 chunk_count;
    u32 block_count;
    u32 depth_count; // max depth + 1
    u32 level;       // the depth being processed
} RunState;

static bool grow_u32(NphAllocator *alloc, u32 **arr, u32 *cap, u32 need)
{
    if (need <= *cap) {
        return true;
    }
    u32 cap2 = *cap == 0U ? 256U : *cap;
    while (cap2 < need) {
        cap2 = cap2 > NPH_U32_MAX / 2U ? need : cap2 * 2U;
    }
    u32 *p =
        (u32 *)NPH_REALLOC(alloc, *arr, sizeof(u32) * *cap, sizeof(u32) * cap2, NPH_ALIGNOF(u32));
    if (p == NULL) {
        return false;
    }
    *arr = p;
    *cap = cap2;
    return true;
}

static bool grow_ptr(NphAllocator *alloc, void ***arr, u32 *cap, u32 need)
{
    if (need <= *cap) {
        return true;
    }
    u32 cap2 = *cap == 0U ? 256U : *cap;
    while (cap2 < need) {
        cap2 = cap2 > NPH_U32_MAX / 2U ? need : cap2 * 2U;
    }
    void **p = (void **)NPH_REALLOC(alloc, *arr, sizeof(void *) * *cap, sizeof(void *) * cap2,
                                    NPH_ALIGNOF(void *));
    if (p == NULL) {
        return false;
    }
    *arr = p;
    *cap = cap2;
    return true;
}

// Runs fn over [0, count) in pieces of `batch`, on the job system when
// there is one, and waits.
static void run_range(NphJobSystem *js, u32 count, u32 batch, NphJobRangeFn fn, void *user)
{
    if (count == 0U) {
        return;
    }
    if (js == NULL || count <= batch) {
        fn(user, 0U, count);
        return;
    }
    NphJobCounter counter = NPH_JOB_COUNTER_INIT;
    if (nph_job_parallel_for(js, count, batch, fn, user, NPH_JOB_PRIORITY_NORMAL, &counter) !=
        NPH_RESULT_OK) {
        fn(user, 0U, count);
        return;
    }
    nph_job_wait(js, &counter);
}

static u32 slot_of(const NphWorld *w, NphEntity e)
{
    return e.value & w->handles.index_mask;
}

static bool chunk_changed(NphChunk *c, u32 since)
{
    const NphArchetype *a = c->archetype;
    const u8 lc = a->column_of[NPH_COMPONENT_ID_LOCAL_TRANSFORM];
    const u8 hc = a->column_of[NPH_COMPONENT_ID_HIERARCHY];
    return (lc != NPH_NO_COLUMN && nph_world_tick_newer(nph_chunk_version(c, lc), since)) ||
           (hc != NPH_NO_COLUMN && nph_world_tick_newer(nph_chunk_version(c, hc), since));
}

// Phase 1: roots, chunk by chunk.
static void roots_job(void *user, u32 begin, u32 end)
{
    RunState *rs = (RunState *)user;
    NphTransformSystem *ts = rs->ts;
    u32 updated = 0U;
    for (u32 i = begin; i < end; ++i) {
        NphChunkView v;
        nph_query_chunk_view(ts->roots, ts->chunks[i], rs->tick, &v);
        const NphLocalTransform *local = NPH_CHUNK_COLUMN(&v, 0, NphLocalTransform);
        NphWorldTransform *world = NPH_CHUNK_COLUMN(&v, 1, NphWorldTransform);
        const NphHierarchy *h = NPH_CHUNK_COLUMN(&v, 2, NphHierarchy);
        for (u32 r = 0; r < v.count; ++r) {
            if (h != NULL && !nph_entity_is_null(h[r].parent)) {
                continue; // a child: the level passes compute it
            }
            world[r] = nph_world_transform_from_local(&local[r]);
            ts->stamps[slot_of(rs->world, v.entities[r])] = rs->run_id;
            updated += 1U;
        }
    }
    nph_atomic_fetch_add_u32(&ts->roots_updated, updated, NPH_MEMORY_ORDER_RELAXED);
}

// Gather pass A: per block of chunks, how many children sit at each depth.
static void count_job(void *user, u32 begin, u32 end)
{
    RunState *rs = (RunState *)user;
    NphTransformSystem *ts = rs->ts;
    for (u32 b = begin; b < end; ++b) {
        u32 *hist = ts->hist + (u64)b * rs->depth_count;
        memset(hist, 0, sizeof(u32) * rs->depth_count);
        const u32 c_end = nph_min_u32((b + 1U) * GATHER_BLOCK_CHUNKS, rs->chunk_count);
        for (u32 ci = b * GATHER_BLOCK_CHUNKS; ci < c_end; ++ci) {
            NphChunk *c = (NphChunk *)ts->chunks[ci];
            const NphHierarchy *h = (const NphHierarchy *)nph_chunk_column(
                c, c->archetype->column_of[NPH_COMPONENT_ID_HIERARCHY]);
            for (u32 r = 0; r < c->count; ++r) {
                if (!nph_entity_is_null(h[r].parent)) {
                    hist[h[r].depth] += 1U;
                }
            }
        }
    }
}

// Gather pass B: each block writes its children's slots at its offsets.
static void fill_job(void *user, u32 begin, u32 end)
{
    RunState *rs = (RunState *)user;
    NphTransformSystem *ts = rs->ts;
    for (u32 b = begin; b < end; ++b) {
        u32 *offs = ts->hist + (u64)b * rs->depth_count;
        const u32 c_end = nph_min_u32((b + 1U) * GATHER_BLOCK_CHUNKS, rs->chunk_count);
        for (u32 ci = b * GATHER_BLOCK_CHUNKS; ci < c_end; ++ci) {
            NphChunk *c = (NphChunk *)ts->chunks[ci];
            const NphHierarchy *h = (const NphHierarchy *)nph_chunk_column(
                c, c->archetype->column_of[NPH_COMPONENT_ID_HIERARCHY]);
            const NphEntity *ents = nph_chunk_entities(c);
            for (u32 r = 0; r < c->count; ++r) {
                if (!nph_entity_is_null(h[r].parent)) {
                    ts->bucket[offs[h[r].depth]++] = slot_of(rs->world, ents[r]);
                }
            }
        }
    }
}

// One depth level: every child whose own chunk changed or whose parent was
// recomputed in this run.
static void level_job(void *user, u32 begin, u32 end)
{
    RunState *rs = (RunState *)user;
    NphTransformSystem *ts = rs->ts;
    const NphWorld *w = rs->world;
    const u32 first = ts->level_start[rs->level];
    u32 updated = 0U;
    for (u32 i = first + begin; i < first + end; ++i) {
        const u32 slot = ts->bucket[i];
        const NphEntityRecord *rec = &w->records[slot];
        NphChunk *c = rec->chunk;
        const NphArchetype *a = c->archetype;
        const NphHierarchy *h = (const NphHierarchy *)nph_chunk_cell(
            c, a->column_of[NPH_COMPONENT_ID_HIERARCHY], rec->row);
        const u32 parent_slot = slot_of(w, h->parent);
        if (!chunk_changed(c, rs->last_tick) && ts->stamps[parent_slot] != rs->run_id) {
            continue;
        }
        const NphLocalTransform *local = (const NphLocalTransform *)nph_chunk_cell(
            c, a->column_of[NPH_COMPONENT_ID_LOCAL_TRANSFORM], rec->row);
        const u8 wc = a->column_of[NPH_COMPONENT_ID_WORLD_TRANSFORM];
        NphWorldTransform *world = (NphWorldTransform *)nph_chunk_cell(c, wc, rec->row);
        const NphEntityRecord *prec = &w->records[parent_slot];
        const u8 pwc = prec->chunk->archetype->column_of[NPH_COMPONENT_ID_WORLD_TRANSFORM];
        if (pwc != NPH_NO_COLUMN) {
            const NphWorldTransform *pw =
                (const NphWorldTransform *)nph_chunk_cell(prec->chunk, pwc, prec->row);
            *world = nph_world_transform_compose(pw, local);
        } else {
            // A parent without a transform (a grouping entity) is the identity.
            *world = nph_world_transform_from_local(local);
        }
        nph_chunk_stamp(c, wc, rs->tick);
        ts->stamps[slot] = rs->run_id;
        updated += 1U;
    }
    nph_atomic_fetch_add_u32(&ts->children_updated, updated, NPH_MEMORY_ORDER_RELAXED);
}

static NphResult transform_run(NphTransformSystem *ts, NphJobSystem *js, u32 tick, u32 last_tick)
{
    NphWorld *w = ts->world;
    RunState rs;
    memset(&rs, 0, sizeof rs);
    rs.ts = ts;
    rs.world = w;
    rs.tick = tick;
    rs.last_tick = last_tick;
    ts->run_id = ts->run_id + 1U == 0U ? 1U : ts->run_id + 1U;
    rs.run_id = ts->run_id;
    nph_atomic_store_u32(&ts->roots_updated, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&ts->children_updated, 0U, NPH_MEMORY_ORDER_RELAXED);
    memset(&ts->stats, 0, sizeof ts->stats);

    // Phase 1: the roots of changed chunks.
    u32 n = nph_query_collect_chunks(ts->roots, last_tick, NULL, 0U);
    if (!grow_ptr(ts->alloc, &ts->chunks, &ts->chunk_cap, n)) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    n = nph_query_collect_chunks(ts->roots, last_tick, ts->chunks, ts->chunk_cap);
    rs.chunk_count = n;
    run_range(js, n, ROOT_BATCH_CHUNKS, roots_job, &rs);
    u32 all_roots = 0U;
    for (u32 i = 0; i < ts->roots->archetype_count; ++i) {
        all_roots += w->archetypes[ts->roots->archetypes[i]]->chunk_count;
    }
    ts->stats.chunks_skipped = all_roots - n;
    ts->stats.roots_updated = nph_atomic_load_u32(&ts->roots_updated, NPH_MEMORY_ORDER_RELAXED);

    // Phase 2: children, level by level. Skipped when no root moved and no
    // child's chunk changed.
    u32 kc = nph_query_collect_chunks(ts->children, 0U, NULL, 0U);
    if (kc == 0U) {
        return NPH_RESULT_OK;
    }
    if (!grow_ptr(ts->alloc, &ts->chunks, &ts->chunk_cap, kc)) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    kc = nph_query_collect_chunks(ts->children, 0U, ts->chunks, ts->chunk_cap);
    bool any = ts->stats.roots_updated > 0U;
    for (u32 i = 0; !any && i < kc; ++i) {
        any = chunk_changed((NphChunk *)ts->chunks[i], last_tick);
    }
    if (!any) {
        return NPH_RESULT_OK;
    }
    rs.chunk_count = kc;
    rs.block_count = (kc + GATHER_BLOCK_CHUNKS - 1U) / GATHER_BLOCK_CHUNKS;
    rs.depth_count = w->max_depth + 1U;
    const u64 hist_len = (u64)rs.block_count * rs.depth_count;
    if (hist_len > NPH_U32_MAX || !grow_u32(ts->alloc, &ts->hist, &ts->hist_cap, (u32)hist_len) ||
        !grow_u32(ts->alloc, &ts->level_start, &ts->level_cap, rs.depth_count + 1U)) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    run_range(js, rs.block_count, 1U, count_job, &rs);

    // Offsets: levels in order, blocks in order within a level, so the
    // buckets are filled deterministically.
    u32 total = 0U;
    for (u32 d = 0; d < rs.depth_count; ++d) {
        ts->level_start[d] = total;
        for (u32 b = 0; b < rs.block_count; ++b) {
            u32 *cell = &ts->hist[(u64)b * rs.depth_count + d];
            const u32 count = *cell;
            *cell = total;
            total += count;
        }
    }
    ts->level_start[rs.depth_count] = total;
    if (!grow_u32(ts->alloc, &ts->bucket, &ts->bucket_cap, total)) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    run_range(js, rs.block_count, 1U, fill_job, &rs);

    for (u32 d = 1; d < rs.depth_count; ++d) {
        const u32 count = ts->level_start[d + 1U] - ts->level_start[d];
        if (count == 0U) {
            continue;
        }
        rs.level = d;
        run_range(js, count, LEVEL_BATCH, level_job, &rs);
        ts->stats.levels += 1U;
    }
    ts->stats.children_updated =
        nph_atomic_load_u32(&ts->children_updated, NPH_MEMORY_ORDER_RELAXED);
    return NPH_RESULT_OK;
}

static void transform_system_fn(NphSystemContext *ctx)
{
    NphTransformSystem *ts = (NphTransformSystem *)ctx->user;
    const NphResult r = transform_run(ts, ctx->jobs, ctx->tick, ctx->last_tick);
    if (r != NPH_RESULT_OK) {
        nph_system_report(ctx, r);
    }
}

NphResult nph_transform_system_create(NphWorld *w, NphAllocator *alloc, NphTransformSystem **out)
{
    NPH_ASSERT(w != NULL && alloc != NULL && out != NULL);
    *out = NULL;
    NphTransformSystem *ts = NPH_ALLOC_TYPE(alloc, NphTransformSystem);
    if (ts == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    ts->world = w;
    ts->alloc = alloc;
    ts->capacity = w->capacity;
    ts->stamps = NPH_ALLOC_ARRAY_ZEROED(alloc, u32, w->capacity);
    NphResult r = ts->stamps != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        const NphQueryTerm terms[3] = {
            nph_term_changed(nph_term_read(NPH_COMPONENT_ID_LOCAL_TRANSFORM)),
            nph_term_write(NPH_COMPONENT_ID_WORLD_TRANSFORM),
            nph_term_changed(nph_term_optional(NPH_COMPONENT_ID_HIERARCHY, NPH_ACCESS_READ)),
        };
        const NphQueryDesc qd = {terms, 3U};
        r = nph_query_create(w, &qd, &ts->roots);
    }
    if (r == NPH_RESULT_OK) {
        const NphQueryTerm terms[3] = {
            nph_term_read(NPH_COMPONENT_ID_HIERARCHY),
            nph_term_with(NPH_COMPONENT_ID_LOCAL_TRANSFORM),
            nph_term_with(NPH_COMPONENT_ID_WORLD_TRANSFORM),
        };
        const NphQueryDesc qd = {terms, 3U};
        r = nph_query_create(w, &qd, &ts->children);
    }
    if (r != NPH_RESULT_OK) {
        nph_transform_system_destroy(ts);
        return r;
    }
    *out = ts;
    return NPH_RESULT_OK;
}

void nph_transform_system_destroy(NphTransformSystem *ts)
{
    if (ts == NULL) {
        return;
    }
    NphAllocator *alloc = ts->alloc;
    nph_query_destroy(ts->roots);
    nph_query_destroy(ts->children);
    if (ts->stamps != NULL) {
        NPH_FREE_ARRAY(alloc, u32, ts->stamps, ts->capacity);
    }
    if (ts->chunks != NULL) {
        NPH_FREE_ARRAY(alloc, void *, ts->chunks, ts->chunk_cap);
    }
    if (ts->hist != NULL) {
        NPH_FREE_ARRAY(alloc, u32, ts->hist, ts->hist_cap);
    }
    if (ts->bucket != NULL) {
        NPH_FREE_ARRAY(alloc, u32, ts->bucket, ts->bucket_cap);
    }
    if (ts->level_start != NULL) {
        NPH_FREE_ARRAY(alloc, u32, ts->level_start, ts->level_cap);
    }
    NPH_FREE_TYPE(alloc, NphTransformSystem, ts);
}

NphSystemDesc nph_transform_system_desc(NphTransformSystem *ts)
{
    static const NphComponentId reads[2] = {NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                            NPH_COMPONENT_ID_HIERARCHY};
    static const NphComponentId writes[1] = {NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = "nph.transform";
    d.run = transform_system_fn;
    d.user = ts;
    d.reads = reads;
    d.read_count = 2U;
    d.writes = writes;
    d.write_count = 1U;
    return d;
}

NphResult nph_transform_system_update(NphTransformSystem *ts, NphJobSystem *jobs)
{
    NPH_ASSERT(ts != NULL);
    NphWorld *w = ts->world;
    const u32 tick = nph_world_advance_tick(w);
    w->frozen += 1U;
    const NphResult r = transform_run(ts, jobs, tick, ts->last_tick);
    w->frozen -= 1U;
    ts->last_tick = tick;
    return r;
}

void nph_transform_system_stats(const NphTransformSystem *ts, NphTransformStats *out)
{
    NPH_ASSERT(ts != NULL && out != NULL);
    *out = ts->stats;
}
