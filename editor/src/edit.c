// SPDX-License-Identifier: Apache-2.0
// The editor's edits. See edit.h.
#include "edit.h"

#include <nepheline/core/alloc.h>
#include <nepheline/core/assert.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/quat.h>
#include <nepheline/scene/component.h>

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

NphWorldTransform editor_world_transform_of(const NphWorld *w, NphEntity e)
{
    NphEntity chain[NPH_HIERARCHY_MAX_DEPTH + 1U];
    u32 n = 0U;
    for (NphEntity c = e; !nph_entity_is_null(c) && n < NPH_HIERARCHY_MAX_DEPTH + 1U;
         c = nph_world_parent(w, c)) {
        chain[n++] = c;
    }
    NphWorldTransform t = nph_world_transform_identity();
    for (u32 i = n; i-- > 0U;) {
        const NphLocalTransform *local =
            NPH_WORLD_GET(w, chain[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
        const NphLocalTransform identity = nph_local_transform_identity();
        t = nph_world_transform_compose(&t, local != NULL ? local : &identity);
    }
    return t;
}

NphLocalTransform editor_local_from_world(const NphWorldTransform *world,
                                          const NphWorldTransform *parent)
{
    NphMat3 linear = nph_world_transform_linear(world);
    NphLocalTransform out;
    if (parent != NULL) {
        NphMat3 inv;
        if (!nph_mat3_inverse(nph_world_transform_linear(parent), &inv)) {
            inv = nph_mat3_identity(); // a degenerate parent: keep the axes as they are
        }
        linear = nph_mat3_mul(inv, linear);
        // The offset from the parent is small next to the world's extent:
        // f32 holds it, while the parent's own translation stays f64.
        const NphVec3 offset = nph_dvec3_relative(world->translation, parent->translation);
        out.translation = nph_dvec3_from_vec3(nph_mat3_mul_vec3(inv, offset));
    } else {
        out.translation = world->translation;
    }
    NphVec3 scale = nph_vec3(nph_vec3_length(linear.c[0]), nph_vec3_length(linear.c[1]),
                             nph_vec3_length(linear.c[2]));
    if (nph_mat3_determinant(linear) < 0.0F) {
        scale.x = -scale.x; // a mirror: one negative scale keeps the rotation proper
    }
    const NphMat3 rot = nph_mat3_orthonormalize(
        nph_mat3_from_cols(nph_vec3_scale(linear.c[0], scale.x != 0.0F ? 1.0F / scale.x : 0.0F),
                           nph_vec3_scale(linear.c[1], scale.y != 0.0F ? 1.0F / scale.y : 0.0F),
                           nph_vec3_scale(linear.c[2], scale.z != 0.0F ? 1.0F / scale.z : 0.0F)));
    out.rotation = nph_quat_normalize_or_identity(nph_quat_from_mat3(rot));
    out.scale = scale;
    return out;
}

bool editor_is_under(const NphWorld *w, NphEntity e, NphEntity ancestor)
{
    u32 guard = 0U;
    for (NphEntity c = e; !nph_entity_is_null(c) && guard <= NPH_HIERARCHY_MAX_DEPTH;
         c = nph_world_parent(w, c), ++guard) {
        if (nph_entity_equals(c, ancestor)) {
            return true;
        }
    }
    return false;
}

void editor_entity_label(const NphWorld *w, NphEntity e, char *out, u64 cap)
{
    const NphName *name = NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_NAME, NphName);
    if (name != NULL && name->text.text[0] != '\0') {
        (void)snprintf(out, (size_t)cap, "%s", name->text.text);
    } else if (nph_world_is_alive(w, e)) {
        (void)snprintf(out, (size_t)cap, "Entity %u", nph_world_entity_index(w, e));
    } else {
        (void)snprintf(out, (size_t)cap, "(gone)");
    }
}

// ---------------------------------------------------------------------------
// Subtrees
// ---------------------------------------------------------------------------

// A subtree in pre-order: entities[i]'s parent is entities[parents[i]]
// (NPH_U32_MAX for the root).
typedef struct Subtree {
    NphAllocator *alloc;
    NphEntity *entities;
    u32 *parents;
    u32 count;
    u32 cap;
} Subtree;

static NphResult subtree_push(Subtree *t, NphEntity e, u32 parent)
{
    if (t->count == t->cap) {
        const u32 cap = t->cap != 0U ? t->cap * 2U : 16U;
        NphEntity *entities =
            (NphEntity *)NPH_REALLOC(t->alloc, t->entities, sizeof(NphEntity) * (u64)t->cap,
                                     sizeof(NphEntity) * (u64)cap, 4U);
        if (entities == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        t->entities = entities;
        u32 *parents = (u32 *)NPH_REALLOC(t->alloc, t->parents, sizeof(u32) * (u64)t->cap,
                                          sizeof(u32) * (u64)cap, 4U);
        if (parents == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        t->parents = parents;
        t->cap = cap;
    }
    t->entities[t->count] = e;
    t->parents[t->count] = parent;
    ++t->count;
    return NPH_RESULT_OK;
}

static NphResult subtree_gather(Subtree *t, const NphWorld *w, NphEntity e, u32 parent)
{
    const u32 index = t->count;
    NPH_TRY(subtree_push(t, e, parent));
    for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
         c = nph_world_next_sibling(w, c)) {
        NPH_TRY(subtree_gather(t, w, c, index));
    }
    return NPH_RESULT_OK;
}

static void subtree_free(Subtree *t)
{
    if (t->entities != NULL) {
        NPH_FREE(t->alloc, t->entities, sizeof(NphEntity) * (u64)t->cap, 4U);
        NPH_FREE(t->alloc, t->parents, sizeof(u32) * (u64)t->cap, 4U);
    }
    memset(t, 0, sizeof *t);
}

// True when another of `entities` is an ancestor of entities[index].
static bool under_another(const NphWorld *w, const NphEntity *entities, u32 count, u32 index)
{
    for (u32 j = 0; j < count; ++j) {
        if (j != index && !nph_entity_equals(entities[j], entities[index]) &&
            editor_is_under(w, entities[index], entities[j])) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------

static NphResult finish(EditorHistory *h, NphResult r)
{
    if (r == NPH_RESULT_OK) {
        editor_history_end(h);
    } else {
        editor_history_abort(h);
    }
    return r;
}

static NphResult create(EditorHistory *h, const char *name, NphEntity parent,
                        const NphLocalTransform *local, NphEntity *out)
{
    const NphComponentId ids[3] = {NPH_COMPONENT_ID_NAME, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                   NPH_COMPONENT_ID_WORLD_TRANSFORM};
    NPH_TRY(editor_history_spawn(h, ids, 3U, out));
    NphName n;
    memset(&n, 0, sizeof n);
    (void)nph_text_set(&n.text, nph_str_from_cstr(name != NULL ? name : "Entity"));
    NPH_TRY(editor_history_set(h, *out, NPH_COMPONENT_ID_NAME, &n));
    if (local != NULL) {
        NPH_TRY(editor_history_set(h, *out, NPH_COMPONENT_ID_LOCAL_TRANSFORM, local));
    }
    if (!nph_entity_is_null(parent)) {
        NPH_TRY(editor_history_set_parent(h, *out, parent));
    }
    return NPH_RESULT_OK;
}

NphResult editor_edit_create(EditorHistory *h, const char *name, NphEntity parent,
                             const NphLocalTransform *local, NphEntity *out)
{
    NPH_ASSERT(h != NULL && out != NULL);
    *out = NPH_ENTITY_NULL;
    NPH_TRY(editor_history_begin(h, "Create Entity"));
    const NphResult r = finish(h, create(h, name, parent, local, out));
    if (r != NPH_RESULT_OK) {
        *out = NPH_ENTITY_NULL;
    }
    return r;
}

// Deletes the leaves first, the children of an entity last first: undo then
// brings each back under its parent, in order.
static NphResult delete_subtree(EditorHistory *h, NphEntity e)
{
    const NphWorld *w = editor_history_world(h);
    for (;;) {
        NphEntity last = NPH_ENTITY_NULL;
        for (NphEntity c = nph_world_first_child(w, e); !nph_entity_is_null(c);
             c = nph_world_next_sibling(w, c)) {
            last = c;
        }
        if (nph_entity_is_null(last)) {
            break;
        }
        NPH_TRY(delete_subtree(h, last));
    }
    return editor_history_delete(h, e);
}

NphResult editor_edit_delete(EditorHistory *h, const NphEntity *entities, u32 count)
{
    NPH_ASSERT(h != NULL && (entities != NULL || count == 0U));
    const NphWorld *w = editor_history_world(h);
    char label[EDITOR_HISTORY_LABEL_MAX];
    if (count == 1U) {
        char name[48];
        editor_entity_label(w, entities[0], name, sizeof name);
        (void)snprintf(label, sizeof label, "Delete %s", name);
    } else {
        (void)snprintf(label, sizeof label, "Delete %u entities", count);
    }
    NPH_TRY(editor_history_begin(h, label));
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
        if (nph_world_is_alive(w, entities[i]) && !under_another(w, entities, count, i)) {
            r = delete_subtree(h, entities[i]);
        }
    }
    return finish(h, r);
}

// Copies one component value of `src` (an entity of `src_world`) onto `dst`.
// Its ENTITY fields that name an entity of the copied subtree point at that
// entity's copy; the others keep their value within one world and become
// null across worlds.
static NphResult copy_component(EditorHistory *h, const NphWorld *src_world, const Subtree *t,
                                const NphEntity *copies, NphEntity src, NphEntity dst,
                                NphComponentId id)
{
    const NphWorld *w = editor_history_world(h);
    const NphComponentDesc *d = nph_world_component_desc(w, id);
    NPH_ASSERT(nph_world_component_desc(src_world, id) != NULL &&
               nph_world_component_desc(src_world, id)->size == d->size);
    if (d->size == 0U) {
        return NPH_RESULT_OK; // a tag: the spawn added it
    }
    u8 value[NPH_ECS_MAX_COMPONENT_SIZE];
    memcpy(value, nph_world_get(src_world, src, id), d->size);
    for (u32 f = 0; f < d->field_count; ++f) {
        const NphFieldDesc *field = &d->fields[f];
        if (field->kind != (u32)NPH_FIELD_ENTITY) {
            continue;
        }
        NphEntity ref;
        memcpy(&ref, value + field->offset, sizeof ref);
        if (nph_entity_is_null(ref)) {
            continue;
        }
        NphEntity mapped = src_world == w ? ref : NPH_ENTITY_NULL;
        for (u32 i = 0; i < t->count; ++i) {
            if (nph_entity_equals(t->entities[i], ref)) {
                mapped = copies[i];
                break;
            }
        }
        memcpy(value + field->offset, &mapped, sizeof mapped);
    }
    return editor_history_set(h, dst, id, value);
}

// Copies `root` of `src` (the history's world or another with its
// components) and its subtree into the history's world under `parent`.
static NphResult copy_root(EditorHistory *h, const NphWorld *src, NphEntity root, NphEntity parent,
                           NphEntity *out)
{
    NphAllocator *alloc = editor_history_allocator(h);
    Subtree t;
    memset(&t, 0, sizeof t);
    t.alloc = alloc;
    NphResult r = subtree_gather(&t, src, root, NPH_U32_MAX);
    NphEntity *copies = NULL;
    if (r == NPH_RESULT_OK) {
        copies = NPH_ALLOC_ARRAY_ZEROED(alloc, NphEntity, t.count);
        r = copies != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    }
    // The entities first (so references between them can be remapped),
    // with every component but the internal Hierarchy.
    const NphWorld *w = editor_history_world(h);
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    for (u32 i = 0; i < t.count && r == NPH_RESULT_OK; ++i) {
        const u32 all =
            nph_world_entity_components(src, t.entities[i], ids, NPH_ECS_MAX_COMPONENTS);
        u32 kept = 0U;
        for (u32 c = 0; c < all; ++c) {
            if ((nph_world_component_desc(src, ids[c])->flags & (u32)NPH_COMPONENT_FLAG_INTERNAL) ==
                0U) {
                ids[kept++] = ids[c];
            }
        }
        r = editor_history_spawn(h, ids, kept, &copies[i]);
    }
    // Then the values, then the hierarchy in pre-order, which keeps the
    // children's order.
    for (u32 i = 0; i < t.count && r == NPH_RESULT_OK; ++i) {
        const u32 n = nph_world_entity_components(w, copies[i], ids, NPH_ECS_MAX_COMPONENTS);
        for (u32 c = 0; c < n && r == NPH_RESULT_OK; ++c) {
            r = copy_component(h, src, &t, copies, t.entities[i], copies[i], ids[c]);
        }
    }
    for (u32 i = 0; i < t.count && r == NPH_RESULT_OK; ++i) {
        const NphEntity to = t.parents[i] == NPH_U32_MAX ? parent : copies[t.parents[i]];
        if (!nph_entity_is_null(to)) {
            r = editor_history_set_parent(h, copies[i], to);
        }
    }
    if (r == NPH_RESULT_OK) {
        *out = copies[0];
    }
    if (copies != NULL) {
        NPH_FREE_ARRAY(alloc, NphEntity, copies, t.count);
    }
    subtree_free(&t);
    return r;
}

NphResult editor_edit_duplicate(EditorHistory *h, const NphEntity *entities, u32 count,
                                NphEntity *out)
{
    NPH_ASSERT(h != NULL && (entities != NULL || count == 0U));
    const NphWorld *w = editor_history_world(h);
    NPH_TRY(editor_history_begin(h, count == 1U ? "Duplicate" : "Duplicate entities"));
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
        NphEntity copy = NPH_ENTITY_NULL;
        if (nph_world_is_alive(w, entities[i]) && !under_another(w, entities, count, i)) {
            r = copy_root(h, w, entities[i], nph_world_parent(w, entities[i]), &copy);
        }
        if (out != NULL) {
            out[i] = copy;
        }
    }
    r = finish(h, r);
    if (r != NPH_RESULT_OK && out != NULL) {
        memset(out, 0, sizeof(NphEntity) * (u64)count);
    }
    return r;
}

NphResult editor_edit_paste(EditorHistory *h, const NphWorld *src, NphEntity root, NphEntity parent,
                            const char *label, NphEntity *out)
{
    NPH_ASSERT(h != NULL && src != NULL && label != NULL && out != NULL);
    *out = NPH_ENTITY_NULL;
    if (!nph_world_is_alive(src, root) ||
        (!nph_entity_is_null(parent) && !nph_world_is_alive(editor_history_world(h), parent))) {
        return NPH_RESULT_NOT_FOUND;
    }
    NPH_TRY(editor_history_begin(h, label));
    NphEntity copy = NPH_ENTITY_NULL;
    const NphResult r = finish(h, copy_root(h, src, root, parent, &copy));
    if (r == NPH_RESULT_OK) {
        *out = copy;
    }
    return r;
}

NphResult editor_edit_rename(EditorHistory *h, NphEntity e, const char *name)
{
    NPH_ASSERT(h != NULL && name != NULL);
    NphName n;
    memset(&n, 0, sizeof n);
    (void)nph_text_set(&n.text, nph_str_from_cstr(name));
    NPH_TRY(editor_history_begin(h, "Rename"));
    return finish(h, editor_history_set(h, e, NPH_COMPONENT_ID_NAME, &n));
}

NphResult editor_edit_reparent(EditorHistory *h, NphEntity e, NphEntity parent)
{
    NPH_ASSERT(h != NULL);
    const NphWorld *w = editor_history_world(h);
    if (!nph_world_is_alive(w, e) ||
        (!nph_entity_is_null(parent) && !nph_world_is_alive(w, parent))) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (!nph_entity_is_null(parent) && editor_is_under(w, parent, e)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphWorldTransform world = editor_world_transform_of(w, e);
    NphWorldTransform parent_world = nph_world_transform_identity();
    if (!nph_entity_is_null(parent)) {
        parent_world = editor_world_transform_of(w, parent);
    }
    NPH_TRY(editor_history_begin(h, "Reparent"));
    NphResult r = editor_history_set_parent(h, e, parent);
    if (r == NPH_RESULT_OK && nph_world_has(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM)) {
        const NphLocalTransform local =
            editor_local_from_world(&world, nph_entity_is_null(parent) ? NULL : &parent_world);
        r = editor_history_set(h, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, &local);
    }
    return finish(h, r);
}

NphResult editor_edit_add_component(EditorHistory *h, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(h != NULL);
    const NphComponentDesc *d = nph_world_component_desc(editor_history_world(h), id);
    char label[EDITOR_HISTORY_LABEL_MAX];
    (void)snprintf(label, sizeof label, "Add %s", d != NULL ? d->name : "component");
    NPH_TRY(editor_history_begin(h, label));
    return finish(h, editor_history_add(h, e, id, NULL));
}

NphResult editor_edit_remove_component(EditorHistory *h, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(h != NULL);
    const NphComponentDesc *d = nph_world_component_desc(editor_history_world(h), id);
    char label[EDITOR_HISTORY_LABEL_MAX];
    (void)snprintf(label, sizeof label, "Remove %s", d != NULL ? d->name : "component");
    NPH_TRY(editor_history_begin(h, label));
    return finish(h, editor_history_remove(h, e, id));
}
