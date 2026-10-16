// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's undo history (CLAUDE.md §8.13 "Undo/redo is a command stack,
// and every edit is a reversible command"; docs/editor.md "Undo and redo").
//
// The edited world is changed through the history, which applies each
// change and records what it needs to go both ways:
//   - set: a component's value before and after;
//   - add / remove: a component and its value;
//   - spawn: an entity created, with its components;
//   - delete: an entity taken out of the world, with every component's
//     value and its parent;
//   - parent: an entity's parent before and after.
//
// A deleted entity is suspended (scene/world.h nph_world_suspend), not
// despawned. Undoing the deletion brings it back under the same handle,
// so the selection, other components' ENTITY fields, and older steps keep
// working. Undoing a spawn suspends the new entity the same way, ready for
// a redo. A suspended handle is released once no step can bring it back:
//   - its deletion falls off the bottom of the history;
//   - a new edit discards the undone spawn that suspended it;
//   - the history is cleared.
//
// Changes are grouped into steps: begin, record any number of changes,
// end. A step is undone or redone as one. A drag records one step:
// editor_history_set_merged keeps updating the last step while the same
// merge key repeats, until editor_history_end_merge.
//
// Each state has an id (editor_history_state): the document compares it
// with the one it saved to know whether there are unsaved changes, and
// undoing back to a saved state reads as clean again.
//
// Failures: a change that fails while recorded is not recorded, and the
// world is as it was before it. A step that fails to undo or redo (the
// world was changed outside the history) clears the history, which is
// logged.
//
// Costs (§1.4): every recorded change allocates its values from the
// history's allocator. The history keeps at most max_steps steps and about
// max_bytes of values, dropping the oldest.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY: the world's owner.

#include <nepheline/scene/world.h>

typedef struct EditorHistory EditorHistory;

typedef struct EditorHistoryDesc {
    NphAllocator *alloc; // required
    NphWorld *world;     // required: the edited world
    u32 max_steps;       // 0: 1000
    u64 max_bytes;       // 0: 64 MiB
} EditorHistoryDesc;

#define EDITOR_HISTORY_LABEL_MAX 64U

// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult editor_history_create(const EditorHistoryDesc *desc, EditorHistory **out);

// Releases the suspended entities only the history could bring back, then
// frees it. Safe on NULL.
void editor_history_destroy(EditorHistory *h);

NphWorld *editor_history_world(const EditorHistory *h);

// The history's allocator, for the edits' scratch.
NphAllocator *editor_history_allocator(const EditorHistory *h);

// Clears the history and binds it to `world` (a hot reload that migrated the
// edited world): the suspended handles the history held are released in the
// new world, which copied them.
void editor_history_rebind(EditorHistory *h, NphWorld *world);

// Drops every step, releasing the suspended entities only they could bring
// back.
void editor_history_clear(EditorHistory *h);

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

// Opens a step (`label` is copied, cut to EDITOR_HISTORY_LABEL_MAX - 1
// bytes). NPH_RESULT_INVALID_STATE when one is open.
NPH_MUST_USE NphResult editor_history_begin(EditorHistory *h, const char *label);

// Closes the step. A step with changes discards the steps that were
// undone (they can no longer be redone) and becomes the newest. An empty
// step is dropped.
void editor_history_end(EditorHistory *h);

// Undoes the changes recorded since editor_history_begin and drops the
// step: an action that failed halfway leaves nothing behind.
void editor_history_abort(EditorHistory *h);

bool editor_history_recording(const EditorHistory *h);

// The changes. Each applies to the world and is recorded in the open
// step; each fails with NPH_RESULT_INVALID_STATE when no step is open,
// NPH_RESULT_NOT_FOUND for an entity that is not alive, and the world's
// errors. A failed change leaves the world as it was.

// Writes a component, adding it when the entity lacks it (recorded as an
// add). `value` holds the component's size in bytes.
NPH_MUST_USE NphResult editor_history_set(EditorHistory *h, NphEntity e, NphComponentId id,
                                          const void *value);

// Adds a component (`value` NULL: its defaults). NPH_RESULT_ALREADY_EXISTS
// when present.
NPH_MUST_USE NphResult editor_history_add(EditorHistory *h, NphEntity e, NphComponentId id,
                                          const void *value);

// Removes a component. NPH_RESULT_NOT_FOUND when absent.
NPH_MUST_USE NphResult editor_history_remove(EditorHistory *h, NphEntity e, NphComponentId id);

// Spawns an entity with the components `ids` at their defaults.
NPH_MUST_USE NphResult editor_history_spawn(EditorHistory *h, const NphComponentId *ids,
                                            u32 id_count, NphEntity *out);

// Takes an entity out of the world (suspended). Its children become roots:
// delete a subtree from the leaves up (editor_edit_delete does) so undo
// brings each child back under its parent.
NPH_MUST_USE NphResult editor_history_delete(EditorHistory *h, NphEntity e);

// Sets an entity's parent (null: a root). NPH_RESULT_INVALID_ARGUMENT for a
// cycle, as nph_world_set_parent.
NPH_MUST_USE NphResult editor_history_set_parent(EditorHistory *h, NphEntity e, NphEntity parent);

// A whole step of one set, merged with the newest step while `merge_key`
// (non-zero) repeats for the same entity and component: a drag is one step
// however many frames it lasts. Opens and closes its own step (no step may
// be open).
NPH_MUST_USE NphResult editor_history_set_merged(EditorHistory *h, NphEntity e, NphComponentId id,
                                                 const void *value, u64 merge_key,
                                                 const char *label);

// Ends the merge in progress: the next merged set starts a new step.
void editor_history_end_merge(EditorHistory *h);

// ---------------------------------------------------------------------------
// Undo and redo
// ---------------------------------------------------------------------------

bool editor_history_can_undo(const EditorHistory *h);
bool editor_history_can_redo(const EditorHistory *h);

// The labels of the step undo or redo would apply, or NULL.
const char *editor_history_undo_label(const EditorHistory *h);
const char *editor_history_redo_label(const EditorHistory *h);

// NPH_RESULT_EMPTY when there is nothing to undo (redo). A failure clears
// the history (the world was changed outside it) and is returned.
NPH_MUST_USE NphResult editor_history_undo(EditorHistory *h);
NPH_MUST_USE NphResult editor_history_redo(EditorHistory *h);

// The id of the current state: it changes with every recorded, merged,
// undone, or redone step, and comes back when undo or redo returns to a
// state. 0 is the state before any step.
u64 editor_history_state(const EditorHistory *h);

u32 editor_history_undo_count(const EditorHistory *h);
u32 editor_history_redo_count(const EditorHistory *h);

// Bytes of recorded values held.
u64 editor_history_bytes(const EditorHistory *h);
