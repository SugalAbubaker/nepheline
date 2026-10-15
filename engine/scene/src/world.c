// SPDX-License-Identifier: Apache-2.0
// The world: component registry, entity table, archetypes, chunk memory,
// and the per-entity component operations (world.h; docs/scene.md).

#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/scene/transform.h>

#include "scene_internal.h"

#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_SCENE, "scene", NPH_LOG_LEVEL_INFO);

#define DEFAULT_CHUNKS_PER_SLAB 64U
#define MIN_INDEX_BITS          10U
// Released entity slots wait until this many others were released before
// they are reused, so a stale handle meets a recycled slot only after the
// generation counter wraps many times over (core/handle.h).
#define REUSE_DELAY 1024U
#define CHUNK_ALIGN 64U

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void nph_world_structural_begin_(NphWorld *w)
{
    NPH_ASSERT_MSG(w->frozen == 0U,
                   "structural change to a world while a schedule stage runs on it; "
                   "record it in a command buffer instead");
    w->epoch += 1U;
}

const NphComponentInfo *nph_world_info_(const NphWorld *w, NphComponentId id)
{
    return id < w->component_count ? &w->components[id] : NULL;
}

NphEntityRecord *nph_world_record_(const NphWorld *w, NphEntity e)
{
    const NphHandle h = {e.value};
    if (!nph_handle_pool_is_valid(&w->handles, h)) {
        return NULL;
    }
    NphEntityRecord *rec = &w->records[nph_handle_pool_index(&w->handles, h)];
    return rec->chunk != NULL ? rec : NULL;
}

void nph_world_event_(NphWorld *w, NphWorldEventType type, NphEntity e, NphComponentId id)
{
    if (w->event_capacity == 0U) {
        return;
    }
    if (w->event_count == w->event_capacity) {
        w->events_dropped += 1U;
        return;
    }
    NphWorldEvent *ev = &w->events[w->event_count++];
    ev->type = (u32)type;
    ev->entity = e;
    ev->component = id;
    ev->tick = nph_world_change_tick_(w);
}

static void stamp_all(NphChunk *c, u32 tick)
{
    const u32 n = c->archetype->column_count;
    for (u32 col = 0; col < n; ++col) {
        nph_chunk_stamp(c, col, tick);
    }
}

// ---------------------------------------------------------------------------
// Chunk memory: 16 KiB chunks carved from slabs
// ---------------------------------------------------------------------------

static u32 slab_find(const NphWorld *w, const void *chunk)
{
    // Slabs are sorted by base; find the last one starting at or before
    // the chunk.
    u32 lo = 0U;
    u32 hi = w->slab_count;
    const uptr p = (uptr)chunk;
    while (hi - lo > 1U) {
        const u32 mid = lo + (hi - lo) / 2U;
        if ((uptr)w->slabs[mid].base <= p) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    NPH_ASSERT(w->slab_count > 0U && (uptr)w->slabs[lo].base <= p &&
               p < (uptr)w->slabs[lo].base + (uptr)w->chunks_per_slab * NPH_ECS_CHUNK_SIZE);
    return lo;
}

static void free_list_push(NphWorld *w, NphChunk *c)
{
    // The link lives in the chunk's first word; the rest is poisoned so a
    // stale chunk pointer is reported by ASan.
    memcpy(c, &w->free_chunks, sizeof(NphChunk *));
    w->free_chunks = c;
    w->free_chunk_count += 1U;
    NPH_ASAN_POISON((u8 *)c + sizeof(NphChunk *), NPH_ECS_CHUNK_SIZE - sizeof(NphChunk *));
}

static bool slab_grow(NphWorld *w)
{
    if (w->slab_count == w->slab_cap) {
        const u32 cap = w->slab_cap == 0U ? 8U : w->slab_cap * 2U;
        NphSlab *slabs = (NphSlab *)NPH_REALLOC(w->alloc, w->slabs, sizeof(NphSlab) * w->slab_cap,
                                                sizeof(NphSlab) * cap, NPH_ALIGNOF(NphSlab));
        if (slabs == NULL) {
            return false;
        }
        w->slabs = slabs;
        w->slab_cap = cap;
    }
    const u64 bytes = (u64)w->chunks_per_slab * NPH_ECS_CHUNK_SIZE;
    u8 *base = (u8 *)NPH_ALLOC(w->alloc, bytes, CHUNK_ALIGN);
    if (base == NULL) {
        return false;
    }
    // Insert sorted by address.
    u32 at = w->slab_count;
    while (at > 0U && (uptr)w->slabs[at - 1U].base > (uptr)base) {
        w->slabs[at] = w->slabs[at - 1U];
        --at;
    }
    w->slabs[at].base = base;
    w->slabs[at].live = 0U;
    w->slabs[at].reserved = 0U;
    w->slab_count += 1U;
    // Push in reverse so the lowest addresses are handed out first.
    for (u32 i = w->chunks_per_slab; i > 0U; --i) {
        free_list_push(w, (NphChunk *)(base + (u64)(i - 1U) * NPH_ECS_CHUNK_SIZE));
    }
    return true;
}

static NphChunk *chunk_acquire(NphWorld *w)
{
    if (w->max_chunks != 0U && w->chunks_in_use >= w->max_chunks) {
        return NULL;
    }
    if (w->free_chunks == NULL && !slab_grow(w)) {
        return NULL;
    }
    NphChunk *c = w->free_chunks;
    NPH_ASAN_UNPOISON(c, NPH_ECS_CHUNK_SIZE);
    NphChunk *next;
    memcpy(&next, c, sizeof next);
    w->free_chunks = next;
    w->free_chunk_count -= 1U;
    w->chunks_in_use += 1U;
    w->slabs[slab_find(w, c)].live += 1U;
    return c;
}

static void chunk_release(NphWorld *w, NphChunk *c)
{
    w->slabs[slab_find(w, c)].live -= 1U;
    w->chunks_in_use -= 1U;
    free_list_push(w, c);
}

void nph_world_trim(NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    // Rebuild the free list without the chunks of empty slabs, then free
    // those slabs.
    NphChunk *keep = NULL;
    u32 keep_count = 0U;
    NphChunk *c = w->free_chunks;
    while (c != NULL) {
        NPH_ASAN_UNPOISON(c, sizeof(NphChunk *));
        NphChunk *next;
        memcpy(&next, c, sizeof next);
        if (w->slabs[slab_find(w, c)].live != 0U) {
            memcpy(c, &keep, sizeof keep);
            keep = c;
            keep_count += 1U;
        }
        NPH_ASAN_POISON(c, sizeof(NphChunk *));
        c = next;
    }
    // Re-link in address order (the list is rebuilt reversed above).
    w->free_chunks = NULL;
    w->free_chunk_count = 0U;
    while (keep != NULL) {
        NPH_ASAN_UNPOISON(keep, sizeof(NphChunk *));
        NphChunk *next;
        memcpy(&next, keep, sizeof next);
        free_list_push(w, keep);
        keep = next;
    }
    NPH_ASSERT(w->free_chunk_count == keep_count);
    u32 out = 0U;
    const u64 bytes = (u64)w->chunks_per_slab * NPH_ECS_CHUNK_SIZE;
    for (u32 i = 0; i < w->slab_count; ++i) {
        if (w->slabs[i].live == 0U) {
            NPH_ASAN_UNPOISON(w->slabs[i].base, bytes);
            NPH_FREE(w->alloc, w->slabs[i].base, bytes, CHUNK_ALIGN);
        } else {
            w->slabs[out++] = w->slabs[i];
        }
    }
    w->slab_count = out;
}

// ---------------------------------------------------------------------------
// Archetypes
// ---------------------------------------------------------------------------

static void archetype_free(NphWorld *w, NphArchetype *a)
{
    if (a == NULL) {
        return;
    }
    const u32 nc = nph_max_u32(a->component_count, 1U);
    const u32 ncol = nph_max_u32(a->column_count, 1U);
    NPH_FREE_ARRAY(w->alloc, NphComponentId, a->components, nc);
    NPH_FREE_ARRAY(w->alloc, NphComponentId, a->column_ids, ncol);
    NPH_FREE_ARRAY(w->alloc, u32, a->column_offsets, ncol);
    NPH_FREE_ARRAY(w->alloc, u32, a->column_sizes, ncol);
    if (a->chunks != NULL) {
        NPH_FREE_ARRAY(w->alloc, NphChunk *, a->chunks, a->chunk_cap);
    }
    NPH_FREE_TYPE(w->alloc, NphArchetype, a);
}

// The byte offset one past the last column for `capacity` rows, or 0 when
// the layout does not fit a chunk.
static u32 archetype_layout(NphArchetype *a, u32 capacity, bool commit)
{
    u64 off = (u64)a->entities_offset + (u64)capacity * sizeof(NphEntity);
    for (u32 col = 0; col < a->column_count; ++col) {
        off = nph_align_up_u64(off, CHUNK_ALIGN);
        if (commit) {
            a->column_offsets[col] = (u32)off;
        }
        off += (u64)capacity * a->column_sizes[col];
    }
    return off <= NPH_ECS_CHUNK_SIZE ? (u32)off : 0U;
}

static NphResult archetype_create(NphWorld *w, const NphComponentMask *mask, NphArchetype **out)
{
    u32 count = 0U;
    u32 columns = 0U;
    u64 row_bytes = sizeof(NphEntity);
    for (u32 id = 0; id < w->component_count; ++id) {
        if (nph_mask_test(mask, id)) {
            count += 1U;
            const u32 size = w->components[id].desc->size;
            if (size > 0U) {
                columns += 1U;
                row_bytes += size;
            }
        }
    }
    if (columns > NPH_MAX_COLUMNS) {
        NPH_LOG_ERROR(&NPH_LOG_SCENE, "an archetype of %u data components exceeds the limit of %u",
                      columns, NPH_MAX_COLUMNS);
        return NPH_RESULT_INVALID_ARGUMENT;
    }

    NphArchetype *a = NPH_ALLOC_TYPE(w->alloc, NphArchetype);
    if (a == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    a->mask = *mask;
    a->component_count = count;
    a->column_count = columns;
    a->components = NPH_ALLOC_ARRAY(w->alloc, NphComponentId, nph_max_u32(count, 1U));
    a->column_ids = NPH_ALLOC_ARRAY(w->alloc, NphComponentId, nph_max_u32(columns, 1U));
    a->column_offsets = NPH_ALLOC_ARRAY_ZEROED(w->alloc, u32, nph_max_u32(columns, 1U));
    a->column_sizes = NPH_ALLOC_ARRAY_ZEROED(w->alloc, u32, nph_max_u32(columns, 1U));
    if (a->components == NULL || a->column_ids == NULL || a->column_offsets == NULL ||
        a->column_sizes == NULL) {
        archetype_free(w, a);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(a->column_of, NPH_NO_COLUMN, sizeof a->column_of);
    u32 ci = 0U;
    u32 col = 0U;
    for (u32 id = 0; id < w->component_count; ++id) {
        if (!nph_mask_test(mask, id)) {
            continue;
        }
        a->components[ci++] = id;
        const u32 size = w->components[id].desc->size;
        if (size > 0U) {
            a->column_ids[col] = id;
            a->column_sizes[col] = size;
            a->column_of[id] = (u8)col;
            col += 1U;
        }
    }

    // Layout: header, versions, entity handles, then the columns, each on
    // a cache line. The capacity starts from a bound that always fits
    // (every column paying the full alignment padding) and grows while
    // the next row still fits.
    a->versions_offset = (u32)nph_align_up_u64(sizeof(NphChunk), 8U);
    a->entities_offset =
        (u32)nph_align_up_u64((u64)a->versions_offset + 4U * (u64)columns, CHUNK_ALIGN);
    const u64 fixed = (u64)a->entities_offset + (u64)columns * CHUNK_ALIGN;
    u32 capacity =
        fixed < NPH_ECS_CHUNK_SIZE ? (u32)((NPH_ECS_CHUNK_SIZE - fixed) / row_bytes) : 0U;
    while (archetype_layout(a, capacity + 1U, false) != 0U) {
        capacity += 1U;
    }
    if (capacity == 0U || archetype_layout(a, capacity, true) == 0U) {
        NPH_LOG_ERROR(&NPH_LOG_SCENE,
                      "an archetype of %u components (%llu bytes per entity) "
                      "does not fit a %u-byte chunk",
                      count, (unsigned long long)row_bytes, NPH_ECS_CHUNK_SIZE);
        archetype_free(w, a);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    a->capacity = capacity;

    // Reserve everything the registration needs, then commit: nothing
    // below can fail, so a failure leaves no half-registered archetype.
    if (w->archetype_count == w->archetype_cap) {
        const u32 cap = w->archetype_cap == 0U ? 16U : w->archetype_cap * 2U;
        NphArchetype **arr = (NphArchetype **)NPH_REALLOC(
            w->alloc, w->archetypes, sizeof(NphArchetype *) * w->archetype_cap,
            sizeof(NphArchetype *) * cap, NPH_ALIGNOF(NphArchetype *));
        if (arr == NULL) {
            archetype_free(w, a);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        w->archetypes = arr;
        w->archetype_cap = cap;
    }
    if (nph_hashmap_reserve(&w->archetype_map, nph_hashmap_len(&w->archetype_map) + 1U) !=
        NPH_RESULT_OK) {
        archetype_free(w, a);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < w->query_count; ++i) {
        if (nph_query_reserve_(w->queries[i], 1U) != NPH_RESULT_OK) {
            archetype_free(w, a);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    a->index = w->archetype_count;
    for (u32 i = 0; i < w->query_count; ++i) {
        nph_query_on_archetype_(w->queries[i], a);
    }
    w->archetypes[w->archetype_count++] = a;
    const u32 index = a->index;
    NPH_VERIFY(nph_hashmap_insert(&w->archetype_map, mask, &index) == NPH_RESULT_OK);
    *out = a;
    return NPH_RESULT_OK;
}

NphResult nph_world_archetype_(NphWorld *w, const NphComponentMask *mask, NphArchetype **out)
{
    const u32 *found = (const u32 *)nph_hashmap_find(&w->archetype_map, mask);
    if (found != NULL) {
        *out = w->archetypes[*found];
        return NPH_RESULT_OK;
    }
    return archetype_create(w, mask, out);
}

// The archetype of `src` with `id` added or removed, through the edge cache.
static NphResult archetype_step(NphWorld *w, NphArchetype *src, NphComponentId id, bool add,
                                NphArchetype **out)
{
    const u64 key = ((u64)src->index << 9U) | ((u64)id << 1U) | (add ? 1U : 0U);
    const u32 *found = (const u32 *)nph_hashmap_find_u64(&w->edges, key);
    if (found != NULL) {
        *out = w->archetypes[*found];
        return NPH_RESULT_OK;
    }
    NphComponentMask mask = src->mask;
    if (add) {
        nph_mask_set(&mask, id);
    } else {
        nph_mask_clear(&mask, id);
    }
    NPH_TRY(nph_world_archetype_(w, &mask, out));
    // A failure to cache the edge only costs a lookup next time.
    const u32 index = (*out)->index;
    NPH_IGNORE_RESULT(nph_hashmap_set_u64(&w->edges, key, &index));
    return NPH_RESULT_OK;
}

static NphResult archetype_reserve_chunks(NphWorld *w, NphArchetype *a, u32 extra)
{
    const u64 need = (u64)a->chunk_count + extra;
    if (need <= a->chunk_cap) {
        return NPH_RESULT_OK;
    }
    if (need > NPH_U32_MAX / 2U) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 cap = a->chunk_cap == 0U ? 4U : a->chunk_cap;
    while (cap < need) {
        cap *= 2U;
    }
    NphChunk **arr =
        (NphChunk **)NPH_REALLOC(w->alloc, a->chunks, sizeof(NphChunk *) * a->chunk_cap,
                                 sizeof(NphChunk *) * cap, NPH_ALIGNOF(NphChunk *));
    if (arr == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    a->chunks = arr;
    a->chunk_cap = cap;
    return NPH_RESULT_OK;
}

// Appends an acquired chunk to `a` (capacity reserved by the caller).
static void archetype_push_chunk(NphArchetype *a, NphChunk *c, u32 tick)
{
    NPH_ASSERT(a->chunk_count < a->chunk_cap);
    c->archetype = a;
    c->count = 0U;
    c->index = a->chunk_count;
    a->chunks[a->chunk_count++] = c;
    stamp_all(c, tick);
}

// Claims a row at the end of `a`. Nothing changes on failure.
static NphResult row_alloc(NphWorld *w, NphArchetype *a, u32 tick, NphChunk **out_chunk,
                           u32 *out_row)
{
    NphChunk *c = a->chunk_count > 0U ? a->chunks[a->chunk_count - 1U] : NULL;
    if (c == NULL || c->count == a->capacity) {
        NPH_TRY(archetype_reserve_chunks(w, a, 1U));
        c = chunk_acquire(w);
        if (c == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        archetype_push_chunk(a, c, tick);
    } else {
        stamp_all(c, tick);
    }
    *out_chunk = c;
    *out_row = c->count++;
    a->entity_count += 1U;
    return NPH_RESULT_OK;
}

// Removes row `row` of `c`, filling it with the archetype's last entity so
// the chunks stay dense.
static void row_remove(NphWorld *w, NphChunk *c, u32 row, u32 tick)
{
    NphArchetype *a = c->archetype;
    NphChunk *last = a->chunks[a->chunk_count - 1U];
    const u32 last_row = last->count - 1U;
    if (last != c || last_row != row) {
        const NphEntity moved = nph_chunk_entities(last)[last_row];
        nph_chunk_entities(c)[row] = moved;
        for (u32 col = 0; col < a->column_count; ++col) {
            memcpy(nph_chunk_cell(c, col, row), nph_chunk_cell(last, col, last_row),
                   a->column_sizes[col]);
            nph_chunk_stamp(c, col, tick);
        }
        NphEntityRecord *rec =
            &w->records[nph_handle_pool_index(&w->handles, (NphHandle){moved.value})];
        rec->chunk = c;
        rec->row = row;
    }
    last->count -= 1U;
    a->entity_count -= 1U;
    if (last->count == 0U) {
        a->chunk_count -= 1U;
        chunk_release(w, last);
    }
}

static void init_defaults(const NphWorld *w, NphChunk *c, u32 row)
{
    const NphArchetype *a = c->archetype;
    for (u32 col = 0; col < a->column_count; ++col) {
        nph_component_init_default(w->components[a->column_ids[col]].desc,
                                   nph_chunk_cell(c, col, row));
    }
}

NphResult nph_world_move_(NphWorld *w, NphEntity e, NphArchetype *dst)
{
    NphEntityRecord *rec = nph_world_record_(w, e);
    NPH_ASSERT(rec != NULL);
    NphChunk *sc = rec->chunk;
    const u32 srow = rec->row;
    const NphArchetype *src = sc->archetype;
    NPH_ASSERT(src != dst);
    const u32 tick = nph_world_change_tick_(w);
    NphChunk *dc;
    u32 drow;
    NPH_TRY(row_alloc(w, dst, tick, &dc, &drow));
    for (u32 col = 0; col < dst->column_count; ++col) {
        const NphComponentId id = dst->column_ids[col];
        const u8 scol = src->column_of[id];
        if (scol != NPH_NO_COLUMN) {
            memcpy(nph_chunk_cell(dc, col, drow), nph_chunk_cell(sc, scol, srow),
                   dst->column_sizes[col]);
        } else {
            nph_component_init_default(w->components[id].desc, nph_chunk_cell(dc, col, drow));
        }
    }
    nph_chunk_entities(dc)[drow] = e;
    row_remove(w, sc, srow, tick);
    rec->chunk = dc;
    rec->row = drow;
    return NPH_RESULT_OK;
}

void *nph_world_cell_mut_(NphWorld *w, NphEntityRecord *rec, NphComponentId id)
{
    const u8 col = rec->chunk->archetype->column_of[id];
    if (col == NPH_NO_COLUMN) {
        return NULL;
    }
    nph_chunk_stamp(rec->chunk, col, nph_world_change_tick_(w));
    return nph_chunk_cell(rec->chunk, col, rec->row);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static NphResult register_component(NphWorld *w, const NphComponentDesc *desc, NphComponentId *out)
{
    const char *reason = "";
    const NphResult valid = nph_component_desc_validate(desc, &reason);
    if (valid != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_SCENE, "component '%s' is invalid: %s",
                      desc != NULL && desc->name != NULL ? desc->name : "(null)", reason);
        return valid;
    }
    const NphStrId name_id = nph_str_id_cstr(desc->name);
    const u64 layout = nph_component_layout_hash(desc);
    const u32 *existing = (const u32 *)nph_hashmap_find_u64(&w->component_names, name_id.value);
    if (existing != NULL) {
        const NphComponentInfo *info = &w->components[*existing];
        if (info->layout_hash == layout && info->desc->flags == desc->flags) {
            *out = *existing;
            return NPH_RESULT_OK;
        }
        NPH_LOG_ERROR(&NPH_LOG_SCENE, "component '%s' is already registered with another layout",
                      desc->name);
        return NPH_RESULT_ALREADY_EXISTS;
    }
    if (w->component_count >= NPH_ECS_MAX_COMPONENTS) {
        return NPH_RESULT_FULL;
    }
    const NphComponentId id = w->component_count;
    NPH_TRY(nph_hashmap_insert_u64(&w->component_names, name_id.value, &id));
    NphComponentInfo *info = &w->components[id];
    memset(info, 0, sizeof *info);
    info->desc = desc;
    info->name_id = name_id;
    info->layout_hash = layout;
    for (u32 i = 0; i < desc->field_count; ++i) {
        if (desc->fields[i].kind == NPH_FIELD_ENTITY) {
            info->entity_field_offsets[info->entity_field_count++] = (u16)desc->fields[i].offset;
        }
    }
    w->component_count += 1U;
    *out = id;
    return NPH_RESULT_OK;
}

NphResult nph_world_register_component(NphWorld *w, const NphComponentDesc *desc,
                                       NphComponentId *out)
{
    NPH_ASSERT(w != NULL && out != NULL);
    *out = NPH_COMPONENT_NONE;
    return register_component(w, desc, out);
}

NphComponentId nph_world_find_component(const NphWorld *w, NphStr name)
{
    NPH_ASSERT(w != NULL);
    const u32 *found =
        (const u32 *)nph_hashmap_find_u64(&w->component_names, nph_str_id(name).value);
    if (found == NULL) {
        return NPH_COMPONENT_NONE;
    }
    // Guard against a 64-bit hash collision between two names.
    return nph_str_equals(nph_str_from_cstr(w->components[*found].desc->name), name)
               ? *found
               : NPH_COMPONENT_NONE;
}

const NphComponentDesc *nph_world_component_desc(const NphWorld *w, NphComponentId id)
{
    NPH_ASSERT(w != NULL);
    return id < w->component_count ? w->components[id].desc : NULL;
}

u32 nph_world_component_count(const NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    return w->component_count;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void world_free(NphWorld *w)
{
    NphAllocator *alloc = w->alloc;
    for (u32 i = 0; i < w->archetype_count; ++i) {
        archetype_free(w, w->archetypes[i]);
    }
    if (w->archetypes != NULL) {
        NPH_FREE_ARRAY(alloc, NphArchetype *, w->archetypes, w->archetype_cap);
    }
    const u64 slab_bytes = (u64)w->chunks_per_slab * NPH_ECS_CHUNK_SIZE;
    for (u32 i = 0; i < w->slab_count; ++i) {
        NPH_ASAN_UNPOISON(w->slabs[i].base, slab_bytes);
        NPH_FREE(alloc, w->slabs[i].base, slab_bytes, CHUNK_ALIGN);
    }
    if (w->slabs != NULL) {
        NPH_FREE_ARRAY(alloc, NphSlab, w->slabs, w->slab_cap);
    }
    if (w->queries != NULL) {
        NPH_FREE_ARRAY(alloc, NphQuery *, w->queries, w->query_cap);
    }
    if (w->records != NULL) {
        NPH_FREE_ARRAY(alloc, NphEntityRecord, w->records, w->capacity);
    }
    if (w->events != NULL) {
        NPH_FREE_ARRAY(alloc, NphWorldEvent, w->events, w->event_capacity);
    }
    nph_handle_pool_free(&w->handles);
    nph_hashmap_free(&w->component_names);
    nph_hashmap_free(&w->archetype_map);
    nph_hashmap_free(&w->edges);
    if (w->strings_ready) {
        nph_str_table_free(&w->strings);
    }
    NPH_FREE_TYPE(alloc, NphWorld, w);
}

NphResult nph_world_create(const NphWorldDesc *desc, NphWorld **out)
{
    NPH_ASSERT(desc != NULL && out != NULL);
    *out = NULL;
    if (desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 capacity =
        desc->max_entities == 0U ? NPH_ECS_DEFAULT_MAX_ENTITIES : desc->max_entities;
    if (capacity > NPH_ECS_MAX_ENTITIES) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphWorld *w = NPH_ALLOC_TYPE(desc->alloc, NphWorld);
    if (w == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    w->alloc = desc->alloc;
    w->capacity = capacity;
    w->chunks_per_slab =
        desc->chunks_per_slab == 0U ? DEFAULT_CHUNKS_PER_SLAB : desc->chunks_per_slab;
    w->max_chunks = desc->max_chunks;

    NphResult r = NPH_RESULT_OK;
    const u32 index_bits = nph_max_u32(MIN_INDEX_BITS, nph_log2_ceil_u32(capacity));
    r = nph_handle_pool_init(&w->handles, w->alloc, capacity, index_bits, REUSE_DELAY);
    if (r == NPH_RESULT_OK) {
        w->records = NPH_ALLOC_ARRAY_ZEROED(w->alloc, NphEntityRecord, capacity);
        r = w->records != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    if (r == NPH_RESULT_OK) {
        const NphHashMapDesc names = nph_hashmap_desc_u64(sizeof(u32));
        r = nph_hashmap_init(&w->component_names, w->alloc, &names);
    }
    if (r == NPH_RESULT_OK) {
        const NphHashMapDesc arch = {sizeof(NphComponentMask), sizeof(u32), NULL, NULL, 0U};
        r = nph_hashmap_init(&w->archetype_map, w->alloc, &arch);
    }
    if (r == NPH_RESULT_OK) {
        const NphHashMapDesc edges = nph_hashmap_desc_u64(sizeof(u32));
        r = nph_hashmap_init(&w->edges, w->alloc, &edges);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_str_table_init(&w->strings, w->alloc, 0U);
        w->strings_ready = r == NPH_RESULT_OK;
    }
    if (r == NPH_RESULT_OK && desc->event_capacity > 0U) {
        w->events = NPH_ALLOC_ARRAY(w->alloc, NphWorldEvent, desc->event_capacity);
        w->event_capacity = w->events != NULL ? desc->event_capacity : 0U;
        r = w->events != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; r == NPH_RESULT_OK && i < NPH_COMPONENT_BUILTIN_COUNT; ++i) {
        NphComponentId id;
        r = register_component(w, nph_builtin_component_desc_(i), &id);
        NPH_ASSERT(r != NPH_RESULT_OK || id == i);
    }
    if (r == NPH_RESULT_OK && desc->registry_from != NULL) {
        const NphWorld *from = desc->registry_from;
        for (u32 i = NPH_COMPONENT_BUILTIN_COUNT; r == NPH_RESULT_OK && i < from->component_count;
             ++i) {
            NphComponentId id;
            r = register_component(w, from->components[i].desc, &id);
            NPH_ASSERT(r != NPH_RESULT_OK || id == i);
        }
    }
    if (r != NPH_RESULT_OK) {
        world_free(w);
        return r;
    }
    *out = w;
    return NPH_RESULT_OK;
}

void nph_world_destroy(NphWorld *w)
{
    if (w == NULL) {
        return;
    }
    NPH_ASSERT_MSG(w->query_count == 0U, "destroy the world's %u queries first", w->query_count);
    NPH_ASSERT_MSG(w->frozen == 0U, "world destroyed while a schedule stage runs on it");
    world_free(w);
}

void nph_world_clear(NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    nph_world_structural_begin_(w);
    const u32 slots = w->handles.bump;
    for (u32 i = 0; i < slots; ++i) {
        NphEntityRecord *rec = &w->records[i];
        if (rec->chunk == NULL) {
            continue;
        }
        const NphEntity e = nph_world_entity_at(w, i);
        nph_world_event_(w, NPH_WORLD_EVENT_DESPAWNED, e, NPH_COMPONENT_NONE);
        NPH_VERIFY(nph_handle_pool_release(&w->handles, (NphHandle){e.value}));
        rec->chunk = NULL;
        rec->row = 0U;
    }
    for (u32 i = 0; i < w->archetype_count; ++i) {
        NphArchetype *a = w->archetypes[i];
        for (u32 c = 0; c < a->chunk_count; ++c) {
            chunk_release(w, a->chunks[c]);
        }
        a->chunk_count = 0U;
        a->entity_count = 0U;
    }
}

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

static NphResult mask_from_ids(const NphWorld *w, const NphComponentId *ids, u32 count,
                               NphComponentMask *out)
{
    memset(out, 0, sizeof *out);
    if (count > 0U && ids == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < count; ++i) {
        if (ids[i] >= w->component_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        nph_mask_set(out, ids[i]);
    }
    return NPH_RESULT_OK;
}

static NphEntity make_entity(const NphWorld *w, u32 index)
{
    const NphEntity e = {(w->handles.generations[index] << w->handles.index_bits) | index};
    return e;
}

NphResult nph_world_spawn(NphWorld *w, const NphComponentId *ids, u32 id_count, NphEntity *out)
{
    NPH_ASSERT(w != NULL && out != NULL);
    *out = NPH_ENTITY_NULL;
    nph_world_structural_begin_(w);
    NphComponentMask mask;
    NPH_TRY(mask_from_ids(w, ids, id_count, &mask));
    if (nph_handle_pool_live(&w->handles) >= w->capacity) {
        return NPH_RESULT_FULL;
    }
    NphArchetype *a;
    NPH_TRY(nph_world_archetype_(w, &mask, &a));
    NphChunk *c;
    u32 row;
    NPH_TRY(row_alloc(w, a, nph_world_change_tick_(w), &c, &row));
    NphHandle h;
    NPH_VERIFY(nph_handle_pool_alloc(&w->handles, &h) == NPH_RESULT_OK);
    const NphEntity e = {h.value};
    nph_chunk_entities(c)[row] = e;
    init_defaults(w, c, row);
    NphEntityRecord *rec = &w->records[nph_handle_pool_index(&w->handles, h)];
    rec->chunk = c;
    rec->row = row;
    nph_world_event_(w, NPH_WORLD_EVENT_SPAWNED, e, NPH_COMPONENT_NONE);
    *out = e;
    return NPH_RESULT_OK;
}

// Fills `count` rows starting at `row` of every column with the defaults:
// one row is initialised, then copied by doubling.
static void fill_defaults(const NphWorld *w, NphChunk *c, u32 row, u32 count)
{
    const NphArchetype *a = c->archetype;
    for (u32 col = 0; col < a->column_count; ++col) {
        const NphComponentDesc *desc = w->components[a->column_ids[col]].desc;
        u8 *base = nph_chunk_cell(c, col, row);
        const u64 size = a->column_sizes[col];
        if (desc->defaults == NULL) {
            memset(base, 0, (size_t)(size * count));
            continue;
        }
        memcpy(base, desc->defaults, (size_t)size);
        u64 done = 1U;
        while (done < count) {
            const u64 n = nph_min_u64(done, count - done);
            memcpy(base + done * size, base, (size_t)(n * size));
            done += n;
        }
    }
}

NphResult nph_world_spawn_batch(NphWorld *w, const NphComponentId *ids, u32 id_count,
                                u32 entity_count, NphEntity *out)
{
    NPH_ASSERT(w != NULL);
    nph_world_structural_begin_(w);
    NphComponentMask mask;
    NPH_TRY(mask_from_ids(w, ids, id_count, &mask));
    if (entity_count == 0U) {
        return NPH_RESULT_OK;
    }
    if ((u64)nph_handle_pool_live(&w->handles) + entity_count > w->capacity) {
        return NPH_RESULT_FULL;
    }
    NphArchetype *a;
    NPH_TRY(nph_world_archetype_(w, &mask, &a));

    // Reserve every chunk the batch needs before touching anything.
    NphChunk *last = a->chunk_count > 0U ? a->chunks[a->chunk_count - 1U] : NULL;
    const u32 room = last != NULL ? a->capacity - last->count : 0U;
    const u32 rest = entity_count > room ? entity_count - room : 0U;
    const u32 new_chunks = (rest + a->capacity - 1U) / a->capacity;
    NPH_TRY(archetype_reserve_chunks(w, a, new_chunks));
    if (w->max_chunks != 0U && (u64)w->chunks_in_use + new_chunks > w->max_chunks) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    while (w->free_chunk_count < new_chunks) {
        if (!slab_grow(w)) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }

    const u32 tick = nph_world_change_tick_(w);
    u32 done = 0U;
    NphChunk *c = room > 0U ? last : NULL;
    while (done < entity_count) {
        if (c == NULL || c->count == a->capacity) {
            c = chunk_acquire(w);
            NPH_ASSERT(c != NULL); // reserved above
            archetype_push_chunk(a, c, tick);
        } else {
            stamp_all(c, tick);
        }
        const u32 n = nph_min_u32(a->capacity - c->count, entity_count - done);
        const u32 first = c->count;
        fill_defaults(w, c, first, n);
        NphEntity *ents = nph_chunk_entities(c);
        for (u32 i = 0; i < n; ++i) {
            NphHandle h;
            NPH_VERIFY(nph_handle_pool_alloc(&w->handles, &h) == NPH_RESULT_OK);
            const NphEntity e = {h.value};
            ents[first + i] = e;
            NphEntityRecord *rec = &w->records[nph_handle_pool_index(&w->handles, h)];
            rec->chunk = c;
            rec->row = first + i;
            if (out != NULL) {
                out[done + i] = e;
            }
            nph_world_event_(w, NPH_WORLD_EVENT_SPAWNED, e, NPH_COMPONENT_NONE);
        }
        c->count += n;
        a->entity_count += n;
        done += n;
    }
    return NPH_RESULT_OK;
}

NphResult nph_world_despawn(NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    nph_world_structural_begin_(w);
    NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (rec->chunk->archetype->column_of[NPH_COMPONENT_ID_HIERARCHY] != NPH_NO_COLUMN) {
        nph_hierarchy_on_despawn_(w, e);
    }
    row_remove(w, rec->chunk, rec->row, nph_world_change_tick_(w));
    rec->chunk = NULL;
    rec->row = 0U;
    NPH_VERIFY(nph_handle_pool_release(&w->handles, (NphHandle){e.value}));
    nph_world_event_(w, NPH_WORLD_EVENT_DESPAWNED, e, NPH_COMPONENT_NONE);
    return NPH_RESULT_OK;
}

bool nph_world_is_alive(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    return nph_world_record_(w, e) != NULL;
}

u32 nph_world_entity_count(const NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    return nph_handle_pool_live(&w->handles);
}

u32 nph_world_entity_capacity(const NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    return w->capacity;
}

u32 nph_world_entity_index(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    NPH_ASSERT_MSG(nph_world_is_alive(w, e), "entity 0x%08x is not alive", e.value);
    return nph_handle_pool_index(&w->handles, (NphHandle){e.value});
}

NphEntity nph_world_entity_at(const NphWorld *w, u32 index)
{
    NPH_ASSERT(w != NULL);
    if (index >= w->handles.bump || w->records[index].chunk == NULL) {
        return NPH_ENTITY_NULL;
    }
    return make_entity(w, index);
}

u32 nph_world_entity_slot_count(const NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    return w->handles.bump;
}

// ---------------------------------------------------------------------------
// Components of one entity
// ---------------------------------------------------------------------------

NphResult nph_world_add(NphWorld *w, NphEntity e, NphComponentId id, const void *value)
{
    NPH_ASSERT(w != NULL);
    nph_world_structural_begin_(w);
    const NphComponentInfo *info = nph_world_info_(w, id);
    if (info == NULL ||
        ((info->desc->flags & NPH_COMPONENT_FLAG_INTERNAL) != 0U && value != NULL)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    NphArchetype *src = rec->chunk->archetype;
    if (nph_mask_test(&src->mask, id)) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    NphArchetype *dst;
    NPH_TRY(archetype_step(w, src, id, true, &dst));
    NPH_TRY(nph_world_move_(w, e, dst));
    if (value != NULL && info->desc->size > 0U) {
        memcpy(nph_chunk_cell(rec->chunk, dst->column_of[id], rec->row), value, info->desc->size);
    }
    nph_world_event_(w, NPH_WORLD_EVENT_ADDED, e, id);
    return NPH_RESULT_OK;
}

NphResult nph_world_remove(NphWorld *w, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(w != NULL);
    nph_world_structural_begin_(w);
    NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL || id >= w->component_count ||
        !nph_mask_test(&rec->chunk->archetype->mask, id)) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (id == NPH_COMPONENT_ID_HIERARCHY) {
        // Without its links the entity leaves the tree: detach it from
        // its parent and its children from it.
        nph_hierarchy_on_despawn_(w, e);
    }
    NphArchetype *dst;
    NPH_TRY(archetype_step(w, rec->chunk->archetype, id, false, &dst));
    NPH_TRY(nph_world_move_(w, e, dst));
    nph_world_event_(w, NPH_WORLD_EVENT_REMOVED, e, id);
    return NPH_RESULT_OK;
}

NphResult nph_world_set(NphWorld *w, NphEntity e, NphComponentId id, const void *value)
{
    NPH_ASSERT(w != NULL);
    const NphComponentInfo *info = nph_world_info_(w, id);
    if (info == NULL || (info->desc->flags & NPH_COMPONENT_FLAG_INTERNAL) != 0U ||
        (value == NULL && info->desc->size > 0U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (!nph_mask_test(&rec->chunk->archetype->mask, id)) {
        return nph_world_add(w, e, id, value);
    }
    void *cell = nph_world_cell_mut_(w, rec, id);
    if (cell != NULL) {
        memcpy(cell, value, info->desc->size);
    }
    nph_world_event_(w, NPH_WORLD_EVENT_SET, e, id);
    return NPH_RESULT_OK;
}

bool nph_world_has(const NphWorld *w, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(w != NULL);
    const NphEntityRecord *rec = nph_world_record_(w, e);
    return rec != NULL && id < w->component_count &&
           nph_mask_test(&rec->chunk->archetype->mask, id);
}

const void *nph_world_get(const NphWorld *w, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(w != NULL);
    const NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL || id >= NPH_ECS_MAX_COMPONENTS) {
        return NULL;
    }
    const u8 col = rec->chunk->archetype->column_of[id];
    return col != NPH_NO_COLUMN ? nph_chunk_cell(rec->chunk, col, rec->row) : NULL;
}

void *nph_world_get_mut(NphWorld *w, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(w != NULL);
    const NphComponentInfo *info = nph_world_info_(w, id);
    if (info == NULL) {
        return NULL;
    }
    NPH_ASSERT_MSG((info->desc->flags & NPH_COMPONENT_FLAG_INTERNAL) == 0U,
                   "component '%s' is maintained by the engine; it is read-only", info->desc->name);
    if ((info->desc->flags & NPH_COMPONENT_FLAG_INTERNAL) != 0U) {
        return NULL;
    }
    NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL) {
        return NULL;
    }
    void *cell = nph_world_cell_mut_(w, rec, id);
    if (cell != NULL) {
        nph_world_event_(w, NPH_WORLD_EVENT_SET, e, id);
    }
    return cell;
}

u32 nph_world_entity_components(const NphWorld *w, NphEntity e, NphComponentId *out, u32 capacity)
{
    NPH_ASSERT(w != NULL);
    const NphEntityRecord *rec = nph_world_record_(w, e);
    if (rec == NULL) {
        return 0U;
    }
    const NphArchetype *a = rec->chunk->archetype;
    const u32 n = nph_min_u32(a->component_count, capacity);
    if (n > 0U) {
        memcpy(out, a->components, sizeof(NphComponentId) * n);
    }
    return a->component_count;
}

// ---------------------------------------------------------------------------
// Ticks
// ---------------------------------------------------------------------------

u32 nph_world_tick(const NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    return w->tick;
}

u32 nph_world_advance_tick(NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    w->tick += 1U;
    return w->tick;
}

void nph_world_clamp_ticks(NphWorld *w, u32 *ticks, u32 tick_count)
{
    NPH_ASSERT(w != NULL);
    const u32 oldest = w->tick - (1U << 30U);
    for (u32 i = 0; i < w->archetype_count; ++i) {
        const NphArchetype *a = w->archetypes[i];
        for (u32 c = 0; c < a->chunk_count; ++c) {
            for (u32 col = 0; col < a->column_count; ++col) {
                if (nph_world_tick_newer(oldest, nph_chunk_version(a->chunks[c], col))) {
                    nph_chunk_stamp(a->chunks[c], col, oldest);
                }
            }
        }
    }
    for (u32 i = 0; i < tick_count; ++i) {
        if (nph_world_tick_newer(oldest, ticks[i])) {
            ticks[i] = oldest;
        }
    }
}

// ---------------------------------------------------------------------------
// Events and introspection
// ---------------------------------------------------------------------------

const NphWorldEvent *nph_world_events(const NphWorld *w, u32 *out_count, u32 *out_dropped)
{
    NPH_ASSERT(w != NULL);
    if (out_count != NULL) {
        *out_count = w->event_count;
    }
    if (out_dropped != NULL) {
        *out_dropped = w->events_dropped;
    }
    return w->events;
}

void nph_world_clear_events(NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    w->event_count = 0U;
    w->events_dropped = 0U;
}

void nph_world_get_stats(const NphWorld *w, NphWorldStats *out)
{
    NPH_ASSERT(w != NULL && out != NULL);
    out->entities = nph_handle_pool_live(&w->handles);
    out->archetypes = w->archetype_count;
    out->chunks = w->chunks_in_use;
    out->free_chunks = w->free_chunk_count;
    out->slabs = w->slab_count;
    out->slab_bytes = (u64)w->slab_count * w->chunks_per_slab * NPH_ECS_CHUNK_SIZE;
    out->queries = w->query_count;
    out->components = w->component_count;
}

u64 nph_world_hash(const NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    u64 h = nph_hash_mix_u64(nph_handle_pool_live(&w->handles));
    for (u32 i = 0; i < w->handles.bump; ++i) {
        const NphEntityRecord *rec = &w->records[i];
        if (rec->chunk == NULL) {
            continue;
        }
        h = nph_hash_combine(h, make_entity(w, i).value);
        const NphArchetype *a = rec->chunk->archetype;
        for (u32 k = 0; k < a->component_count; ++k) {
            const NphComponentId id = a->components[k];
            h = nph_hash_combine(h, id);
            const u8 col = a->column_of[id];
            if (col != NPH_NO_COLUMN) {
                h = nph_component_hash_fields(w->components[id].desc,
                                              nph_chunk_cell(rec->chunk, col, rec->row), h);
            }
        }
    }
    return h;
}

NphStrTable *nph_world_strings(NphWorld *w)
{
    NPH_ASSERT(w != NULL);
    return &w->strings;
}
