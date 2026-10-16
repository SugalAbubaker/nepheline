// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's edits (docs/editor.md "Editing"): what the outliner, the
// inspector, the viewport, the menus, and the shortcuts do to the edited
// world. Each is one undoable step of the history (history.h), and the
// tests drive the same functions.
//
// Entities the editor creates carry Name, LocalTransform, and
// WorldTransform: the transform system and the scene renderer need the
// last, and nothing adds it by itself.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY.

#include <nepheline/scene/transform.h>

#include "history.h"

// A new entity named `name` (NULL: "Entity") under `parent` (null: a
// root), placed at `local` relative to the parent (NULL: identity).
NPH_MUST_USE NphResult editor_edit_create(EditorHistory *h, const char *name, NphEntity parent,
                                          const NphLocalTransform *local, NphEntity *out);

// Deletes the entities and everything under them, as one step. An entity
// under another one being deleted goes once. Dead handles are skipped.
NPH_MUST_USE NphResult editor_edit_delete(EditorHistory *h, const NphEntity *entities, u32 count);

// Copies the entities and everything under them, as one step. Each copy
// goes under its original's parent, and its references (ENTITY fields) to
// entities inside its copied subtree point at their copies. `out` (count
// entries, may be NULL) receives each given entity's copy, or null for one
// under another given entity.
NPH_MUST_USE NphResult editor_edit_duplicate(EditorHistory *h, const NphEntity *entities, u32 count,
                                             NphEntity *out);

// Copies `root` of another world and everything under it into the
// history's world, as one step labelled `label`, under `parent` (null: a
// root); how an imported scene arrives (docs/editor.md "Assets"). `src`
// must have the history world's components (a world made with
// NphWorldDesc.registry_from). References (ENTITY fields) to entities of
// the copied subtree point at their copies, and references to anything
// else become null: `src`'s handles mean nothing in the history's world.
// `out` receives the copy of `root`. NPH_RESULT_NOT_FOUND when `root` is
// not alive in `src`, or `parent` in the history's world.
NPH_MUST_USE NphResult editor_edit_paste(EditorHistory *h, const NphWorld *src, NphEntity root,
                                         NphEntity parent, const char *label, NphEntity *out);

// Renames (adding a Name when missing).
NPH_MUST_USE NphResult editor_edit_rename(EditorHistory *h, NphEntity e, const char *name);

// Moves `e` under `parent` (null: a root) and keeps where it is in the
// world: its LocalTransform is recomputed. NPH_RESULT_INVALID_ARGUMENT
// when `parent` is `e` or under it.
NPH_MUST_USE NphResult editor_edit_reparent(EditorHistory *h, NphEntity e, NphEntity parent);

// Adds a component at its defaults, or removes one.
NPH_MUST_USE NphResult editor_edit_add_component(EditorHistory *h, NphEntity e, NphComponentId id);
NPH_MUST_USE NphResult editor_edit_remove_component(EditorHistory *h, NphEntity e,
                                                    NphComponentId id);

// ---------------------------------------------------------------------------
// Queries the edits and the panels share
// ---------------------------------------------------------------------------

// The world transform of `e` from the LocalTransforms up its parents:
// current even before this frame's transform update. An entity without a
// LocalTransform counts as the identity.
NphWorldTransform editor_world_transform_of(const NphWorld *w, NphEntity e);

// The LocalTransform that puts an entity at `world` under a parent at
// `parent` (NULL: a root).
NphLocalTransform editor_local_from_world(const NphWorldTransform *world,
                                          const NphWorldTransform *parent);

// True when `e` is `ancestor` or under it.
bool editor_is_under(const NphWorld *w, NphEntity e, NphEntity ancestor);

// The entity's name for display: its Name, or "Entity <slot>" without one.
void editor_entity_label(const NphWorld *w, NphEntity e, char *out, u64 cap);
