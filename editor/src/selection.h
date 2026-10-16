// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's selection (docs/editor.md "Selection"): the entities the
// outliner, the inspector, the gizmo, and the edits act on, with a primary
// one (the last clicked: the inspector's). Entities are kept by handle, so
// a selection survives play-in-editor (the play copy keeps the handles)
// and an undo that brings a deleted entity back. Each frame prunes entities
// that are no longer alive.
//
// Thread-safety: plain data; no shared state.

#include <nepheline/scene/world.h>

#define EDITOR_SELECTION_MAX 1024U

typedef struct EditorSelection {
    NphEntity items[EDITOR_SELECTION_MAX];
    u32 count;
    NphEntity primary; // null when nothing is selected
} EditorSelection;

void editor_selection_clear(EditorSelection *s);

// Selects only `e` (null: nothing).
void editor_selection_set(EditorSelection *s, NphEntity e);

// Adds `e` (it becomes the primary). Past EDITOR_SELECTION_MAX entities,
// further ones are not added.
void editor_selection_add(EditorSelection *s, NphEntity e);

// Removes `e` when selected, adds it otherwise (a ctrl-click).
void editor_selection_toggle(EditorSelection *s, NphEntity e);

bool editor_selection_contains(const EditorSelection *s, NphEntity e);

// Drops the entities `w` does not have alive; the primary moves to the last
// one left.
void editor_selection_prune(EditorSelection *s, const NphWorld *w);
