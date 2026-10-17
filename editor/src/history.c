// SPDX-License-Identifier: Apache-2.0
// The editor's undo history. See history.h.
#include "history.h"

#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>
#include <nepheline/scene/component.h>
#include <nepheline/scene/transform.h>

#include "editor_log.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_MAX_STEPS 1000U
#define DEFAULT_MAX_BYTES ((u64)64U * 1024U * 1024U)

typedef enum OpKind {
    OP_SET = 0,
    OP_ADD,
    OP_REMOVE,
    OP_SPAWN,
    OP_DELETE,
    OP_PARENT,
} OpKind;

// A component of an entity, with its value (DELETE) or without (SPAWN).
typedef struct Snapshot {
    NphComponentId id;
    u32 size;
    u8 *value; // `size` bytes, or NULL (a tag, or a SPAWN's defaults)
} Snapshot;

typedef struct Op {
    u32 kind; // OpKind
    NphEntity entity;
    NphComponentId component; // SET, ADD, REMOVE
    u32 size;                 // SET, ADD, REMOVE: the component's size
    u8 *before;               // SET: the old value; REMOVE: the removed value
    u8 *after;                // SET, ADD: the new value
    NphEntity parent_before;  // PARENT; DELETE: the parent it had
    NphEntity parent_after;   // PARENT
    Snapshot *components;     // SPAWN, DELETE
    u32 component_count;
} Op;

typedef struct Step {
    char label[EDITOR_HISTORY_LABEL_MAX];
    u64 before; // the state the step started from
    u64 state;  // the state it leads to
    Op *ops;
    u32 op_count;
    u32 op_cap;
    u64 bytes; // values and arrays held
} Step;

struct EditorHistory {
    NphAllocator *alloc;
    NphWorld *world;
    // steps[0, done) are done (undo takes the last); steps[done, count) were
    // undone (redo takes steps[done]).
    Step *steps;
    u32 count;
    u32 done;
    u32 cap;
    u32 max_steps;
    u64 max_bytes;
    u64 bytes;
    Step open; // the step being recorded
    bool recording;
    u64 merge_key;   // the merge in progress, or 0
    u64 merge_state; // the state its step leads to
    u64 state;
    u64 next_state;
};

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

static u8 *copy_value(EditorHistory *h, const void *src, u32 size)
{
    if (size == 0U || src == NULL) {
        return NULL;
    }
    u8 *p = (u8 *)NPH_ALLOC(h->alloc, size, 16U);
    if (p != NULL) {
        memcpy(p, src, size);
    }
    return p;
}

static void free_value(EditorHistory *h, u8 *p, u32 size)
{
    if (p != NULL) {
        NPH_FREE(h->alloc, p, size, 16U);
    }
}

static void free_snapshots(EditorHistory *h, Snapshot *s, u32 count)
{
    if (s == NULL) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        free_value(h, s[i].value, s[i].size);
    }
    NPH_FREE_ARRAY(h->alloc, Snapshot, s, count);
}

static u64 op_bytes(const Op *op)
{
    u64 b = (op->before != NULL ? op->size : 0U) + (op->after != NULL ? op->size : 0U);
    b += (u64)op->component_count * sizeof(Snapshot);
    for (u32 i = 0; i < op->component_count; ++i) {
        b += op->components[i].value != NULL ? op->components[i].size : 0U;
    }
    return b;
}

static void op_free(EditorHistory *h, Op *op)
{
    free_value(h, op->before, op->size);
    free_value(h, op->after, op->size);
    free_snapshots(h, op->components, op->component_count);
    memset(op, 0, sizeof *op);
}

static void step_free(EditorHistory *h, Step *s)
{
    for (u32 i = 0; i < s->op_count; ++i) {
        op_free(h, &s->ops[i]);
    }
    if (s->ops != NULL) {
        NPH_FREE_ARRAY(h->alloc, Op, s->ops, s->op_cap);
    }
    memset(s, 0, sizeof *s);
}

// ---------------------------------------------------------------------------
// Applying ops
// ---------------------------------------------------------------------------

static const NphComponentDesc *editable_desc(const EditorHistory *h, NphComponentId id)
{
    const NphComponentDesc *d = nph_world_component_desc(h->world, id);
    if (d == NULL || (d->flags & (u32)NPH_COMPONENT_FLAG_INTERNAL) != 0U) {
        return NULL;
    }
    return d;
}

// Brings a deleted entity back: its components, their values, its parent.
static NphResult restore(EditorHistory *h, const Op *op)
{
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    for (u32 i = 0; i < op->component_count; ++i) {
        ids[i] = op->components[i].id;
    }
    NPH_TRY(nph_world_resume(h->world, op->entity, ids, op->component_count));
    for (u32 i = 0; i < op->component_count; ++i) {
        const Snapshot *s = &op->components[i];
        if (s->value != NULL) {
            NPH_TRY(nph_world_set(h->world, op->entity, s->id, s->value));
        }
    }
    if (!nph_entity_is_null(op->parent_before)) {
        NPH_TRY(nph_world_set_parent(h->world, op->entity, op->parent_before));
    }
    return NPH_RESULT_OK;
}

static NphResult spawn_again(EditorHistory *h, const Op *op)
{
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    for (u32 i = 0; i < op->component_count; ++i) {
        ids[i] = op->components[i].id;
    }
    return nph_world_resume(h->world, op->entity, ids, op->component_count);
}

static NphResult op_undo(EditorHistory *h, const Op *op)
{
    NphWorld *w = h->world;
    switch ((OpKind)op->kind) {
    case OP_SET:
        return op->before != NULL ? nph_world_set(w, op->entity, op->component, op->before)
                                  : NPH_RESULT_OK;
    case OP_ADD:
        return nph_world_remove(w, op->entity, op->component);
    case OP_REMOVE:
        return nph_world_add(w, op->entity, op->component, op->before);
    case OP_SPAWN:
        return nph_world_suspend(w, op->entity);
    case OP_DELETE:
        return restore(h, op);
    case OP_PARENT:
        return nph_world_set_parent(w, op->entity, op->parent_before);
    default:
        NPH_ASSERT_MSG(false, "unknown history op kind");
    }
    return NPH_RESULT_UNKNOWN;
}

static NphResult op_redo(EditorHistory *h, const Op *op)
{
    NphWorld *w = h->world;
    switch ((OpKind)op->kind) {
    case OP_SET:
        return op->after != NULL ? nph_world_set(w, op->entity, op->component, op->after)
                                 : NPH_RESULT_OK;
    case OP_ADD:
        return nph_world_add(w, op->entity, op->component, op->after);
    case OP_REMOVE:
        return nph_world_remove(w, op->entity, op->component);
    case OP_SPAWN:
        return spawn_again(h, op);
    case OP_DELETE:
        return nph_world_suspend(w, op->entity);
    case OP_PARENT:
        return nph_world_set_parent(w, op->entity, op->parent_after);
    default:
        NPH_ASSERT_MSG(false, "unknown history op kind");
    }
    return NPH_RESULT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Dropping steps: the handles only a dropped step could bring back go
// ---------------------------------------------------------------------------

static void release_if_suspended(EditorHistory *h, NphEntity e)
{
    if (nph_world_is_suspended(h->world, e)) {
        nph_world_release(h->world, e);
    }
}

// The entities a step of `kind` ops left suspended.
static void release_ops(EditorHistory *h, const Step *s, OpKind kind)
{
    for (u32 i = 0; i < s->op_count; ++i) {
        if (s->ops[i].kind == (u32)kind) {
            release_if_suspended(h, s->ops[i].entity);
        }
    }
}

// A done step goes: what it deleted stays deleted.
static void forget_done(EditorHistory *h, Step *s)
{
    release_ops(h, s, OP_DELETE);
    h->bytes -= s->bytes;
    step_free(h, s);
}

// An undone step goes: what it spawned is never spawned again.
static void forget_undone(EditorHistory *h, Step *s)
{
    release_ops(h, s, OP_SPAWN);
    h->bytes -= s->bytes;
    step_free(h, s);
}

static void discard_redo(EditorHistory *h)
{
    for (u32 i = h->done; i < h->count; ++i) {
        forget_undone(h, &h->steps[i]);
    }
    h->count = h->done;
}

static void drop_oldest(EditorHistory *h)
{
    NPH_ASSERT(h->done > 0U);
    forget_done(h, &h->steps[0]);
    memmove(h->steps, h->steps + 1, sizeof(Step) * (h->count - 1U));
    --h->count;
    --h->done;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult editor_history_create(const EditorHistoryDesc *desc, EditorHistory **out)
{
    NPH_ASSERT(desc != NULL && desc->alloc != NULL && desc->world != NULL && out != NULL);
    *out = NULL;
    EditorHistory *h = NPH_ALLOC_TYPE(desc->alloc, EditorHistory);
    if (h == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    h->alloc = desc->alloc;
    h->world = desc->world;
    h->max_steps = desc->max_steps != 0U ? desc->max_steps : DEFAULT_MAX_STEPS;
    h->max_bytes = desc->max_bytes != 0U ? desc->max_bytes : DEFAULT_MAX_BYTES;
    h->next_state = 1U;
    *out = h;
    return NPH_RESULT_OK;
}

void editor_history_clear(EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    if (h->recording) {
        editor_history_abort(h);
    }
    discard_redo(h);
    while (h->done > 0U) {
        forget_done(h, &h->steps[h->done - 1U]);
        --h->done;
    }
    h->count = 0U;
    h->merge_key = 0U;
    NPH_ASSERT(h->bytes == 0U);
}

void editor_history_destroy(EditorHistory *h)
{
    if (h == NULL) {
        return;
    }
    editor_history_clear(h);
    if (h->steps != NULL) {
        NPH_FREE_ARRAY(h->alloc, Step, h->steps, h->cap);
    }
    NPH_FREE_TYPE(h->alloc, EditorHistory, h);
}

NphWorld *editor_history_world(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->world;
}

NphAllocator *editor_history_allocator(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->alloc;
}

void editor_history_rebind(EditorHistory *h, NphWorld *world)
{
    NPH_ASSERT(h != NULL && world != NULL);
    h->world = world; // the releases below go to the world that has the handles now
    editor_history_clear(h);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

NphResult editor_history_begin(EditorHistory *h, const char *label)
{
    NPH_ASSERT(h != NULL);
    if (h->recording) {
        return NPH_RESULT_INVALID_STATE;
    }
    memset(&h->open, 0, sizeof h->open);
    (void)snprintf(h->open.label, sizeof h->open.label, "%s", label != NULL ? label : "Edit");
    h->recording = true;
    h->merge_key = 0U;
    return NPH_RESULT_OK;
}

bool editor_history_recording(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->recording;
}

static NphResult grow_steps(EditorHistory *h)
{
    if (h->count < h->cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = h->cap != 0U ? h->cap * 2U : 32U;
    Step *steps = (Step *)NPH_REALLOC(h->alloc, h->steps, sizeof(Step) * (u64)h->cap,
                                      sizeof(Step) * (u64)cap, NPH_ALIGNOF(Step));
    if (steps == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    h->steps = steps;
    h->cap = cap;
    return NPH_RESULT_OK;
}

void editor_history_end(EditorHistory *h)
{
    NPH_ASSERT(h != NULL && h->recording);
    h->recording = false;
    if (h->open.op_count == 0U) {
        step_free(h, &h->open);
        return;
    }
    discard_redo(h);
    if (grow_steps(h) != NPH_RESULT_OK) {
        // Out of memory for the list itself: the change stays, but cannot be
        // undone. Older steps would undo onto a world they no longer
        // describe, so they go too.
        NPH_LOG_ERROR(&LOG_EDITOR, "out of memory for the undo history; history cleared");
        release_ops(h, &h->open, OP_DELETE);
        step_free(h, &h->open);
        editor_history_clear(h);
        return;
    }
    Step *s = &h->steps[h->count++];
    *s = h->open;
    memset(&h->open, 0, sizeof h->open);
    s->before = h->state;
    s->state = h->next_state++;
    h->state = s->state;
    h->done = h->count;
    h->bytes += s->bytes;
    while (h->count > 1U && (h->count > h->max_steps || h->bytes > h->max_bytes)) {
        drop_oldest(h);
    }
}

void editor_history_abort(EditorHistory *h)
{
    NPH_ASSERT(h != NULL && h->recording);
    Step *s = &h->open;
    for (u32 i = s->op_count; i-- > 0U;) {
        const Op *op = &s->ops[i];
        const NphResult r = op_undo(h, op);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_EDITOR, "undoing an aborted '%s': %s", s->label,
                          nph_result_string(r));
        }
        if (op->kind == (u32)OP_SPAWN) {
            release_if_suspended(h, op->entity); // never spawned again
        }
    }
    step_free(h, s);
    h->recording = false;
}

static NphResult push_op(EditorHistory *h, const Op *op)
{
    Step *s = &h->open;
    if (s->op_count == s->op_cap) {
        const u32 cap = s->op_cap != 0U ? s->op_cap * 2U : 4U;
        Op *ops = (Op *)NPH_REALLOC(h->alloc, s->ops, sizeof(Op) * (u64)s->op_cap,
                                    sizeof(Op) * (u64)cap, NPH_ALIGNOF(Op));
        if (ops == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        s->ops = ops;
        s->op_cap = cap;
    }
    s->ops[s->op_count++] = *op;
    s->bytes += op_bytes(op) + sizeof(Op);
    return NPH_RESULT_OK;
}

static NphResult check_entity(const EditorHistory *h, NphEntity e)
{
    if (!h->recording) {
        return NPH_RESULT_INVALID_STATE;
    }
    return nph_world_is_alive(h->world, e) ? NPH_RESULT_OK : NPH_RESULT_NOT_FOUND;
}

NphResult editor_history_add(EditorHistory *h, NphEntity e, NphComponentId id, const void *value)
{
    NPH_ASSERT(h != NULL);
    NPH_TRY(check_entity(h, e));
    const NphComponentDesc *d = editable_desc(h, id);
    if (d == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (nph_world_has(h->world, e, id)) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    NPH_TRY(nph_world_add(h->world, e, id, value));
    Op op;
    memset(&op, 0, sizeof op);
    op.kind = (u32)OP_ADD;
    op.entity = e;
    op.component = id;
    op.size = d->size;
    op.after = copy_value(h, nph_world_get(h->world, e, id), d->size);
    NphResult r = d->size == 0U || op.after != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        r = push_op(h, &op);
    }
    if (r != NPH_RESULT_OK) {
        free_value(h, op.after, op.size);
        NPH_VERIFY(nph_world_remove(h->world, e, id) == NPH_RESULT_OK);
    }
    return r;
}

NphResult editor_history_set(EditorHistory *h, NphEntity e, NphComponentId id, const void *value)
{
    NPH_ASSERT(h != NULL && value != NULL);
    NPH_TRY(check_entity(h, e));
    const NphComponentDesc *d = editable_desc(h, id);
    if (d == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!nph_world_has(h->world, e, id)) {
        return editor_history_add(h, e, id, value);
    }
    if (d->size == 0U || memcmp(nph_world_get(h->world, e, id), value, d->size) == 0) {
        return NPH_RESULT_OK; // nothing changes, nothing to record
    }
    Op op;
    memset(&op, 0, sizeof op);
    op.kind = (u32)OP_SET;
    op.entity = e;
    op.component = id;
    op.size = d->size;
    op.before = copy_value(h, nph_world_get(h->world, e, id), d->size);
    op.after = copy_value(h, value, d->size);
    NphResult r = op.before != NULL && op.after != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        r = nph_world_set(h->world, e, id, value);
        if (r == NPH_RESULT_OK) {
            r = push_op(h, &op);
            if (r != NPH_RESULT_OK) {
                NPH_VERIFY(nph_world_set(h->world, e, id, op.before) == NPH_RESULT_OK);
            }
        }
    }
    if (r != NPH_RESULT_OK) {
        free_value(h, op.before, op.size);
        free_value(h, op.after, op.size);
    }
    return r;
}

NphResult editor_history_remove(EditorHistory *h, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(h != NULL);
    NPH_TRY(check_entity(h, e));
    const NphComponentDesc *d = editable_desc(h, id);
    if (d == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!nph_world_has(h->world, e, id)) {
        return NPH_RESULT_NOT_FOUND;
    }
    Op op;
    memset(&op, 0, sizeof op);
    op.kind = (u32)OP_REMOVE;
    op.entity = e;
    op.component = id;
    op.size = d->size;
    op.before = copy_value(h, nph_world_get(h->world, e, id), d->size);
    if (d->size != 0U && op.before == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphResult r = nph_world_remove(h->world, e, id);
    if (r == NPH_RESULT_OK) {
        r = push_op(h, &op);
        if (r != NPH_RESULT_OK) {
            NPH_VERIFY(nph_world_add(h->world, e, id, op.before) == NPH_RESULT_OK);
        }
    }
    if (r != NPH_RESULT_OK) {
        free_value(h, op.before, op.size);
    }
    return r;
}

NphResult editor_history_spawn(EditorHistory *h, const NphComponentId *ids, u32 id_count,
                               NphEntity *out)
{
    NPH_ASSERT(h != NULL && out != NULL && (ids != NULL || id_count == 0U));
    *out = NPH_ENTITY_NULL;
    if (!h->recording) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (id_count > NPH_ECS_MAX_COMPONENTS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < id_count; ++i) {
        if (editable_desc(h, ids[i]) == NULL) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    Op op;
    memset(&op, 0, sizeof op);
    op.kind = (u32)OP_SPAWN;
    if (id_count > 0U) {
        op.components = NPH_ALLOC_ARRAY_ZEROED(h->alloc, Snapshot, id_count);
        if (op.components == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        for (u32 i = 0; i < id_count; ++i) {
            op.components[i].id = ids[i];
        }
        op.component_count = id_count;
    }
    NphResult r = nph_world_spawn(h->world, ids, id_count, &op.entity);
    if (r == NPH_RESULT_OK) {
        r = push_op(h, &op);
        if (r != NPH_RESULT_OK) {
            NPH_VERIFY(nph_world_despawn(h->world, op.entity) == NPH_RESULT_OK);
        }
    }
    if (r != NPH_RESULT_OK) {
        free_snapshots(h, op.components, op.component_count);
        return r;
    }
    *out = op.entity;
    return NPH_RESULT_OK;
}

NphResult editor_history_set_parent(EditorHistory *h, NphEntity e, NphEntity parent)
{
    NPH_ASSERT(h != NULL);
    NPH_TRY(check_entity(h, e));
    if (!nph_entity_is_null(parent) && !nph_world_is_alive(h->world, parent)) {
        return NPH_RESULT_NOT_FOUND;
    }
    const NphEntity before = nph_world_parent(h->world, e);
    if (nph_entity_equals(before, parent)) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(nph_world_set_parent(h->world, e, parent));
    Op op;
    memset(&op, 0, sizeof op);
    op.kind = (u32)OP_PARENT;
    op.entity = e;
    op.parent_before = before;
    op.parent_after = parent;
    const NphResult r = push_op(h, &op);
    if (r != NPH_RESULT_OK) {
        NPH_VERIFY(nph_world_set_parent(h->world, e, before) == NPH_RESULT_OK);
    }
    return r;
}

// Records every child of `e` leaving it, the last child first: undo then
// brings them back under `e` in their order once `e` is back.
static NphResult detach_children(EditorHistory *h, NphEntity e)
{
    NphWorld *w = h->world;
    u32 n = 0U;
    for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
         c = nph_world_next_sibling(w, c)) {
        ++n;
    }
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    NphEntity *children = NPH_ALLOC_ARRAY(h->alloc, NphEntity, n);
    if (children == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 i = 0U;
    for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
         c = nph_world_next_sibling(w, c)) {
        children[i++] = c;
    }
    NphResult r = NPH_RESULT_OK;
    for (i = n; i-- > 0U && r == NPH_RESULT_OK;) {
        r = editor_history_set_parent(h, children[i], NPH_ENTITY_NULL);
    }
    NPH_FREE_ARRAY(h->alloc, NphEntity, children, n);
    return r;
}

NphResult editor_history_delete(EditorHistory *h, NphEntity e)
{
    NPH_ASSERT(h != NULL);
    NPH_TRY(check_entity(h, e));
    NphWorld *w = h->world;
    NPH_TRY(detach_children(h, e));
    // Every component but the internal Hierarchy, whose parent link the op
    // restores by itself.
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    const u32 all = nph_world_entity_components(w, e, ids, NPH_ECS_MAX_COMPONENTS);
    u32 kept = 0U;
    for (u32 i = 0; i < all; ++i) {
        if (editable_desc(h, ids[i]) != NULL) {
            ids[kept++] = ids[i];
        }
    }
    Op op;
    memset(&op, 0, sizeof op);
    op.kind = (u32)OP_DELETE;
    op.entity = e;
    op.parent_before = nph_world_parent(w, e);
    if (kept > 0U) {
        op.components = NPH_ALLOC_ARRAY_ZEROED(h->alloc, Snapshot, kept);
        if (op.components == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        op.component_count = kept;
    }
    for (u32 i = 0; i < kept; ++i) {
        const NphComponentDesc *d = editable_desc(h, ids[i]);
        Snapshot *s = &op.components[i];
        s->id = ids[i];
        s->size = d->size;
        s->value = copy_value(h, nph_world_get(w, e, ids[i]), d->size);
        if (d->size != 0U && s->value == NULL) {
            free_snapshots(h, op.components, op.component_count);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    NphResult r = nph_world_suspend(w, e);
    if (r == NPH_RESULT_OK) {
        r = push_op(h, &op);
        if (r != NPH_RESULT_OK) {
            NPH_VERIFY(restore(h, &op) == NPH_RESULT_OK);
        }
    }
    if (r != NPH_RESULT_OK) {
        free_snapshots(h, op.components, op.component_count);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Merged sets
// ---------------------------------------------------------------------------

NphResult editor_history_set_merged(EditorHistory *h, NphEntity e, NphComponentId id,
                                    const void *value, u64 merge_key, const char *label)
{
    NPH_ASSERT(h != NULL && value != NULL);
    if (h->recording) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (merge_key != 0U && merge_key == h->merge_key && h->state == h->merge_state &&
        h->done == h->count && h->done > 0U && h->steps[h->done - 1U].op_count == 1U) {
        Step *s = &h->steps[h->done - 1U];
        Op *op = &s->ops[0];
        if (op->kind == (u32)OP_SET && nph_entity_equals(op->entity, e) && op->component == id &&
            nph_world_has(h->world, e, id)) {
            if (memcmp(op->after, value, op->size) == 0) {
                return NPH_RESULT_OK;
            }
            NPH_TRY(nph_world_set(h->world, e, id, value));
            memcpy(op->after, value, op->size);
            s->state = h->next_state++;
            h->state = s->state;
            h->merge_state = s->state;
            return NPH_RESULT_OK;
        }
    }
    NPH_TRY(editor_history_begin(h, label));
    const NphResult r = editor_history_set(h, e, id, value);
    if (r != NPH_RESULT_OK) {
        editor_history_abort(h);
        return r;
    }
    const bool recorded = h->open.op_count > 0U;
    editor_history_end(h);
    if (recorded) {
        h->merge_key = merge_key;
        h->merge_state = h->state;
    }
    return NPH_RESULT_OK;
}

void editor_history_end_merge(EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    h->merge_key = 0U;
}

// ---------------------------------------------------------------------------
// Undo and redo
// ---------------------------------------------------------------------------

bool editor_history_can_undo(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return !h->recording && h->done > 0U;
}

bool editor_history_can_redo(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return !h->recording && h->done < h->count;
}

const char *editor_history_undo_label(const EditorHistory *h)
{
    return editor_history_can_undo(h) ? h->steps[h->done - 1U].label : NULL;
}

const char *editor_history_redo_label(const EditorHistory *h)
{
    return editor_history_can_redo(h) ? h->steps[h->done].label : NULL;
}

static NphResult broken(EditorHistory *h, const char *what, const Step *s, NphResult r)
{
    NPH_LOG_ERROR(&LOG_EDITOR,
                  "%s '%s' failed (%s): the world changed outside the history; history cleared",
                  what, s->label, nph_result_string(r));
    editor_history_clear(h);
    return r;
}

NphResult editor_history_undo(EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    if (h->recording) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (h->done == 0U) {
        return NPH_RESULT_EMPTY;
    }
    const Step *s = &h->steps[h->done - 1U];
    for (u32 i = s->op_count; i-- > 0U;) {
        const NphResult r = op_undo(h, &s->ops[i]);
        if (r != NPH_RESULT_OK) {
            return broken(h, "undoing", s, r);
        }
    }
    --h->done;
    h->state = s->before;
    h->merge_key = 0U;
    return NPH_RESULT_OK;
}

NphResult editor_history_redo(EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    if (h->recording) {
        return NPH_RESULT_INVALID_STATE;
    }
    if (h->done == h->count) {
        return NPH_RESULT_EMPTY;
    }
    const Step *s = &h->steps[h->done];
    for (u32 i = 0; i < s->op_count; ++i) {
        const NphResult r = op_redo(h, &s->ops[i]);
        if (r != NPH_RESULT_OK) {
            return broken(h, "redoing", s, r);
        }
    }
    ++h->done;
    h->state = s->state;
    h->merge_key = 0U;
    return NPH_RESULT_OK;
}

u64 editor_history_state(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->state;
}

u32 editor_history_undo_count(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->done;
}

u32 editor_history_redo_count(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->count - h->done;
}

u64 editor_history_bytes(const EditorHistory *h)
{
    NPH_ASSERT(h != NULL);
    return h->bytes;
}
