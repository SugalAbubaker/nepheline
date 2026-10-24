// SPDX-License-Identifier: Apache-2.0
#pragma once

// Queries: which archetypes hold the entities a system works on, and
// iteration over their chunks (CLAUDE.md §8.7; docs/scene.md "Queries").
//
// A query is a list of terms. A REQUIRED term matches archetypes that have
// the component, an EXCLUDED term archetypes that do not, and an OPTIONAL
// term matches either (its column is NULL where it is absent). Each term
// that reads or writes data declares its access, which is how a schedule
// knows which systems may run at the same time.
//
// A query caches the archetypes it matches. The world tests every new
// archetype against every live query, so matching costs nothing per
// iteration, and iteration walks the matched archetypes' chunks:
//
//     NphQueryIter it;
//     nph_query_iter(query, &it);
//     NphChunkView view;
//     while (nph_query_next(&it, &view)) {
//         NphVec3 *vel = NPH_CHUNK_COLUMN(&view, 0, NphVec3);
//         for (u32 i = 0; i < view.count; ++i) { ... }
//     }
//
// Change filters: a term with `changed` set makes the query skip chunks in
// which none of the changed-flagged columns were written after the
// iteration's `since` tick (an archetype that has none of the flagged
// components, all of them OPTIONAL, is not filtered). Granularity is the
// chunk: one write anywhere in a chunk's column marks the whole column.
// Producing a chunk with a WRITE term stamps that column with the
// iteration's tick, whether or not the caller writes.
//
// Thread-safety: creating and destroying a query is a change to the world
// (single thread). Iterating is a read of the world's structure: several
// threads may iterate the same or different queries at once while nothing
// changes the world's structure, and a structural change during an
// iteration is detected (asserted) when the iteration continues.

#include <nepheline/scene/world.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphQuery NphQuery;

#define NPH_QUERY_MAX_TERMS 16U

typedef enum NphQueryOp {
    NPH_QUERY_REQUIRED = 0,
    NPH_QUERY_OPTIONAL,
    NPH_QUERY_EXCLUDED,
} NphQueryOp;

typedef enum NphAccess {
    NPH_ACCESS_NONE = 0, // presence only: a filter, no column
    NPH_ACCESS_READ,
    NPH_ACCESS_WRITE,
} NphAccess;

typedef struct NphQueryTerm {
    NphComponentId component;
    u8 op;      // NphQueryOp
    u8 access;  // NphAccess (EXCLUDED terms have none)
    u8 changed; // non-zero: a change filter on this column (REQUIRED or OPTIONAL)
    u8 reserved;
} NphQueryTerm;

// Term builders.
NPH_INLINE NphQueryTerm nph_term_read(NphComponentId id)
{
    const NphQueryTerm t = {id, NPH_QUERY_REQUIRED, NPH_ACCESS_READ, 0U, 0U};
    return t;
}
NPH_INLINE NphQueryTerm nph_term_write(NphComponentId id)
{
    const NphQueryTerm t = {id, NPH_QUERY_REQUIRED, NPH_ACCESS_WRITE, 0U, 0U};
    return t;
}
NPH_INLINE NphQueryTerm nph_term_with(NphComponentId id)
{
    const NphQueryTerm t = {id, NPH_QUERY_REQUIRED, NPH_ACCESS_NONE, 0U, 0U};
    return t;
}
NPH_INLINE NphQueryTerm nph_term_without(NphComponentId id)
{
    const NphQueryTerm t = {id, NPH_QUERY_EXCLUDED, NPH_ACCESS_NONE, 0U, 0U};
    return t;
}
NPH_INLINE NphQueryTerm nph_term_optional(NphComponentId id, NphAccess access)
{
    const NphQueryTerm t = {id, NPH_QUERY_OPTIONAL, (u8)access, 0U, 0U};
    return t;
}
NPH_INLINE NphQueryTerm nph_term_changed(NphQueryTerm t)
{
    t.changed = 1U;
    return t;
}

typedef struct NphQueryDesc {
    const NphQueryTerm *terms;
    u32 term_count; // 1 .. NPH_QUERY_MAX_TERMS
} NphQueryDesc;

// Creates a query. NPH_RESULT_INVALID_ARGUMENT for no terms, too many, an
// unregistered component, a component named in two terms, an EXCLUDED term
// with access or a change filter, a change filter on a term without
// access, data access to a tag, or no REQUIRED term (a query must be
// anchored to something; use nph_term_with on a component every entity of
// interest has). NPH_RESULT_OUT_OF_MEMORY on allocation failure.
NPH_MUST_USE NphResult nph_query_create(NphWorld *world, const NphQueryDesc *desc, NphQuery **out);

// Destroys a query. Safe on NULL.
void nph_query_destroy(NphQuery *query);

NphWorld *nph_query_world(const NphQuery *query);
u32 nph_query_term_count(const NphQuery *query);
const NphQueryTerm *nph_query_terms(const NphQuery *query);

// Number of archetypes the query matches (including empty ones).
u32 nph_query_archetype_count(const NphQuery *query);

// Number of entities the query matches now. O(matched archetypes).
u32 nph_query_entity_count(const NphQuery *query);

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

typedef struct NphChunkView {
    const NphEntity *entities;          // `count` handles
    void *columns[NPH_QUERY_MAX_TERMS]; // per term: the column, or NULL (no access, absent)
    u32 count;                          // entities in the chunk, at least 1
    u32 archetype;                      // the archetype's index (stable for the world)
    const void *chunk;                  // opaque; identifies the chunk
} NphChunkView;

// The column of term `term` as a T array.
#define NPH_CHUNK_COLUMN(view, term, T) ((T *)(view)->columns[(term)])

typedef struct NphQueryIter {
    const NphQuery *query;
    u32 archetype_cursor;
    u32 chunk_cursor;
    u32 since_tick; // change filters: chunks written after this tick
    u32 write_tick; // stamped on WRITE columns
    u32 filter;     // non-zero: apply the change filters
    u32 reserved;
    u64 epoch; // the world's structural epoch at the start
} NphQueryIter;

// Starts an iteration over every matching chunk. WRITE columns are
// stamped with the world's change tick; change filters are ignored.
void nph_query_iter(const NphQuery *query, NphQueryIter *it);

// Starts an iteration that skips chunks whose change-filtered columns were
// not written after `since_tick`, stamping WRITE columns with `write_tick`
// (a schedule passes the system's previous and current ticks).
void nph_query_iter_since(const NphQuery *query, u32 since_tick, u32 write_tick, NphQueryIter *it);

// The next chunk, or false at the end.
bool nph_query_next(NphQueryIter *it, NphChunkView *out);

// Gathers the chunks an iteration would produce, as opaque pointers, into
// `out` (up to `capacity`), without stamping anything; returns how many
// there are (which may exceed `capacity`: call again with more room). For
// splitting an iteration across jobs with nph_query_chunk_view.
u32 nph_query_collect_chunks(const NphQuery *query, u32 since_tick, void **out, u32 capacity);

// The view of one collected chunk, stamping WRITE columns with
// `write_tick`. Different chunks may be viewed from different threads at
// once.
void nph_query_chunk_view(const NphQuery *query, void *chunk, u32 write_tick, NphChunkView *out);

// The chunk holding the live entity `e`, as nph_query_collect_chunks gives
// chunks, and the entity's row in it (`out_row`; may be NULL): for writing
// the entities another source names, such as the bodies a physics step
// moved, through nph_query_chunk_view without visiting every chunk. NULL for
// a dead entity. Valid until the next structural change.
void *nph_world_entity_chunk(const NphWorld *world, NphEntity e, u32 *out_row);

// True when `chunk` (from nph_query_collect_chunks or
// nph_world_entity_chunk) belongs to an archetype the query matches, which
// nph_query_chunk_view requires.
bool nph_query_matches_chunk(const NphQuery *query, const void *chunk);

#ifdef __cplusplus
}
#endif
