// SPDX-License-Identifier: Apache-2.0
// Command buffers: recorded structural changes, applied later in recording
// order (command.h; docs/scene.md "Command buffers").

#include <nepheline/core/assert.h>
#include <nepheline/scene/command.h>
#include <nepheline/scene/transform.h>

#include "scene_internal.h"

#include <string.h>

// A block header; `cap` bytes of commands follow it.
struct NphCommandBlock {
    NphCommandBlock *next;
    u32 used;
    u32 cap;
};

static u8 *block_data(NphCommandBlock *b)
{
    return (u8 *)(b + 1);
}

static const u8 *block_cdata(const NphCommandBlock *b)
{
    return (const u8 *)(b + 1);
}

typedef enum CmdType {
    CMD_SPAWN = 1,
    CMD_DESPAWN,
    CMD_ADD,
    CMD_SET,
    CMD_REMOVE,
    CMD_SET_PARENT,
} CmdType;

// Every command starts with this header; a component value (value_size
// bytes, 0 for "the default") follows. Commands are 8-byte aligned.
typedef struct CmdHeader {
    u32 type;
    u32 size; // header + value, rounded up to 8
    NphEntity a;
    NphEntity b;
    NphComponentId id;
    u32 value_size;
} CmdHeader;

#define CMD_ALIGN 8U

void nph_commands_init(NphCommandBuffer *cb, NphWorld *world, NphAllocator *alloc, u32 block_size)
{
    NPH_ASSERT(cb != NULL && world != NULL && alloc != NULL);
    memset(cb, 0, sizeof *cb);
    cb->world = world;
    cb->alloc = alloc;
    cb->block_size = block_size == 0U ? NPH_COMMAND_BLOCK_SIZE : block_size;
    cb->error = NPH_RESULT_OK;
}

static void free_blocks(NphCommandBuffer *cb)
{
    NphCommandBlock *b = cb->first;
    while (b != NULL) {
        NphCommandBlock *next = b->next;
        NPH_FREE(cb->alloc, b, sizeof(NphCommandBlock) + (u64)b->cap, 16U);
        b = next;
    }
    cb->first = NULL;
    cb->last = NULL;
}

void nph_commands_free(NphCommandBuffer *cb)
{
    NPH_ASSERT(cb != NULL);
    if (cb->alloc != NULL) {
        free_blocks(cb);
    }
    cb->command_count = 0U;
    cb->pending_count = 0U;
    cb->error = NPH_RESULT_OK;
}

void nph_commands_reset(NphCommandBuffer *cb)
{
    nph_commands_free(cb);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

static u32 index_bits(const NphWorld *w)
{
    return w->handles.index_bits;
}

bool nph_commands_is_pending(const NphWorld *w, NphEntity e)
{
    NPH_ASSERT(w != NULL);
    return e.value != 0U && (e.value >> index_bits(w)) == 0U;
}

// Appends a command; false (and the sticky error set) when out of memory.
static bool push(NphCommandBuffer *cb, CmdType type, NphEntity a, NphEntity b, NphComponentId id,
                 const void *value, u32 value_size)
{
    if (cb->error != NPH_RESULT_OK) {
        return false;
    }
    const u32 total = nph_align_up_u32((u32)sizeof(CmdHeader) + value_size, CMD_ALIGN);
    NphCommandBlock *block = cb->last;
    if (block == NULL || block->cap - block->used < total) {
        const u32 cap = nph_max_u32(cb->block_size, total);
        block = (NphCommandBlock *)NPH_ALLOC(cb->alloc, sizeof(NphCommandBlock) + (u64)cap, 16U);
        if (block == NULL) {
            cb->error = NPH_RESULT_OUT_OF_MEMORY;
            return false;
        }
        block->next = NULL;
        block->used = 0U;
        block->cap = cap;
        if (cb->last != NULL) {
            cb->last->next = block;
        } else {
            cb->first = block;
        }
        cb->last = block;
    }
    const CmdHeader h = {(u32)type, total, a, b, id, value_size};
    u8 *dst = block_data(block) + block->used;
    memcpy(dst, &h, sizeof h);
    if (value_size > 0U) {
        memcpy(dst + sizeof h, value, value_size);
    }
    block->used += total;
    cb->command_count += 1U;
    return true;
}

static NphResult sticky(const NphCommandBuffer *cb)
{
    return cb->error != NPH_RESULT_OK ? cb->error : NPH_RESULT_OUT_OF_MEMORY;
}

// A target is a live-looking handle: a world entity, or a pending handle
// this buffer issued.
static bool valid_target(const NphCommandBuffer *cb, NphEntity e)
{
    if (e.value == 0U) {
        return false;
    }
    if (nph_commands_is_pending(cb->world, e)) {
        return e.value <= cb->pending_count;
    }
    return true;
}

NphEntity nph_commands_spawn(NphCommandBuffer *cb)
{
    NPH_ASSERT(cb != NULL);
    if (cb->error != NPH_RESULT_OK) {
        return NPH_ENTITY_NULL;
    }
    // Pending handles are the local index with a zero generation, which no
    // live entity has; the index must fit the world's index bits.
    const u32 local = cb->pending_count + 1U;
    if ((local >> index_bits(cb->world)) != 0U) {
        cb->error = NPH_RESULT_FULL;
        return NPH_ENTITY_NULL;
    }
    const NphEntity e = {local};
    if (!push(cb, CMD_SPAWN, e, NPH_ENTITY_NULL, NPH_COMPONENT_NONE, NULL, 0U)) {
        return NPH_ENTITY_NULL;
    }
    cb->pending_count = local;
    return e;
}

NphResult nph_commands_despawn(NphCommandBuffer *cb, NphEntity e)
{
    NPH_ASSERT(cb != NULL);
    if (!valid_target(cb, e)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return push(cb, CMD_DESPAWN, e, NPH_ENTITY_NULL, NPH_COMPONENT_NONE, NULL, 0U) ? NPH_RESULT_OK
                                                                                   : sticky(cb);
}

static NphResult record_value(NphCommandBuffer *cb, CmdType type, NphEntity e, NphComponentId id,
                              const void *value)
{
    if (!valid_target(cb, e)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphComponentInfo *info = nph_world_info_(cb->world, id);
    if (info == NULL || (info->desc->flags & NPH_COMPONENT_FLAG_INTERNAL) != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (type == CMD_SET && value == NULL && info->desc->size > 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 size = value != NULL ? info->desc->size : 0U;
    return push(cb, type, e, NPH_ENTITY_NULL, id, value, size) ? NPH_RESULT_OK : sticky(cb);
}

NphResult nph_commands_add(NphCommandBuffer *cb, NphEntity e, NphComponentId id, const void *value)
{
    NPH_ASSERT(cb != NULL);
    return record_value(cb, CMD_ADD, e, id, value);
}

NphResult nph_commands_set(NphCommandBuffer *cb, NphEntity e, NphComponentId id, const void *value)
{
    NPH_ASSERT(cb != NULL);
    return record_value(cb, CMD_SET, e, id, value);
}

NphResult nph_commands_remove(NphCommandBuffer *cb, NphEntity e, NphComponentId id)
{
    NPH_ASSERT(cb != NULL);
    if (!valid_target(cb, e) || id >= cb->world->component_count) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return push(cb, CMD_REMOVE, e, NPH_ENTITY_NULL, id, NULL, 0U) ? NPH_RESULT_OK : sticky(cb);
}

NphResult nph_commands_set_parent(NphCommandBuffer *cb, NphEntity child, NphEntity parent)
{
    NPH_ASSERT(cb != NULL);
    if (!valid_target(cb, child) || (parent.value != 0U && !valid_target(cb, parent))) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return push(cb, CMD_SET_PARENT, child, parent, NPH_COMPONENT_NONE, NULL, 0U) ? NPH_RESULT_OK
                                                                                 : sticky(cb);
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

typedef struct Cursor {
    const NphCommandBlock *block;
    u32 offset;
} Cursor;

// Reads the command at the cursor; false at the end.
static bool cursor_peek(const Cursor *c, CmdHeader *out, const u8 **out_value)
{
    const NphCommandBlock *b = c->block;
    u32 off = c->offset;
    while (b != NULL && off >= b->used) {
        b = b->next;
        off = 0U;
    }
    if (b == NULL) {
        return false;
    }
    memcpy(out, block_cdata(b) + off, sizeof *out);
    *out_value = block_cdata(b) + off + sizeof *out;
    return true;
}

static void cursor_advance(Cursor *c)
{
    while (c->block != NULL && c->offset >= c->block->used) {
        c->block = c->block->next;
        c->offset = 0U;
    }
    if (c->block != NULL) {
        CmdHeader h;
        memcpy(&h, block_cdata(c->block) + c->offset, sizeof h);
        c->offset += h.size;
    }
}

typedef struct Applier {
    NphCommandBuffer *cb;
    NphWorld *world;
    NphEntity *map; // pending local index -> spawned entity
} Applier;

static NphEntity resolve(void *user, NphEntity e)
{
    const Applier *ap = (const Applier *)user;
    if (nph_commands_is_pending(ap->world, e) && e.value <= ap->cb->pending_count) {
        return ap->map[e.value];
    }
    return e;
}

// The component set of a pending entity: the ADD and SET commands that
// directly follow its spawn, so the entity is created in its final
// archetype instead of moving through one archetype per component.
static u32 gather_spawn_components(const Cursor *at, NphEntity pending, NphComponentId *ids)
{
    Cursor c = *at;
    cursor_advance(&c); // past the spawn
    u32 n = 0U;
    CmdHeader h;
    const u8 *value;
    while (cursor_peek(&c, &h, &value) && (h.type == CMD_ADD || h.type == CMD_SET) &&
           h.a.value == pending.value) {
        bool seen = false;
        for (u32 i = 0; i < n; ++i) {
            seen = seen || ids[i] == h.id;
        }
        if (!seen && n < NPH_ECS_MAX_COMPONENTS) {
            ids[n++] = h.id;
        }
        cursor_advance(&c);
    }
    return n;
}

// Value bytes with pending handles in ENTITY fields resolved.
typedef union ValueBuffer {
    u8 bytes[NPH_ECS_MAX_COMPONENT_SIZE];
    u64 align_u64;
    f64 align_f64;
} ValueBuffer;

static NphResult apply_value(Applier *ap, const CmdHeader *h, const u8 *value, NphCommandStats *st)
{
    NphWorld *w = ap->world;
    const NphEntity e = resolve(ap, h->a);
    if (!nph_world_is_alive(w, e)) {
        st->skipped += 1U;
        return NPH_RESULT_OK;
    }
    const NphComponentInfo *info = nph_world_info_(w, h->id);
    const void *data = NULL;
    ValueBuffer buffer;
    if (h->value_size > 0U) {
        if (info->entity_field_count > 0U) {
            memcpy(buffer.bytes, value, h->value_size);
            nph_component_remap_entities_(info, buffer.bytes, resolve, ap);
            data = buffer.bytes;
        } else {
            data = value;
        }
    }
    NphResult r;
    if (!nph_world_has(w, e, h->id)) {
        r = nph_world_add(w, e, h->id, data);
    } else if (data != NULL) {
        r = nph_world_set(w, e, h->id, data);
    } else {
        r = NPH_RESULT_OK; // an ADD of the default to a present component, or a tag
    }
    if (r == NPH_RESULT_OK) {
        st->applied += 1U;
    }
    return r;
}

static NphResult apply_spawn(Applier *ap, const Cursor *at, const CmdHeader *h, NphCommandStats *st)
{
    NphComponentId ids[NPH_ECS_MAX_COMPONENTS];
    const u32 n = gather_spawn_components(at, h->a, ids);
    NphEntity e;
    NPH_TRY(nph_world_spawn(ap->world, ids, n, &e));
    ap->map[h->a.value] = e;
    st->applied += 1U;
    return NPH_RESULT_OK;
}

static NphResult apply_one(Applier *ap, const CmdHeader *h, const u8 *value, NphCommandStats *st)
{
    NphWorld *w = ap->world;
    switch ((CmdType)h->type) {
    case CMD_SPAWN:
        return NPH_RESULT_OK; // the first pass created it
    case CMD_DESPAWN: {
        const NphEntity e = resolve(ap, h->a);
        if (nph_world_despawn(w, e) == NPH_RESULT_OK) {
            st->applied += 1U;
        } else {
            st->skipped += 1U;
        }
        return NPH_RESULT_OK;
    }
    case CMD_ADD:
    case CMD_SET:
        return apply_value(ap, h, value, st);
    case CMD_REMOVE: {
        const NphEntity e = resolve(ap, h->a);
        if (nph_world_has(w, e, h->id)) {
            NPH_TRY(nph_world_remove(w, e, h->id));
            st->applied += 1U;
        } else {
            st->skipped += 1U;
        }
        return NPH_RESULT_OK;
    }
    case CMD_SET_PARENT: {
        const NphEntity child = resolve(ap, h->a);
        const NphEntity parent = resolve(ap, h->b);
        if (!nph_world_is_alive(w, child) ||
            (!nph_entity_is_null(h->b) && !nph_world_is_alive(w, parent))) {
            st->skipped += 1U;
            return NPH_RESULT_OK;
        }
        const NphResult r = nph_world_set_parent(w, child, parent);
        if (r == NPH_RESULT_OK) {
            st->applied += 1U;
            return NPH_RESULT_OK;
        }
        if (r == NPH_RESULT_INVALID_ARGUMENT || r == NPH_RESULT_OVERFLOW) {
            // A cycle or an over-deep tree created by commands from two
            // systems: the later command loses.
            NPH_LOG_WARN(&NPH_LOG_SCENE, "a recorded reparenting was skipped: %s",
                         nph_result_string(r));
            st->skipped += 1U;
            return NPH_RESULT_OK;
        }
        return r;
    }
    }
    NPH_UNREACHABLE();
    return NPH_RESULT_CORRUPT;
}

NphResult nph_commands_apply(NphCommandBuffer *cb, NphCommandStats *out_stats)
{
    NPH_ASSERT(cb != NULL);
    NphCommandStats st = {0U, 0U};
    if (out_stats != NULL) {
        *out_stats = st;
    }
    if (cb->error != NPH_RESULT_OK) {
        const NphResult r = cb->error;
        nph_commands_reset(cb);
        return r;
    }
    if (cb->command_count == 0U) {
        return NPH_RESULT_OK;
    }
    Applier ap = {cb, cb->world, NULL};
    const u32 map_len = cb->pending_count + 1U;
    if (cb->pending_count > 0U) {
        ap.map = NPH_ALLOC_ARRAY_ZEROED(cb->alloc, NphEntity, map_len);
        if (ap.map == NULL) {
            nph_commands_reset(cb);
            return NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    NphResult r = NPH_RESULT_OK;
    CmdHeader h;
    const u8 *value;
    // Pass 1: create every entity spawned through the buffer, in order, so
    // that a value recorded anywhere in it can refer to any of them.
    Cursor c = {cb->first, 0U};
    while (r == NPH_RESULT_OK && cb->pending_count > 0U && cursor_peek(&c, &h, &value)) {
        if (h.type == CMD_SPAWN) {
            r = apply_spawn(&ap, &c, &h, &st);
        }
        cursor_advance(&c);
    }
    // Pass 2: everything else, in recording order.
    c.block = cb->first;
    c.offset = 0U;
    while (r == NPH_RESULT_OK && cursor_peek(&c, &h, &value)) {
        r = apply_one(&ap, &h, value, &st);
        cursor_advance(&c);
    }
    if (ap.map != NULL) {
        NPH_FREE_ARRAY(cb->alloc, NphEntity, ap.map, map_len);
    }
    nph_commands_reset(cb);
    if (out_stats != NULL) {
        *out_stats = st;
    }
    return r;
}
