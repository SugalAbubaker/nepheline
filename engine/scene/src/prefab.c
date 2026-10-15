// SPDX-License-Identifier: Apache-2.0
// Prefabs: template worlds and their instantiation (prefab.h; docs/scene.md
// "Prefabs"). Overrides are computed and applied by the scene serializer
// (scene_text.c).

#include <nepheline/core/assert.h>
#include <nepheline/scene/prefab.h>
#include <nepheline/scene/transform.h>

#include "scene_internal.h"

#include <string.h>

NPH_COMPONENT_DEFINE(NphPrefabInstance, NPH_PREFAB_INSTANCE_FIELDS, "PrefabInstance", 0U, NULL)

#define DEFAULT_PREFAB_ENTITIES 4096U

struct NphPrefab {
    NphAllocator *alloc;
    NphWorld *world;
    char *name;
    u64 name_len;
    NphStrId id;
};

NphResult nph_prefab_create(NphStr name, const NphWorld *registry, NphAllocator *alloc,
                            u32 max_entities, NphPrefab **out)
{
    NPH_ASSERT(registry != NULL && alloc != NULL && out != NULL);
    *out = NULL;
    if (name.len == 0U || name.len > NPH_TEXT_CAPACITY - 1U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphPrefab *p = NPH_ALLOC_TYPE(alloc, NphPrefab);
    if (p == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    p->alloc = alloc;
    p->id = nph_str_id(name);
    NphResult r = nph_str_dup(alloc, name, &p->name);
    p->name_len = r == NPH_RESULT_OK ? name.len : 0U;
    if (r == NPH_RESULT_OK) {
        NphWorldDesc wd;
        memset(&wd, 0, sizeof wd);
        wd.alloc = alloc;
        wd.max_entities = max_entities == 0U ? DEFAULT_PREFAB_ENTITIES : max_entities;
        wd.registry_from = registry;
        r = nph_world_create(&wd, &p->world);
    }
    if (r != NPH_RESULT_OK) {
        nph_prefab_destroy(p);
        return r;
    }
    *out = p;
    return NPH_RESULT_OK;
}

void nph_prefab_destroy(NphPrefab *p)
{
    if (p == NULL) {
        return;
    }
    nph_world_destroy(p->world);
    if (p->name != NULL) {
        nph_str_free(p->alloc, p->name, p->name_len);
    }
    NPH_FREE_TYPE(p->alloc, NphPrefab, p);
}

NphWorld *nph_prefab_world(NphPrefab *p)
{
    NPH_ASSERT(p != NULL);
    return p->world;
}

const NphWorld *nph_prefab_world_const(const NphPrefab *p)
{
    NPH_ASSERT(p != NULL);
    return p->world;
}

NphStr nph_prefab_name(const NphPrefab *p)
{
    NPH_ASSERT(p != NULL);
    return nph_str(p->name, p->name_len);
}

NphStrId nph_prefab_id(const NphPrefab *p)
{
    NPH_ASSERT(p != NULL);
    return p->id;
}

u32 nph_prefab_entity_count(const NphPrefab *p)
{
    NPH_ASSERT(p != NULL);
    return nph_world_entity_count(p->world);
}

NphEntity nph_prefab_entity(const NphPrefab *p, u32 index)
{
    NPH_ASSERT(p != NULL);
    const u32 slots = nph_world_entity_slot_count(p->world);
    u32 k = 0U;
    for (u32 i = 0; i < slots; ++i) {
        const NphEntity e = nph_world_entity_at(p->world, i);
        if (nph_entity_is_null(e)) {
            continue;
        }
        if (k == index) {
            return e;
        }
        k += 1U;
    }
    return NPH_ENTITY_NULL;
}

// ---------------------------------------------------------------------------
// Instantiation
// ---------------------------------------------------------------------------

typedef struct Remap {
    const NphWorld *prefab_world;
    const u32 *index_of_slot; // prefab slot -> instance index, NPH_U32_MAX when none
    const NphEntity *created; // instance index -> new entity
} Remap;

static NphEntity remap_entity(void *user, NphEntity e)
{
    const Remap *m = (const Remap *)user;
    if (!nph_world_is_alive(m->prefab_world, e)) {
        return NPH_ENTITY_NULL;
    }
    const u32 index = m->index_of_slot[nph_world_entity_index(m->prefab_world, e)];
    return index != NPH_U32_MAX ? m->created[index] : NPH_ENTITY_NULL;
}

static bool registries_match(const NphWorld *a, const NphWorld *b)
{
    if (b->component_count < a->component_count) {
        return false;
    }
    for (u32 i = 0; i < a->component_count; ++i) {
        if (a->components[i].layout_hash != b->components[i].layout_hash) {
            return false;
        }
    }
    return true;
}

NphResult nph_prefab_instantiate(const NphPrefab *p, NphWorld *w, NphEntity *out_root,
                                 NphEntity *out_entities)
{
    NPH_ASSERT(p != NULL && w != NULL);
    if (out_root != NULL) {
        *out_root = NPH_ENTITY_NULL;
    }
    const NphWorld *pw = p->world;
    if (!registries_match(pw, w)) {
        NPH_LOG_ERROR(&NPH_LOG_SCENE, "prefab '%s': the target world's component registry differs",
                      p->name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 n = nph_world_entity_count(pw);
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    if ((u64)nph_world_entity_count(w) + n > nph_world_entity_capacity(w)) {
        return NPH_RESULT_FULL;
    }
    const u32 slots = nph_world_entity_slot_count(pw);
    NphEntity *source = NPH_ALLOC_ARRAY(w->alloc, NphEntity, n);
    NphEntity *created = NPH_ALLOC_ARRAY_ZEROED(w->alloc, NphEntity, n);
    u32 *index_of_slot = NPH_ALLOC_ARRAY(w->alloc, u32, slots);
    NphResult r = (source != NULL && created != NULL && index_of_slot != NULL)
                      ? NPH_RESULT_OK
                      : NPH_RESULT_OUT_OF_MEMORY;
    NphStrId interned = NPH_STR_ID_NULL;
    if (r == NPH_RESULT_OK) {
        r = nph_str_table_intern(&w->strings, nph_str(p->name, p->name_len), &interned);
    }
    u32 made = 0U;
    if (r == NPH_RESULT_OK) {
        u32 k = 0U;
        for (u32 i = 0; i < slots; ++i) {
            const NphEntity e = nph_world_entity_at(pw, i);
            index_of_slot[i] = nph_entity_is_null(e) ? NPH_U32_MAX : k;
            if (!nph_entity_is_null(e)) {
                source[k++] = e;
            }
        }
        NPH_ASSERT(k == n);
        // One spawn per entity, directly in its final archetype: the
        // prefab entity's components plus PrefabInstance.
        for (u32 i = 0; r == NPH_RESULT_OK && i < n; ++i) {
            NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
            u32 count =
                nph_world_entity_components(pw, source[i], ids, NPH_ECS_MAX_COMPONENTS - 1U);
            bool has_instance = false;
            for (u32 c = 0; c < count; ++c) {
                has_instance = has_instance || ids[c] == NPH_COMPONENT_ID_PREFAB_INSTANCE;
            }
            if (!has_instance) {
                ids[count++] = NPH_COMPONENT_ID_PREFAB_INSTANCE;
            }
            r = nph_world_spawn(w, ids, count, &created[i]);
            made = r == NPH_RESULT_OK ? i + 1U : i;
        }
    }
    if (r == NPH_RESULT_OK) {
        Remap map = {pw, index_of_slot, created};
        for (u32 i = 0; i < n; ++i) {
            const NphEntityRecord *src = nph_world_record_(pw, source[i]);
            NphEntityRecord *dst = nph_world_record_(w, created[i]);
            const NphArchetype *sa = src->chunk->archetype;
            for (u32 col = 0; col < sa->column_count; ++col) {
                const NphComponentId id = sa->column_ids[col];
                if (id == NPH_COMPONENT_ID_HIERARCHY || id == NPH_COMPONENT_ID_PREFAB_INSTANCE) {
                    continue; // links are rebuilt below; the instance record is set below
                }
                void *cell = nph_world_cell_mut_(w, dst, id);
                memcpy(cell, nph_chunk_cell(src->chunk, col, src->row), sa->column_sizes[col]);
                nph_component_remap_entities_(&w->components[id], cell, remap_entity, &map);
            }
            NphPrefabInstance *inst =
                (NphPrefabInstance *)nph_world_cell_mut_(w, dst, NPH_COMPONENT_ID_PREFAB_INSTANCE);
            inst->prefab = interned;
            inst->index = i;
            inst->root = created[0];
        }
        // The hierarchy, children in the prefab's order.
        for (u32 i = 0; r == NPH_RESULT_OK && i < n; ++i) {
            for (NphEntity c = nph_world_first_child(pw, source[i]); !nph_entity_is_null(c);
                 c = nph_world_next_sibling(pw, c)) {
                const u32 ci = index_of_slot[nph_world_entity_index(pw, c)];
                r = nph_world_set_parent(w, created[ci], created[i]);
                if (r != NPH_RESULT_OK) {
                    break;
                }
            }
        }
    }
    if (r != NPH_RESULT_OK) {
        for (u32 i = 0; i < made; ++i) {
            NPH_IGNORE_RESULT(nph_world_despawn(w, created[i]));
        }
    } else {
        if (out_root != NULL) {
            *out_root = created[0];
        }
        if (out_entities != NULL) {
            memcpy(out_entities, created, sizeof(NphEntity) * n);
        }
    }
    if (source != NULL) {
        NPH_FREE_ARRAY(w->alloc, NphEntity, source, n);
    }
    if (created != NULL) {
        NPH_FREE_ARRAY(w->alloc, NphEntity, created, n);
    }
    if (index_of_slot != NULL) {
        NPH_FREE_ARRAY(w->alloc, u32, index_of_slot, slots);
    }
    return r;
}
