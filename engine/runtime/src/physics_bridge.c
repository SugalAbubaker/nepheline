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
#include <nepheline/core/assert.h>
#include <nepheline/core/profile.h>
#include <nepheline/math/mat.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/shape.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/scene/query.h>
#include <nepheline/scene/transform.h>

#include "engine_internal.h"

#include <string.h>

#define NO_RECORD NPH_U32_MAX

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
} BodyRecord;

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
    u32 slot_cap;
    NphArrayU32 follows;       // slots of the records under parents (BODY_FOLLOWS)
    NphArrayU32 targeted;      // entities whose kinematic bodies this step moves to a target
    NphArrayU32 last_targeted; // the last step's
    u32 event_cursor;          // world events read so far
    u32 events_dropped;        // the stream's drop count when last read
    b32 full_pass;             // look at every chunk at the next step
    u32 step;                  // steps since play began
    NphPhysicsStats stats;
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

static void destroy_record(PhysicsBridge *b, u32 index)
{
    NPH_ASSERT(index < b->count);
    BodyRecord *r = &b->records[index];
    if (b->physics != NULL && !nph_physics_body_is_null(r->body)) {
        nph_physics_body_destroy(b->physics, r->body); // its shape too
    }
    if ((r->flags & BODY_FOLLOWS) != 0U) {
        remove_slot(&b->follows, r->slot);
    }
    b->slot_record[r->slot] = NO_RECORD;
    const u32 last = b->count - 1U;
    if (index != last) {
        b->records[index] = b->records[last];
        b->slot_record[b->records[index].slot] = index;
    }
    b->count = last;
}

static void destroy_all(PhysicsBridge *b)
{
    while (b->count > 0U) {
        destroy_record(b, b->count - 1U);
    }
    NPH_ARRAY_CLEAR(&b->follows);
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
    if (grown == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < cap; ++i) {
        grown[i] = i < b->slot_cap ? b->slot_record[i] : NO_RECORD;
    }
    if (b->slot_record != NULL) {
        NPH_FREE_ARRAY(b->alloc, u32, b->slot_record, b->slot_cap);
    }
    b->slot_record = grown;
    b->slot_cap = cap;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Poses
// ---------------------------------------------------------------------------

typedef struct EntityPose {
    NphDVec3 position;
    NphQuat rotation;
    NphVec3 scale; // absolute, per local axis
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
           a->mask == b->mask && a->group == b->group && a->hit_events == b->hit_events;
}

// A rotation a body takes (unit within the physics module's 1e-3): a
// LocalTransform's as it is when it is unit to rounding, so a body written
// back reads back unchanged, else normalised (the identity for zero).
static NphQuat body_rotation(NphQuat q)
{
    return nph_quat_is_normalized(q, 1e-6F) ? q : nph_quat_normalize_or_identity(q);
}

// The rotation of a frame's axes with their scale and shear taken out
// (Gram-Schmidt from X; a mirrored frame turns into the rotation of its
// unmirrored twin).
static NphQuat axes_rotation(NphVec3 xa, NphVec3 ya)
{
    const NphVec3 x = nph_vec3_normalize_or(xa, nph_vec3_unit_x());
    NphVec3 y = nph_vec3_sub(ya, nph_vec3_scale(x, nph_vec3_dot(ya, x)));
    y = nph_vec3_normalize_or(y, nph_vec3_perpendicular(x));
    const NphVec3 z = nph_vec3_cross(x, y);
    return nph_quat_normalize(nph_quat_from_mat3(nph_mat3_from_cols(x, y, z)));
}

static const NphWorldTransform *parent_transform(const PhysicsBridge *b, NphEntity e)
{
    const NphEntity parent = nph_world_parent(b->world, e);
    if (nph_entity_is_null(parent)) {
        return NULL;
    }
    return NPH_WORLD_GET(b->world, parent, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
}

// Where the entity is in the world: its LocalTransform for a root, composed
// with its parent's world transform (as of the step's start) otherwise.
static EntityPose entity_pose(const PhysicsBridge *b, NphEntity e, const NphLocalTransform *local)
{
    EntityPose p;
    const NphWorldTransform *pw = parent_transform(b, e);
    if (pw == NULL) {
        p.position = local->translation;
        p.rotation = body_rotation(local->rotation);
        p.scale = nph_vec3_abs(local->scale);
        p.parented = false;
        return p;
    }
    const NphWorldTransform w = nph_world_transform_compose(pw, local);
    p.position = w.translation;
    p.rotation = axes_rotation(w.x_axis, w.y_axis);
    p.scale =
        nph_vec3(nph_vec3_length(w.x_axis), nph_vec3_length(w.y_axis), nph_vec3_length(w.z_axis));
    p.parented = true;
    return p;
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
    const NphQuat parent_rotation = axes_rotation(pw->x_axis, pw->y_axis);
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
    if (nph_physics_shape_is_null(r->shape) || !(m.mass > 0.0F)) {
        m.center = nph_vec3_zero();
        m.inertia = nph_mat3_from_diagonal(nph_vec3_splat(0.1F * mass));
    } else {
        m.inertia = nph_mat3_scale(m.inertia, mass / m.mass);
    }
    m.mass = mass;
    nph_physics_body_set_mass_data(b->physics, r->body, &m);
}

static NphPhysicsFilter collider_filter(const NphCollider *c)
{
    NphPhysicsFilter f;
    f.category = c->category;
    f.mask = c->mask;
    f.group = c->group;
    return f;
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
                     "entity 0x%08x: its Collider is no shape at its scale; its body has none",
                     r->entity.value);
        r->flags |= BODY_UNSHAPED;
        return;
    }
    NphPhysicsShapeDesc sd = nph_physics_shape_desc_default(g);
    sd.pose = pose;
    sd.density = nonneg(c->density);
    sd.friction = nonneg(c->friction);
    sd.restitution = nph_f32_clamp(finite_or(c->restitution, 0.0F), 0.0F, 1.0F);
    sd.filter = collider_filter(c);
    sd.hit_events = c->hit_events != 0 ? 1 : 0;
    sd.user_data = r->entity.value;
    const NphResult res = nph_physics_shape_create(b->physics, r->body, &sd, &r->shape);
    if (res != NPH_RESULT_OK) {
        NPH_LOG_WARN(&NPH_LOG_RUNTIME, "entity 0x%08x: its Collider's shape: %s", r->entity.value,
                     nph_result_string(res));
        r->shape = NPH_PHYSICS_SHAPE_NULL;
        r->flags |= BODY_UNSHAPED;
    }
}

// Makes the record's body (and shape) from its cached components and pose.
static NphResult make_body(PhysicsBridge *b, BodyRecord *r)
{
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
        make_shape(b, r, &r->collider);
    }
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

static NphResult create_record(PhysicsBridge *b, NphEntity e, const NphLocalTransform *local,
                               const NphRigidBody *rigid, const NphCollider *collider)
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
    const EntityPose p = entity_pose(b, e, local);
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
    if (r == NULL) {
        return create_record(b, e, local, rigid, collider);
    }
    // A body type changes in the physics world only as a new body.
    const bool remake = (r->flags & BODY_FAILED) != 0U ||
                        (rigid != NULL && body_type(rigid) != body_type(&r->rigid));
    if (remake) {
        const u32 index = b->slot_record[r->slot];
        destroy_record(b, index);
        return create_record(b, e, local, rigid, collider);
    }
    if (rigid != NULL && !rigid_equal(rigid, &r->rigid)) {
        update_rigid(b, r, rigid);
    }
    const EntityPose p = entity_pose(b, e, local);
    const bool rescaled = !vec3_bits_equal(p.scale, r->scale);
    r->scale = p.scale;
    if (collider != NULL && (rescaled || !collider_equal(collider, &r->collider))) {
        r->collider = *collider;
        if (!nph_physics_body_is_null(r->body)) {
            if (!nph_physics_shape_is_null(r->shape)) {
                nph_physics_shape_destroy(b->physics, r->shape);
            }
            make_shape(b, r, collider);
            if (rigid != NULL && rigid->mass > 0.0F) {
                apply_mass(b, r, rigid->mass); // over the new shape's
            }
        }
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
    for (u32 i = b->event_cursor; i < count && b->count > 0U; ++i) {
        const NphWorldEvent *ev = &events[i];
        bool drop = ev->type == NPH_WORLD_EVENT_DESPAWNED;
        if (ev->type == NPH_WORLD_EVENT_ADDED || ev->type == NPH_WORLD_EVENT_REMOVED) {
            drop = ev->component == ids->rigid_body || ev->component == ids->collider ||
                   ev->component == NPH_COMPONENT_ID_LOCAL_TRANSFORM;
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
            destroy_record(b, index);
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
        if (!nph_world_is_alive(b->world, b->records[i].entity) ||
            !qualifies(b, b->records[i].entity)) {
            destroy_record(b, i);
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
    return NPH_RESULT_OK;
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
    // The last step's targets, then this step's.
    const NphArrayU32 last = b->last_targeted;
    b->last_targeted = b->targeted;
    b->targeted = last;
    NPH_ARRAY_CLEAR(&b->targeted);
    read_events(b);
    NphResult r = change_pass(b, ctx->last_tick, ctx->tick, dt);
    follow_parents(b, dt);
    stop_targets(b);
    NPH_PROFILE_END();
    if (r == NPH_RESULT_OK) {
        NPH_PROFILE_BEGIN("runtime.physics_step");
        r = nph_physics_world_step(b->physics, dt, b->engine->jobs);
        NPH_PROFILE_END();
    }
    if (r == NPH_RESULT_OK) {
        NPH_PROFILE_BEGIN("runtime.physics_write_back");
        write_back(b, ctx->tick);
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
    b->rigid_query = NULL;
    b->static_query = NULL;
    b->write_query = NULL;
}

static NphResult make_queries(PhysicsBridge *b)
{
    const NphRuntimeComponents *ids = &b->engine->ids;
    const NphQueryTerm rigid_terms[3] = {
        nph_term_changed(nph_term_read(ids->rigid_body)),
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_LOCAL_TRANSFORM)),
        nph_term_changed(nph_term_optional(ids->collider, NPH_ACCESS_READ))};
    const NphQueryTerm static_terms[3] = {
        nph_term_changed(nph_term_read(ids->collider)),
        nph_term_changed(nph_term_read(NPH_COMPONENT_ID_LOCAL_TRANSFORM)),
        nph_term_without(ids->rigid_body)};
    const NphQueryTerm write_terms[2] = {nph_term_write(ids->rigid_body),
                                         nph_term_write(NPH_COMPONENT_ID_LOCAL_TRANSFORM)};
    const NphQueryDesc rq = {rigid_terms, 3U};
    const NphQueryDesc sq = {static_terms, 3U};
    const NphQueryDesc wq = {write_terms, 2U};
    NphResult r = nph_query_create(b->world, &rq, &b->rigid_query);
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &sq, &b->static_query);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_query_create(b->world, &wq, &b->write_query);
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
    NPH_ARRAY_INIT(&b->targeted, alloc);
    NPH_ARRAY_INIT(&b->last_targeted, alloc);
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
    }
    NPH_ARRAY_FREE(&b->follows);
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
            if (!nph_world_is_alive(world, b->records[i].entity) ||
                !qualifies(b, b->records[i].entity)) {
                destroy_record(b, i);
            }
        }
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
        // Every body before the game's play_begin, which may join them.
        r = change_pass(b, 0U, nph_world_tick(b->world),
                        (f32)nph_fixed_clock_step_seconds(&b->engine->clock));
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
    return change_pass(b, 0U, nph_world_tick(b->world),
                       (f32)nph_fixed_clock_step_seconds(&b->engine->clock));
}

void physics_bridge_end(PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    if (b->physics == NULL) {
        return;
    }
    destroy_all(b);
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
    const NphComponentId writes[2] = {ids->rigid_body, NPH_COMPONENT_ID_LOCAL_TRANSFORM};
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
    d.write_count = 2U;
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
    return r != NULL ? r->body : NPH_PHYSICS_BODY_NULL;
}

const NphPhysicsStats *physics_bridge_stats(const PhysicsBridge *b)
{
    NPH_ASSERT(b != NULL);
    return &b->stats;
}
