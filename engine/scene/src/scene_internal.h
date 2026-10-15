// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal structures of the scene module: the component registry,
// archetypes, chunks, and the world (docs/scene.md "Storage").

#include <nepheline/core/atomic.h>
#include <nepheline/core/handle.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/string_id.h>
#include <nepheline/scene/query.h>

// ---------------------------------------------------------------------------
// Component masks: one bit per component id
// ---------------------------------------------------------------------------

#define NPH_MASK_WORDS (NPH_ECS_MAX_COMPONENTS / 64U)

typedef struct NphComponentMask {
    u64 w[NPH_MASK_WORDS];
} NphComponentMask;

NPH_INLINE void nph_mask_set(NphComponentMask *m, NphComponentId id)
{
    m->w[id >> 6U] |= (u64)1U << (id & 63U);
}
NPH_INLINE void nph_mask_clear(NphComponentMask *m, NphComponentId id)
{
    m->w[id >> 6U] &= ~((u64)1U << (id & 63U));
}
NPH_INLINE bool nph_mask_test(const NphComponentMask *m, NphComponentId id)
{
    return ((m->w[id >> 6U] >> (id & 63U)) & 1U) != 0U;
}
// True when every bit of `sub` is set in `m`.
NPH_INLINE bool nph_mask_contains(const NphComponentMask *m, const NphComponentMask *sub)
{
    for (u32 i = 0; i < NPH_MASK_WORDS; ++i) {
        if ((m->w[i] & sub->w[i]) != sub->w[i]) {
            return false;
        }
    }
    return true;
}
NPH_INLINE bool nph_mask_intersects(const NphComponentMask *a, const NphComponentMask *b)
{
    for (u32 i = 0; i < NPH_MASK_WORDS; ++i) {
        if ((a->w[i] & b->w[i]) != 0U) {
            return true;
        }
    }
    return false;
}
NPH_INLINE bool nph_mask_equals(const NphComponentMask *a, const NphComponentMask *b)
{
    for (u32 i = 0; i < NPH_MASK_WORDS; ++i) {
        if (a->w[i] != b->w[i]) {
            return false;
        }
    }
    return true;
}
NPH_INLINE void nph_mask_or(NphComponentMask *dst, const NphComponentMask *src)
{
    for (u32 i = 0; i < NPH_MASK_WORDS; ++i) {
        dst->w[i] |= src->w[i];
    }
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

typedef struct NphComponentInfo {
    const NphComponentDesc *desc;
    NphStrId name_id;
    u64 layout_hash;
    u32 entity_field_count;
    u16 entity_field_offsets[NPH_ECS_MAX_FIELDS]; // byte offsets of ENTITY fields
} NphComponentInfo;

// ---------------------------------------------------------------------------
// Chunks and archetypes
// ---------------------------------------------------------------------------

typedef struct NphArchetype NphArchetype;

// The header at the start of every 16 KiB chunk. It is followed, at the
// archetype's offsets, by one change version per column, the entity
// handles, and the columns.
typedef struct NphChunk NphChunk;
struct NphChunk {
    NphArchetype *archetype; // NULL while the chunk is free
    NphChunk *next_free;     // the free list's link while the chunk is free
    u32 count;               // rows in use
    u32 index;               // position in archetype->chunks
};

// Column index meaning "no column" (absent, or a tag).
#define NPH_NO_COLUMN 0xFFU
// Columns per archetype; the chunk layout could not hold more anyway.
#define NPH_MAX_COLUMNS 128U

struct NphArchetype {
    NphComponentMask mask;
    u32 index;
    u32 component_count; // including tags
    u32 column_count;    // components with data
    u32 capacity;        // rows per chunk
    u32 versions_offset; // NphAtomicU32[column_count]
    u32 entities_offset; // NphEntity[capacity]
    u32 entity_count;
    u32 chunk_count;
    u32 chunk_cap;
    NphChunk **chunks;
    NphComponentId *components; // ascending ids, component_count
    NphComponentId *column_ids; // ascending ids with data, column_count
    u32 *column_offsets;        // per column: byte offset in a chunk
    u32 *column_sizes;
    u8 column_of[NPH_ECS_MAX_COMPONENTS]; // id -> column, NPH_NO_COLUMN when none
};

NPH_INLINE NphAtomicU32 *nph_chunk_versions(NphChunk *chunk)
{
    return (NphAtomicU32 *)((u8 *)chunk + chunk->archetype->versions_offset);
}
NPH_INLINE NphEntity *nph_chunk_entities(NphChunk *chunk)
{
    return (NphEntity *)((u8 *)chunk + chunk->archetype->entities_offset);
}
NPH_INLINE u8 *nph_chunk_column(NphChunk *chunk, u32 column)
{
    return (u8 *)chunk + chunk->archetype->column_offsets[column];
}
NPH_INLINE u8 *nph_chunk_cell(NphChunk *chunk, u32 column, u32 row)
{
    return nph_chunk_column(chunk, column) + (u64)row * chunk->archetype->column_sizes[column];
}
NPH_INLINE u32 nph_chunk_version(NphChunk *chunk, u32 column)
{
    return nph_atomic_load_u32(&nph_chunk_versions(chunk)[column], NPH_MEMORY_ORDER_RELAXED);
}
// Version writes are relaxed atomics: two threads may stamp the same
// column with the same tick (the transform system writes rows of one chunk
// from several jobs), and readers only need to see some recent value.
NPH_INLINE void nph_chunk_stamp(NphChunk *chunk, u32 column, u32 tick)
{
    nph_atomic_store_u32(&nph_chunk_versions(chunk)[column], tick, NPH_MEMORY_ORDER_RELAXED);
}

// ---------------------------------------------------------------------------
// The world
// ---------------------------------------------------------------------------

typedef struct NphEntityRecord {
    NphChunk *chunk; // NULL for a free slot
    u32 row;
    u32 reserved;
} NphEntityRecord;

typedef struct NphSlab {
    u8 *base;
    u32 live; // chunks of this slab in use
    u32 reserved;
} NphSlab;

struct NphWorld {
    NphAllocator *alloc;

    // Registry.
    NphComponentInfo components[NPH_ECS_MAX_COMPONENTS];
    u32 component_count;
    NphHashMap component_names; // name id (u64) -> component id (u32)

    // Entities.
    NphHandlePool handles;
    NphEntityRecord *records; // per slot
    u32 capacity;

    // Archetypes.
    NphArchetype **archetypes;
    u32 archetype_count;
    u32 archetype_cap;
    NphHashMap archetype_map; // NphComponentMask -> archetype index
    NphHashMap edges;         // (archetype << 9 | component << 1 | add) -> archetype index

    // Chunk memory.
    NphSlab *slabs; // sorted by base address
    u32 slab_count;
    u32 slab_cap;
    u32 chunks_per_slab;
    u32 max_chunks;
    u32 chunks_in_use;
    u32 free_chunk_count;
    NphChunk *free_chunks; // linked through the first word of each free chunk

    // Queries.
    NphQuery **queries;
    u32 query_count;
    u32 query_cap;

    // The deepest hierarchy depth ever assigned: an upper bound for the
    // transform system's level buckets.
    u32 max_depth;

    // Change detection and safety checks.
    u32 tick;
    u32 frozen; // non-zero while a schedule stage runs: structural changes assert
    u64 epoch;  // bumped by every structural change

    // Event stream.
    NphWorldEvent *events;
    u32 event_capacity;
    u32 event_count;
    u32 events_dropped;

    NphStrTable strings;
    bool strings_ready;
};

struct NphQuery {
    NphWorld *world;
    NphQueryTerm terms[NPH_QUERY_MAX_TERMS];
    u32 term_count;
    u32 slot; // index in world->queries
    NphComponentMask required;
    NphComponentMask excluded;
    bool has_change_filter;
    u32 *archetypes; // matched archetype indices, in creation order
    u32 archetype_count;
    u32 archetype_cap;
};

// ---------------------------------------------------------------------------
// Internal functions shared by the module's source files
// ---------------------------------------------------------------------------

// The tick stamped by writes made outside a system run.
NPH_INLINE u32 nph_world_change_tick_(const NphWorld *w)
{
    return w->tick + 1U;
}

// Asserts that the world's structure may change now and records the change.
void nph_world_structural_begin_(NphWorld *w);

const NphComponentInfo *nph_world_info_(const NphWorld *w, NphComponentId id);

// The record of a live entity, or NULL.
NphEntityRecord *nph_world_record_(const NphWorld *w, NphEntity e);

// Appends an event when the stream is on.
void nph_world_event_(NphWorld *w, NphWorldEventType type, NphEntity e, NphComponentId id);

// The archetype for `mask`, created on first use.
NPH_MUST_USE NphResult nph_world_archetype_(NphWorld *w, const NphComponentMask *mask,
                                            NphArchetype **out);

// Moves a live entity to `dst` (which must differ from its archetype):
// components present in both are copied, those only in `dst` get their
// defaults. Nothing changes on failure.
NPH_MUST_USE NphResult nph_world_move_(NphWorld *w, NphEntity e, NphArchetype *dst);

// Allocates `count` entity handles without placing them in an archetype
// (they are not alive until placed), so their slots follow the order of
// `out`. NPH_RESULT_FULL at the capacity.
NPH_MUST_USE NphResult nph_world_reserve_(NphWorld *w, u32 count, NphEntity *out);

// Places reserved handles in the archetype of `ids`, rows in the order of
// `handles`, with default values. All or nothing.
NPH_MUST_USE NphResult nph_world_place_(NphWorld *w, const NphComponentId *ids, u32 id_count,
                                        u32 count, const NphEntity *handles);

// Releases reserved handles that were never placed (placed ones are left
// alone; despawn them).
void nph_world_unreserve_(NphWorld *w, const NphEntity *handles, u32 count);

// Writes a component cell, stamping the column with the change tick.
void *nph_world_cell_mut_(NphWorld *w, NphEntityRecord *rec, NphComponentId id);

// Remaps every ENTITY field of a component value through `map`
// (an entity -> entity function).
typedef NphEntity (*NphEntityMapFn)(void *user, NphEntity e);
void nph_component_remap_entities_(const NphComponentInfo *info, void *value, NphEntityMapFn fn,
                                   void *user);

// Query bookkeeping called by the world when it creates an archetype:
// first every query reserves room for one more (may fail), then each
// records the archetype if it matches (cannot fail).
NPH_MUST_USE NphResult nph_query_reserve_(NphQuery *q, u32 extra);
void nph_query_on_archetype_(NphQuery *q, const NphArchetype *arch);

// Hierarchy maintenance called by the world (transform.c).
void nph_hierarchy_on_despawn_(NphWorld *w, NphEntity e);

// Built-in component descriptors (transform.c, prefab.c).
const NphComponentDesc *nph_builtin_component_desc_(u32 index);
