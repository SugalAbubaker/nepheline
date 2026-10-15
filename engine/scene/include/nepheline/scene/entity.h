// SPDX-License-Identifier: Apache-2.0
#pragma once

// Entity and component identifiers (CLAUDE.md §8.7 "generational 32-bit
// entity IDs"; docs/scene.md; docs/adr/0020-ecs.md).
//
// An entity is a 32-bit generational handle issued by its world: the low
// bits are a slot index, the high bits a generation that the world bumps
// when the entity is despawned, so a kept handle to a despawned entity is
// detected as stale instead of reaching whatever reuses the slot (§2
// "Handles over pointers"). The split between the two is chosen per world
// from its capacity (world.h). Zero is never a live entity, so a zero-filled
// component holds "no entity".
//
// Thread-safety: these are values; every function here is pure and
// NPH_THREADSAFE.

#include <nepheline/core/base.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphEntity {
    u32 value;
} NphEntity;

#define NPH_ENTITY_NULL ((NphEntity){0U})

NPH_INLINE bool nph_entity_is_null(NphEntity e)
{
    return e.value == 0U;
}

NPH_INLINE bool nph_entity_equals(NphEntity a, NphEntity b)
{
    return a.value == b.value;
}

// A component type's index in its world's registry, in [0,
// NPH_ECS_MAX_COMPONENTS). Ids are dense and assigned in registration
// order, so two worlds that register the same components in the same order
// (or copy one registry, world.h) agree on every id.
typedef u32 NphComponentId;

#define NPH_ECS_MAX_COMPONENTS 256U
#define NPH_COMPONENT_NONE     NPH_U32_MAX

#ifdef __cplusplus
}
#endif
