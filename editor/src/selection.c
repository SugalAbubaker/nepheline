// SPDX-License-Identifier: Apache-2.0
// The editor's selection. See selection.h.
#include "selection.h"

#include <nepheline/core/assert.h>

#include <string.h>

void editor_selection_clear(EditorSelection *s)
{
    s->count = 0U;
    s->primary = NPH_ENTITY_NULL;
}

bool editor_selection_contains(const EditorSelection *s, NphEntity e)
{
    for (u32 i = 0; i < s->count; ++i) {
        if (nph_entity_equals(s->items[i], e)) {
            return true;
        }
    }
    return false;
}

void editor_selection_add(EditorSelection *s, NphEntity e)
{
    if (nph_entity_is_null(e)) {
        return;
    }
    if (!editor_selection_contains(s, e)) {
        if (s->count == EDITOR_SELECTION_MAX) {
            return;
        }
        s->items[s->count++] = e;
    }
    s->primary = e;
}

void editor_selection_set(EditorSelection *s, NphEntity e)
{
    editor_selection_clear(s);
    editor_selection_add(s, e);
}

static void remove_at(EditorSelection *s, u32 i)
{
    memmove(s->items + i, s->items + i + 1U, sizeof(NphEntity) * (s->count - i - 1U));
    --s->count;
}

static void fix_primary(EditorSelection *s)
{
    if (!editor_selection_contains(s, s->primary)) {
        s->primary = s->count > 0U ? s->items[s->count - 1U] : NPH_ENTITY_NULL;
    }
}

void editor_selection_toggle(EditorSelection *s, NphEntity e)
{
    for (u32 i = 0; i < s->count; ++i) {
        if (nph_entity_equals(s->items[i], e)) {
            remove_at(s, i);
            fix_primary(s);
            return;
        }
    }
    editor_selection_add(s, e);
}

void editor_selection_prune(EditorSelection *s, const NphWorld *w)
{
    for (u32 i = s->count; i-- > 0U;) {
        if (!nph_world_is_alive(w, s->items[i])) {
            remove_at(s, i);
        }
    }
    fix_primary(s);
}
