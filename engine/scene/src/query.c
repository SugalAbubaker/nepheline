// SPDX-License-Identifier: Apache-2.0
// Queries: archetype matching, cached per query, and chunk iteration with
// change filters (query.h; docs/scene.md "Queries").

#include <nepheline/core/assert.h>

#include "scene_internal.h"

#include <string.h>

static bool archetype_matches(const NphQuery *q, const NphArchetype *a)
{
    return nph_mask_contains(&a->mask, &q->required) &&
           !nph_mask_intersects(&a->mask, &q->excluded);
}

NphResult nph_query_reserve_(NphQuery *q, u32 extra)
{
    const u64 need = (u64)q->archetype_count + extra;
    if (need <= q->archetype_cap) {
        return NPH_RESULT_OK;
    }
    if (need > NPH_U32_MAX / 2U) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 cap = q->archetype_cap == 0U ? 8U : q->archetype_cap;
    while (cap < need) {
        cap *= 2U;
    }
    NphAllocator *alloc = q->world->alloc;
    u32 *arr = (u32 *)NPH_REALLOC(alloc, q->archetypes, sizeof(u32) * q->archetype_cap,
                                  sizeof(u32) * cap, NPH_ALIGNOF(u32));
    if (arr == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    q->archetypes = arr;
    q->archetype_cap = cap;
    return NPH_RESULT_OK;
}

void nph_query_on_archetype_(NphQuery *q, const NphArchetype *arch)
{
    if (!archetype_matches(q, arch)) {
        return;
    }
    NPH_ASSERT(q->archetype_count < q->archetype_cap);
    q->archetypes[q->archetype_count++] = arch->index;
}

static NphResult validate(const NphWorld *w, const NphQueryDesc *desc)
{
    if (desc == NULL || desc->term_count == 0U || desc->term_count > NPH_QUERY_MAX_TERMS ||
        desc->terms == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    bool anchored = false;
    for (u32 i = 0; i < desc->term_count; ++i) {
        const NphQueryTerm *t = &desc->terms[i];
        if (t->component >= w->component_count || t->op > NPH_QUERY_EXCLUDED ||
            t->access > NPH_ACCESS_WRITE) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        for (u32 j = 0; j < i; ++j) {
            if (desc->terms[j].component == t->component) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
        }
        if (t->op == NPH_QUERY_EXCLUDED && (t->access != NPH_ACCESS_NONE || t->changed != 0U)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (t->changed != 0U && t->access == NPH_ACCESS_NONE) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (t->access != NPH_ACCESS_NONE && w->components[t->component].desc->size == 0U) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        anchored = anchored || t->op == NPH_QUERY_REQUIRED;
    }
    return anchored ? NPH_RESULT_OK : NPH_RESULT_INVALID_ARGUMENT;
}

static void query_free(NphQuery *q)
{
    NphAllocator *alloc = q->world->alloc;
    if (q->archetypes != NULL) {
        NPH_FREE_ARRAY(alloc, u32, q->archetypes, q->archetype_cap);
    }
    NPH_FREE_TYPE(alloc, NphQuery, q);
}

NphResult nph_query_create(NphWorld *w, const NphQueryDesc *desc, NphQuery **out)
{
    NPH_ASSERT(w != NULL && out != NULL);
    *out = NULL;
    NPH_ASSERT_MSG(w->frozen == 0U, "queries are created outside schedule stages");
    NPH_TRY(validate(w, desc));

    NphQuery *q = NPH_ALLOC_TYPE(w->alloc, NphQuery);
    if (q == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    q->world = w;
    q->term_count = desc->term_count;
    memcpy(q->terms, desc->terms, sizeof(NphQueryTerm) * desc->term_count);
    for (u32 i = 0; i < desc->term_count; ++i) {
        const NphQueryTerm *t = &desc->terms[i];
        if (t->op == NPH_QUERY_REQUIRED) {
            nph_mask_set(&q->required, t->component);
        } else if (t->op == NPH_QUERY_EXCLUDED) {
            nph_mask_set(&q->excluded, t->component);
        }
        q->has_change_filter = q->has_change_filter || t->changed != 0U;
    }

    if (nph_query_reserve_(q, w->archetype_count) != NPH_RESULT_OK) {
        query_free(q);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (w->query_count == w->query_cap) {
        const u32 cap = w->query_cap == 0U ? 8U : w->query_cap * 2U;
        NphQuery **arr =
            (NphQuery **)NPH_REALLOC(w->alloc, w->queries, sizeof(NphQuery *) * w->query_cap,
                                     sizeof(NphQuery *) * cap, NPH_ALIGNOF(NphQuery *));
        if (arr == NULL) {
            query_free(q);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        w->queries = arr;
        w->query_cap = cap;
    }
    for (u32 i = 0; i < w->archetype_count; ++i) {
        nph_query_on_archetype_(q, w->archetypes[i]);
    }
    q->slot = w->query_count;
    w->queries[w->query_count++] = q;
    *out = q;
    return NPH_RESULT_OK;
}

void nph_query_destroy(NphQuery *q)
{
    if (q == NULL) {
        return;
    }
    NphWorld *w = q->world;
    NPH_ASSERT_MSG(w->frozen == 0U, "queries are destroyed outside schedule stages");
    NPH_ASSERT(q->slot < w->query_count && w->queries[q->slot] == q);
    NphQuery *last = w->queries[w->query_count - 1U];
    w->queries[q->slot] = last;
    last->slot = q->slot;
    w->query_count -= 1U;
    query_free(q);
}

NphWorld *nph_query_world(const NphQuery *q)
{
    NPH_ASSERT(q != NULL);
    return q->world;
}

u32 nph_query_term_count(const NphQuery *q)
{
    NPH_ASSERT(q != NULL);
    return q->term_count;
}

const NphQueryTerm *nph_query_terms(const NphQuery *q)
{
    NPH_ASSERT(q != NULL);
    return q->terms;
}

u32 nph_query_archetype_count(const NphQuery *q)
{
    NPH_ASSERT(q != NULL);
    return q->archetype_count;
}

u32 nph_query_entity_count(const NphQuery *q)
{
    NPH_ASSERT(q != NULL);
    u32 n = 0U;
    for (u32 i = 0; i < q->archetype_count; ++i) {
        n += q->world->archetypes[q->archetypes[i]]->entity_count;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

static bool chunk_passes(const NphQuery *q, NphChunk *c, u32 since)
{
    const NphArchetype *a = c->archetype;
    bool any = false;
    for (u32 t = 0; t < q->term_count; ++t) {
        if (q->terms[t].changed == 0U) {
            continue;
        }
        const u8 col = a->column_of[q->terms[t].component];
        if (col == NPH_NO_COLUMN) {
            continue;
        }
        any = true;
        if (nph_world_tick_newer(nph_chunk_version(c, col), since)) {
            return true;
        }
    }
    return !any;
}

static void fill_view(const NphQuery *q, NphChunk *c, u32 write_tick, NphChunkView *out)
{
    const NphArchetype *a = c->archetype;
    out->entities = nph_chunk_entities(c);
    out->count = c->count;
    out->archetype = a->index;
    out->chunk = c;
    for (u32 t = 0; t < NPH_QUERY_MAX_TERMS; ++t) {
        out->columns[t] = NULL;
    }
    for (u32 t = 0; t < q->term_count; ++t) {
        const NphQueryTerm *term = &q->terms[t];
        if (term->access == NPH_ACCESS_NONE) {
            continue;
        }
        const u8 col = a->column_of[term->component];
        if (col == NPH_NO_COLUMN) {
            continue;
        }
        out->columns[t] = nph_chunk_column(c, col);
        if (term->access == NPH_ACCESS_WRITE) {
            nph_chunk_stamp(c, col, write_tick);
        }
    }
}

void nph_query_iter(const NphQuery *q, NphQueryIter *it)
{
    NPH_ASSERT(q != NULL && it != NULL);
    memset(it, 0, sizeof *it);
    it->query = q;
    it->write_tick = nph_world_change_tick_(q->world);
    it->epoch = q->world->epoch;
}

void nph_query_iter_since(const NphQuery *q, u32 since_tick, u32 write_tick, NphQueryIter *it)
{
    NPH_ASSERT(q != NULL && it != NULL);
    memset(it, 0, sizeof *it);
    it->query = q;
    it->since_tick = since_tick;
    it->write_tick = write_tick;
    it->filter = q->has_change_filter ? 1U : 0U;
    it->epoch = q->world->epoch;
}

bool nph_query_next(NphQueryIter *it, NphChunkView *out)
{
    NPH_ASSERT(it != NULL && out != NULL);
    const NphQuery *q = it->query;
    const NphWorld *w = q->world;
    NPH_ASSERT_MSG(it->epoch == w->epoch, "the world's structure changed during a query iteration; "
                                          "record structural changes in a command buffer");
    while (it->archetype_cursor < q->archetype_count) {
        const NphArchetype *a = w->archetypes[q->archetypes[it->archetype_cursor]];
        while (it->chunk_cursor < a->chunk_count) {
            NphChunk *c = a->chunks[it->chunk_cursor++];
            if (it->filter != 0U && !chunk_passes(q, c, it->since_tick)) {
                continue;
            }
            fill_view(q, c, it->write_tick, out);
            return true;
        }
        it->archetype_cursor += 1U;
        it->chunk_cursor = 0U;
    }
    return false;
}

u32 nph_query_collect_chunks(const NphQuery *q, u32 since_tick, void **out, u32 capacity)
{
    NPH_ASSERT(q != NULL);
    const NphWorld *w = q->world;
    u32 n = 0U;
    for (u32 i = 0; i < q->archetype_count; ++i) {
        const NphArchetype *a = w->archetypes[q->archetypes[i]];
        for (u32 k = 0; k < a->chunk_count; ++k) {
            NphChunk *c = a->chunks[k];
            if (q->has_change_filter && !chunk_passes(q, c, since_tick)) {
                continue;
            }
            if (n < capacity) {
                out[n] = c;
            }
            n += 1U;
        }
    }
    return n;
}

void nph_query_chunk_view(const NphQuery *q, void *chunk, u32 write_tick, NphChunkView *out)
{
    NPH_ASSERT(q != NULL && chunk != NULL && out != NULL);
    NphChunk *c = chunk;
    NPH_ASSERT(archetype_matches(q, c->archetype));
    fill_view(q, c, write_tick, out);
}
