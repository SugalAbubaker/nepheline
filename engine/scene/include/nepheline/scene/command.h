// SPDX-License-Identifier: Apache-2.0
#pragma once

// Command buffers: structural changes recorded now and applied later
// (CLAUDE.md §8.7; docs/scene.md "Command buffers").
//
// Systems run in parallel while the world's structure is frozen, so a
// system that spawns, despawns, adds, or removes records the change in a
// command buffer and the schedule applies the buffers at the end of the
// stage, in a deterministic order (schedule.h). Code outside a schedule
// can use them the same way.
//
// An entity spawned through a buffer gets a pending handle at once, valid
// only inside that buffer: later commands of the same buffer may target it,
// and ENTITY fields of component values recorded in the same buffer may
// refer to it. Applying the buffer first creates every entity spawned
// through it (in recording order, each directly in the archetype of the
// adds and sets that immediately follow its spawn), then applies the other
// commands in recording order, replacing pending handles everywhere they
// were recorded. Pending handles never reach the world and are never valid
// in another buffer.
//
// Commands that target an entity that is no longer alive when the buffer
// is applied (it was despawned by an earlier command, or by another
// buffer applied first) are skipped and counted; that is the normal
// outcome of two systems deciding to remove the same thing.
//
// Memory: commands are stored in blocks from the buffer's allocator,
// typically a per-worker scratch arena that is reset every frame (a
// schedule's), so recording allocates nothing from the heap. A failure to
// allocate is sticky: the recording call returns NPH_RESULT_OUT_OF_MEMORY,
// every later call is ignored, and apply reports the error without
// changing the world.
//
// Thread-safety: a buffer belongs to one thread while it records (or to
// one fiber; see schedule.h for buffers that follow a job across workers).
// Applying it is a structural change to the world.

#include <nepheline/scene/world.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphCommandBlock NphCommandBlock;

typedef struct NphCommandBuffer {
    NphWorld *world;
    NphAllocator *alloc;
    NphCommandBlock *first;
    NphCommandBlock *last;
    u32 command_count;
    u32 pending_count; // entities spawned through the buffer
    NphResult error;   // the first failure, sticky
    u32 block_size;    // bytes per block
} NphCommandBuffer;

// Default block size: room for a few hundred small commands.
#define NPH_COMMAND_BLOCK_SIZE 16384U

// Starts an empty buffer for `world` whose blocks come from `alloc`
// (block_size 0: NPH_COMMAND_BLOCK_SIZE; larger commands get their own
// block). Nothing is allocated until the first command.
void nph_commands_init(NphCommandBuffer *cb, NphWorld *world, NphAllocator *alloc, u32 block_size);

// Releases the blocks. Safe on a zero-filled buffer.
void nph_commands_free(NphCommandBuffer *cb);

// Drops every command and the sticky error, releasing the blocks.
void nph_commands_reset(NphCommandBuffer *cb);

NPH_INLINE bool nph_commands_is_empty(const NphCommandBuffer *cb)
{
    return cb->command_count == 0U;
}

// Records a spawn with no components and returns its pending handle, or
// NPH_ENTITY_NULL after an allocation failure (the sticky error says so).
NphEntity nph_commands_spawn(NphCommandBuffer *cb);

// Records a despawn (as nph_world_despawn).
NPH_MUST_USE NphResult nph_commands_despawn(NphCommandBuffer *cb, NphEntity e);

// Records an add (as nph_world_add); the value is copied now (NULL: the
// default). Adding a component the entity already has when the buffer is
// applied overwrites it.
NPH_MUST_USE NphResult nph_commands_add(NphCommandBuffer *cb, NphEntity e, NphComponentId id,
                                        const void *value);

// Records a set (as nph_world_set: adds when missing).
NPH_MUST_USE NphResult nph_commands_set(NphCommandBuffer *cb, NphEntity e, NphComponentId id,
                                        const void *value);

// Records a remove; removing a component the entity does not have is a
// skipped command, not an error.
NPH_MUST_USE NphResult nph_commands_remove(NphCommandBuffer *cb, NphEntity e, NphComponentId id);

// Records a reparenting (as nph_world_set_parent, transform.h).
NPH_MUST_USE NphResult nph_commands_set_parent(NphCommandBuffer *cb, NphEntity child,
                                               NphEntity parent);

typedef struct NphCommandStats {
    u32 applied;
    u32 skipped; // targets that were not alive, removes of absent components
} NphCommandStats;

// Applies the commands in recording order and resets the buffer. Returns
// the sticky recording error without applying anything, or the first
// failure of the world (OUT_OF_MEMORY, FULL); commands before a failure
// stay applied, so a failure here is a resource-exhaustion error the caller
// treats as fatal for the frame. `out_stats` may be NULL.
NPH_MUST_USE NphResult nph_commands_apply(NphCommandBuffer *cb, NphCommandStats *out_stats);

// True for a handle returned by nph_commands_spawn (any buffer).
bool nph_commands_is_pending(const NphWorld *world, NphEntity e);

#ifdef __cplusplus
}
#endif
