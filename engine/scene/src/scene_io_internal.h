// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the text and binary scene formats: the saved set of entities
// in file order, and error reporting.

#include <nepheline/scene/scene_io.h>

#include "scene_internal.h"

#define NPH_SAVE_NONE NPH_U32_MAX

// The entities being saved, in file order (hierarchy pre-order: roots in
// slot order, each followed by its saved descendants, children in their
// list order), and the file id of each world slot.
typedef struct NphSaveSet {
    NphAllocator *alloc;
    NphEntity *order; // count entities, file order
    u32 count;
    u32 order_cap;
    u32 *id_of_slot; // slot_count entries: the file id, or NPH_SAVE_NONE
    u32 slot_count;
} NphSaveSet;

NPH_MUST_USE NphResult nph_saveset_build(const NphWorld *w, const NphEntity *entities, u32 count,
                                         NphAllocator *alloc, NphSaveSet *out);
void nph_saveset_free(NphSaveSet *s);

// The file id of `e`, or NPH_SAVE_NONE when it is not saved (or dead).
NPH_INLINE u32 nph_saveset_id(const NphSaveSet *s, const NphWorld *w, NphEntity e)
{
    if (!nph_world_is_alive(w, e)) {
        return NPH_SAVE_NONE;
    }
    const u32 slot = nph_world_entity_index(w, e);
    return slot < s->slot_count ? s->id_of_slot[slot] : NPH_SAVE_NONE;
}

// Writes a formatted message into result->error (when result is not NULL).
NPH_PRINTF_LIKE(2, 3)
void nph_scene_error(NphSceneResult *result, const char *fmt, ...);

// The component may be written to a file: not TRANSIENT, not Hierarchy
// (written as "parent").
NPH_INLINE bool nph_scene_component_saved(const NphComponentDesc *desc)
{
    return (desc->flags & (NPH_COMPONENT_FLAG_TRANSIENT | NPH_COMPONENT_FLAG_INTERNAL)) == 0U;
}
