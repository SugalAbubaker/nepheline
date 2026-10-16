// SPDX-License-Identifier: Apache-2.0
#pragma once

// The ECS world (CLAUDE.md §8.7; docs/scene.md; docs/adr/0020-ecs.md):
// entities, the component registry, and archetype storage.
//
// Storage is archetype-based. Every distinct set of components an entity
// can have is an archetype, and an archetype stores its entities in 16 KiB
// chunks, structure-of-arrays: one column per component, each a packed
// array, so a system that touches two components of a million entities
// streams two arrays and nothing else (§2 "Data-oriented design"; the
// layout of Unity's DOTS and of flecs, Mertens, "Building an ECS", 2019).
// Chunks of an archetype are kept dense: despawning or moving an entity
// fills its row with the archetype's last entity, so every chunk but the
// last is full and iteration never skips holes.
//
// Adding or removing a component moves the entity to another archetype
// (copying its other components across). These are structural changes:
// they are made on one thread, never while a schedule stage or a query
// iteration is running on the world (asserted). Jobs record structural
// changes in command buffers (command.h) instead.
//
// Change detection: every chunk keeps a version per column, the world tick
// at which the column was last written through a write query or a write
// function here. Queries can skip chunks whose columns have not changed
// since a given tick (query.h), which is what keeps static content off the
// per-frame budget (§2 "cost scales with ... changed content").
//
// Costs the caller can see (§1.4): nph_world_create allocates the entity
// table for `max_entities` up front. Chunks are carved from slabs that the
// world allocates on demand (spawning into a full archetype may allocate a
// slab) and keeps until it is destroyed or trimmed; registering components
// and creating archetypes allocate their bookkeeping. Nothing else
// allocates.
//
// Thread-safety: a world belongs to one thread for every function that
// changes it. Concurrent reads (nph_world_get, nph_world_is_alive, query
// iteration) are safe while no thread changes the world's structure; a
// schedule (schedule.h) relies on exactly that.

#include <nepheline/core/alloc.h>
#include <nepheline/core/log.h>
#include <nepheline/core/result.h>
#include <nepheline/scene/component.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_SCENE);

typedef struct NphWorld NphWorld;

// Size of every chunk.
#define NPH_ECS_CHUNK_SIZE 16384U

// Entity capacity limits. The entity handle's index takes
// ceil(log2(max_entities)) bits (at least 10); the rest of its 32 bits are
// the generation.
#define NPH_ECS_DEFAULT_MAX_ENTITIES 65536U
#define NPH_ECS_MAX_ENTITIES         (1U << 24U)

// Built-in components, registered by every world in this order before any
// other, so their ids are constants (transform.h, prefab.h).
#define NPH_COMPONENT_ID_NAME            0U
#define NPH_COMPONENT_ID_LOCAL_TRANSFORM 1U
#define NPH_COMPONENT_ID_WORLD_TRANSFORM 2U
#define NPH_COMPONENT_ID_HIERARCHY       3U
#define NPH_COMPONENT_ID_PREFAB_INSTANCE 4U
#define NPH_COMPONENT_BUILTIN_COUNT      5U

typedef struct NphWorldDesc {
    NphAllocator *alloc; // required: the entity table, bookkeeping, and chunk slabs
    u32 max_entities;    // 0: NPH_ECS_DEFAULT_MAX_ENTITIES; at most NPH_ECS_MAX_ENTITIES
    u32 chunks_per_slab; // 0: 64 (a 1 MiB slab)
    u32 max_chunks;      // 0: no limit; otherwise spawning beyond it fails with OUT_OF_MEMORY
    u32 event_capacity;  // 0: no event stream; otherwise the stream's fixed capacity
    // Copies this world's component registry (same ids, same descriptors)
    // so that entities can move between the two worlds: prefabs, the
    // editor's play-in-editor snapshot. NULL registers only the built-ins.
    const NphWorld *registry_from;
} NphWorldDesc;

// Creates a world and registers the built-in components.
// NPH_RESULT_INVALID_ARGUMENT for a NULL allocator or an out-of-range
// capacity, NPH_RESULT_OUT_OF_MEMORY when the tables cannot be allocated.
// Thread-safety: NPH_THREADSAFE (the new world belongs to the caller).
NPH_MUST_USE NphResult nph_world_create(const NphWorldDesc *desc, NphWorld **out);

// Releases everything. Queries created on the world must be destroyed
// first (asserted). Safe on NULL.
void nph_world_destroy(NphWorld *world);

// Removes every entity, keeping the registry, the queries, and the chunk
// memory for reuse. Entity handles from before are stale afterwards.
void nph_world_clear(NphWorld *world);

// Makes the empty world `dst` issue `src`'s entity handles: every handle
// alive in `src` is reserved in `dst` (not alive until a scene load places
// it, scene_io.h NphSceneLoadDesc.handles), and every other slot keeps its
// generation, so a handle stale in `src` is stale in `dst` too. With a
// scene saved from `src` and loaded into `dst` this way, `dst` is a copy
// whose entities have the original handles: hot reload migrates a world
// to new component layouts like this, and play-in-editor copies the
// edited world, so references held outside the world (a selection, the
// undo history, game state) stay valid. NPH_RESULT_INVALID_ARGUMENT when
// `dst` holds or has reserved entities or the capacities differ.
NPH_MUST_USE NphResult nph_world_adopt_handles(NphWorld *dst, const NphWorld *src);

// Returns the chunk slabs that hold no live chunk to the allocator.
void nph_world_trim(NphWorld *world);

// ---------------------------------------------------------------------------
// Component registry
// ---------------------------------------------------------------------------

// Registers a component type and returns its id. The descriptor must stay
// valid for the world's lifetime (the generated descriptors are statics).
// Registering a descriptor whose name is already registered returns that
// id when the layouts match (idempotent registration from several
// modules) and NPH_RESULT_ALREADY_EXISTS when they differ.
// NPH_RESULT_INVALID_ARGUMENT for a descriptor that fails
// nph_component_desc_validate, NPH_RESULT_FULL at NPH_ECS_MAX_COMPONENTS.
NPH_MUST_USE NphResult nph_world_register_component(NphWorld *world, const NphComponentDesc *desc,
                                                    NphComponentId *out);

// The id of the component named `name`, or NPH_COMPONENT_NONE.
NphComponentId nph_world_find_component(const NphWorld *world, NphStr name);

// The descriptor of `id`, or NULL for an id that is not registered.
const NphComponentDesc *nph_world_component_desc(const NphWorld *world, NphComponentId id);

u32 nph_world_component_count(const NphWorld *world);

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

// Creates an entity with the given components (in any order; duplicates
// are ignored), each set to its default value. NPH_RESULT_FULL at the
// entity capacity, NPH_RESULT_OUT_OF_MEMORY when a chunk cannot be
// allocated, NPH_RESULT_INVALID_ARGUMENT for an unregistered id or an
// archetype that does not fit a chunk.
NPH_MUST_USE NphResult nph_world_spawn(NphWorld *world, const NphComponentId *ids, u32 id_count,
                                       NphEntity *out);

// Creates `entity_count` entities with the same components at once:
// archetype resolution happens once and the rows are filled chunk by chunk,
// the fast path for loading and for benchmarks. `out` (may be NULL)
// receives the handles. All or nothing: on failure no entity is created.
NPH_MUST_USE NphResult nph_world_spawn_batch(NphWorld *world, const NphComponentId *ids,
                                             u32 id_count, u32 entity_count, NphEntity *out);

// Destroys `e`. Its children (transform.h) are detached and become roots;
// it is detached from its parent. NPH_RESULT_NOT_FOUND for a stale or null
// handle.
NPH_MUST_USE NphResult nph_world_despawn(NphWorld *world, NphEntity e);

bool nph_world_is_alive(const NphWorld *world, NphEntity e);

u32 nph_world_entity_count(const NphWorld *world);

// The largest entity count this world can hold.
u32 nph_world_entity_capacity(const NphWorld *world);

// The slot index of a live entity, in [0, capacity): for per-entity side
// tables. Asserts that `e` is alive.
u32 nph_world_entity_index(const NphWorld *world, NphEntity e);

// The live entity occupying slot `index`, or NPH_ENTITY_NULL. With
// nph_world_entity_slot_count this walks every entity in slot order, the
// order the serializer and the state hash use.
NphEntity nph_world_entity_at(const NphWorld *world, u32 index);

// One past the highest slot index ever used.
u32 nph_world_entity_slot_count(const NphWorld *world);

// ---------------------------------------------------------------------------
// Components of one entity
// ---------------------------------------------------------------------------

// Adds component `id` to `e`, initialised from `value` (NULL: the
// default). NPH_RESULT_ALREADY_EXISTS when `e` has it, NPH_RESULT_NOT_FOUND
// for a dead entity, NPH_RESULT_INVALID_ARGUMENT for an unregistered id or
// an INTERNAL component with a value (those are added by their owner).
NPH_MUST_USE NphResult nph_world_add(NphWorld *world, NphEntity e, NphComponentId id,
                                     const void *value);

// Removes component `id`. NPH_RESULT_NOT_FOUND when `e` is dead or does not
// have it.
NPH_MUST_USE NphResult nph_world_remove(NphWorld *world, NphEntity e, NphComponentId id);

// Overwrites component `id` of `e` with `value`, adding it when missing.
// Not structural when the component is present. NPH_RESULT_NOT_FOUND for a
// dead entity.
NPH_MUST_USE NphResult nph_world_set(NphWorld *world, NphEntity e, NphComponentId id,
                                     const void *value);

bool nph_world_has(const NphWorld *world, NphEntity e, NphComponentId id);

// The component's data, or NULL when `e` is dead, lacks it, or it is a
// tag. Valid until the next structural change. Concurrent calls are safe.
const void *nph_world_get(const NphWorld *world, NphEntity e, NphComponentId id);

// As nph_world_get, for writing: the column's change version is stamped
// with the world's change tick (and a SET event recorded), so change
// queries see the write.
void *nph_world_get_mut(NphWorld *world, NphEntity e, NphComponentId id);

// Writes the ids of `e`'s components, in ascending order, to `out` (up to
// `capacity`), and returns how many it has (0 for a dead entity).
u32 nph_world_entity_components(const NphWorld *world, NphEntity e, NphComponentId *out,
                                u32 capacity);

#define NPH_WORLD_GET(world, e, id, T)     ((const T *)nph_world_get((world), (e), (id)))
#define NPH_WORLD_GET_MUT(world, e, id, T) ((T *)nph_world_get_mut((world), (e), (id)))

// ---------------------------------------------------------------------------
// Change ticks
// ---------------------------------------------------------------------------

// The tick of the most recent system run (schedule.h). Writes made through
// this header are stamped one past it, so the next system run sees them
// as changed. Ticks compare with wrap-around (nph_world_tick_newer); a
// schedule clamps versions older than 2^30 ticks so that the comparison
// never flips (docs/scene.md "Change detection").
u32 nph_world_tick(const NphWorld *world);

// Advances the tick and returns the new value: what a schedule does before
// each system run. Callers that run systems by hand do the same.
u32 nph_world_advance_tick(NphWorld *world);

// True when tick `a` is later than `b` (wrap-around comparison).
NPH_INLINE bool nph_world_tick_newer(u32 a, u32 b)
{
    return (i32)(a - b) > 0;
}

// Raises every column version and every tick in `ticks` that is older than
// world tick - 2^30 to that value. A schedule calls it for its systems;
// code that keeps its own `since` ticks across very long runs calls it
// with them.
void nph_world_clamp_ticks(NphWorld *world, u32 *ticks, u32 tick_count);

// ---------------------------------------------------------------------------
// Event stream (for the editor and, later, networking)
// ---------------------------------------------------------------------------

typedef enum NphWorldEventType {
    NPH_WORLD_EVENT_SPAWNED = 0,
    NPH_WORLD_EVENT_DESPAWNED,
    NPH_WORLD_EVENT_ADDED,   // component added
    NPH_WORLD_EVENT_REMOVED, // component removed
    NPH_WORLD_EVENT_SET,     // component written through nph_world_set or get_mut
    NPH_WORLD_EVENT_PARENT_CHANGED,
} NphWorldEventType;

typedef struct NphWorldEvent {
    u32 type; // NphWorldEventType
    NphEntity entity;
    NphComponentId component; // NPH_COMPONENT_NONE for entity events
    u32 tick;
} NphWorldEvent;

// The events recorded since the last clear, oldest first. With a zero
// event_capacity the stream is off and nothing is recorded. When the
// stream is full further events are dropped and counted.
const NphWorldEvent *nph_world_events(const NphWorld *world, u32 *out_count, u32 *out_dropped);
void nph_world_clear_events(NphWorld *world);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

typedef struct NphWorldStats {
    u32 entities;
    u32 archetypes;
    u32 chunks;      // in use
    u32 free_chunks; // carved, unused
    u32 slabs;       // allocated
    u64 slab_bytes;  // memory held for chunks
    u32 queries;
    u32 components;
} NphWorldStats;

void nph_world_get_stats(const NphWorld *world, NphWorldStats *out);

// A hash of the world's contents: every live entity in slot order, its
// handle, and the field values of each of its components in id order.
// Equal hashes mean equal worlds; the determinism tests compare them
// across runs and worker counts (§9 "Determinism tests").
u64 nph_world_hash(const NphWorld *world);

// The world's string table, used to write NPH_FIELD_STR_ID fields as text.
// Strings interned here survive until the world is destroyed.
NphStrTable *nph_world_strings(NphWorld *world);

#ifdef __cplusplus
}
#endif
