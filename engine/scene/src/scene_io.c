// SPDX-License-Identifier: Apache-2.0
// The saved set of a scene in file order, shared by both formats
// (scene_io.h; docs/scene.md "Scene files").

#include <nepheline/core/assert.h>
#include <nepheline/scene/transform.h>

#include "scene_io_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Marks a slot that is saved but has no id yet.
#define MEMBER (NPH_SAVE_NONE - 1U)

void nph_scene_error_(NphSceneResult *result, const char *fmt, ...)
{
    if (result == NULL) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(result->error, sizeof result->error, fmt, args);
    va_end(args);
}

static bool is_member(const NphSaveSet *s, const NphWorld *w, NphEntity e)
{
    if (!nph_world_is_alive(w, e)) {
        return false;
    }
    return s->id_of_slot[nph_world_entity_index(w, e)] != NPH_SAVE_NONE;
}

static NphEntity first_member_from(const NphSaveSet *s, const NphWorld *w, NphEntity e)
{
    while (!nph_entity_is_null(e) && !is_member(s, w, e)) {
        e = nph_world_next_sibling(w, e);
    }
    return e;
}

static void assign(NphSaveSet *s, const NphWorld *w, NphEntity e)
{
    s->id_of_slot[nph_world_entity_index(w, e)] = s->count;
    s->order[s->count++] = e;
}

// Pre-order walk of the saved part of the subtree of `root`. The links lead
// back up, so no stack is needed; children that are not saved are skipped
// (their saved descendants are roots of their own).
static void walk(NphSaveSet *s, const NphWorld *w, NphEntity root)
{
    assign(s, w, root);
    NphEntity cur = root;
    for (;;) {
        const NphEntity child = first_member_from(s, w, nph_world_first_child(w, cur));
        if (!nph_entity_is_null(child)) {
            cur = child;
            assign(s, w, cur);
            continue;
        }
        // Up until a saved next sibling exists.
        for (;;) {
            if (nph_entity_equals(cur, root)) {
                return;
            }
            const NphEntity sib = first_member_from(s, w, nph_world_next_sibling(w, cur));
            if (!nph_entity_is_null(sib)) {
                cur = sib;
                assign(s, w, cur);
                break;
            }
            cur = nph_world_parent(w, cur);
        }
    }
}

NphResult nph_saveset_build_(const NphWorld *w, const NphEntity *entities, u32 count,
                             NphAllocator *alloc, NphSaveSet *out)
{
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    out->slot_count = nph_world_entity_slot_count(w);
    out->order_cap = nph_max_u32(nph_world_entity_count(w), 1U);
    out->order = NPH_ALLOC_ARRAY(alloc, NphEntity, out->order_cap);
    out->id_of_slot = NPH_ALLOC_ARRAY(alloc, u32, nph_max_u32(out->slot_count, 1U));
    if (out->order == NULL || out->id_of_slot == NULL) {
        nph_saveset_free_(out);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < out->slot_count; ++i) {
        out->id_of_slot[i] = NPH_SAVE_NONE;
    }
    if (entities == NULL) {
        for (u32 i = 0; i < out->slot_count; ++i) {
            if (!nph_entity_is_null(nph_world_entity_at(w, i))) {
                out->id_of_slot[i] = MEMBER;
            }
        }
    } else {
        for (u32 i = 0; i < count; ++i) {
            if (nph_world_is_alive(w, entities[i])) {
                out->id_of_slot[nph_world_entity_index(w, entities[i])] = MEMBER;
            }
        }
    }
    // Roots in slot order: members whose parent is not saved.
    for (u32 i = 0; i < out->slot_count; ++i) {
        if (out->id_of_slot[i] != MEMBER) {
            continue;
        }
        const NphEntity e = nph_world_entity_at(w, i);
        if (!is_member(out, w, nph_world_parent(w, e))) {
            walk(out, w, e);
        }
    }
    return NPH_RESULT_OK;
}

void nph_saveset_free_(NphSaveSet *s)
{
    if (s->order != NULL) {
        NPH_FREE_ARRAY(s->alloc, NphEntity, s->order, s->order_cap);
    }
    if (s->id_of_slot != NULL) {
        NPH_FREE_ARRAY(s->alloc, u32, s->id_of_slot, nph_max_u32(s->slot_count, 1U));
    }
    memset(s, 0, sizeof *s);
}
