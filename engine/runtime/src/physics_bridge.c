// SPDX-License-Identifier: Apache-2.0
// The physics bridge: the active world's RigidBody and Collider components
// as the bodies of a physics world while the world plays, stepped by the
// simulation's physics system, which writes the bodies' motion back into
// their entities. See components.h "Physics", docs/runtime.md "Physics",
// and docs/adr/0050-physics-in-the-engine.md.
//
// Finding what the game changed: every record keeps the components its body
// was made from or last wrote back. A step looks at the chunks whose
// RigidBody, Collider, or LocalTransform columns were written since the
// system's previous run (scene/query.h change filters; the bridge's own
// writes carry its own run's tick, so it never sees them), and compares
// each entity there with its record: only what differs is applied. The
// world's event stream says what went (despawns, removed components), and
// is read up to where the bridge left it, at every step and at the frame's
// sync point before the engine clears it.
//
// Writing back: the step's move events name the bodies that moved; each
// one's chunk and row come from the world (nph_world_entity_chunk), so a
// step touches the chunks of moving bodies only, through a query view that
// stamps them with the run's tick (no per-entity events: a pile of a
// thousand boxes would overflow the stream every frame).
//
// Compounds (ADR-0108): a Collider without a RigidBody under an entity with
// one, no closer one between them, is a part: a shape of that entity's body
// rather than a body of its own, placed by the LocalTransforms from the
// body's entity down to its own (exact inputs, so a moving body's parts read
// the same every step). Each body's parts are a list through their entity
// slots, which outlives the body's record when the body is made again.
//
// Collision assets (ADR-0108): an ASSET Collider's shapes are its asset's
// hulls and meshes, the asset held while playing and its blocks, checked
// once by the physics module's validators, used in place; at another scale
// (or turned), a copy built from the moved points, shared by every shape of
// the same block made the same way and freed with the last. Assets load
// before the first step at play begin, and in the background during play,
// a body waiting for its shapes.
//
// Triggers (ADR-0108): a trigger Collider's shapes are sensors. After each
// step the sensors' events become Collider-level ones: per pair of trigger
// and visitor entities the overlapping shapes are counted, and a pair enters
// when its count leaves zero over the step and leaves when it returns.
//
// Physics materials (ADR-0108): a Collider naming one makes its shapes with
// the material's values over its own. The materials load as collision assets
// do (before the first step at play begin, in the background during play, the
// shapes waiting), each read once and let go; one that cannot be read leaves
// the Colliders naming it their own values. A material cooked again during
// play loads again, and the shapes naming it take its new values in place.
//
// Joints (ADR-0108): a Joint component's joint is made once both its bodies
// exist. Its frames come from the world's poses when it is first made, when
// its structure changes (kind, other, anchors, axis, collision), and when it
// is joined again after breaking; a change of its settings (limits, motor,
// spring), or a body made again for the same entity, makes it again from the
// frames it had, so its angle and length keep their zero.
//
// Characters (ADR-0108): a CharacterController's character is the physics
// module's (physics/character.h), moved by the bridge every step before the
// physics step; its kinematic body's ghost capsule goes where it went over
// the step, so that triggers and rays see it there. On the ground, what it
// stands on carries it by where its point of contact went over the last step
// (that body's motion, turns included); off the ground it keeps the velocity
// it left with, and gravity pulls it. Characters are met every step by a
// query of their own; one not met is gone.
#include <nepheline/asset/collision.h>
#include <nepheline/asset/physics_material.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/profile.h>
#include <nepheline/math/mat.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/character.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/joint.h>
#include <nepheline/physics/mesh.h>
#include <nepheline/physics/shape.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/scene/query.h>
#include <nepheline/scene/transform.h>

#include "engine_internal.h"

#include <string.h>

#define NO_RECORD NPH_U32_MAX
#define NO_COPY   NPH_U32_MAX

// A physics material's modes are the physics module's, numbered alike.
NPH_STATIC_ASSERT((u32)NPH_PHYSICS_MATERIAL_COMBINE_AVERAGE == (u32)NPH_PHYSICS_COMBINE_AVERAGE &&
                      (u32)NPH_PHYSICS_MATERIAL_COMBINE_MIN == (u32)NPH_PHYSICS_COMBINE_MIN &&
                      (u32)NPH_PHYSICS_MATERIAL_COMBINE_MULTIPLY ==
                          (u32)NPH_PHYSICS_COMBINE_MULTIPLY &&
                      (u32)NPH_PHYSICS_MATERIAL_COMBINE_MAX == (u32)NPH_PHYSICS_COMBINE_MAX &&
                      (u32)NPH_PHYSICS_MATERIAL_COMBINE_COUNT == (u32)NPH_PHYSICS_COMBINE_COUNT,
                  "physics materials number their combine modes as the physics module does");

typedef enum BodyFlags {
    BODY_RIGID = 1U << 0U, // made from a RigidBody (else a Collider's static body)
    // Under a parent: a static or kinematic body follows it every step, and
    // a dynamic one's LocalTransform is written against it every step.
    BODY_FOLLOWS = 1U << 1U,
    BODY_FAILED =
        1U << 2U, // the body could not be made (logged): retried when its components change
    BODY_SHAPED = 1U << 3U,   // made with a Collider
    BODY_UNSHAPED = 1U << 4U, // its Collider could not be made (logged): a body without a shape
    // Its entity has a parent (BODY_FOLLOWS too, unless the follow list
    // could not grow): a root's LocalTransform is its pose in the world.
    BODY_PARENTED = 1U << 5U,
    // A part: a shape of the body of `owner`, an entity above it (no body of
    // its own; `body` is the owner's).
    BODY_PART = 1U << 6U,
    // A part with entities between it and its body, whose changes the bridge
    // does not see: its frame is checked every step.
    BODY_DEEP = 1U << 7U,
    // Its ASSET Collider's asset is loading: its shapes come when it is
    // ready (poll_awaiting), a dynamic body asleep until then.
    BODY_AWAITING = 1U << 8U,
} BodyFlags;

typedef struct BodyRecord {
    NphEntity entity;
    u32 slot; // the entity's slot in the world
    u32 flags;
    u32 target_step; // a kinematic body moved to a target at this step (0: never)
    NphPhysicsBody body;
    NphPhysicsShape shape;
    // What the body was made from, or last wrote back: the game's changes
    // are what differs. What the write-back touches comes first, together
    // (three cache lines a moving body), the Collider last.
    NphRigidBody rigid; // zeroed without a RigidBody
    NphLocalTransform local;
    NphDVec3 position; // the pose the body was given: the entity's in the world
    NphQuat rotation;
    NphVec3 scale; // the world scale its shape was made at
    NphCollider collider;
    // A part's (BODY_PART): the entity whose body it is a shape of, that
    // entity's slot, the next part of that body (an entity slot, NO_RECORD
    // for the last), and its frame in the body's.
    NphEntity owner;
    u32 owner_slot;
    u32 part_next;
    NphPhysicsPose frame;
    u64 chain_hash; // a deep part's: what its frame was composed from (place_deep_parts)
    // An ASSET Collider's shapes, one a shape of its asset (null where one
    // was refused), each with the copy it uses (NO_COPY: the asset's block
    // in place): `asset_count` of each, the bridge's allocations.
    NphPhysicsShape *asset_shapes;
    u32 *asset_copies;
    u32 asset_count;
} BodyRecord;

typedef enum CollisionState {
    COLLISION_LOADING = 0,
    COLLISION_READY = 1,
    COLLISION_FAILED = 2,
} CollisionState;

// A collision asset the world's Colliders name, held while playing: its
// shapes' kinds and blocks, checked once when it became ready.
typedef struct CollisionEntry {
    NphGuid guid;
    u32 state; // CollisionState
    u32 shape_count;
    NphAssetHandle asset;
    u32 *kinds;                // per shape: NphCollisionShapeKind
    NphCollisionBlock *blocks; // per shape
} CollisionEntry;

// What a shape's surface is made of: a Collider's own values, or its physics
// material's.
typedef struct Surface {
    f32 density;
    f32 friction;
    f32 restitution;
    NphPhysicsCombine friction_combine;
    NphPhysicsCombine restitution_combine;
} Surface;

typedef enum MaterialState {
    MATERIAL_LOADING = 0,
    MATERIAL_READY = 1,
    MATERIAL_FAILED = 2, // the Colliders naming it keep their own values
} MaterialState;

// A physics material the world's Colliders name, known while playing: its
// values, read once its asset was ready (and the asset let go), and the load
// in flight, the first or a reload's (`reloading`: the values stay until it
// is ready).
typedef struct MaterialEntry {
    NphGuid guid;
    u32 state; // MaterialState
    b32 reloading;
    NphAssetHandle asset;
    Surface surface; // when READY
} MaterialEntry;

// A block's copy at a scale and turn: the asset, the shape, and the scale's
// three and the rotation's four words as bits.
typedef struct CopyKey {
    NphGuid guid;
    u32 shape;
    u32 bits[7];
} CopyKey;

typedef struct TriggerKey {
    u32 trigger; // entity values
    u32 visitor;
} TriggerKey;

// A trigger and a Collider in it: their overlapping shapes, the visitor's
// body's entity, and, while a step's events touch it, its count before them.
typedef struct TriggerPair {
    u32 count;
    u32 body;
    u32 before;
    u32 touched;
} TriggerPair;

typedef NPH_ARRAY(TriggerKey) TriggerKeyArray;
typedef NPH_ARRAY(NphTriggerEvent) TriggerEventArray;

enum {
    JOINT_BROKEN = 1U << 0U, // broke (or came broken): made again when `broken` goes false
    JOINT_FAILED =
        1U << 1U, // could not be made (logged): again when the component or a body changes
    JOINT_DIRTY = 1U << 2U,   // its component changed: made again
    JOINT_REFRAME = 1U << 3U, // the next make takes its frames from the world's poses
};

// A Joint component's joint: the bodies it was made between (or waited or
// failed for) and the entities they are of, the component as last seen, and
// the description it was made from, whose frames a body made again keeps.
typedef struct JointRecord {
    NphEntity entity;
    u32 flags; // JOINT_*
    NphPhysicsJoint joint;
    NphPhysicsBody body_a;
    NphPhysicsBody body_b;
    NphEntity owner_a;
    NphEntity owner_b;
    NphJoint component;
    NphPhysicsJointDesc desc;
    u32 generation; // the bridge's body_generation when last settled
} JointRecord;

enum {
    // Its component is no capsule (logged): no body, and it does not move until it changes.
    CHARACTER_INVALID = 1U << 0U,
};

// A CharacterController's character: its kinematic body and ghost capsule, the physics
// module's description and state, and what the bridge keeps between steps: the velocity it
// keeps off the ground, the translation it last wrote or read (another is the game's: a
// teleport), and what it stood on at its last move with that body's pose then (what carries
// it).
typedef struct CharacterRecord {
    NphEntity entity;
    u32 flags; // CHARACTER_*
    u32 mark;  // the character pass that last met it
    NphPhysicsBody body;
    NphPhysicsShape shape;
    NphCharacterController settings; // as made: its capsule's and moves' settings compared
    NphPhysicsCharacterDesc desc;
    NphPhysicsCharacter state;
    NphVec3 fall; // m/s off the ground: its ground's and a jump's when it left, and gravity's
    NphDVec3 written;
    NphPhysicsBody carrier;
    NphDVec3 carrier_position;
    NphQuat carrier_rotation;
} CharacterRecord;

typedef struct ScaledCopy {
    CopyKey key;
    u32 kind; // NphCollisionShapeKind; 0: a free slot
    u32 refs;
    void *block; // NphPhysicsHull * or NphPhysicsMesh *, the bridge's allocation
} ScaledCopy;

struct PhysicsBridge {
    NphEngine *engine;
    NphAllocator *alloc;
    NphWorld *world;          // the active world
    NphQuery *rigid_query;    // RigidBody, LocalTransform, Collider (optional): changes
    NphQuery *static_query;   // Collider, LocalTransform, no RigidBody: changes
    NphQuery *write_query;    // RigidBody, LocalTransform: the write-back
    NphPhysicsWorld *physics; // while playing
    BodyRecord *records;
    u32 count;
    u32 cap;
    u32 *slot_record; // per entity slot: its record, or NO_RECORD
    u32 *slot_parts;  // per entity slot: the first part of its body (a slot), or NO_RECORD
    u32 slot_cap;
    NphArrayU32 follows;       // slots of the records under parents (BODY_FOLLOWS)
    NphArrayU32 deep_parts;    // slots of the parts checked every step (BODY_DEEP)
    NphArrayU32 targeted;      // entities whose kinematic bodies this step moves to a target
    NphArrayU32 last_targeted; // the last step's
    u32 event_cursor;          // world events read so far
    u32 events_dropped;        // the stream's drop count when last read
    b32 full_pass;             // look at every chunk at the next step
    u32 step;                  // steps since play began
    NphPhysicsStats stats;
    // Collision assets (ADR-0108) while playing.
    CollisionEntry *collisions;
    u32 collision_count;
    u32 collision_cap;
    NphHashMap collision_index; // NphGuid -> u32
    ScaledCopy *copies;
    u32 copy_count; // slots used, free ones among them
    u32 copy_cap;
    NphHashMap copy_index; // CopyKey -> u32
    NphArrayU32 copy_free; // free slots
    NphArrayU32 awaiting;  // slots of the records waiting for an asset (BODY_AWAITING)
    b32 load_sync;         // play begin: assets load before the first step
    // Physics materials (ADR-0108) while playing.
    MaterialEntry *materials;
    u32 material_count;
    u32 material_cap;
    NphHashMap material_index; // NphGuid -> u32
    u32 material_reloads;      // entries with a reload in flight
    // Joints (ADR-0108).
    NphQuery *joint_query; // Joint (changes), LocalTransform
    JointRecord *joints;
    u32 joint_count;
    u32 joint_cap;
    NphHashMap joint_index;    // entity value -> u32
    NphPhysicsBody world_body; // the world's end of joints to no entity, made with the first
    // Raised whenever a body or part record is made or destroyed: a joint settled at the
    // current one has its bodies (settle_joints).
    u32 body_generation;
    // Triggers (ADR-0108).
    NphHashMap trigger_pairs;     // TriggerKey -> TriggerPair
    TriggerKeyArray trigger_step; // the pairs this step's events touched, in order
    TriggerEventArray entered;    // the last step's
    TriggerEventArray left;
    // Characters (ADR-0108).
    NphQuery *character_query; // CharacterController, LocalTransform: both written
    CharacterRecord *characters;
    u32 character_count;
    u32 character_cap;
    NphHashMap character_index; // entity value -> u32
    u32 character_mark;         // character passes so far
};

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

// The record of an entity, alive or one the event stream says went: its
// slot's record, when that is the entity's and not a later one's.
static u32 record_index(const PhysicsBridge *b, NphEntity e)
{
    if (b->slot_record == NULL || nph_entity_is_null(e)) {
        return NO_RECORD;
    }
    const u32 slot = nph_world_handle_slot(b->world, e);
    if (slot >= b->slot_cap) {
        return NO_RECORD;
    }
    const u32 index = b->slot_record[slot];
    return index != NO_RECORD && b->records[index].entity.value == e.value ? index : NO_RECORD;
}

static BodyRecord *record_of(const PhysicsBridge *b, NphEntity e)
{
    const u32 index = record_index(b, e);
    return index != NO_RECORD ? &b->records[index] : NULL;
}

static void remove_slot(NphArrayU32 *list, u32 slot)
{
    for (u32 i = 0; i < list->len; ++i) {
        if (list->data[i] == slot) {
            list->data[i] = list->data[list->len - 1U];
            list->len -= 1U;
            return;
        }
    }
}

static void reweigh(PhysicsBridge *b, NphEntity owner);
static void drop_shapes(PhysicsBridge *b, BodyRecord *r, bool destroy);
static void make_shapes(PhysicsBridge *b, BodyRecord *r);

// Takes a part out of its body's list.
static void unlink_part(PhysicsBridge *b, const BodyRecord *part)
{
    u32 *at = &b->slot_parts[part->owner_slot];
    while (*at != NO_RECORD) {
        if (*at == part->slot) {
            *at = part->part_next;
            return;
        }
        at = &b->records[b->slot_record[*at]].part_next;
    }
}

// Destroys a record and what it made. A body's parts lose their shapes
// with it: kept to take new ones when the body is made again in place
// (`keep_parts`), or destroyed for the next step's full pass to make again
// as whatever they are now (another body's parts, or bodies of their own).
static void destroy_record(PhysicsBridge *b, u32 index, bool keep_parts)
{
    NPH_ASSERT(index < b->count);
    b->body_generation += 1U;
    BodyRecord *r = &b->records[index];
    const u32 slot = r->slot;
    const bool part = (r->flags & BODY_PART) != 0U;
    NphEntity owner = NPH_ENTITY_NULL;
    if (part) {
        unlink_part(b, r);
        if ((r->flags & BODY_DEEP) != 0U) {
            remove_slot(&b->deep_parts, slot);
        }
        drop_shapes(b, r, !nph_physics_body_is_null(r->body));
        owner = r->owner;
    } else {
        if (b->physics != NULL && !nph_physics_body_is_null(r->body)) {
            nph_physics_body_destroy(b->physics, r->body); // its shapes, its parts' too
        }
        drop_shapes(b, r, false);
        for (u32 s = b->slot_parts[slot]; s != NO_RECORD;) {
            BodyRecord *p = &b->records[b->slot_record[s]];
            drop_shapes(b, p, false);
            p->body = NPH_PHYSICS_BODY_NULL;
            s = p->part_next;
        }
        if ((r->flags & BODY_FOLLOWS) != 0U) {
            remove_slot(&b->follows, slot);
        }
    }
    b->slot_record[slot] = NO_RECORD;
    const u32 last = b->count - 1U;
    if (index != last) {
        b->records[index] = b->records[last];
        b->slot_record[b->records[index].slot] = index;
    }
    b->count = last;
    if (!nph_entity_is_null(owner)) {
        reweigh(b, owner); // its body's mass over what is left
    }
    if (!part && !keep_parts && b->slot_parts[slot] != NO_RECORD) {
        u32 s = b->slot_parts[slot];
        b->slot_parts[slot] = NO_RECORD;
        while (s != NO_RECORD) {
            const u32 pi = b->slot_record[s];
            s = b->records[pi].part_next;
            destroy_record(b, pi, false); // no shape left, and out of no list
        }
        b->full_pass = 1;
    }
}

static void destroy_all(PhysicsBridge *b)
{
    while (b->count > 0U) {
        destroy_record(b, b->count - 1U, false);
    }
    for (u32 i = 0; i < b->slot_cap; ++i) {
        b->slot_parts[i] = NO_RECORD;
    }
    NPH_ARRAY_CLEAR(&b->follows);
    NPH_ARRAY_CLEAR(&b->deep_parts);
    NPH_ARRAY_CLEAR(&b->awaiting);
    NPH_ARRAY_CLEAR(&b->targeted);
    NPH_ARRAY_CLEAR(&b->last_targeted);
}

static NphResult grow_records(PhysicsBridge *b)
{
    if (b->count < b->cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(64U, 2U * b->cap);
    BodyRecord *grown = NPH_ALLOC_ARRAY(b->alloc, BodyRecord, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->records != NULL) {
        memcpy(grown, b->records, (u64)b->count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, BodyRecord, b->records, b->cap);
    }
    b->records = grown;
    b->cap = cap;
    return NPH_RESULT_OK;
}

// The slot table covers the world's capacity.
static NphResult size_slots(PhysicsBridge *b)
{
    const u32 cap = nph_world_entity_capacity(b->world);
    if (cap <= b->slot_cap) {
        return NPH_RESULT_OK;
    }
    u32 *grown = NPH_ALLOC_ARRAY(b->alloc, u32, cap);
    u32 *parts = NPH_ALLOC_ARRAY(b->alloc, u32, cap);
    if (grown == NULL || parts == NULL) {
        if (grown != NULL) {
            NPH_FREE_ARRAY(b->alloc, u32, grown, cap);
        }
        if (parts != NULL) {
            NPH_FREE_ARRAY(b->alloc, u32, parts, cap);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < cap; ++i) {
        grown[i] = i < b->slot_cap ? b->slot_record[i] : NO_RECORD;
        parts[i] = i < b->slot_cap ? b->slot_parts[i] : NO_RECORD;
    }
    if (b->slot_record != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_record, b->slot_cap);
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_parts, b->slot_cap);
    }
    b->slot_record = grown;
    b->slot_parts = parts;
    b->slot_cap = cap;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Poses
// ---------------------------------------------------------------------------

typedef struct EntityPose {
    NphDVec3 position;
    NphQuat rotation;
    NphVec3 scale; // per local axis, negative where the entity mirrors
    bool parented; // under a parent with a world transform
} EntityPose;

static bool quat_bits_equal(NphQuat a, NphQuat b)
{
    return nph_f32_to_bits(a.x) == nph_f32_to_bits(b.x) &&
           nph_f32_to_bits(a.y) == nph_f32_to_bits(b.y) &&
           nph_f32_to_bits(a.z) == nph_f32_to_bits(b.z) &&
           nph_f32_to_bits(a.w) == nph_f32_to_bits(b.w);
}

static bool vec3_bits_equal(NphVec3 a, NphVec3 b)
{
    return nph_f32_to_bits(a.x) == nph_f32_to_bits(b.x) &&
           nph_f32_to_bits(a.y) == nph_f32_to_bits(b.y) &&
           nph_f32_to_bits(a.z) == nph_f32_to_bits(b.z);
}

static bool dvec3_bits_equal(NphDVec3 a, NphDVec3 b)
{
    return nph_f64_to_bits(a.x) == nph_f64_to_bits(b.x) &&
           nph_f64_to_bits(a.y) == nph_f64_to_bits(b.y) &&
           nph_f64_to_bits(a.z) == nph_f64_to_bits(b.z);
}

static bool f32_bits_equal(f32 a, f32 b)
{
    return nph_f32_to_bits(a) == nph_f32_to_bits(b);
}

// Components by their fields' bits: a game that writes what was there
// changes nothing, and -0 or a NaN typed in an inspector is a change.
static bool rigid_equal(const NphRigidBody *a, const NphRigidBody *b)
{
    return a->type == b->type && f32_bits_equal(a->mass, b->mass) &&
           vec3_bits_equal(a->linear_velocity, b->linear_velocity) &&
           vec3_bits_equal(a->angular_velocity, b->angular_velocity) &&
           f32_bits_equal(a->linear_damping, b->linear_damping) &&
           f32_bits_equal(a->angular_damping, b->angular_damping) &&
           f32_bits_equal(a->gravity_scale, b->gravity_scale) && a->locks == b->locks &&
           a->bullet == b->bullet && a->can_sleep == b->can_sleep;
}

static bool collider_equal(const NphCollider *a, const NphCollider *b)
{
    return a->shape == b->shape && f32_bits_equal(a->radius, b->radius) &&
           f32_bits_equal(a->half_height, b->half_height) &&
           vec3_bits_equal(a->half_extents, b->half_extents) &&
           vec3_bits_equal(a->center, b->center) && quat_bits_equal(a->rotation, b->rotation) &&
           f32_bits_equal(a->density, b->density) && f32_bits_equal(a->friction, b->friction) &&
           f32_bits_equal(a->restitution, b->restitution) && a->category == b->category &&
           nph_guid_equals(a->physics_material, b->physics_material) && a->mask == b->mask &&
           a->group == b->group && a->hit_events == b->hit_events &&
           nph_guid_equals(a->collision, b->collision) && a->trigger == b->trigger;
}

// A rotation a body takes (unit within the physics module's 1e-3): a
// LocalTransform's as it is when it is unit to rounding, so a body written
// back reads back unchanged, else normalised (the identity for zero).
static NphQuat body_rotation(NphQuat q)
{
    return nph_quat_is_normalized(q, 1e-6F) ? q : nph_quat_normalize_or_identity(q);
}

static const NphWorldTransform *transform_of(const PhysicsBridge *b, NphEntity parent)
{
    if (nph_entity_is_null(parent)) {
        return NULL;
    }
    return NPH_WORLD_GET(b->world, parent, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
}

static const NphWorldTransform *parent_transform(const PhysicsBridge *b, NphEntity e)
{
    return transform_of(b, nph_world_parent(b->world, e));
}

// Where an entity under `parent` (none: a root) is in the world: its
// LocalTransform for a root, composed with its parent's world transform (as
// of the step's start) otherwise.
static EntityPose entity_pose_under(const PhysicsBridge *b, NphEntity parent,
                                    const NphLocalTransform *local)
{
    EntityPose p;
    const NphWorldTransform *pw = transform_of(b, parent);
    if (pw == NULL) {
        p.position = local->translation;
        p.rotation = body_rotation(local->rotation);
        p.scale = local->scale; // its signs the mirror the rotation leaves out
        p.parented = false;
        return p;
    }
    const NphWorldTransform w = nph_world_transform_compose(pw, local);
    p.position = w.translation;
    p.rotation = nph_body_axes_rotation(w.x_axis, w.y_axis);
    p.scale = nph_body_axes_scale(w.x_axis, w.y_axis, w.z_axis);
    p.parented = true;
    return p;
}

static EntityPose entity_pose(const PhysicsBridge *b, NphEntity e, const NphLocalTransform *local)
{
    return entity_pose_under(b, nph_world_parent(b->world, e), local);
}

// The LocalTransform that puts the entity at a body's pose: its
// translation and rotation relative to the parent's world transform (the
// parent's scale and shear divided out of the translation; the rotation is
// relative to the parent's axes), its scale kept.
static void local_from_pose(const PhysicsBridge *b, NphEntity e, NphDVec3 position,
                            NphQuat rotation, NphLocalTransform *local)
{
    const NphWorldTransform *pw = parent_transform(b, e);
    if (pw == NULL) {
        local->translation = position;
        local->rotation = rotation;
        return;
    }
    NphMat3 inv;
    if (!nph_mat3_inverse(nph_world_transform_linear(pw), &inv)) {
        return; // a degenerate parent: nothing sensible to write
    }
    const NphVec3 d = nph_dvec3_to_vec3(nph_dvec3_sub(position, pw->translation));
    // The offset in f32 is relative to the parent, near it: full precision
    // where it matters (§8.2).
    local->translation = nph_dvec3_from_vec3(nph_mat3_mul_vec3(inv, d));
    const NphQuat parent_rotation = nph_body_axes_rotation(pw->x_axis, pw->y_axis);
    local->rotation =
        nph_quat_normalize(nph_quat_mul(nph_quat_conjugate(parent_rotation), rotation));
}

// ---------------------------------------------------------------------------
// Bodies
// ---------------------------------------------------------------------------

static NphPhysicsBodyType body_type(const NphRigidBody *rb)
{
    switch (rb->type) {
    case NPH_RIGID_BODY_STATIC:
        return NPH_PHYSICS_BODY_STATIC;
    case NPH_RIGID_BODY_KINEMATIC:
        return NPH_PHYSICS_BODY_KINEMATIC;
    default:
        return NPH_PHYSICS_BODY_DYNAMIC;
    }
}

static f32 finite_or(f32 v, f32 fallback)
{
    return nph_f32_is_finite(v) ? v : fallback;
}

static f32 nonneg(f32 v)
{
    return nph_f32_is_finite(v) && v > 0.0F ? v : 0.0F;
}

static NphVec3 finite_vec3(NphVec3 v)
{
    return nph_vec3_is_finite(v) ? v : nph_vec3_zero();
}

// A RigidBody's mass, when it names one, replaces the shapes' with their
// distribution kept: the tensor scaled by the ratio (a point of that mass,
// rotating like a ball of 0.5 m, without a shape).
static void apply_mass(PhysicsBridge *b, const BodyRecord *r, f32 mass)
{
    NphPhysicsMassData none;
    memset(&none, 0, sizeof none);
    nph_physics_body_set_mass_data(b->physics, r->body, &none); // the shapes' again
    if (!(nph_f32_is_finite(mass) && mass > 0.0F) ||
        nph_physics_body_type(b->physics, r->body) != NPH_PHYSICS_BODY_DYNAMIC) {
        return;
    }
    NphPhysicsMassData m = nph_physics_body_mass_data(b->physics, r->body);
    if (!(m.mass > 0.0F)) { // no shape with mass, its parts' counted
        m.center = nph_vec3_zero();
        m.inertia = nph_mat3_from_diagonal(nph_vec3_splat(0.1F * mass));
    } else {
        m.inertia = nph_mat3_scale(m.inertia, mass / m.mass);
    }
    m.mass = mass;
    nph_physics_body_set_mass_data(b->physics, r->body, &m);
}

// A body's explicit mass (its RigidBody's `mass`) over its shapes as they
// are now, when it has one.
static void reweigh(PhysicsBridge *b, NphEntity owner)
{
    BodyRecord *o = record_of(b, owner);
    if (o != NULL && (o->flags & BODY_RIGID) != 0U && !nph_physics_body_is_null(o->body) &&
        o->rigid.mass > 0.0F) {
        apply_mass(b, o, o->rigid.mass);
    }
}

static NphPhysicsFilter collider_filter(const NphCollider *c)
{
    NphPhysicsFilter f;
    f.category = c->category;
    f.mask = c->mask;
    f.group = c->group;
    return f;
}

// The surface a Collider gives its shapes: its physics material's when it
// names one that is ready, its own values otherwise.
static Surface surface_of(const PhysicsBridge *b, const NphCollider *c)
{
    Surface s;
    s.density = nonneg(c->density);
    s.friction = nonneg(c->friction);
    s.restitution = nph_f32_clamp(finite_or(c->restitution, 0.0F), 0.0F, 1.0F);
    s.friction_combine = NPH_PHYSICS_COMBINE_DEFAULT;
    s.restitution_combine = NPH_PHYSICS_COMBINE_DEFAULT;
    if (!nph_guid_is_null(c->physics_material)) {
        const u32 *known = NPH_HASHMAP_FIND(&b->material_index, u32, &c->physics_material);
        if (known != NULL && b->materials[*known].state == (u32)MATERIAL_READY) {
            s = b->materials[*known].surface;
        }
    }
    return s;
}

// A Collider's surface, filter, and events on a shape of the record's.
static void apply_shape_desc(const PhysicsBridge *b, const BodyRecord *r, const NphCollider *c,
                             NphPhysicsShapeDesc *sd)
{
    const Surface surface = surface_of(b, c);
    sd->density = surface.density;
    sd->friction = surface.friction;
    sd->restitution = surface.restitution;
    sd->friction_combine = surface.friction_combine;
    sd->restitution_combine = surface.restitution_combine;
    sd->filter = collider_filter(c);
    sd->hit_events = c->hit_events != 0 ? 1 : 0;
    sd->sensor = c->trigger != 0 ? 1 : 0;
    sd->user_data = r->entity.value;
}

// The record's shape from its Collider at its scale (none when the
// Collider names none that is valid: logged once, until it changes).
static void make_shape(PhysicsBridge *b, BodyRecord *r, const NphCollider *c)
{
    r->shape = NPH_PHYSICS_SHAPE_NULL;
    r->flags &= ~(u32)BODY_UNSHAPED;
    NphPhysicsGeometry g;
    NphPhysicsPose pose;
    if (!nph_collider_shape(c, r->scale, &g, &pose)) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                     "entity 0x%08x: its Collider is no shape at its scale; none is made",
                     r->entity.value);
        r->flags |= BODY_UNSHAPED;
        return;
    }
    if ((r->flags & BODY_PART) != 0U) {
        // In its body's frame: the part's frame, then the Collider's place in it.
        pose.position =
            nph_vec3_add(r->frame.position, nph_quat_rotate(r->frame.rotation, pose.position));
        pose.rotation = nph_quat_normalize(nph_quat_mul(r->frame.rotation, pose.rotation));
    }
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    apply_shape_desc(b, r, c, &sd);
    sd.pose = pose;
    const NphResult res = nph_physics_shape_create(b->physics, r->body, &sd, &r->shape);
    if (res != NPH_RESULT_OK) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "entity 0x%08x: its Collider's shape: %s", r->entity.value,
                     nph_result_string(res));
        r->shape = NPH_PHYSICS_SHAPE_NULL;
        r->flags |= BODY_UNSHAPED;
    }
}

// ---------------------------------------------------------------------------
// Parts (ADR-0108)
// ---------------------------------------------------------------------------

#define PART_MAX_DEPTH 64U

// The entity whose body a Collider without a RigidBody under `parent` is
// part of: the nearest above it with a RigidBody (and a LocalTransform), or
// none. Asked of the bridge's own records, not the world's archetypes, from
// the parent the visit looks up for the pose anyway: a change pass visits
// the RigidBodies before the Colliders, so every body has its record by then.
// Asking the world, and for the parent again, cost a full pass over 10k
// grouped Colliders 6 %; this costs it 1 % (engine_frame_physics_10k_*).
static NphEntity find_owner(const PhysicsBridge *b, NphEntity parent)
{
    NphEntity p = parent;
    for (u32 depth = 0U; !nph_entity_is_null(p) && depth < PART_MAX_DEPTH; ++depth) {
        const BodyRecord *r = record_of(b, p);
        if (r != NULL && (r->flags & BODY_RIGID) != 0U) {
            return p;
        }
        p = nph_world_parent(b->world, p);
    }
    return NPH_ENTITY_NULL;
}

// The world scale of a part's body's entity: as the body's record last saw
// it, or from its components before it has one.
static NphVec3 owner_scale(const PhysicsBridge *b, NphEntity owner)
{
    const BodyRecord *o = record_of(b, owner);
    if (o != NULL) {
        return o->scale;
    }
    const NphLocalTransform *lt =
        NPH_WORLD_GET(b->world, owner, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    return lt != NULL ? entity_pose(b, owner, lt).scale : nph_vec3_one();
}

// A part's frame in its body's and its scale: the LocalTransforms from the
// body's entity down to the part's (`local`), composed under the body's
// world scale. `out_between` counts the entities between them. False when
// the body's entity is no longer above it.
static bool part_frame(const PhysicsBridge *b, const BodyRecord *r, const NphLocalTransform *local,
                       NphPhysicsPose *out_frame, NphVec3 *out_scale, u32 *out_between)
{
    NphEntity chain[PART_MAX_DEPTH];
    u32 n = 0U;
    NphEntity p = nph_world_parent(b->world, r->entity);
    while (p.value != r->owner.value) {
        if (nph_entity_is_null(p) || n == PART_MAX_DEPTH) {
            return false;
        }
        chain[n++] = p;
        p = nph_world_parent(b->world, p);
    }
    const NphVec3 s = owner_scale(b, r->owner);
    NphWorldTransform m = nph_world_transform_identity();
    m.x_axis = nph_vec3(s.x, 0.0F, 0.0F);
    m.y_axis = nph_vec3(0.0F, s.y, 0.0F);
    m.z_axis = nph_vec3(0.0F, 0.0F, s.z);
    for (u32 i = n; i-- > 0U;) {
        const NphLocalTransform *lt =
            NPH_WORLD_GET(b->world, chain[i], NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
        if (lt != NULL) {
            m = nph_world_transform_compose(&m, lt);
        }
    }
    m = nph_world_transform_compose(&m, local);
    out_frame->position = nph_dvec3_to_vec3(m.translation);
    out_frame->rotation = nph_body_axes_rotation(m.x_axis, m.y_axis);
    *out_scale = nph_body_axes_scale(m.x_axis, m.y_axis, m.z_axis);
    *out_between = n;
    return true;
}

static void set_deep(PhysicsBridge *b, BodyRecord *r, bool deep)
{
    const bool now = (r->flags & BODY_DEEP) != 0U;
    if (deep == now) {
        return;
    }
    if (deep) {
        if (NPH_ARRAY_PUSH(&b->deep_parts, r->slot) != NPH_RESULT_OK) {
            return; // out of memory: placed again only when its own components change
        }
        r->flags |= BODY_DEEP;
    } else {
        remove_slot(&b->deep_parts, r->slot);
        r->flags &= ~(u32)BODY_DEEP;
    }
}

// A hash of what a deep part's frame is composed from: the entities between
// it and its body, their LocalTransforms, and the body's scale (its own
// LocalTransform changes only through the change pass).
static u64 chain_hash(const PhysicsBridge *b, const BodyRecord *r)
{
    const NphVec3 s = owner_scale(b, r->owner);
    u64 h = nph_hash64(&s, sizeof s, r->owner.value);
    NphEntity p = nph_world_parent(b->world, r->entity);
    for (u32 depth = 0U;
         !nph_entity_is_null(p) && p.value != r->owner.value && depth < PART_MAX_DEPTH; ++depth) {
        const NphLocalTransform *lt =
            NPH_WORLD_GET(b->world, p, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
        h = nph_hash64(&p.value, sizeof p.value, h);
        if (lt != NULL) { // field by field: the struct's padding is no one's to read
            h = nph_hash64(&lt->translation, sizeof lt->translation, h);
            h = nph_hash64(&lt->rotation, sizeof lt->rotation, h);
            h = nph_hash64(&lt->scale, sizeof lt->scale, h);
        }
        p = nph_world_parent(b->world, p);
    }
    return h;
}

// A part placed where its entity is in its body (its shape made again when
// that moved, or `force`); true when its shape was made again.
static bool place_part(PhysicsBridge *b, BodyRecord *r, bool force)
{
    NphPhysicsPose frame;
    NphVec3 scale;
    u32 between = 0U;
    if (!part_frame(b, r, &r->local, &frame, &scale, &between)) {
        // Its body's entity is not above it now: the change pass that saw
        // the hierarchy change makes it again (read_events).
        return false;
    }
    set_deep(b, r, between > 0U);
    if (between > 0U) {
        r->chain_hash = chain_hash(b, r);
    }
    const bool moved = !vec3_bits_equal(frame.position, r->frame.position) ||
                       !quat_bits_equal(frame.rotation, r->frame.rotation) ||
                       !vec3_bits_equal(scale, r->scale);
    if (!moved && !force) {
        return false;
    }
    r->frame = frame;
    r->scale = scale;
    drop_shapes(b, r, true);
    if (!nph_physics_body_is_null(r->body)) {
        make_shapes(b, r);
    }
    return true;
}

static NphResult create_part(PhysicsBridge *b, NphEntity e, NphEntity owner,
                             const NphLocalTransform *local, const NphCollider *collider)
{
    NPH_TRY(grow_records(b));
    b->body_generation += 1U;
    const u32 index = b->count++;
    BodyRecord *r = &b->records[index];
    memset(r, 0, sizeof *r);
    r->entity = e;
    r->slot = nph_world_entity_index(b->world, e);
    b->slot_record[r->slot] = index;
    r->flags = (u32)BODY_PART | (u32)BODY_SHAPED;
    r->collider = *collider;
    r->local = *local;
    r->owner = owner;
    r->owner_slot = nph_world_handle_slot(b->world, owner);
    r->part_next = b->slot_parts[r->owner_slot];
    b->slot_parts[r->owner_slot] = r->slot;
    const BodyRecord *o = record_of(b, owner);
    r->body = o != NULL ? o->body : NPH_PHYSICS_BODY_NULL;
    r->shape = NPH_PHYSICS_SHAPE_NULL;
    if (place_part(b, r, true)) {
        reweigh(b, owner);
    }
    return NPH_RESULT_OK;
}

// A part's components changed: its shape made again where it differs.
static void update_part(PhysicsBridge *b, BodyRecord *r, const NphLocalTransform *local,
                        const NphCollider *collider)
{
    const bool reshaped = !collider_equal(collider, &r->collider);
    r->collider = *collider;
    r->local = *local;
    if (place_part(b, r, reshaped)) {
        reweigh(b, r->owner);
    }
}

// A body just made takes its parts' shapes (before its explicit mass).
static void attach_parts(PhysicsBridge *b, const BodyRecord *owner)
{
    for (u32 s = b->slot_parts[owner->slot]; s != NO_RECORD;) {
        BodyRecord *p = &b->records[b->slot_record[s]];
        drop_shapes(b, p, false); // gone with the body before this one
        p->body = owner->body;
        NPH_IGNORE_RESULT(place_part(b, p, true));
        s = p->part_next;
    }
}

// A body's entity changed scale: its parts placed again.
static void replace_parts(PhysicsBridge *b, const BodyRecord *owner)
{
    bool any = false;
    for (u32 s = b->slot_parts[owner->slot]; s != NO_RECORD;) {
        BodyRecord *p = &b->records[b->slot_record[s]];
        any = place_part(b, p, false) || any;
        s = p->part_next;
    }
    if (any) {
        reweigh(b, owner->entity);
    }
}

// Parts with entities between them and their bodies, whose changes the
// bridge does not see: their inputs hashed every step, and placed again when
// those changed (backwards: one that is no longer deep leaves the list). The
// hash spares the composition: 2000 deep parts cost a step 0.43 ms composed
// every step, 0.17 ms hashed (engine_frame_physics_1k_compounds_deep_spinning
// against _compounds_spinning).
static void place_deep_parts(PhysicsBridge *b)
{
    for (u32 i = b->deep_parts.len; i-- > 0U;) {
        BodyRecord *p = &b->records[b->slot_record[b->deep_parts.data[i]]];
        const u64 h = chain_hash(b, p);
        if (h == p->chain_hash) {
            continue;
        }
        p->chain_hash = h;
        if (place_part(b, p, false)) {
            reweigh(b, p->owner);
        }
    }
}

// ---------------------------------------------------------------------------
// Collision assets (ADR-0108)
// ---------------------------------------------------------------------------

static void log_collision(NphGuid guid, const char *what, NphResult r)
{
    char text[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(guid, text);
    NPH_LOG_WARN(&NPH_LOG_RUNTIME, "collision asset %s %s: %s", text, what, nph_result_string(r));
}

static void free_collision_arrays(PhysicsBridge *b, CollisionEntry *c)
{
    if (c->kinds != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, c->kinds, c->shape_count);
        NPH_FREE_ARRAY(b->alloc, NphCollisionBlock, c->blocks, c->shape_count);
    }
    c->kinds = NULL;
    c->blocks = NULL;
}

// A ready asset's shapes: its view, and each block checked by the physics
// module's validators, once.
static NphResult read_collision(PhysicsBridge *b, CollisionEntry *c, const NphAsset *asset)
{
    NphCollisionView v;
    NPH_TRY(nph_collision_view(asset, &v));
    const u32 n = v.header->shape_count;
    u32 *kinds = NPH_ALLOC_ARRAY(b->alloc, u32, n);
    NphCollisionBlock *blocks = NPH_ALLOC_ARRAY(b->alloc, NphCollisionBlock, n);
    if (kinds == NULL || blocks == NULL) {
        if (kinds != NULL) {
            NPH_FREE_ARRAY(b->alloc, u32, kinds, n);
        }
        if (blocks != NULL) {
            NPH_FREE_ARRAY(b->alloc, NphCollisionBlock, blocks, n);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    c->shape_count = n;
    c->kinds = kinds;
    c->blocks = blocks;
    for (u32 i = 0U; i < n; ++i) {
        kinds[i] = v.shapes[i].kind;
        blocks[i] = nph_collision_shape_block(asset, &v, i);
        const bool valid = kinds[i] == (u32)NPH_COLLISION_SHAPE_HULL
                               ? nph_physics_hull_validate(blocks[i].data, blocks[i].size, 0.0F)
                               : nph_physics_mesh_validate(blocks[i].data, blocks[i].size);
        if (!valid) {
            free_collision_arrays(b, c);
            return NPH_RESULT_CORRUPT;
        }
    }
    return NPH_RESULT_OK;
}

// A collision entry loading: ready or failed when its asset is (held when
// ready: the shapes use its blocks in place).
static void poll_collision(PhysicsBridge *b, u32 index)
{
    CollisionEntry *c = &b->collisions[index];
    if (c->state != (u32)COLLISION_LOADING) {
        return;
    }
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    const NphAssetState st = nph_asset_state(loader, c->asset);
    if (st == NPH_ASSET_STATE_LOADING) {
        return;
    }
    NphResult r = nph_asset_error(loader, c->asset);
    if (st == NPH_ASSET_STATE_READY) {
        r = read_collision(b, c, nph_asset_get(loader, c->asset));
    } else if (r == NPH_RESULT_OK) {
        r = NPH_RESULT_NOT_FOUND;
    }
    c->state = r == NPH_RESULT_OK ? (u32)COLLISION_READY : (u32)COLLISION_FAILED;
    if (r != NPH_RESULT_OK) {
        nph_asset_release(loader, c->asset);
        c->asset = NPH_ASSET_HANDLE_NULL;
        log_collision(c->guid, "makes no shapes", r);
    }
}

static NphResult grow_collisions(PhysicsBridge *b)
{
    if (b->collision_count < b->collision_cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(16U, 2U * b->collision_cap);
    CollisionEntry *grown = NPH_ALLOC_ARRAY(b->alloc, CollisionEntry, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->collisions != NULL) {
        memcpy(grown, b->collisions, (u64)b->collision_count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, CollisionEntry, b->collisions, b->collision_cap);
    }
    b->collisions = grown;
    b->collision_cap = cap;
    return NPH_RESULT_OK;
}

// The entry of `guid`, its load started when it is new: before returning at
// play begin (load_sync), in the background otherwise. NO_RECORD for the
// null GUID or when the table cannot grow.
static u32 collision_of(PhysicsBridge *b, NphGuid guid)
{
    if (nph_guid_is_null(guid)) {
        return NO_RECORD;
    }
    const u32 *known = NPH_HASHMAP_FIND(&b->collision_index, u32, &guid);
    if (known != NULL) {
        poll_collision(b, *known);
        return *known;
    }
    if (grow_collisions(b) != NPH_RESULT_OK) {
        return NO_RECORD;
    }
    const u32 index = b->collision_count;
    if (nph_hashmap_insert(&b->collision_index, &guid, &index) != NPH_RESULT_OK) {
        return NO_RECORD;
    }
    CollisionEntry *c = &b->collisions[index];
    memset(c, 0, sizeof *c);
    c->guid = guid;
    b->collision_count += 1U;
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    const NphResult r = b->load_sync != 0
                            ? nph_asset_load_guid(loader, guid, &c->asset)
                            : nph_asset_load_guid_async(loader, guid, NULL, &c->asset);
    if (r != NPH_RESULT_OK) {
        c->state = (u32)COLLISION_FAILED;
        c->asset = NPH_ASSET_HANDLE_NULL;
        log_collision(guid, "cannot be loaded", r);
        return index;
    }
    c->state = (u32)COLLISION_LOADING;
    poll_collision(b, index); // finished already (at play begin, or loaded before)
    return index;
}

static void release_collisions(PhysicsBridge *b)
{
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    for (u32 i = 0U; i < b->collision_count; ++i) {
        CollisionEntry *c = &b->collisions[i];
        if (!nph_asset_handle_is_null(c->asset)) {
            nph_asset_release(loader, c->asset);
        }
        free_collision_arrays(b, c);
    }
    b->collision_count = 0U;
    nph_hashmap_clear(&b->collision_index);
}

// ---------------------------------------------------------------------------
// Physics materials (ADR-0108)
// ---------------------------------------------------------------------------

static void log_material(NphGuid guid, const char *what, NphResult r)
{
    char text[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(guid, text);
    NPH_LOG_WARN(&NPH_LOG_RUNTIME, "physics material %s %s: %s", text, what, nph_result_string(r));
}

// The surface of a valid header (nph_physics_material_header_valid).
static Surface header_surface(const NphPhysicsMaterialHeader *h)
{
    Surface s;
    s.density = h->density;
    s.friction = h->friction;
    s.restitution = h->restitution;
    s.friction_combine = (NphPhysicsCombine)h->friction_combine;
    s.restitution_combine = (NphPhysicsCombine)h->restitution_combine;
    return s;
}

// A ready asset's values, through its view.
static NphResult read_material(const NphAsset *asset, Surface *out)
{
    NphPhysicsMaterialView v;
    NPH_TRY(nph_physics_material_view(asset, &v));
    *out = header_surface(v.header);
    return NPH_RESULT_OK;
}

// A shape's surface set in place: true when its density changed (its body
// weighed again).
static bool set_surface(PhysicsBridge *b, NphPhysicsShape shape, const Surface *s)
{
    nph_physics_shape_set_friction(b->physics, shape, s->friction);
    nph_physics_shape_set_restitution(b->physics, shape, s->restitution);
    nph_physics_shape_set_friction_combine(b->physics, shape, s->friction_combine);
    nph_physics_shape_set_restitution_combine(b->physics, shape, s->restitution_combine);
    if (f32_bits_equal(nph_physics_shape_density(b->physics, shape), s->density)) {
        return false;
    }
    nph_physics_shape_set_density(b->physics, shape, s->density);
    return true;
}

// The shapes of every record whose Collider names material `index` take its
// values as they are now, in place, from the next step's contacts; a new
// density weighs their bodies again, an explicit mass kept. Nothing wakes:
// what rests stays at rest until something touches it.
static void resurface(PhysicsBridge *b, u32 index)
{
    const NphGuid guid = b->materials[index].guid;
    for (u32 i = 0U; i < b->count; ++i) {
        BodyRecord *r = &b->records[i];
        if (!nph_guid_equals(r->collider.physics_material, guid) ||
            nph_physics_body_is_null(r->body)) {
            continue;
        }
        const Surface surface = surface_of(b, &r->collider);
        bool weighed = false;
        if (!nph_physics_shape_is_null(r->shape)) {
            weighed = set_surface(b, r->shape, &surface);
        }
        for (u32 k = 0U; k < r->asset_count; ++k) {
            if (!nph_physics_shape_is_null(r->asset_shapes[k])) {
                weighed = set_surface(b, r->asset_shapes[k], &surface) || weighed;
            }
        }
        if (weighed) {
            reweigh(b, (r->flags & BODY_PART) != 0U ? r->owner : r->entity);
        }
    }
}

// A material entry's load: once it finishes, its values (READY) or none
// (FAILED, logged), its asset let go either way. A reload that fails keeps
// the values there were; one that succeeds puts its values on the shapes.
static void poll_material(PhysicsBridge *b, u32 index)
{
    MaterialEntry *m = &b->materials[index];
    if (nph_asset_handle_is_null(m->asset)) {
        return;
    }
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    const NphAssetState st = nph_asset_state(loader, m->asset);
    if (st == NPH_ASSET_STATE_LOADING) {
        return;
    }
    NphResult r = nph_asset_error(loader, m->asset);
    Surface surface = m->surface;
    if (st == NPH_ASSET_STATE_READY) {
        r = read_material(nph_asset_get(loader, m->asset), &surface);
    } else if (r == NPH_RESULT_OK) {
        r = NPH_RESULT_NOT_FOUND;
    }
    nph_asset_release(loader, m->asset);
    m->asset = NPH_ASSET_HANDLE_NULL;
    const bool reload = m->reloading != 0;
    if (reload) {
        m->reloading = 0;
        b->material_reloads -= 1U;
    }
    if (r != NPH_RESULT_OK) {
        if (!reload) {
            m->state = (u32)MATERIAL_FAILED;
        }
        log_material(m->guid,
                     reload ? "failed to load again; it keeps the values it had"
                            : "cannot be read; the Colliders naming it keep their own values",
                     r);
        return;
    }
    m->surface = surface;
    m->state = (u32)MATERIAL_READY;
    if (reload) {
        resurface(b, index);
    }
}

static NphResult grow_materials(PhysicsBridge *b)
{
    if (b->material_count < b->material_cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(16U, 2U * b->material_cap);
    MaterialEntry *grown = NPH_ALLOC_ARRAY(b->alloc, MaterialEntry, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->materials != NULL) {
        memcpy(grown, b->materials, (u64)b->material_count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, MaterialEntry, b->materials, b->material_cap);
    }
    b->materials = grown;
    b->material_cap = cap;
    return NPH_RESULT_OK;
}

// The entry of `guid`, its load started when it is new: before returning at
// play begin (load_sync), in the background otherwise. NO_RECORD for the
// null GUID or when the table cannot grow.
static u32 material_of(PhysicsBridge *b, NphGuid guid)
{
    if (nph_guid_is_null(guid)) {
        return NO_RECORD;
    }
    const u32 *known = NPH_HASHMAP_FIND(&b->material_index, u32, &guid);
    if (known != NULL) {
        poll_material(b, *known);
        return *known;
    }
    if (grow_materials(b) != NPH_RESULT_OK) {
        return NO_RECORD;
    }
    const u32 index = b->material_count;
    if (nph_hashmap_insert(&b->material_index, &guid, &index) != NPH_RESULT_OK) {
        return NO_RECORD;
    }
    MaterialEntry *m = &b->materials[index];
    memset(m, 0, sizeof *m);
    m->guid = guid;
    b->material_count += 1U;
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    const NphResult r = b->load_sync != 0
                            ? nph_asset_load_guid(loader, guid, &m->asset)
                            : nph_asset_load_guid_async(loader, guid, NULL, &m->asset);
    if (r != NPH_RESULT_OK) {
        m->state = (u32)MATERIAL_FAILED;
        m->asset = NPH_ASSET_HANDLE_NULL;
        log_material(guid, "cannot be loaded; the Colliders naming it keep their own values", r);
        return index;
    }
    m->state = (u32)MATERIAL_LOADING;
    poll_material(b, index); // finished already (at play begin, or loaded before)
    return index;
}

// True while the physics material a Collider names is loading (its load
// started when it is new).
static bool material_loading(PhysicsBridge *b, const NphCollider *c)
{
    const u32 index = material_of(b, c->physics_material);
    return index != NO_RECORD && b->materials[index].state == (u32)MATERIAL_LOADING;
}

// The materials whose reloads are in flight, polled.
static void poll_reloads(PhysicsBridge *b)
{
    for (u32 i = 0U; i < b->material_count && b->material_reloads > 0U; ++i) {
        if (b->materials[i].reloading != 0) {
            poll_material(b, i);
        }
    }
}

static void release_materials(PhysicsBridge *b)
{
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    for (u32 i = 0U; i < b->material_count; ++i) {
        if (!nph_asset_handle_is_null(b->materials[i].asset)) {
            nph_asset_release(loader, b->materials[i].asset);
        }
    }
    b->material_count = 0U;
    b->material_reloads = 0U;
    nph_hashmap_clear(&b->material_index);
}

static void free_copy_block(PhysicsBridge *b, ScaledCopy *c)
{
    if (c->kind == (u32)NPH_COLLISION_SHAPE_HULL) {
        nph_physics_hull_free((NphPhysicsHull *)c->block, b->alloc);
    } else {
        nph_physics_mesh_free((NphPhysicsMesh *)c->block, b->alloc);
    }
    c->block = NULL;
    c->kind = 0U;
}

static void release_copy(PhysicsBridge *b, u32 index)
{
    ScaledCopy *c = &b->copies[index];
    NPH_ASSERT(c->kind != 0U && c->refs > 0U);
    c->refs -= 1U;
    if (c->refs > 0U) {
        return;
    }
    NPH_IGNORE_RESULT(nph_hashmap_remove(&b->copy_index, &c->key));
    free_copy_block(b, c);
    // Out of memory keeps the slot from reuse: it stays free, unused.
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&b->copy_free, index));
}

// A block made again with every point moved by `m` (the entity's scale
// after the Collider's turn): a hull from its moved vertices, a mesh from
// its moved vertices and its triangles, wound again where `m` mirrors.
static NphResult build_copy(PhysicsBridge *b, u32 kind, const void *block, NphMat3 m, void **out)
{
    *out = NULL;
    if (kind == (u32)NPH_COLLISION_SHAPE_HULL) {
        const NphPhysicsHull *h = (const NphPhysicsHull *)block;
        NphVec3 points[NPH_PHYSICS_HULL_MAX_VERTICES];
        const NphVec3 *v = nph_physics_hull_vertices(h);
        for (u32 i = 0U; i < h->vertex_count; ++i) {
            points[i] = nph_mat3_mul_vec3(m, v[i]);
        }
        NphPhysicsHullDesc d;
        memset(&d, 0, sizeof d);
        d.points = points;
        d.point_count = h->vertex_count;
        d.max_vertices = NPH_PHYSICS_HULL_MAX_VERTICES;
        NphPhysicsHull *built = NULL;
        NPH_TRY(nph_physics_hull_build(&d, b->alloc, &built));
        *out = built;
        return NPH_RESULT_OK;
    }
    const NphPhysicsMesh *src = (const NphPhysicsMesh *)block;
    const u64 vbytes = (u64)src->vertex_count * sizeof(NphVec3);
    const u64 tbytes = (u64)src->triangle_count * 3U * sizeof(u32);
    NphVec3 *vertices = (NphVec3 *)NPH_ALLOC(b->alloc, vbytes, 16U);
    u32 *indices = (u32 *)NPH_ALLOC(b->alloc, tbytes, 16U);
    NphResult r = vertices != NULL && indices != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        const NphVec3 *v = nph_physics_mesh_vertices(src);
        for (u32 i = 0U; i < src->vertex_count; ++i) {
            vertices[i] = nph_mat3_mul_vec3(m, v[i]);
        }
        const bool mirrored = nph_mat3_determinant(m) < 0.0F;
        const u32 *t = nph_physics_mesh_triangles(src);
        for (u32 i = 0U; i < src->triangle_count; ++i) {
            const u32 k = 3U * i;
            indices[k] = t[k];
            indices[k + 1U] = t[k + (mirrored ? 2U : 1U)];
            indices[k + 2U] = t[k + (mirrored ? 1U : 2U)];
        }
        NphPhysicsMeshDesc d;
        memset(&d, 0, sizeof d);
        d.vertices = vertices;
        d.vertex_count = src->vertex_count;
        d.indices = indices;
        d.triangle_count = src->triangle_count;
        d.weld_tolerance = -1.0F; // welded when it was cooked: exact copies only
        d.two_sided = (src->flags & (u32)NPH_PHYSICS_MESH_TWO_SIDED) != 0U ? 1 : 0;
        NphPhysicsMesh *built = NULL;
        r = nph_physics_mesh_build(&d, b->alloc, &built);
        *out = built;
    }
    if (vertices != NULL) {
        NPH_FREE(b->alloc, vertices, vbytes, 16U);
    }
    if (indices != NULL) {
        NPH_FREE(b->alloc, indices, tbytes, 16U);
    }
    return r;
}

static NphResult take_copy_slot(PhysicsBridge *b, u32 *out)
{
    if (b->copy_free.len > 0U) {
        *out = NPH_ARRAY_POP(&b->copy_free);
        return NPH_RESULT_OK;
    }
    if (b->copy_count == b->copy_cap) {
        const u32 cap = nph_max_u32(16U, 2U * b->copy_cap);
        ScaledCopy *grown = NPH_ALLOC_ARRAY(b->alloc, ScaledCopy, cap);
        if (grown == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (b->copies != NULL) {
            memcpy(grown, b->copies, (u64)b->copy_count * sizeof *grown);
            NPH_FREE_ARRAY(b->alloc, ScaledCopy, b->copies, b->copy_cap);
        }
        b->copies = grown;
        b->copy_cap = cap;
    }
    *out = b->copy_count++;
    return NPH_RESULT_OK;
}

// The copy of `entry`'s shape `shape` at scale `scale` after turn `turn`,
// shared (a reference taken) or built; NO_COPY when it cannot be (logged).
static u32 copy_of(PhysicsBridge *b, const CollisionEntry *entry, u32 shape, NphVec3 scale,
                   NphQuat turn)
{
    CopyKey key;
    memset(&key, 0, sizeof key);
    key.guid = entry->guid;
    key.shape = shape;
    key.bits[0] = nph_f32_to_bits(scale.x);
    key.bits[1] = nph_f32_to_bits(scale.y);
    key.bits[2] = nph_f32_to_bits(scale.z);
    key.bits[3] = nph_f32_to_bits(turn.x);
    key.bits[4] = nph_f32_to_bits(turn.y);
    key.bits[5] = nph_f32_to_bits(turn.z);
    key.bits[6] = nph_f32_to_bits(turn.w);
    const u32 *known = NPH_HASHMAP_FIND(&b->copy_index, u32, &key);
    if (known != NULL) {
        b->copies[*known].refs += 1U;
        return *known;
    }
    const NphMat3 m =
        nph_mat3_mul(nph_mat3_from_diagonal(scale), nph_mat3_from_quat(nph_quat_normalize(turn)));
    void *block = NULL;
    NphResult r = build_copy(b, entry->kinds[shape], entry->blocks[shape].data, m, &block);
    u32 index = NO_COPY;
    if (r == NPH_RESULT_OK) {
        r = take_copy_slot(b, &index);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_insert(&b->copy_index, &key, &index);
        if (r != NPH_RESULT_OK) {
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&b->copy_free, index));
        }
    }
    if (r != NPH_RESULT_OK) {
        if (block != NULL) {
            ScaledCopy doomed = {key, entry->kinds[shape], 0U, block};
            free_copy_block(b, &doomed);
        }
        log_collision(entry->guid, "cannot be scaled", r);
        return NO_COPY;
    }
    ScaledCopy *c = &b->copies[index];
    c->key = key;
    c->kind = entry->kinds[shape];
    c->refs = 1U;
    c->block = block;
    return index;
}

static void free_copies(PhysicsBridge *b)
{
    for (u32 i = 0U; i < b->copy_count; ++i) {
        if (b->copies[i].kind != 0U) {
            free_copy_block(b, &b->copies[i]); // none left in a record's hands at play end
        }
    }
    b->copy_count = 0U;
    nph_hashmap_clear(&b->copy_index);
    NPH_ARRAY_CLEAR(&b->copy_free);
}

// Waits for its asset: a dynamic body asleep until its shapes come.
static void await_asset(PhysicsBridge *b, BodyRecord *r)
{
    if ((r->flags & BODY_AWAITING) == 0U &&
        NPH_ARRAY_PUSH(&b->awaiting, r->slot) == NPH_RESULT_OK) {
        r->flags |= BODY_AWAITING;
    }
    if (!nph_physics_body_is_null(r->body) &&
        nph_physics_body_type(b->physics, r->body) == NPH_PHYSICS_BODY_DYNAMIC) {
        nph_physics_body_set_awake(b->physics, r->body, false);
    }
}

// An ASSET Collider's shapes on the record's body: every shape of its
// asset, a mesh refused on a dynamic body, in place at a scale of one and
// otherwise from a copy, at the Collider's place (in a part's frame).
static void make_asset_shapes(PhysicsBridge *b, BodyRecord *r)
{
    r->flags &= ~(u32)BODY_UNSHAPED;
    const u32 index = collision_of(b, r->collider.collision);
    if (index == NO_RECORD || b->collisions[index].state == (u32)COLLISION_FAILED) {
        r->flags |= BODY_UNSHAPED;
        return;
    }
    if (b->collisions[index].state == (u32)COLLISION_LOADING) {
        await_asset(b, r);
        return;
    }
    const CollisionEntry *entry = &b->collisions[index];
    const u32 n = entry->shape_count;
    NphPhysicsShape *shapes = NPH_ALLOC_ARRAY(b->alloc, NphPhysicsShape, n);
    u32 *copies = NPH_ALLOC_ARRAY(b->alloc, u32, n);
    if (shapes == NULL || copies == NULL) {
        if (shapes != NULL) {
            NPH_FREE_ARRAY(b->alloc, NphPhysicsShape, shapes, n);
        }
        if (copies != NULL) {
            NPH_FREE_ARRAY(b->alloc, u32, copies, n);
        }
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "entity 0x%08x: no room for its Collider's shapes",
                     r->entity.value);
        r->flags |= BODY_UNSHAPED;
        return;
    }
    r->asset_shapes = shapes;
    r->asset_copies = copies;
    r->asset_count = n;
    const NphCollider *c = &r->collider;
    const NphVec3 scale = r->scale;
    // The Collider's place in the entity's frame, its centre mirrored with
    // it (components.c); its turn applies before the scale.
    NphPhysicsGeometry unused;
    NphPhysicsPose place;
    NphCollider as_box = *c;
    as_box.shape = NPH_COLLIDER_BOX;
    as_box.half_extents = nph_vec3_one();
    NPH_IGNORE_RESULT(nph_collider_shape(&as_box, scale, &unused, &place));
    const bool in_place = nph_f32_to_bits(scale.x) == nph_f32_to_bits(1.0F) &&
                          nph_f32_to_bits(scale.y) == nph_f32_to_bits(1.0F) &&
                          nph_f32_to_bits(scale.z) == nph_f32_to_bits(1.0F);
    const bool dynamic = nph_physics_body_type(b->physics, r->body) == NPH_PHYSICS_BODY_DYNAMIC;
    const bool trigger = c->trigger != 0;
    bool refused = false;
    for (u32 i = 0U; i < n; ++i) {
        r->asset_shapes[i] = NPH_PHYSICS_SHAPE_NULL;
        r->asset_copies[i] = NO_COPY;
        const u32 kind = entry->kinds[i];
        if (kind == (u32)NPH_COLLISION_SHAPE_MESH && (dynamic || trigger)) {
            refused = true; // a mesh bounds no volume to move a body with, or to be inside of
            continue;
        }
        NphPhysicsPose pose = place;
        const void *block = entry->blocks[i].data;
        if (!in_place) {
            r->asset_copies[i] = copy_of(b, entry, i, scale, place.rotation);
            if (r->asset_copies[i] == NO_COPY) {
                continue;
            }
            block = b->copies[r->asset_copies[i]].block;
            pose.rotation = nph_quat_identity(); // the copy is turned already
        }
        const NphPhysicsGeometry g = kind == (u32)NPH_COLLISION_SHAPE_HULL
                                         ? nph_physics_geometry_hull((const NphPhysicsHull *)block)
                                         : nph_physics_geometry_mesh((const NphPhysicsMesh *)block);
        if ((r->flags & BODY_PART) != 0U) {
            pose.position =
                nph_vec3_add(r->frame.position, nph_quat_rotate(r->frame.rotation, pose.position));
            pose.rotation = nph_quat_normalize(nph_quat_mul(r->frame.rotation, pose.rotation));
        }
        NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
        apply_shape_desc(b, r, c, &sd);
        sd.pose = pose;
        const NphResult res =
            nph_physics_shape_create(b->physics, r->body, &sd, &r->asset_shapes[i]);
        if (res != NPH_RESULT_OK) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME, "entity 0x%08x: its Collider's shape %u: %s",
                         r->entity.value, i, nph_result_string(res));
            r->asset_shapes[i] = NPH_PHYSICS_SHAPE_NULL;
            if (r->asset_copies[i] != NO_COPY) {
                release_copy(b, r->asset_copies[i]);
                r->asset_copies[i] = NO_COPY;
            }
        }
    }
    if (refused) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                     "entity 0x%08x: its collision's meshes are left out of %s (use hulls)",
                     r->entity.value, trigger ? "a trigger" : "a dynamic body");
    }
}

// Destroys a record's shapes (`destroy`; a destroyed body took them with
// it) and lets go of what they used: their copies, the arrays of an ASSET
// Collider's, a wait for an asset.
static void drop_shapes(PhysicsBridge *b, BodyRecord *r, bool destroy)
{
    if (destroy && b->physics != NULL) {
        if (!nph_physics_shape_is_null(r->shape)) {
            nph_physics_shape_destroy(b->physics, r->shape);
        }
        for (u32 i = 0U; i < r->asset_count; ++i) {
            if (!nph_physics_shape_is_null(r->asset_shapes[i])) {
                nph_physics_shape_destroy(b->physics, r->asset_shapes[i]);
            }
        }
    }
    r->shape = NPH_PHYSICS_SHAPE_NULL;
    if (r->asset_count > 0U) {
        for (u32 i = 0U; i < r->asset_count; ++i) {
            if (r->asset_copies[i] != NO_COPY) {
                release_copy(b, r->asset_copies[i]);
            }
        }
        NPH_FREE_ARRAY(b->alloc, NphPhysicsShape, r->asset_shapes, r->asset_count);
        NPH_FREE_ARRAY(b->alloc, u32, r->asset_copies, r->asset_count);
    }
    r->asset_shapes = NULL;
    r->asset_copies = NULL;
    r->asset_count = 0U;
    if ((r->flags & BODY_AWAITING) != 0U) {
        remove_slot(&b->awaiting, r->slot);
        r->flags &= ~(u32)BODY_AWAITING;
    }
}

// True while what a Collider's shapes are made of loads: its collision asset
// (ASSET) or its physics material (each load started when it is new).
static bool shapes_wait(PhysicsBridge *b, const NphCollider *c)
{
    bool loading = false;
    if (c->shape == (u32)NPH_COLLIDER_ASSET) {
        const u32 index = collision_of(b, c->collision);
        loading = index != NO_RECORD && b->collisions[index].state == (u32)COLLISION_LOADING;
    }
    return material_loading(b, c) || loading;
}

// A record's shapes from its Collider, on its body, once what they are made
// of has loaded: until then the record waits (await_asset).
static void make_shapes(PhysicsBridge *b, BodyRecord *r)
{
    if (shapes_wait(b, &r->collider)) {
        r->flags &= ~(u32)BODY_UNSHAPED;
        await_asset(b, r);
        return;
    }
    if (r->collider.shape == (u32)NPH_COLLIDER_ASSET) {
        make_asset_shapes(b, r);
    } else {
        make_shape(b, r, &r->collider);
    }
}

// The records whose assets were loading: shapes for those now ready (the
// body woken, its explicit mass over them), none for those that failed.
static void poll_awaiting(PhysicsBridge *b)
{
    for (u32 i = b->awaiting.len; i-- > 0U;) {
        BodyRecord *r = &b->records[b->slot_record[b->awaiting.data[i]]];
        if (shapes_wait(b, &r->collider)) {
            continue;
        }
        remove_slot(&b->awaiting, r->slot);
        r->flags &= ~(u32)BODY_AWAITING;
        if (nph_physics_body_is_null(r->body)) {
            continue;
        }
        make_shapes(b, r);
        nph_physics_body_set_awake(b->physics, r->body, true);
        reweigh(b, (r->flags & BODY_PART) != 0U ? r->owner : r->entity);
    }
}

// Makes the record's body (and shape) from its cached components and pose.
static NphResult make_body(PhysicsBridge *b, BodyRecord *r)
{
    b->body_generation += 1U;
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    const bool rigid = (r->flags & BODY_RIGID) != 0U;
    bd.type = rigid ? body_type(&r->rigid) : NPH_PHYSICS_BODY_STATIC;
    bd.position = r->position;
    bd.rotation = r->rotation;
    if (rigid) {
        bd.linear_velocity = finite_vec3(r->rigid.linear_velocity);
        bd.angular_velocity = finite_vec3(r->rigid.angular_velocity);
        bd.linear_damping = nonneg(r->rigid.linear_damping);
        bd.angular_damping = nonneg(r->rigid.angular_damping);
        bd.gravity_scale = finite_or(r->rigid.gravity_scale, 1.0F);
        bd.locks = r->rigid.locks & 0x3FU;
        bd.enable_sleep = r->rigid.can_sleep != 0 ? 1 : 0;
        bd.bullet = r->rigid.bullet != 0 ? 1 : 0;
    }
    bd.user_data = r->entity.value;
    r->body = NPH_PHYSICS_BODY_NULL;
    r->shape = NPH_PHYSICS_SHAPE_NULL;
    const NphResult res = nph_physics_body_create(b->physics, &bd, &r->body);
    if (res != NPH_RESULT_OK) {
        if ((r->flags & BODY_FAILED) == 0U) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                         "entity 0x%08x: no body (%s) until its components change", r->entity.value,
                         nph_result_string(res));
        }
        r->flags |= BODY_FAILED;
        r->body = NPH_PHYSICS_BODY_NULL;
        return res == NPH_RESULT_OUT_OF_MEMORY ? res : NPH_RESULT_OK;
    }
    r->flags &= ~(u32)BODY_FAILED;
    if ((r->flags & BODY_SHAPED) != 0U) {
        make_shapes(b, r);
    }
    attach_parts(b, r);
    if (rigid && r->rigid.mass > 0.0F) {
        apply_mass(b, r, r->rigid.mass);
    }
    return NPH_RESULT_OK;
}

static bool is_dynamic(const BodyRecord *r)
{
    return (r->flags & BODY_RIGID) != 0U && body_type(&r->rigid) == NPH_PHYSICS_BODY_DYNAMIC;
}

static void set_follows(PhysicsBridge *b, BodyRecord *r, bool follows)
{
    r->flags = follows ? (r->flags | (u32)BODY_PARENTED) : (r->flags & ~(u32)BODY_PARENTED);
    const bool now = (r->flags & BODY_FOLLOWS) != 0U;
    if (follows == now) {
        return;
    }
    if (follows) {
        if (NPH_ARRAY_PUSH(&b->follows, r->slot) != NPH_RESULT_OK) {
            return; // out of memory: it does not follow (logged by the caller's next failure)
        }
        r->flags |= BODY_FOLLOWS;
    } else {
        remove_slot(&b->follows, r->slot);
        r->flags &= ~(u32)BODY_FOLLOWS;
    }
}

static NphResult create_record(PhysicsBridge *b, NphEntity e, NphEntity parent,
                               const NphLocalTransform *local, const NphRigidBody *rigid,
                               const NphCollider *collider)
{
    NPH_TRY(grow_records(b));
    const u32 index = b->count++;
    BodyRecord *r = &b->records[index];
    memset(r, 0, sizeof *r);
    r->entity = e;
    r->slot = nph_world_entity_index(b->world, e);
    b->slot_record[r->slot] = index;
    if (rigid != NULL) {
        r->flags |= BODY_RIGID;
        r->rigid = *rigid;
    }
    if (collider != NULL) {
        r->flags |= BODY_SHAPED;
        r->collider = *collider;
    }
    r->local = *local;
    const EntityPose p = entity_pose_under(b, parent, local);
    r->position = p.position;
    r->rotation = p.rotation;
    r->scale = p.scale;
    set_follows(b, r, p.parented);
    return make_body(b, r);
}

// Moves the body to a new pose of its entity's: a teleport for a dynamic or
// static body (drawn where it lands, not on the way: interpolation reset),
// a move over the step for a kinematic one.
static void move_body(PhysicsBridge *b, BodyRecord *r, const EntityPose *p, f32 dt)
{
    r->position = p->position;
    r->rotation = p->rotation;
    if (nph_physics_body_is_null(r->body)) {
        return;
    }
    const NphPhysicsBodyType type = nph_physics_body_type(b->physics, r->body);
    // Before the first step (the game's play_begin), every body is placed,
    // kinematic ones too: nothing has moved yet.
    if (type != NPH_PHYSICS_BODY_KINEMATIC || b->step == 0U) {
        nph_physics_body_set_transform(b->physics, r->body, p->position, p->rotation);
        if (type == NPH_PHYSICS_BODY_DYNAMIC) {
            nph_physics_body_set_awake(b->physics, r->body, true);
        }
        engine_reset_interpolation(b->engine, r->entity);
        return;
    }
    // The velocities that bring the body's frame there by the step's end:
    // it pushes what it meets on the way, as a moving platform does.
    const NphDVec3 from = nph_physics_body_position(b->physics, r->body);
    const NphQuat from_rotation = nph_physics_body_rotation(b->physics, r->body);
    const f32 inv_dt = 1.0F / dt;
    NphVec3 v = nph_vec3_scale(nph_dvec3_to_vec3(nph_dvec3_sub(p->position, from)), inv_dt);
    NphVec3 axis;
    f32 angle = 0.0F;
    nph_quat_to_axis_angle(
        nph_quat_normalize(nph_quat_mul(p->rotation, nph_quat_conjugate(from_rotation))), &axis,
        &angle);
    NphVec3 w = nph_vec3_scale(axis, angle * inv_dt);
    // The physics module moves the frame's origin by the centre's velocity
    // plus the turn about it: the origin's velocity is v, so the centre's
    // is v + w x (centre - origin).
    const NphVec3 arm = nph_dvec3_to_vec3(
        nph_dvec3_sub(nph_physics_body_center_of_mass(b->physics, r->body), from));
    v = nph_vec3_add(v, nph_vec3_cross(w, arm));
    v = finite_vec3(v);
    w = finite_vec3(w);
    nph_physics_body_set_linear_velocity(b->physics, r->body, v);
    nph_physics_body_set_angular_velocity(b->physics, r->body, w);
    r->target_step = b->step;
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&b->targeted, r->entity.value)); // else it coasts on
}

// The RigidBody's fields that changed since the record's copy, applied.
static void update_rigid(PhysicsBridge *b, BodyRecord *r, const NphRigidBody *now)
{
    const NphRigidBody was = r->rigid;
    r->rigid = *now;
    if (nph_physics_body_is_null(r->body)) {
        return;
    }
    NphPhysicsWorld *w = b->physics;
    if (!vec3_bits_equal(now->linear_velocity, was.linear_velocity)) {
        nph_physics_body_set_linear_velocity(w, r->body, finite_vec3(now->linear_velocity));
    }
    if (!vec3_bits_equal(now->angular_velocity, was.angular_velocity)) {
        nph_physics_body_set_angular_velocity(w, r->body, finite_vec3(now->angular_velocity));
    }
    if (nph_f32_to_bits(now->linear_damping) != nph_f32_to_bits(was.linear_damping) ||
        nph_f32_to_bits(now->angular_damping) != nph_f32_to_bits(was.angular_damping)) {
        nph_physics_body_set_damping(w, r->body, nonneg(now->linear_damping),
                                     nonneg(now->angular_damping));
    }
    if (nph_f32_to_bits(now->gravity_scale) != nph_f32_to_bits(was.gravity_scale)) {
        nph_physics_body_set_gravity_scale(w, r->body, finite_or(now->gravity_scale, 1.0F));
    }
    if (now->locks != was.locks) {
        nph_physics_body_set_locks(w, r->body, now->locks & 0x3FU);
    }
    if (now->can_sleep != was.can_sleep) {
        nph_physics_body_set_sleep(w, r->body, now->can_sleep != 0, 0.05F);
    }
    if (now->bullet != was.bullet) {
        nph_physics_body_set_bullet(w, r->body, now->bullet != 0);
    }
    if (nph_f32_to_bits(now->mass) != nph_f32_to_bits(was.mass)) {
        apply_mass(b, r, now->mass);
    }
}

// An entity of a changed chunk: its record made, or brought up to date.
static NphResult visit(PhysicsBridge *b, NphEntity e, const NphLocalTransform *local,
                       const NphRigidBody *rigid, const NphCollider *collider, f32 dt)
{
    BodyRecord *r = record_of(b, e);
    const NphEntity parent = nph_world_parent(b->world, e);
    if (rigid == NULL && collider != NULL) {
        // A Collider alone: a part of the body above it, or a static body of
        // its own; one that turned from one into the other is made again.
        const NphEntity owner = find_owner(b, parent);
        const bool part = !nph_entity_is_null(owner);
        if (r != NULL &&
            (((r->flags & BODY_PART) != 0U) != part || (part && r->owner.value != owner.value))) {
            destroy_record(b, b->slot_record[r->slot], false);
            r = NULL;
        }
        if (part) {
            if (r == NULL) {
                return create_part(b, e, owner, local, collider);
            }
            update_part(b, r, local, collider);
            return NPH_RESULT_OK;
        }
    }
    if (r == NULL) {
        return create_record(b, e, parent, local, rigid, collider);
    }
    // A body type changes in the physics world only as a new body, which
    // takes its parts' shapes.
    const bool remake = (r->flags & BODY_FAILED) != 0U ||
                        (rigid != NULL && body_type(rigid) != body_type(&r->rigid));
    if (remake) {
        const u32 index = b->slot_record[r->slot];
        destroy_record(b, index, true);
        return create_record(b, e, parent, local, rigid, collider);
    }
    if (rigid != NULL && !rigid_equal(rigid, &r->rigid)) {
        update_rigid(b, r, rigid);
    }
    const EntityPose p = entity_pose_under(b, parent, local);
    const bool rescaled = !vec3_bits_equal(p.scale, r->scale);
    r->scale = p.scale;
    if (collider != NULL && (rescaled || !collider_equal(collider, &r->collider))) {
        r->collider = *collider;
        if (!nph_physics_body_is_null(r->body)) {
            drop_shapes(b, r, true);
            make_shapes(b, r);
            if (rigid != NULL && rigid->mass > 0.0F) {
                apply_mass(b, r, rigid->mass); // over the new shapes'
            }
        }
    }
    if (rescaled) {
        replace_parts(b, r); // placed under the body's new scale
    }
    // A dynamic body moves when the game moves its entity (a teleport), not
    // when its parent moves; a static or kinematic one goes wherever its
    // entity is.
    const bool placed = !dvec3_bits_equal(local->translation, r->local.translation) ||
                        !quat_bits_equal(local->rotation, r->local.rotation);
    const bool elsewhere =
        !dvec3_bits_equal(p.position, r->position) || !quat_bits_equal(p.rotation, r->rotation);
    r->local = *local;
    if (is_dynamic(r) ? placed : elsewhere) {
        move_body(b, r, &p, dt);
    }
    set_follows(b, r, p.parented);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Triggers (ADR-0108)
// ---------------------------------------------------------------------------

// The record of `entity` when `shape` is one of its shapes.
static const BodyRecord *shape_record(const PhysicsBridge *b, u32 entity, NphPhysicsShape shape)
{
    const NphEntity e = {entity};
    const BodyRecord *r = record_of(b, e);
    if (r == NULL) {
        return NULL;
    }
    if (r->shape.value == shape.value) {
        return r;
    }
    for (u32 i = 0U; i < r->asset_count; ++i) {
        if (r->asset_shapes[i].value == shape.value) {
            return r;
        }
    }
    return NULL;
}

static const CharacterRecord *character_of(const PhysicsBridge *b, NphEntity e);

// The entity whose body a visitor's shape is on, when the bridge made the
// shape: a Collider's (its compound's for a part), or a character's ghost.
static bool visitor_body(const PhysicsBridge *b, u32 entity, NphPhysicsShape shape, u32 *out)
{
    const BodyRecord *v = shape_record(b, entity, shape);
    if (v != NULL) {
        *out = (v->flags & BODY_PART) != 0U ? v->owner.value : v->entity.value;
        return true;
    }
    const NphEntity e = {entity};
    const CharacterRecord *c = character_of(b, e);
    if (c != NULL && !nph_physics_body_is_null(c->body) && c->shape.value == shape.value) {
        *out = entity;
        return true;
    }
    return false;
}

// One sensor event into its pair's count: `+1` begun, `-1` ended. A begin
// counts only between shapes the bridge made (the game's own sensors and
// bodies report through the physics world); an end counts where a begin did.
static void count_trigger(PhysicsBridge *b, const NphPhysicsSensorEvent *ev, bool begun)
{
    TriggerKey key;
    key.trigger = (u32)ev->sensor_user_data;
    key.visitor = (u32)ev->visitor_user_data;
    TriggerPair *pair = NPH_HASHMAP_FIND(&b->trigger_pairs, TriggerPair, &key);
    if (begun && pair == NULL) {
        const BodyRecord *t = shape_record(b, key.trigger, ev->sensor);
        u32 body = 0U;
        if (t == NULL || !visitor_body(b, key.visitor, ev->visitor, &body)) {
            return;
        }
        TriggerPair fresh;
        memset(&fresh, 0, sizeof fresh);
        fresh.body = body;
        if (nph_hashmap_insert(&b->trigger_pairs, &key, &fresh) != NPH_RESULT_OK) {
            return; // out of memory: this overlap is not reported
        }
        pair = NPH_HASHMAP_FIND(&b->trigger_pairs, TriggerPair, &key);
    }
    if (pair == NULL || (!begun && pair->count == 0U)) {
        return;
    }
    if (pair->touched == 0U) {
        if (NPH_ARRAY_PUSH(&b->trigger_step, key) != NPH_RESULT_OK) {
            return;
        }
        pair->touched = 1U;
        pair->before = pair->count;
    }
    pair->count = begun ? pair->count + 1U : pair->count - 1U;
}

// The step's sensor events as Collider-level trigger events: the pairs whose
// count left zero entered, those that came back to it left.
static void collect_triggers(PhysicsBridge *b)
{
    NPH_ARRAY_CLEAR(&b->entered);
    NPH_ARRAY_CLEAR(&b->left);
    NPH_ARRAY_CLEAR(&b->trigger_step);
    const NphPhysicsEvents e = nph_physics_world_events(b->physics);
    for (u32 i = 0U; i < e.sensor_end_count; ++i) {
        count_trigger(b, &e.sensor_end[i], false);
    }
    for (u32 i = 0U; i < e.sensor_begin_count; ++i) {
        count_trigger(b, &e.sensor_begin[i], true);
    }
    for (u32 i = 0U; i < b->trigger_step.len; ++i) {
        const TriggerKey key = b->trigger_step.data[i];
        TriggerPair *pair = NPH_HASHMAP_FIND(&b->trigger_pairs, TriggerPair, &key);
        NPH_ASSERT(pair != NULL);
        pair->touched = 0U;
        NphTriggerEvent ev;
        ev.trigger.value = key.trigger;
        ev.visitor.value = key.visitor;
        ev.body.value = pair->body;
        if (pair->before == 0U && pair->count > 0U) {
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&b->entered, ev));
        } else if (pair->before > 0U && pair->count == 0U) {
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&b->left, ev));
        }
        if (pair->count == 0U) {
            NPH_IGNORE_RESULT(nph_hashmap_remove(&b->trigger_pairs, &key));
        }
    }
}

// ---------------------------------------------------------------------------
// Joints (ADR-0108)
// ---------------------------------------------------------------------------

// The body an end of a joint is on, and the entity whose body it is: an
// entity's own (its RigidBody's or Collider's, its compound's for a part),
// or the nearest above it with one.
static NphPhysicsBody joint_end(const PhysicsBridge *b, NphEntity e, NphEntity *out_owner)
{
    *out_owner = NPH_ENTITY_NULL;
    const BodyRecord *r = record_of(b, e);
    NphEntity p = nph_world_parent(b->world, e);
    for (u32 depth = 0U; r == NULL && !nph_entity_is_null(p) && depth < PART_MAX_DEPTH; ++depth) {
        r = record_of(b, p);
        p = nph_world_parent(b->world, p);
    }
    if (r == NULL || nph_physics_body_is_null(r->body)) {
        return NPH_PHYSICS_BODY_NULL;
    }
    *out_owner = (r->flags & BODY_PART) != 0U ? r->owner : r->entity;
    return r->body;
}

// The world's end of joints to no entity: a static body at the origin
// without shapes, made with the first.
static NphPhysicsBody world_anchor(PhysicsBridge *b)
{
    if (nph_physics_body_is_null(b->world_body)) {
        NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
        bd.type = NPH_PHYSICS_BODY_STATIC;
        if (nph_physics_body_create(b->physics, &bd, &b->world_body) != NPH_RESULT_OK) {
            b->world_body = NPH_PHYSICS_BODY_NULL;
        }
    }
    return b->world_body;
}

// Where `point` of an entity's frame is in the world, and the direction
// `dir` of its frame (scale and mirror included; its X for none).
static bool entity_point(const PhysicsBridge *b, NphEntity e, NphVec3 point, NphVec3 dir,
                         NphDVec3 *out_point, NphVec3 *out_dir)
{
    const NphLocalTransform *lt =
        NPH_WORLD_GET(b->world, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    if (lt == NULL) {
        return false;
    }
    const EntityPose p = entity_pose(b, e, lt);
    *out_point = nph_dvec3_add_vec3(
        p.position, nph_quat_rotate(p.rotation, nph_vec3_mul(finite_vec3(point), p.scale)));
    *out_dir =
        nph_vec3_normalize_or(nph_quat_rotate(p.rotation, nph_vec3_mul(finite_vec3(dir), p.scale)),
                              nph_quat_rotate(p.rotation, nph_vec3_unit_x()));
    return true;
}

// The component's settings into the description of its kind (its frames and
// a distance's length as they are).
static void joint_settings(const NphJoint *c, NphPhysicsJointDesc *d)
{
    const bool limit = c->limit != 0;
    const bool motor = c->motor != 0;
    const bool spring = c->spring != 0;
    const f32 lower = finite_or(c->lower, 0.0F);
    const f32 upper = finite_or(c->upper, 0.0F);
    const f32 speed = finite_or(c->motor_speed, 0.0F);
    const f32 force = nonneg(c->motor_force);
    const f32 hertz = finite_or(c->spring_hertz, 0.0F);
    const f32 damping = nonneg(c->spring_damping);
    const f32 target = finite_or(c->spring_target, 0.0F);
    d->collide_connected = c->collide_connected != 0 ? 1 : 0;
    switch (d->type) {
    case NPH_PHYSICS_JOINT_BALL:
        d->ball.enable_swing_limit = limit ? 1 : 0;
        d->ball.swing_y = nph_f32_clamp(upper, 1e-3F, 3.1405927F);
        d->ball.swing_z = d->ball.swing_y;
        break;
    case NPH_PHYSICS_JOINT_HINGE:
        d->hinge.enable_limit = limit ? 1 : 0;
        d->hinge.lower = lower;
        d->hinge.upper = upper;
        d->hinge.enable_motor = motor ? 1 : 0;
        d->hinge.motor_speed = speed;
        d->hinge.max_motor_torque = force;
        d->hinge.enable_spring = spring ? 1 : 0;
        d->hinge.spring_hertz = hertz;
        d->hinge.spring_damping_ratio = damping;
        d->hinge.spring_target = target;
        break;
    case NPH_PHYSICS_JOINT_SLIDER:
        d->slider.enable_limit = limit ? 1 : 0;
        d->slider.lower = lower;
        d->slider.upper = upper;
        d->slider.enable_motor = motor ? 1 : 0;
        d->slider.motor_speed = speed;
        d->slider.max_motor_force = force;
        d->slider.enable_spring = spring ? 1 : 0;
        d->slider.spring_hertz = hertz;
        d->slider.spring_damping_ratio = damping;
        d->slider.spring_target = target;
        break;
    case NPH_PHYSICS_JOINT_DISTANCE:
        d->distance.enable_limit = limit ? 1 : 0;
        d->distance.min_length = nph_f32_max(lower, 0.0F);
        d->distance.max_length = upper;
        d->distance.enable_spring = spring ? 1 : 0;
        d->distance.spring_hertz = hertz;
        d->distance.spring_damping_ratio = damping;
        d->distance.enable_motor = motor ? 1 : 0;
        d->distance.motor_speed = speed;
        d->distance.max_motor_force = force;
        break;
    default:
        break; // fixed: nothing to set
    }
}

static const NphPhysicsJointType k_joint_types[5] = {
    NPH_PHYSICS_JOINT_BALL, NPH_PHYSICS_JOINT_HINGE, NPH_PHYSICS_JOINT_FIXED,
    NPH_PHYSICS_JOINT_SLIDER, NPH_PHYSICS_JOINT_DISTANCE};

// A joint's frames from the world's poses: both at `anchor` with X along
// `axis`, but a distance joint's other frame at `other_anchor`, the two as
// far apart as they are now.
static bool joint_frames(PhysicsBridge *b, JointRecord *j, NphPhysicsBody a, NphPhysicsBody o)
{
    const NphJoint *c = &j->component;
    NphDVec3 at;
    NphVec3 axis;
    if (!entity_point(b, j->entity, c->anchor, c->axis, &at, &axis)) {
        return false;
    }
    NphPhysicsJointDesc *d = &j->desc;
    *d = nph_physics_joint_desc_default(k_joint_types[c->kind], a, o);
    nph_physics_joint_frames_at(b->physics, a, o, at, axis, &d->frame_a, &d->frame_b);
    if (c->kind == (u32)NPH_JOINT_DISTANCE) {
        NphDVec3 end = nph_dvec3_from_vec3(finite_vec3(c->other_anchor));
        NphVec3 unused;
        if (!nph_entity_is_null(c->other) &&
            !entity_point(b, c->other, c->other_anchor, nph_vec3_unit_x(), &end, &unused)) {
            return false;
        }
        NphPhysicsPose ignored;
        nph_physics_joint_frames_at(b->physics, a, o, end, axis, &ignored, &d->frame_b);
        d->distance.length = (f32)nph_dvec3_length(nph_dvec3_sub(end, at));
    }
    return true;
}

// Makes the record's joint between `a` (of `oa`) and `o` (of `oo`): from its
// kept frames, unless it has none or must take them anew.
static void make_joint(PhysicsBridge *b, JointRecord *j, NphPhysicsBody a, NphEntity oa,
                       NphPhysicsBody o, NphEntity oo)
{
    const bool reframe = (j->flags & JOINT_REFRAME) != 0U || oa.value != j->owner_a.value ||
                         oo.value != j->owner_b.value;
    j->body_a = a;
    j->body_b = o;
    j->owner_a = oa;
    j->owner_b = oo;
    j->joint = NPH_PHYSICS_JOINT_NULL;
    j->flags &= ~(u32)(JOINT_DIRTY | JOINT_FAILED);
    NphResult r = NPH_RESULT_INVALID_ARGUMENT;
    if (j->component.kind <= (u32)NPH_JOINT_DISTANCE) {
        r = NPH_RESULT_OK;
        if (reframe || j->desc.type != k_joint_types[j->component.kind]) {
            r = joint_frames(b, j, a, o) ? NPH_RESULT_OK : NPH_RESULT_NOT_FOUND;
        }
    }
    if (r == NPH_RESULT_OK) {
        j->flags &= ~(u32)JOINT_REFRAME;
        j->desc.body_a = a;
        j->desc.body_b = o;
        j->desc.user_data = j->entity.value;
        joint_settings(&j->component, &j->desc);
        r = nph_physics_joint_create(b->physics, &j->desc, &j->joint);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                     "entity 0x%08x: its Joint cannot be made (%s) until it or a body changes",
                     j->entity.value, nph_result_string(r));
        j->joint = NPH_PHYSICS_JOINT_NULL;
        j->flags |= JOINT_FAILED;
    }
}

static bool joint_made(const PhysicsBridge *b, const JointRecord *j)
{
    return !nph_physics_joint_is_null(j->joint) && nph_physics_joint_is_valid(b->physics, j->joint);
}

static void unmake_joint(PhysicsBridge *b, JointRecord *j)
{
    if (joint_made(b, j)) {
        nph_physics_joint_destroy(b->physics, j->joint);
    }
    j->joint = NPH_PHYSICS_JOINT_NULL;
}

static void drop_joint(PhysicsBridge *b, u32 index)
{
    JointRecord *j = &b->joints[index];
    unmake_joint(b, j);
    NPH_IGNORE_RESULT(nph_hashmap_remove(&b->joint_index, &j->entity.value));
    const u32 last = b->joint_count - 1U;
    if (index != last) {
        b->joints[index] = b->joints[last];
        NPH_IGNORE_RESULT(nph_hashmap_set(&b->joint_index, &b->joints[index].entity.value, &index));
    }
    b->joint_count = last;
}

static void drop_joint_of(PhysicsBridge *b, NphEntity e)
{
    const u32 *index = NPH_HASHMAP_FIND(&b->joint_index, u32, &e.value);
    if (index != NULL && b->joints[*index].entity.value == e.value) {
        drop_joint(b, *index);
    }
}

static NphResult grow_joints(PhysicsBridge *b)
{
    if (b->joint_count < b->joint_cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(16U, 2U * b->joint_cap);
    JointRecord *grown = NPH_ALLOC_ARRAY(b->alloc, JointRecord, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->joints != NULL) {
        memcpy(grown, b->joints, (u64)b->joint_count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, JointRecord, b->joints, b->joint_cap);
    }
    b->joints = grown;
    b->joint_cap = cap;
    return NPH_RESULT_OK;
}

// Joints by their fields' bits, as Colliders are (collider_equal).
static bool joint_equal(const NphJoint *a, const NphJoint *b)
{
    return a->kind == b->kind && a->other.value == b->other.value &&
           vec3_bits_equal(a->anchor, b->anchor) && vec3_bits_equal(a->axis, b->axis) &&
           vec3_bits_equal(a->other_anchor, b->other_anchor) && a->limit == b->limit &&
           f32_bits_equal(a->lower, b->lower) && f32_bits_equal(a->upper, b->upper) &&
           a->motor == b->motor && f32_bits_equal(a->motor_speed, b->motor_speed) &&
           f32_bits_equal(a->motor_force, b->motor_force) && a->spring == b->spring &&
           f32_bits_equal(a->spring_hertz, b->spring_hertz) &&
           f32_bits_equal(a->spring_damping, b->spring_damping) &&
           f32_bits_equal(a->spring_target, b->spring_target) &&
           a->collide_connected == b->collide_connected &&
           f32_bits_equal(a->break_force, b->break_force) && a->broken == b->broken;
}

// A Joint seen changed (or new): what changed decides whether the next make
// keeps the frames.
static NphResult visit_joint(PhysicsBridge *b, NphEntity e, const NphJoint *c)
{
    const u32 *known = NPH_HASHMAP_FIND(&b->joint_index, u32, &e.value);
    if (known == NULL) {
        NPH_TRY(grow_joints(b));
        const u32 index = b->joint_count;
        NPH_TRY(nph_hashmap_insert(&b->joint_index, &e.value, &index));
        JointRecord *j = &b->joints[index];
        memset(j, 0, sizeof *j);
        j->entity = e;
        j->component = *c;
        j->flags = (u32)JOINT_REFRAME | (c->broken != 0 ? (u32)JOINT_BROKEN : (u32)JOINT_DIRTY);
        b->joint_count += 1U;
        return NPH_RESULT_OK;
    }
    JointRecord *j = &b->joints[*known];
    if (joint_equal(&j->component, c)) {
        return NPH_RESULT_OK;
    }
    const NphJoint was = j->component;
    j->component = *c;
    unmake_joint(b, j);
    j->flags &= ~(u32)(JOINT_FAILED | JOINT_BROKEN);
    if (c->broken != 0) {
        j->flags |= JOINT_BROKEN; // the game broke it
        return NPH_RESULT_OK;
    }
    j->flags |= JOINT_DIRTY;
    const bool structure = was.kind != c->kind || was.other.value != c->other.value ||
                           !vec3_bits_equal(was.anchor, c->anchor) ||
                           !vec3_bits_equal(was.axis, c->axis) ||
                           !vec3_bits_equal(was.other_anchor, c->other_anchor) ||
                           was.collide_connected != c->collide_connected;
    if (structure || was.broken != 0) {
        j->flags |= JOINT_REFRAME; // anew where things are (joined again after breaking too)
    }
    return NPH_RESULT_OK;
}

// The Joints of changed chunks (every one in a full pass).
static NphResult joint_pass(PhysicsBridge *b, bool full, u32 since, u32 tick)
{
    NphQueryIter it;
    NphChunkView v;
    if (full) {
        nph_query_iter(b->joint_query, &it);
    } else {
        nph_query_iter_since(b->joint_query, since, tick, &it);
    }
    while (nph_query_next(&it, &v)) {
        const NphJoint *c = NPH_CHUNK_COLUMN(&v, 0, NphJoint);
        for (u32 i = 0; i < v.count; ++i) {
            NPH_TRY(visit_joint(b, v.entities[i], &c[i]));
        }
    }
    return NPH_RESULT_OK;
}

// Every joint made where both its bodies exist, made again where a body
// was, or where its component changed. A joint settled since the last body
// record came or went, its component unchanged, is as it was: looking its
// bodies up again cost 1000 joints 14 us a step (engine_frame_physics_1k_
// hinged), which checking their breaking forces, 55 us, now outweighs.
static void settle_joints(PhysicsBridge *b)
{
    for (u32 i = 0U; i < b->joint_count; ++i) {
        JointRecord *j = &b->joints[i];
        if ((j->flags & JOINT_BROKEN) != 0U ||
            (j->generation == b->body_generation && (j->flags & JOINT_DIRTY) == 0U)) {
            continue;
        }
        j->generation = b->body_generation;
        NphEntity oa;
        NphEntity oo = NPH_ENTITY_NULL;
        const NphPhysicsBody a = joint_end(b, j->entity, &oa);
        const NphPhysicsBody o = nph_entity_is_null(j->component.other)
                                     ? world_anchor(b)
                                     : joint_end(b, j->component.other, &oo);
        const bool same = a.value == j->body_a.value && o.value == j->body_b.value;
        const bool dirty = (j->flags & JOINT_DIRTY) != 0U;
        if (same && !dirty && (joint_made(b, j) || (j->flags & JOINT_FAILED) != 0U)) {
            continue; // as made, or failed for these bodies (logged)
        }
        unmake_joint(b, j);
        if (nph_physics_body_is_null(a) || nph_physics_body_is_null(o)) {
            continue; // waiting for its bodies
        }
        make_joint(b, j, a, oa, o, oo);
    }
}

// Joints whose last step's force passed their strength break: `broken`
// written true, the joint left unmade until it goes false.
static void break_joints(PhysicsBridge *b)
{
    const NphRuntimeComponents *ids = &b->engine->ids;
    for (u32 i = 0U; i < b->joint_count; ++i) {
        JointRecord *j = &b->joints[i];
        const f32 strength = j->component.break_force;
        if (!(nph_f32_is_finite(strength) && strength > 0.0F) || !joint_made(b, j)) {
            continue;
        }
        NphVec3 force;
        NphVec3 torque;
        nph_physics_joint_reaction(b->physics, j->joint, &force, &torque);
        if (!(nph_vec3_length_sq(force) > strength * strength)) {
            continue;
        }
        unmake_joint(b, j);
        j->flags |= JOINT_BROKEN;
        j->component.broken = 1; // as written below: the change pass sees nothing new
        NphJoint *c = NPH_WORLD_GET_MUT(b->world, j->entity, ids->joint, NphJoint);
        if (c != NULL) {
            c->broken = 1;
        }
    }
}

// ---------------------------------------------------------------------------
// Characters (ADR-0108)
// ---------------------------------------------------------------------------

// The surfaces a move reports, for its pushes and the velocity it keeps (a move slides at most
// its four iterations, each meeting a surface or two).
#define CHARACTER_HITS 8U

// The settings a character is made from: its capsule, what its moves climb and meet. Its push,
// its gravity, and the game's commands are read at every move.
static bool character_settings_equal(const NphCharacterController *a,
                                     const NphCharacterController *b)
{
    return f32_bits_equal(a->radius, b->radius) && f32_bits_equal(a->height, b->height) &&
           f32_bits_equal(a->step_height, b->step_height) &&
           f32_bits_equal(a->max_slope, b->max_slope) &&
           f32_bits_equal(a->snap_distance, b->snap_distance) && a->category == b->category &&
           a->mask == b->mask;
}

// The physics module's description of a CharacterController's character: its capsule upright
// (a ball under twice its radius in height), the physics module's skin and slides, its limits,
// and its filter. False when that is no character (a radius that is not positive and finite).
static bool character_desc(const NphCharacterController *c, NphPhysicsCharacterDesc *d)
{
    if (!(nph_f32_is_finite(c->radius) && c->radius > 0.0F)) {
        return false;
    }
    const f32 half = nph_f32_max(0.5F * nonneg(c->height) - c->radius, 0.0F);
    *d = nph_physics_character_desc_default(nph_physics_geometry_capsule(half, c->radius));
    d->max_slope = nph_f32_clamp(finite_or(c->max_slope, d->max_slope), 0.0F, 1.5533F);
    d->step_height = nonneg(c->step_height);
    d->snap_distance = nonneg(c->snap_distance);
    d->filter.category = c->category;
    d->filter.mask = c->mask;
    return nph_physics_character_desc_validate(d) == NPH_RESULT_OK;
}

// How far a character's capsule's centre is above its feet, the entity's origin: a skin under
// its bottom, so a character placed on a floor stands on it.
static f32 character_rise(const CharacterRecord *r)
{
    return r->desc.skin + r->desc.shape.capsule.half_height + r->desc.shape.capsule.radius;
}

static NphDVec3 character_feet(const CharacterRecord *r)
{
    return nph_dvec3_add_vec3(r->state.position, nph_vec3(0.0F, -character_rise(r), 0.0F));
}

// Where the entity is in the world: its feet.
static NphDVec3 entity_feet(const PhysicsBridge *b, NphEntity e, const NphLocalTransform *lt)
{
    return entity_pose(b, e, lt).position;
}

// What the character stands on, and that body's pose: what carries it at its next move (none
// off the ground).
static void note_carrier(PhysicsBridge *b, CharacterRecord *r)
{
    r->carrier = NPH_PHYSICS_BODY_NULL;
    const NphPhysicsBody g = r->state.ground_body;
    if (r->state.ground != NPH_PHYSICS_CHARACTER_WALKABLE || nph_physics_body_is_null(g)) {
        return;
    }
    r->carrier = g;
    r->carrier_position = nph_physics_body_position(b->physics, g);
    r->carrier_rotation = nph_physics_body_rotation(b->physics, g);
}

// Where the point the character stood on went since its last move, with the body it stood on:
// what that body's motion carries it by. Nothing when the body went or did not move (exactly:
// its pose's bits). A rise under a micrometre is a level motion's rounding, and none: any rise
// keeps the physics module's move from walking (character.h), which climbs steps and keeps to
// the ground.
static NphVec3 carried(const PhysicsBridge *b, const CharacterRecord *r)
{
    const NphPhysicsBody g = r->carrier;
    if (nph_physics_body_is_null(g) || !nph_physics_body_is_valid(b->physics, g)) {
        return nph_vec3_zero();
    }
    const NphDVec3 position = nph_physics_body_position(b->physics, g);
    const NphQuat rotation = nph_physics_body_rotation(b->physics, g);
    if (dvec3_bits_equal(position, r->carrier_position) &&
        quat_bits_equal(rotation, r->carrier_rotation)) {
        return nph_vec3_zero();
    }
    const NphVec3 arm = nph_quat_rotate_inverse(
        r->carrier_rotation, nph_dvec3_relative(r->state.ground_point, r->carrier_position));
    const NphDVec3 now = nph_dvec3_add_vec3(position, nph_quat_rotate(rotation, arm));
    NphVec3 d = finite_vec3(nph_dvec3_relative(now, r->state.ground_point));
    if (nph_f32_abs(d.y) < 1e-6F) {
        d.y = 0.0F;
    }
    return d;
}

// The character where `feet` are, its ground found there, at rest; its body put there.
static void place_character(PhysicsBridge *b, CharacterRecord *r, NphDVec3 feet)
{
    const NphDVec3 center = nph_dvec3_add_vec3(feet, nph_vec3(0.0F, character_rise(r), 0.0F));
    NPH_IGNORE_RESULT(
        nph_physics_character_init(b->physics, &r->desc, center, nph_quat_identity(), &r->state));
    r->fall = nph_vec3_zero();
    note_carrier(b, r);
    if (!nph_physics_body_is_null(r->body)) {
        nph_physics_body_set_transform(b->physics, r->body, center, nph_quat_identity());
        nph_physics_body_set_linear_velocity(b->physics, r->body, nph_vec3_zero());
    }
}

static void unmake_character(PhysicsBridge *b, CharacterRecord *r)
{
    if (b->physics != NULL && !nph_physics_body_is_null(r->body)) {
        nph_physics_body_destroy(b->physics, r->body);
    }
    r->body = NPH_PHYSICS_BODY_NULL;
    r->shape = NPH_PHYSICS_SHAPE_NULL;
}

// The character's body (kinematic, its capsule a ghost that rays and triggers see by its
// filter) and state where `feet` are, from its component; none, logged once until it changes,
// when the component is no character. A body that cannot be made is logged: the character
// moves without one, unseen.
static void make_character(PhysicsBridge *b, CharacterRecord *r, const NphCharacterController *c,
                           NphDVec3 feet)
{
    unmake_character(b, r);
    r->settings = *c;
    if (!character_desc(c, &r->desc)) {
        if ((r->flags & CHARACTER_INVALID) == 0U) {
            NPH_LOG_WARN(&NPH_LOG_RUNTIME,
                         "entity 0x%08x: its CharacterController is no capsule; it does not move "
                         "until it changes",
                         r->entity.value);
        }
        r->flags |= CHARACTER_INVALID;
        return;
    }
    r->flags &= ~(u32)CHARACTER_INVALID;
    NphPhysicsBodyDesc bd = nph_physics_body_desc_default();
    bd.type = NPH_PHYSICS_BODY_KINEMATIC;
    bd.position = nph_dvec3_add_vec3(feet, nph_vec3(0.0F, character_rise(r), 0.0F));
    bd.user_data = r->entity.value;
    NphResult res = nph_physics_body_create(b->physics, &bd, &r->body);
    if (res == NPH_RESULT_OK) {
        NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(r->desc.shape);
        sd.ghost = 1;
        sd.contact_events = 0;
        sd.filter.category = c->category;
        sd.filter.mask = c->mask;
        sd.filter.group = 0;
        sd.user_data = r->entity.value;
        res = nph_physics_shape_create(b->physics, r->body, &sd, &r->shape);
        if (res != NPH_RESULT_OK) {
            unmake_character(b, r);
        }
    } else {
        r->body = NPH_PHYSICS_BODY_NULL;
    }
    if (res != NPH_RESULT_OK) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "entity 0x%08x: no body for its character (%s): unseen",
                     r->entity.value, nph_result_string(res));
    }
    // Its own moves pass through its own body.
    r->desc.filter.ignore = r->body;
    place_character(b, r, feet);
}

// The translation of `e`'s LocalTransform that puts it at `position` in the world: against its
// parent's world transform (its scale and shear divided out, as local_from_pose does), or the
// position itself for a root; `keep` for a degenerate parent.
static NphDVec3 local_translation(const PhysicsBridge *b, NphEntity e, NphDVec3 position,
                                  NphDVec3 keep)
{
    const NphWorldTransform *pw = parent_transform(b, e);
    if (pw == NULL) {
        return position;
    }
    NphMat3 inv;
    if (!nph_mat3_inverse(nph_world_transform_linear(pw), &inv)) {
        return keep;
    }
    const NphVec3 d = nph_dvec3_to_vec3(nph_dvec3_sub(position, pw->translation));
    return nph_dvec3_from_vec3(nph_mat3_mul_vec3(inv, d));
}

// A character's step: where it goes for the game's commands, its ground's motion and gravity,
// what it pushes on the way, the velocity it keeps, its body put where it went, and all of it
// written back.
static void move_character(PhysicsBridge *b, CharacterRecord *r, NphCharacterController *c,
                           NphLocalTransform *lt, f32 dt)
{
    NphPhysicsWorld *w = b->physics;
    const NphVec3 up = nph_vec3_unit_y();
    const NphDVec3 from = r->state.position;
    const NphVec3 move = finite_vec3(c->move);
    const NphVec3 walk = nph_vec3_mul_add(move, up, -nph_vec3_dot(move, up)); // along the ground
    NphVec3 displacement;
    if (r->state.ground == NPH_PHYSICS_CHARACTER_WALKABLE) {
        // On its feet: it walks, what it stands on carries it, and it may jump. Should this
        // move leave the ground, it keeps its ground's velocity and the jump's.
        const f32 jump = nph_f32_is_finite(c->jump) && c->jump > 0.0F ? c->jump : 0.0F;
        displacement = nph_vec3_add(nph_vec3_scale(walk, dt), carried(b, r));
        displacement = nph_vec3_mul_add(displacement, up, jump * dt);
        r->fall = nph_vec3_mul_add(r->state.ground_velocity, up, jump);
    } else {
        // Off its feet (in the air, or on a slope too steep to stand on): gravity pulls it, and
        // it keeps what it has.
        const NphVec3 g = nph_physics_world_gravity(w);
        r->fall = nph_vec3_mul_add(r->fall, g, finite_or(c->gravity_scale, 1.0F) * dt);
        displacement = nph_vec3_scale(nph_vec3_add(walk, r->fall), dt);
    }
    displacement = finite_vec3(displacement);
    NphPhysicsCharacterHit hits[CHARACTER_HITS];
    u32 hit_count = 0U;
    if (nph_physics_character_move(w, &r->desc, &r->state, displacement, hits, CHARACTER_HITS,
                                   &hit_count) != NPH_RESULT_OK) {
        hit_count = 0U; // not for a valid description and a finite displacement
    }
    const f32 inv_dt = 1.0F / dt;
    const f32 push = nonneg(c->push_mass);
    if (push > 0.0F && hit_count > 0U) {
        nph_physics_character_push(w, hits, hit_count, push, nph_vec3_scale(displacement, inv_dt));
    }
    // What it ran into takes its part of the velocity it keeps: a ceiling stops a jump, a wall
    // the way into it, a steep slope all but the way along it.
    for (u32 i = 0U; i < hit_count; ++i) {
        const f32 into = nph_vec3_dot(r->fall, hits[i].normal);
        if (into < 0.0F) {
            r->fall = nph_vec3_mul_add(r->fall, hits[i].normal, -into);
        }
    }
    note_carrier(b, r);
    const NphDVec3 to = r->state.position;
    if (!nph_physics_body_is_null(r->body)) {
        // Its body goes there over the step, as a kinematic platform does, its proxy kept by the
        // step's refit: a teleport moves a proxy in the tree and looks for its pairs again at
        // every step (0.8 us a character). Triggers and rays after the step see it where the
        // character went; the other characters' moves see it where it was.
        const NphVec3 to_go = nph_dvec3_relative(to, nph_physics_body_position(w, r->body));
        nph_physics_body_set_linear_velocity(w, r->body,
                                             finite_vec3(nph_vec3_scale(to_go, inv_dt)));
    }
    lt->translation = local_translation(b, r->entity, character_feet(r), lt->translation);
    r->written = lt->translation;
    const bool standing = r->state.ground == NPH_PHYSICS_CHARACTER_WALKABLE;
    c->velocity = nph_vec3_scale(nph_dvec3_relative(to, from), inv_dt);
    c->grounded = standing ? 1 : 0;
    c->ground = standing ? physics_bridge_entity(b, r->state.ground_body) : NPH_ENTITY_NULL;
    c->jump = 0.0F;
}

static NphResult grow_characters(PhysicsBridge *b)
{
    if (b->character_count < b->character_cap) {
        return NPH_RESULT_OK;
    }
    const u32 cap = nph_max_u32(8U, 2U * b->character_cap);
    CharacterRecord *grown = NPH_ALLOC_ARRAY(b->alloc, CharacterRecord, cap);
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    if (b->characters != NULL) {
        memcpy(grown, b->characters, (u64)b->character_count * sizeof *grown);
        NPH_FREE_ARRAY(b->alloc, CharacterRecord, b->characters, b->character_cap);
    }
    b->characters = grown;
    b->character_cap = cap;
    return NPH_RESULT_OK;
}

static void drop_character(PhysicsBridge *b, u32 index)
{
    CharacterRecord *r = &b->characters[index];
    unmake_character(b, r);
    NPH_IGNORE_RESULT(nph_hashmap_remove(&b->character_index, &r->entity.value));
    const u32 last = b->character_count - 1U;
    if (index != last) {
        b->characters[index] = b->characters[last];
        NPH_IGNORE_RESULT(
            nph_hashmap_set(&b->character_index, &b->characters[index].entity.value, &index));
    }
    b->character_count = last;
}

static const CharacterRecord *character_of(const PhysicsBridge *b, NphEntity e)
{
    const u32 *index = NPH_HASHMAP_FIND(&b->character_index, u32, &e.value);
    return index != NULL && b->characters[*index].entity.value == e.value ? &b->characters[*index]
                                                                          : NULL;
}

// A character the pass met: made where its entity is when new, made again where it stands when
// its settings changed, put where the game wrote its translation; with `move`, moved.
static NphResult visit_character(PhysicsBridge *b, NphEntity e, NphCharacterController *c,
                                 NphLocalTransform *lt, bool move, f32 dt)
{
    const u32 *known = NPH_HASHMAP_FIND(&b->character_index, u32, &e.value);
    CharacterRecord *r = NULL;
    if (known == NULL) {
        NPH_TRY(grow_characters(b));
        const u32 index = b->character_count;
        NPH_TRY(nph_hashmap_insert(&b->character_index, &e.value, &index));
        b->character_count += 1U;
        r = &b->characters[index];
        memset(r, 0, sizeof *r);
        r->entity = e;
        r->written = lt->translation;
        make_character(b, r, c, entity_feet(b, e, lt));
    } else {
        r = &b->characters[*known];
        const bool placed = !dvec3_bits_equal(lt->translation, r->written);
        r->written = lt->translation;
        if (!character_settings_equal(c, &r->settings)) {
            const bool was_valid = (r->flags & CHARACTER_INVALID) == 0U;
            make_character(b, r, c,
                           was_valid && !placed ? character_feet(r) : entity_feet(b, e, lt));
        } else if (placed && (r->flags & CHARACTER_INVALID) == 0U) {
            place_character(b, r, entity_feet(b, e, lt)); // the game's teleport
        }
    }
    r->mark = b->character_mark;
    if (move && (r->flags & CHARACTER_INVALID) == 0U) {
        move_character(b, r, c, lt, dt);
    }
    return NPH_RESULT_OK;
}

// Every character: made, brought up to date and, with `move`, moved for this step (each seeing
// the others' bodies where they were when the step began, whatever their order); those the
// pass did not meet (despawned, their component or LocalTransform gone) dropped with their
// bodies.
static NphResult character_pass(PhysicsBridge *b, bool move, u32 tick, f32 dt)
{
    if (b->character_count == 0U && nph_query_archetype_count(b->character_query) == 0U) {
        return NPH_RESULT_OK; // a world that never had a character pays nothing for them
    }
    b->character_mark += 1U;
    NphQueryIter it;
    NphChunkView v;
    nph_query_iter_since(b->character_query, 0U, tick, &it);
    NphResult r = NPH_RESULT_OK;
    while (r == NPH_RESULT_OK && nph_query_next(&it, &v)) {
        NphCharacterController *c = NPH_CHUNK_COLUMN(&v, 0, NphCharacterController);
        NphLocalTransform *lt = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; r == NPH_RESULT_OK && i < v.count; ++i) {
            r = visit_character(b, v.entities[i], &c[i], &lt[i], move, dt);
        }
    }
    if (r != NPH_RESULT_OK) {
        return r; // the records not met yet are kept: the next pass meets them
    }
    for (u32 i = b->character_count; i-- > 0U;) {
        if (b->characters[i].mark != b->character_mark) {
            drop_character(b, i);
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

static bool qualifies(const PhysicsBridge *b, NphEntity e)
{
    const NphRuntimeComponents *ids = &b->engine->ids;
    return nph_world_has(b->world, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM) &&
           (nph_world_has(b->world, e, ids->rigid_body) ||
            nph_world_has(b->world, e, ids->collider));
}

// The records of entities that went, or whose components changed in kind
// (made again by the change pass, which sees their new chunks), from the
// events since the last read; every record checked when events were lost.
static void read_events(PhysicsBridge *b)
{
    u32 count = 0U;
    u32 dropped = 0U;
    const NphWorldEvent *events = nph_world_events(b->world, &count, &dropped);
    const NphRuntimeComponents *ids = &b->engine->ids;
    for (u32 i = b->event_cursor; i < count && (b->count > 0U || b->joint_count > 0U); ++i) {
        const NphWorldEvent *ev = &events[i];
        if (ev->type == NPH_WORLD_EVENT_DESPAWNED ||
            (ev->type == NPH_WORLD_EVENT_REMOVED && ev->component == ids->joint)) {
            drop_joint_of(b, ev->entity);
        }
        bool drop = ev->type == NPH_WORLD_EVENT_DESPAWNED;
        if (ev->type == NPH_WORLD_EVENT_ADDED || ev->type == NPH_WORLD_EVENT_REMOVED) {
            // A CharacterController coming takes the place of the entity's
            // body, and one going gives it back (its character goes by the
            // character pass, which no longer meets it).
            const bool character = ev->component == ids->character_controller;
            drop = ev->component == ids->rigid_body || ev->component == ids->collider ||
                   ev->component == NPH_COMPONENT_ID_LOCAL_TRANSFORM || character;
            if (ev->component == ids->rigid_body ||
                ev->component == NPH_COMPONENT_ID_LOCAL_TRANSFORM || character) {
                // Colliders below it may have become parts of its body, or
                // stopped being: every one is looked at again.
                b->full_pass = 1;
            }
        }
        if (ev->type == NPH_WORLD_EVENT_PARENT_CHANGED) {
            // Its pose in the world may have jumped, and whether it follows
            // a parent changed: made again where it is now.
            drop = true;
        }
        if (!drop) {
            continue;
        }
        const u32 index = record_index(b, ev->entity);
        if (index != NO_RECORD) {
            destroy_record(b, index, false);
        }
        if (ev->type == NPH_WORLD_EVENT_PARENT_CHANGED) {
            b->full_pass = 1; // its chunk may not have changed
        }
    }
    b->event_cursor = count;
    if (dropped == b->events_dropped) {
        return;
    }
    b->events_dropped = dropped;
    for (u32 i = b->count; i-- > 0U;) {
        if (i < b->count && (!nph_world_is_alive(b->world, b->records[i].entity) ||
                             !qualifies(b, b->records[i].entity))) {
            destroy_record(b, i, false);
        }
    }
    for (u32 i = b->joint_count; i-- > 0U;) {
        const NphEntity e = b->joints[i].entity;
        if (!nph_world_is_alive(b->world, e) || !nph_world_has(b->world, e, ids->joint)) {
            drop_joint(b, i);
        }
    }
    b->full_pass = 1;
}

static NphResult change_pass(PhysicsBridge *b, u32 since, u32 tick, f32 dt)
{
    const bool full = b->full_pass != 0;
    b->full_pass = 0;
    NphQueryIter it;
    NphChunkView v;
    if (full) {
        nph_query_iter(b->rigid_query, &it);
    } else {
        nph_query_iter_since(b->rigid_query, since, tick, &it);
    }
    while (nph_query_next(&it, &v)) {
        const NphRigidBody *rb = NPH_CHUNK_COLUMN(&v, 0, NphRigidBody);
        const NphLocalTransform *lt = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        const NphCollider *c = NPH_CHUNK_COLUMN(&v, 2, NphCollider);
        for (u32 i = 0; i < v.count; ++i) {
            NPH_TRY(visit(b, v.entities[i], &lt[i], &rb[i], c != NULL ? &c[i] : NULL, dt));
        }
    }
    if (full) {
        nph_query_iter(b->static_query, &it);
    } else {
        nph_query_iter_since(b->static_query, since, tick, &it);
    }
    while (nph_query_next(&it, &v)) {
        const NphCollider *c = NPH_CHUNK_COLUMN(&v, 0, NphCollider);
        const NphLocalTransform *lt = NPH_CHUNK_COLUMN(&v, 1, NphLocalTransform);
        for (u32 i = 0; i < v.count; ++i) {
            NPH_TRY(visit(b, v.entities[i], &lt[i], NULL, &c[i], dt));
        }
    }
    return joint_pass(b, full, since, tick);
}

// Static and kinematic bodies under parents follow them.
static void follow_parents(PhysicsBridge *b, f32 dt)
{
    for (u32 i = 0; i < b->follows.len; ++i) {
        BodyRecord *r = &b->records[b->slot_record[b->follows.data[i]]];
        if (is_dynamic(r)) {
            continue; // it moves in the world: its LocalTransform follows it (write_back)
        }
        const EntityPose p = entity_pose(b, r->entity, &r->local);
        if (!dvec3_bits_equal(p.position, r->position) ||
            !quat_bits_equal(p.rotation, r->rotation)) {
            move_body(b, r, &p, dt);
        }
    }
}

// Kinematic bodies the last step moved to a target stop, unless this step
// moves them again.
static void stop_targets(PhysicsBridge *b)
{
    for (u32 i = 0; i < b->last_targeted.len; ++i) {
        const NphEntity e = {b->last_targeted.data[i]};
        const BodyRecord *r = record_of(b, e);
        if (r == NULL || r->target_step == b->step || nph_physics_body_is_null(r->body) ||
            nph_physics_body_type(b->physics, r->body) != NPH_PHYSICS_BODY_KINEMATIC) {
            continue;
        }
        nph_physics_body_set_linear_velocity(b->physics, r->body, nph_vec3_zero());
        nph_physics_body_set_angular_velocity(b->physics, r->body, nph_vec3_zero());
    }
}

// The view of the chunk holding `e` for writing, a chunk viewed once for a
// run of entities in it (a moving pile's bodies sit in few chunks); false
// when the entity is not one the write-back reaches.
typedef struct WriteView {
    void *chunk;
    NphChunkView view;
} WriteView;

static bool write_view(PhysicsBridge *b, WriteView *wv, NphEntity e, u32 tick, u32 *row)
{
    void *chunk = nph_world_entity_chunk(b->world, e, row);
    if (chunk == wv->chunk) {
        return chunk != NULL; // viewed, so matched
    }
    if (chunk == NULL || !nph_query_matches_chunk(b->write_query, chunk)) {
        return false;
    }
    nph_query_chunk_view(b->write_query, chunk, tick, &wv->view);
    wv->chunk = chunk;
    return true;
}

// Writes a body's pose into its entity's LocalTransform (against its parent
// as it was at the step's start) and its velocities into its RigidBody.
static void write_body(PhysicsBridge *b, BodyRecord *r, const WriteView *wv, u32 row,
                       NphDVec3 position, NphQuat rotation, NphVec3 linear, NphVec3 angular)
{
    NphRigidBody *rb = &NPH_CHUNK_COLUMN(&wv->view, 0, NphRigidBody)[row];
    NphLocalTransform *lt = &NPH_CHUNK_COLUMN(&wv->view, 1, NphLocalTransform)[row];
    const bool parented = (r->flags & BODY_PARENTED) != 0U;
    if (parented) {
        local_from_pose(b, r->entity, position, rotation, lt);
    } else {
        lt->translation = position; // a root's: no parent to look up
        lt->rotation = rotation;
    }
    rb->linear_velocity = linear;
    rb->angular_velocity = angular;
    // The rest of both is the record's already (the change pass, which ran
    // before the step, made them equal, and nothing has run since).
    r->rigid.linear_velocity = linear;
    r->rigid.angular_velocity = angular;
    r->local.translation = lt->translation;
    r->local.rotation = lt->rotation;
    // The pose the next step compares with: the one the LocalTransform now
    // gives (for a root, exactly the body's).
    if (parented) {
        const EntityPose p = entity_pose(b, r->entity, lt);
        r->position = p.position;
        r->rotation = p.rotation;
    } else {
        r->position = lt->translation;
        r->rotation = body_rotation(lt->rotation);
    }
}

// The bodies the step moved, into their entities; and dynamic bodies under
// parents, their LocalTransforms against where the parents are now, so that
// they stay where their bodies are when a parent moves.
static void write_back(PhysicsBridge *b, u32 tick)
{
    const NphPhysicsEvents events = nph_physics_world_events(b->physics);
    WriteView wv;
    memset(&wv, 0, sizeof wv);
    for (u32 i = 0; i < events.move_count; ++i) {
        const NphPhysicsBodyMoveEvent *m = &events.move[i];
        const NphEntity e = {(u32)m->user_data};
        BodyRecord *r = record_of(b, e);
        u32 row = 0U;
        if (r == NULL || (r->flags & BODY_RIGID) == 0U || r->body.value != m->body.value ||
            !write_view(b, &wv, e, tick, &row)) {
            continue; // a body the game made itself
        }
        write_body(b, r, &wv, row, m->position, m->rotation, m->linear_velocity,
                   m->angular_velocity);
    }
    for (u32 i = 0; i < b->follows.len; ++i) {
        BodyRecord *r = &b->records[b->slot_record[b->follows.data[i]]];
        if (!is_dynamic(r) || nph_physics_body_is_null(r->body)) {
            continue;
        }
        const NphDVec3 position = nph_physics_body_position(b->physics, r->body);
        const NphQuat rotation = nph_physics_body_rotation(b->physics, r->body);
        NphLocalTransform at = r->local;
        local_from_pose(b, r->entity, position, rotation, &at);
        const bool moved = !dvec3_bits_equal(at.translation, r->local.translation) ||
                           !quat_bits_equal(at.rotation, r->local.rotation);
        u32 row = 0U;
        if (moved && write_view(b, &wv, r->entity, tick, &row)) {
            write_body(b, r, &wv, row, position, rotation,
                       nph_physics_body_linear_velocity(b->physics, r->body),
                       nph_physics_body_angular_velocity(b->physics, r->body));
        }
    }
}

static void physics_system(NphSystemContext *ctx)
{
    PhysicsBridge *b = (PhysicsBridge *)ctx->user;
    if (b->physics == NULL || b->world != ctx->world) {
        return;
    }
    NPH_PROFILE_BEGIN("runtime.physics_changes");
    const f32 dt = (f32)ctx->dt;
    b->step += 1U;
    // A step that fails reports no trigger events, as the physics world none of its own.
    NPH_ARRAY_CLEAR(&b->entered);
    NPH_ARRAY_CLEAR(&b->left);
    // The last step's targets, then this step's.
    const NphArrayU32 last = b->last_targeted;
    b->last_targeted = b->targeted;
    b->targeted = last;
    NPH_ARRAY_CLEAR(&b->targeted);
    read_events(b);
    if (b->material_reloads > 0U) {
        poll_reloads(b);
    }
    poll_awaiting(b);
    NphResult r = change_pass(b, ctx->last_tick, ctx->tick, dt);
    place_deep_parts(b);
    settle_joints(b);
    follow_parents(b, dt);
    stop_targets(b);
    NPH_PROFILE_END();
    if (r == NPH_RESULT_OK &&
        (b->character_count > 0U || nph_query_archetype_count(b->character_query) > 0U)) {
        // With every body where the game put it: the characters' moves, before the step.
        NPH_PROFILE_BEGIN("runtime.physics_characters");
        r = character_pass(b, true, ctx->tick, dt);
        NPH_PROFILE_END();
    }
    if (r == NPH_RESULT_OK) {
        NPH_PROFILE_BEGIN("runtime.physics_step");
        r = nph_physics_world_step(b->physics, dt, b->engine->jobs);
        NPH_PROFILE_END();
    }
    if (r == NPH_RESULT_OK) {
        NPH_PROFILE_BEGIN("runtime.physics_write_back");
        write_back(b, ctx->tick);
        collect_triggers(b);
        break_joints(b);
        b->stats = nph_physics_world_stats(b->physics);
        NPH_PROFILE_END();
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_RUNTIME, "the physics step: %s", nph_result_string(r));
        nph_system_report(ctx, r);
    }
}

// ---------------------------------------------------------------------------
// The bridge
// ---------------------------------------------------------------------------

static void destroy_queries(PhysicsBridge *b)
{
    nph_query_destroy(b->rigid_query);
    nph_query_destroy(b->static_query);
    nph_query_destroy(b->write_query);
    nph_query_destroy(b->joint_query);
    nph_query_destroy(b->character_query);
    b->rigid_query = NULL;
    b->static_query = NULL;
    b->write_query = NULL;
    b->joint_query = NULL;
    b->character_query = NULL;
}

static NphResult make_queries(PhysicsBridge *b)
{
    const NphRuntimeComponents *ids = &b->engine->ids;
    // An entity with a CharacterController is its character, whatever else it has.
    const NphQueryTerm rigid_terms[4] = {
        nph_term_changed(nph_term_read(ids->rigid_body)),
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_LOCAL_TRANSFORM)),
        nph_term_changed(nph_term_optional(ids->collider, NPH_ACCESS_READ)),
        nph_term_without(ids->character_controller)};
    const NphQueryTerm static_terms[4] = {
        nph_term_changed(nph_term_read(ids->collider)),
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_LOCAL_TRANSFORM)),
        nph_term_without(ids->rigid_body), nph_term_without(ids->character_controller)};
    const NphQueryTerm write_terms[2] = {nph_term_write(ids->rigid_body),
                                         nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    const NphQueryTerm joint_terms[2] = {nph_term_changed(nph_term_read(ids->joint)),
                                         nph_term_read(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    const NphQueryTerm character_terms[2] = {nph_term_write(ids->character_controller),
                                             nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    const NphQueryDesc rq = {rigid_terms, 4U};
    const NphQueryDesc sq = {static_terms, 4U};
    const NphQueryDesc wq = {write_terms, 2U};
    const NphQueryDesc jq = {joint_terms, 2U};
    const NphQueryDesc cq = {character_terms, 2U};
    NphResult r = nph_query_create(b->world, &rq, &b->rigid_query);
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &sq, &b->static_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &wq, &b->write_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &jq, &b->joint_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &cq, &b->character_query);
    }
    if (r != NPH_RESULT_OK) {
        destroy_queries(b);
    }
    return r;
}

NphResult physics_bridge_create(NphEngine *e, PhysicsBridge **out)
{
    NPH_ASSERT(e != NULL && out != NULL);
    *out = NULL;
    NphAllocator *alloc = engine_alloc(e, ENGINE_ALLOC_PHYSICS);
    PhysicsBridge *b = NPH_ALLOC_TYPE(alloc, PhysicsBridge);
    if (b == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memset(b, 0, sizeof *b);
    b->engine = e;
    b->alloc = alloc;
    NPH_ARRAY_INIT(&b->follows, alloc);
    NPH_ARRAY_INIT(&b->deep_parts, alloc);
    NPH_ARRAY_INIT(&b->awaiting, alloc);
    NPH_ARRAY_INIT(&b->copy_free, alloc);
    NPH_ARRAY_INIT(&b->trigger_step, alloc);
    NPH_ARRAY_INIT(&b->entered, alloc);
    NPH_ARRAY_INIT(&b->left, alloc);
    NPH_ARRAY_INIT(&b->targeted, alloc);
    NPH_ARRAY_INIT(&b->last_targeted, alloc);
    NphHashMapDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.key_size = (u32)sizeof(NphGuid);
    cd.val_size = (u32)sizeof(u32);
    NphHashMapDesc kd = cd;
    kd.key_size = (u32)sizeof(CopyKey);
    NphHashMapDesc td = cd;
    td.key_size = (u32)sizeof(TriggerKey);
    td.val_size = (u32)sizeof(TriggerPair);
    NphHashMapDesc jd = cd;
    jd.key_size = (u32)sizeof(u32);
    NphResult r = nph_hashmap_init(&b->collision_index, alloc, &cd);
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&b->copy_index, alloc, &kd);
        if (r != NPH_RESULT_OK) {
            nph_hashmap_free(&b->collision_index);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&b->trigger_pairs, alloc, &td);
        if (r != NPH_RESULT_OK) {
            nph_hashmap_free(&b->collision_index);
            nph_hashmap_free(&b->copy_index);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&b->joint_index, alloc, &jd);
        if (r != NPH_RESULT_OK) {
            nph_hashmap_free(&b->collision_index);
            nph_hashmap_free(&b->copy_index);
            nph_hashmap_free(&b->trigger_pairs);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&b->material_index, alloc, &cd);
        if (r != NPH_RESULT_OK) {
            nph_hashmap_free(&b->collision_index);
            nph_hashmap_free(&b->copy_index);
            nph_hashmap_free(&b->trigger_pairs);
            nph_hashmap_free(&b->joint_index);
        }
    }
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_init(&b->character_index, alloc, &jd);
        if (r != NPH_RESULT_OK) {
            nph_hashmap_free(&b->collision_index);
            nph_hashmap_free(&b->copy_index);
            nph_hashmap_free(&b->trigger_pairs);
            nph_hashmap_free(&b->joint_index);
            nph_hashmap_free(&b->material_index);
        }
    }
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(alloc, PhysicsBridge, b);
        return r;
    }
    *out = b;
    return NPH_RESULT_OK;
}

void physics_bridge_destroy(PhysicsBridge *b)
{
    if (b == NULL) {
        return;
    }
    physics_bridge_end(b);
    destroy_queries(b);
    if (b->records != NULL) {
        NPH_FREE_ARRAY(b->alloc, BodyRecord, b->records, b->cap);
    }
    if (b->slot_record != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_record, b->slot_cap);
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_parts, b->slot_cap);
    }
    NPH_ARRAY_FREE(&b->follows);
    NPH_ARRAY_FREE(&b->deep_parts);
    NPH_ARRAY_FREE(&b->awaiting);
    NPH_ARRAY_FREE(&b->copy_free);
    if (b->collisions != NULL) {
        NPH_FREE_ARRAY(b->alloc, CollisionEntry, b->collisions, b->collision_cap);
    }
    if (b->copies != NULL) {
        NPH_FREE_ARRAY(b->alloc, ScaledCopy, b->copies, b->copy_cap);
    }
    nph_hashmap_free(&b->collision_index);
    nph_hashmap_free(&b->copy_index);
    nph_hashmap_free(&b->trigger_pairs);
    nph_hashmap_free(&b->joint_index);
    nph_hashmap_free(&b->material_index);
    if (b->materials != NULL) {
        NPH_FREE_ARRAY(b->alloc, MaterialEntry, b->materials, b->material_cap);
    }
    if (b->joints != NULL) {
        NPH_FREE_ARRAY(b->alloc, JointRecord, b->joints, b->joint_cap);
    }
    nph_hashmap_free(&b->character_index);
    if (b->characters != NULL) {
        NPH_FREE_ARRAY(b->alloc, CharacterRecord, b->characters, b->character_cap);
    }
    NPH_ARRAY_FREE(&b->trigger_step);
    NPH_ARRAY_FREE(&b->entered);
    NPH_ARRAY_FREE(&b->left);
    NPH_ARRAY_FREE(&b->targeted);
    NPH_ARRAY_FREE(&b->last_targeted);
    NPH_FREE_TYPE(b->alloc, PhysicsBridge, b);
}

NphResult physics_bridge_set_world(PhysicsBridge *b, NphWorld *world)
{
    NPH_ASSERT(b != NULL);
    destroy_queries(b);
    b->world = world;
    b->event_cursor = 0U;
    b->events_dropped = 0U;
    if (world == NULL) {
        physics_bridge_end(b);
        return NPH_RESULT_OK;
    }
    NphResult r = make_queries(b);
    if (r == NPH_RESULT_OK && b->physics != NULL) {
        // A world rebuilt for a game's new component layouts keeps every
        // handle (and slot): the bodies stay, and the next step compares
        // every entity with them (the rebuild's own events say nothing new).
        (void)nph_world_events(world, &b->event_cursor, &b->events_dropped);
        r = size_slots(b);
        for (u32 i = b->count; r == NPH_RESULT_OK && i-- > 0U;) {
            if (i < b->count && (!nph_world_is_alive(world, b->records[i].entity) ||
                                 !qualifies(b, b->records[i].entity))) {
                destroy_record(b, i, false);
            }
        }
        for (u32 i = b->joint_count; i-- > 0U;) {
            const NphEntity e = b->joints[i].entity;
            if (!nph_world_is_alive(world, e) || !nph_world_has(world, e, b->engine->ids.joint)) {
                drop_joint(b, i);
            }
        }
        // Characters keep theirs too; one the world no longer has goes at the next step's
        // character pass, which does not meet it.
        b->full_pass = 1;
    }
    return r;
}

static NphPhysicsWorldDesc world_desc(const PhysicsBridge *b)
{
    const EngineCvars *cv = &b->engine->cv;
    NphPhysicsWorldDesc d = nph_physics_world_desc_default();
    d.alloc = b->alloc;
    d.solver = (NphPhysicsSolver)nph_cvar_get_int(&cv->phys_solver);
    d.substeps = (u32)nph_cvar_get_int(&cv->phys_substeps);
    d.enable_sleep = nph_cvar_get_bool(&cv->phys_sleep) ? 1 : 0;
    d.enable_continuous = nph_cvar_get_bool(&cv->phys_continuous) ? 1 : 0;
    return d;
}

NphResult physics_bridge_begin(PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL && b->world != NULL && b->physics == NULL);
    const NphPhysicsWorldDesc d = world_desc(b);
    NPH_TRY(nph_physics_world_create(&d, &b->physics));
    b->step = 0U;
    b->full_pass = 1;
    // The bodies are made from the world as it is: what its events say
    // happened before is in it already.
    (void)nph_world_events(b->world, &b->event_cursor, &b->events_dropped);
    NphResult r = size_slots(b);
    if (r == NPH_RESULT_OK) {
        // Every body before the game's play_begin, which may join them, its
        // collision assets loaded first.
        b->load_sync = 1;
        r = change_pass(b, 0U, nph_world_tick(b->world),
                        (f32)nph_fixed_clock_step_seconds(&b->engine->clock));
        b->load_sync = 0;
    }
    if (r == NPH_RESULT_OK) {
        settle_joints(b); // the joints too, before play_begin, and the characters' bodies
        r = character_pass(b, false, nph_world_tick(b->world), 0.0F);
    }
    if (r != NPH_RESULT_OK) {
        physics_bridge_end(b);
    }
    return r;
}

NphResult physics_bridge_refresh(PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (b->physics == NULL || b->world == NULL) {
        return NPH_RESULT_OK;
    }
    read_events(b);
    b->full_pass = 1;
    b->load_sync = 1; // what play_begin made has its shapes before the first step
    NphResult r = change_pass(b, 0U, nph_world_tick(b->world),
                              (f32)nph_fixed_clock_step_seconds(&b->engine->clock));
    b->load_sync = 0;
    if (r == NPH_RESULT_OK) {
        settle_joints(b);
        r = character_pass(b, false, nph_world_tick(b->world), 0.0F);
    }
    return r;
}

void physics_bridge_end(PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (b->physics == NULL) {
        return;
    }
    destroy_all(b);
    free_copies(b);
    release_collisions(b);
    release_materials(b);
    nph_hashmap_clear(&b->trigger_pairs);
    NPH_ARRAY_CLEAR(&b->trigger_step);
    NPH_ARRAY_CLEAR(&b->entered);
    NPH_ARRAY_CLEAR(&b->left);
    b->joint_count = 0U; // their joints go with the world
    nph_hashmap_clear(&b->joint_index);
    b->character_count = 0U; // and the characters' bodies
    nph_hashmap_clear(&b->character_index);
    b->world_body = NPH_PHYSICS_BODY_NULL;
    nph_physics_world_destroy(b->physics);
    b->physics = NULL;
    memset(&b->stats, 0, sizeof b->stats);
}

NphResult physics_bridge_add_system(PhysicsBridge *b, NphSchedule *schedule)
{
    NPH_ASSERT(b != NULL && schedule != NULL);
    const NphRuntimeComponents *ids = &b->engine->ids;
    const NphComponentId reads[3] = {ids->collider, NPH_COMPONENT_ID_WORLD_TRANSFORM,
                                     NPH_COMPONENT_ID_HIERARCHY};
    // Joint's `broken` is written when one breaks, and CharacterController's what a character
    // did at every step.
    const NphComponentId writes[4] = {ids->rigid_body, NPH_COMPONENT_ID_LOCAL_TRANSFORM, ids->joint,
                                      ids->character_controller};
    // After the game's systems (registered before it), which move bodies,
    // and before the world transforms are composed from where they went.
    const char *const before[1] = {NPH_TRANSFORM_SYSTEM_NAME};
    NphSystemDesc d;
    memset(&d, 0, sizeof d);
    d.name = NPH_PHYSICS_SYSTEM_NAME;
    d.run = physics_system;
    d.user = b;
    d.reads = reads;
    d.read_count = 3U;
    d.writes = writes;
    d.write_count = 4U;
    d.before = before;
    d.before_count = 1U;
    // Alone in its stage: nothing runs beside the step, so game systems may
    // use the physics world (queries, characters, joints) in the stages
    // before and after it.
    d.flags = (u32)NPH_SYSTEM_MAIN_THREAD;
    return nph_schedule_add_system(schedule, &d, NULL);
}

void physics_bridge_sync(PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (b->physics != NULL && b->world != NULL) {
        read_events(b);
    }
    // The engine clears the world's events (and their drop count) next.
    b->event_cursor = 0U;
    b->events_dropped = 0U;
}

static void draw_line(void *user, NphVec3 a, NphVec3 c, u32 rgba)
{
    (void)nph_render_debug_line((NphRenderer *)user, a, c, rgba, 0U);
}

void physics_bridge_draw(const PhysicsBridge *b, NphRenderer *r, u32 flags)
{
    NPH_ASSERT(b != NULL && r != NULL);
    if (b->physics == NULL || flags == 0U) {
        return;
    }
    NphPhysicsDebugDraw d;
    d.line = draw_line;
    d.user = r;
    d.origin = nph_render_debug_origin(r);
    d.flags = flags;
    nph_physics_world_draw(b->physics, &d);
}

NphPhysicsWorld *physics_bridge_world(const PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    return b->physics;
}

NphPhysicsBody physics_bridge_body(const PhysicsBridge *b, NphEntity e)
{
    NPH_ASSERT(b != NULL);
    const BodyRecord *r = record_of(b, e);
    if (r != NULL) {
        return r->body;
    }
    const CharacterRecord *c = b->physics != NULL ? character_of(b, e) : NULL;
    return c != NULL ? c->body : NPH_PHYSICS_BODY_NULL;
}

NphEntity physics_bridge_entity(const PhysicsBridge *b, NphPhysicsBody body)
{
    NPH_ASSERT(b != NULL);
    if (b->physics == NULL || !nph_physics_body_is_valid(b->physics, body)) {
        return NPH_ENTITY_NULL;
    }
    // Each of the bridge's bodies holds its entity in its user data; a body
    // of the game's own holds what the game chose, so the entity's record
    // must hold this very body.
    const NphEntity e = {(u32)nph_physics_body_user_data(b->physics, body)};
    const BodyRecord *r = record_of(b, e);
    if (r != NULL) {
        return r->body.value == body.value ? e : NPH_ENTITY_NULL;
    }
    const CharacterRecord *c = character_of(b, e);
    return c != NULL && c->body.value == body.value ? e : NPH_ENTITY_NULL;
}

NphPhysicsJoint physics_bridge_joint(const PhysicsBridge *b, NphEntity e)
{
    NPH_ASSERT(b != NULL);
    if (b->physics == NULL || nph_entity_is_null(e)) {
        return NPH_PHYSICS_JOINT_NULL;
    }
    const u32 *index = NPH_HASHMAP_FIND(&b->joint_index, u32, &e.value);
    if (index == NULL || b->joints[*index].entity.value != e.value ||
        !joint_made(b, &b->joints[*index])) {
        return NPH_PHYSICS_JOINT_NULL;
    }
    return b->joints[*index].joint;
}

bool physics_bridge_reload(PhysicsBridge *b, NphGuid guid)
{
    NPH_ASSERT(b != NULL);
    if (b->physics == NULL || nph_guid_is_null(guid)) {
        return false;
    }
    const u32 *known = NPH_HASHMAP_FIND(&b->material_index, u32, &guid);
    if (known == NULL) {
        return false;
    }
    MaterialEntry *m = &b->materials[*known];
    NphAssetLoader *loader = nph_engine_loader(b->engine);
    // The file was written again: a load from now on reads it as it is, and one in flight read
    // it as it was.
    nph_asset_forget(loader, nph_asset_find_guid(loader, guid));
    if (!nph_asset_handle_is_null(m->asset)) {
        nph_asset_release(loader, m->asset);
        m->asset = NPH_ASSET_HANDLE_NULL;
    }
    const NphResult r = nph_asset_load_guid_async(loader, guid, NULL, &m->asset);
    if (r != NPH_RESULT_OK) {
        m->asset = NPH_ASSET_HANDLE_NULL;
        log_material(guid, "cannot be loaded again; it keeps the values it had", r);
        if (m->state == (u32)MATERIAL_LOADING) {
            m->state = (u32)MATERIAL_FAILED; // its first load gone: its Colliders' own values
        }
        if (m->reloading != 0) {
            m->reloading = 0;
            b->material_reloads -= 1U;
        }
        return true;
    }
    // A first load started again is still the first: the shapes naming it wait on.
    if (m->state != (u32)MATERIAL_LOADING && m->reloading == 0) {
        m->reloading = 1;
        b->material_reloads += 1U;
    }
    return true;
}

NphResult physics_bridge_material_set(PhysicsBridge *b, NphGuid guid,
                                      const NphPhysicsMaterialHeader *values)
{
    NPH_ASSERT(b != NULL && values != NULL);
    if (b->physics == NULL || nph_guid_is_null(guid)) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (!nph_physics_material_header_valid(values)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 *known = NPH_HASHMAP_FIND(&b->material_index, u32, &guid);
    if (known == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    const u32 index = *known;
    MaterialEntry *m = &b->materials[index];
    // What is set is newer than any load in flight, the first or a reload's.
    if (!nph_asset_handle_is_null(m->asset)) {
        nph_asset_release(nph_engine_loader(b->engine), m->asset);
        m->asset = NPH_ASSET_HANDLE_NULL;
    }
    if (m->reloading != 0) {
        m->reloading = 0;
        b->material_reloads -= 1U;
    }
    m->surface = header_surface(values);
    m->state = (u32)MATERIAL_READY; // Colliders waiting for it make their shapes at the next step
    resurface(b, index);
    return NPH_RESULT_OK;
}

NphTriggerEvents physics_bridge_triggers(const PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    NphTriggerEvents t;
    t.entered = b->entered.data;
    t.entered_count = b->entered.len;
    t.left = b->left.data;
    t.left_count = b->left.len;
    return t;
}

const NphPhysicsStats *physics_bridge_stats(const PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    return &b->stats;
}
