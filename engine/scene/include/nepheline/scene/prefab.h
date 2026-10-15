// SPDX-License-Identifier: Apache-2.0
#pragma once

// Prefabs with overrides (CLAUDE.md §8.7 "Prefabs with overrides";
// docs/scene.md "Prefabs").
//
// A prefab is a template: a small world of entities (a hierarchy, usually)
// that is instantiated into other worlds. Every instantiated entity carries
// a PrefabInstance component naming the prefab, the entity of the prefab it
// came from, and the root of its instance. When a scene is saved, an
// instance entity is written as "this entity of that prefab, plus these
// differences" (field overrides, added and removed components), so a
// change to the prefab reaches every instance on the next load, except
// where the instance overrode it.
//
// ENTITY fields that point inside the prefab point inside the instance
// after instantiation, and are written relative to the instance on save,
// so references between the parts of a prefab survive both.
//
// Prefabs are flat: a prefab does not contain instances of other prefabs
// (instantiating one into a prefab's world records nothing that the
// prefab's own save understands). Nested prefabs are a Phase 9 editor
// feature (docs/adr/0021-scene-serialization.md).
//
// Thread-safety: a prefab is immutable once loaded and may be instantiated
// into different worlds from different threads; instantiation is a change
// to the target world.

#include <nepheline/scene/world.h>

#ifdef __cplusplus
extern "C" {
#endif

// The built-in component on every entity created by nph_prefab_instantiate.
#define NPH_PREFAB_INSTANCE_FIELDS(F)                                                              \
    F(STR_ID, prefab, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))                                    \
    F(U32, index, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))                                        \
    F(ENTITY, root, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))
NPH_COMPONENT_STRUCT(NphPrefabInstance, NPH_PREFAB_INSTANCE_FIELDS);
NPH_COMPONENT_DECLARE(NphPrefabInstance);

typedef struct NphPrefab NphPrefab;

// Creates an empty prefab named `name` whose world copies the component
// registry of `registry` (the worlds it will be instantiated into must
// register the same components in the same order, or copy the same
// registry). `max_entities` 0 means 4096. The name is how scene files
// refer to the prefab (scene_io.h).
NPH_MUST_USE NphResult nph_prefab_create(NphStr name, const NphWorld *registry, NphAllocator *alloc,
                                         u32 max_entities, NphPrefab **out);

void nph_prefab_destroy(NphPrefab *prefab);

// The prefab's world: build or load its entities here (nph_world_spawn,
// nph_scene_load_text into it, ...) before instantiating. Instance order is
// the world's slot order, so index 0 is the first entity created, which by
// convention is the root of the prefab's hierarchy.
NphWorld *nph_prefab_world(NphPrefab *prefab);
const NphWorld *nph_prefab_world_const(const NphPrefab *prefab);

NphStr nph_prefab_name(const NphPrefab *prefab);
NphStrId nph_prefab_id(const NphPrefab *prefab);

// Number of entities in the prefab (instance indices are 0 .. count-1).
u32 nph_prefab_entity_count(const NphPrefab *prefab);

// The prefab entity with instance index `index`, or NPH_ENTITY_NULL.
NphEntity nph_prefab_entity(const NphPrefab *prefab, u32 index);

// Instantiates the prefab into `world`: one entity per prefab entity with
// the same components and values (ENTITY fields that point inside the
// prefab are redirected to the new entities, others are cleared), the same
// hierarchy, and a PrefabInstance component. `out_root` (may be NULL)
// receives the instance's entity for index 0; `out_entities` (may be NULL)
// receives all of them, in index order. The prefab's name is interned in
// the world's string table. All or nothing.
NPH_MUST_USE NphResult nph_prefab_instantiate(const NphPrefab *prefab, NphWorld *world,
                                              NphEntity *out_root, NphEntity *out_entities);

// Finds the prefab named by a PrefabInstance (scene loading resolves
// instances through this).
typedef const NphPrefab *(*NphPrefabResolveFn)(void *user, NphStrId prefab);

#ifdef __cplusplus
}
#endif
