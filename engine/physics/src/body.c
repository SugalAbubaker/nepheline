// SPDX-License-Identifier: Apache-2.0
// Bodies: creation, destruction, mass properties, poses, velocities, forces
// (body.h; docs/physics.md "Bodies").

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <float.h>
#include <string.h>

NphPhysicsBodyDesc nph_physics_body_desc_default(void)
{
    NphPhysicsBodyDesc d;
    memset(&d, 0, sizeof d);
    d.type = NPH_PHYSICS_BODY_DYNAMIC;
    d.rotation = nph_quat_identity();
    d.gravity_scale = 1.0F;
    d.sleep_threshold = 0.05F;
    d.enable_sleep = 1;
    d.awake = 1;
    d.gyroscopic = 1;
    return d;
}

static bool finite_nonneg(f32 v)
{
    return nph_f32_is_finite(v) && v >= 0.0F;
}

static bool desc_valid(const NphPhysicsBodyDesc *d)
{
    const NphDVec3 p = d->position;
    const NphQuat q = d->rotation;
    return (u32)d->type <= (u32)NPH_PHYSICS_BODY_DYNAMIC && nph_f64_is_finite(p.x) &&
           nph_f64_is_finite(p.y) && nph_f64_is_finite(p.z) && nph_f32_is_finite(q.x) &&
           nph_f32_is_finite(q.y) && nph_f32_is_finite(q.z) && nph_f32_is_finite(q.w) &&
           nph_quat_is_normalized(q, 1e-3F) && nph_vec3_is_finite(d->linear_velocity) &&
           nph_vec3_is_finite(d->angular_velocity) && finite_nonneg(d->linear_damping) &&
           finite_nonneg(d->angular_damping) && nph_f32_is_finite(d->gravity_scale) &&
           finite_nonneg(d->sleep_threshold) && (d->locks & ~0x3FU) == 0U;
}

// The bitsets of the graph's colours cover every body slot.
static NphResult grow_color_bitsets(NphPhysicsWorld *w, u32 slots)
{
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS; ++c) {
        NphBitset *b = &w->colors[c].bodies;
        if (b->bit_count < slots) {
            NPH_TRY(nph_bitset_resize(b, nph_max_u32(slots, 2U * b->bit_count)));
        }
    }
    return NPH_RESULT_OK;
}

NphResult nph_physics_body_create(NphPhysicsWorld *world, const NphPhysicsBodyDesc *desc,
                                  NphPhysicsBody *out)
{
    NPH_ASSERT(world != NULL && out != NULL);
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_body_create during a step");
    if (desc == NULL || !desc_valid(desc)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 slot = 0U;
    u32 generation = 0U;
    NPH_TRY(phys_slots_alloc(&world->body_slots, &slot, &generation));
    NphResult r = NPH_RESULT_OK;
    if (slot >= world->bodies.len) {
        r = NPH_ARRAY_RESIZE(&world->bodies, slot + 1U);
    }
    if (r == NPH_RESULT_OK) {
        r = grow_color_bitsets(world, slot + 1U);
    }
    if (r == NPH_RESULT_OK) {
        // The free list's capacity covers every issued slot, so releasing
        // this one later cannot fail.
        r = NPH_ARRAY_RESERVE(&world->body_slots.free, world->body_slots.generation.len);
    }
    if (r != NPH_RESULT_OK) {
        phys_slots_release(&world->body_slots, slot);
        return r;
    }
    PhysBody *b = &world->bodies.data[slot];
    memset(b, 0, sizeof *b);
    b->type = (u32)desc->type;
    b->locks = desc->locks;
    b->origin = desc->position;
    b->rotation = nph_quat_normalize(desc->rotation);
    b->linear_velocity =
        desc->type == NPH_PHYSICS_BODY_STATIC ? nph_vec3_zero() : desc->linear_velocity;
    b->angular_velocity =
        desc->type == NPH_PHYSICS_BODY_STATIC ? nph_vec3_zero() : desc->angular_velocity;
    b->linear_damping = desc->linear_damping;
    b->angular_damping = desc->angular_damping;
    b->gravity_scale = desc->gravity_scale;
    b->sleep_threshold = desc->sleep_threshold;
    b->flags =
        (desc->enable_sleep != 0 ? PHYS_BODY_ENABLE_SLEEP : 0U) |
        (desc->gyroscopic != 0 ? PHYS_BODY_GYROSCOPIC : 0U) |
        (desc->bullet != 0 && desc->type == NPH_PHYSICS_BODY_DYNAMIC ? PHYS_BODY_BULLET : 0U);
    b->shape_head = PHYS_NULL;
    b->contact_head = PHYS_NULL;
    b->joint_head = PHYS_NULL;
    b->island = PHYS_NULL;
    b->island_prev = PHYS_NULL;
    b->island_next = PHYS_NULL;
    b->awake_slot = PHYS_NULL;
    b->move_event = PHYS_NULL;
    b->user_data = desc->user_data;
    b->handle = phys_handle(slot, generation);
    phys_body_update_mass(world, b);
    if (desc->type == NPH_PHYSICS_BODY_DYNAMIC) {
        r = phys_island_add_body(world, slot);
    }
    if (r == NPH_RESULT_OK && desc->type != NPH_PHYSICS_BODY_STATIC) {
        // A body created asleep sleeps alone: its own island, already still.
        r = phys_body_set_awake_list(world, slot, desc->awake != 0);
    }
    if (r != NPH_RESULT_OK) {
        if (b->island != PHYS_NULL) {
            phys_island_remove_body(world, slot);
        }
        b->handle = 0U;
        phys_slots_release(&world->body_slots, slot);
        return r;
    }
    world->bullets += (b->flags & PHYS_BODY_BULLET) != 0U ? 1U : 0U;
    *out = (NphPhysicsBody){b->handle};
    return NPH_RESULT_OK;
}

void nph_physics_body_destroy(NphPhysicsWorld *world, NphPhysicsBody body)
{
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_body_destroy during a step");
    PhysBody *b = phys_body(world, body);
    NPH_ASSERT_MSG(b != NULL, "nph_physics_body_destroy: stale body handle");
    if (b == NULL) {
        return;
    }
    const u32 slot = phys_handle_slot(body.value);
    // Joints first (the bodies at their other ends wake), then contacts
    // (touching neighbours wake: what rested on this body must fall), then
    // the shapes, the island, the awake list.
    while (b->joint_head != PHYS_NULL) {
        const u32 j = b->joint_head >> 1U;
        const PhysJoint *joint = &world->joints.data[j];
        const u32 other = joint->body_a == slot ? joint->body_b : joint->body_a;
        phys_joint_destroy(world, j, false);
        NPH_IGNORE_RESULT(phys_wake_body(world, other));
    }
    while (b->contact_head != PHYS_NULL) {
        const u32 c = b->contact_head >> 1U;
        const PhysContact *contact = &world->contacts.data[c];
        const u32 other = contact->body_a == slot ? contact->body_b : contact->body_a;
        const bool touching = (contact->flags & PHYS_CONTACT_TOUCHING) != 0U;
        phys_contact_destroy(world, c, false);
        if (touching) {
            NPH_IGNORE_RESULT(phys_wake_body(world, other));
        }
    }
    while (b->shape_head != PHYS_NULL) {
        const u32 s = b->shape_head;
        PhysShape *shape = &world->shapes.data[s];
        b->shape_head = shape->next;
        phys_shape_destroy_proxy(world, s);
        shape->handle = 0U;
        phys_slots_release(&world->shape_slots, s);
    }
    b->shape_count = 0U;
    if (b->awake_slot != PHYS_NULL) {
        NPH_IGNORE_RESULT(phys_body_set_awake_list(world, slot, false));
    }
    if (b->island != PHYS_NULL) {
        phys_island_remove_body(world, slot);
    }
    world->bullets -= (b->flags & PHYS_BODY_BULLET) != 0U ? 1U : 0U;
    b->handle = 0U;
    phys_slots_release(&world->body_slots, slot);
}

bool nph_physics_body_is_valid(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    return phys_body(world, body) != NULL;
}

// The body behind a handle, asserted valid; NULL for a stale one in builds
// without asserts.
static PhysBody *checked(const NphPhysicsWorld *w, NphPhysicsBody h)
{
    PhysBody *b = phys_body(w, h);
    NPH_ASSERT_MSG(b != NULL, "physics: stale body handle 0x%08x", h.value);
    return b;
}

NphPhysicsBodyType nph_physics_body_type(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? (NphPhysicsBodyType)b->type : NPH_PHYSICS_BODY_STATIC;
}

NphDVec3 nph_physics_body_position(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? b->origin : nph_dvec3_zero();
}

NphQuat nph_physics_body_rotation(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? b->rotation : nph_quat_identity();
}

NphDVec3 nph_physics_body_center_of_mass(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? b->center : nph_dvec3_zero();
}

static void refresh_proxies(NphPhysicsWorld *w, u32 slot)
{
    PhysBody *b = &w->bodies.data[slot];
    for (u32 s = b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
        PhysShape *shape = &w->shapes.data[s];
        shape->aabb = phys_shape_world_aabb(b, shape);
        const f32 margin = b->type == NPH_PHYSICS_BODY_STATIC ? NPH_PHYSICS_SPECULATIVE_DISTANCE
                                                              : NPH_PHYSICS_AABB_MARGIN;
        shape->fat_aabb = nph_aabb_grow(shape->aabb, margin);
        nph_aabb_tree_move_proxy(&w->trees[shape->tree], shape->proxy, shape->fat_aabb);
        // A full move buffer means an allocation failed; the next teleport
        // or step finds the pair again.
        NPH_IGNORE_RESULT(phys_move_buffer_push(w, s));
    }
}

void nph_physics_body_set_transform(NphPhysicsWorld *world, NphPhysicsBody body, NphDVec3 position,
                                    NphQuat rotation)
{
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_body_set_transform during a step");
    PhysBody *b = checked(world, body);
    if (b == NULL) {
        return;
    }
    NPH_ASSERT(nph_f64_is_finite(position.x) && nph_f64_is_finite(position.y) &&
               nph_f64_is_finite(position.z) && nph_quat_is_normalized(rotation, 1e-3F));
    const u32 slot = phys_handle_slot(body.value);
    b->origin = position;
    b->rotation = nph_quat_normalize(rotation);
    b->center = nph_dvec3_add_vec3(b->origin, nph_quat_rotate(b->rotation, b->local_center));
    refresh_proxies(world, slot);
    if (b->type == NPH_PHYSICS_BODY_STATIC) {
        // What rested on it must notice it is gone.
        for (u32 key = b->contact_head; key != PHYS_NULL;) {
            const PhysContact *c = &world->contacts.data[key >> 1U];
            const u32 other = (key & 1U) == 0U ? c->body_b : c->body_a;
            key = c->next[key & 1U];
            NPH_IGNORE_RESULT(phys_wake_body(world, other));
        }
    } else {
        NPH_IGNORE_RESULT(phys_wake_body(world, slot));
    }
}

NphVec3 nph_physics_body_linear_velocity(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? b->linear_velocity : nph_vec3_zero();
}

NphVec3 nph_physics_body_angular_velocity(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? b->angular_velocity : nph_vec3_zero();
}

void nph_physics_body_set_linear_velocity(NphPhysicsWorld *world, NphPhysicsBody body,
                                          NphVec3 velocity)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    if (b == NULL || b->type == NPH_PHYSICS_BODY_STATIC) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(velocity));
    b->linear_velocity = velocity;
    if (nph_vec3_length_sq(velocity) > 0.0F) {
        NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
    }
}

void nph_physics_body_set_angular_velocity(NphPhysicsWorld *world, NphPhysicsBody body,
                                           NphVec3 velocity)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    if (b == NULL || b->type == NPH_PHYSICS_BODY_STATIC) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(velocity));
    b->angular_velocity = velocity;
    if (nph_vec3_length_sq(velocity) > 0.0F) {
        NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
    }
}

static PhysBody *dynamic_body(NphPhysicsWorld *w, NphPhysicsBody h)
{
    NPH_ASSERT_MSG(!w->stepping, "physics: body changed during a step");
    PhysBody *b = checked(w, h);
    if (b == NULL || b->type != NPH_PHYSICS_BODY_DYNAMIC) {
        return NULL;
    }
    return b;
}

void nph_physics_body_apply_force(NphPhysicsWorld *world, NphPhysicsBody body, NphVec3 force,
                                  NphDVec3 point)
{
    PhysBody *b = dynamic_body(world, body);
    if (b == NULL) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(force));
    b->force = nph_vec3_add(b->force, force);
    b->torque =
        nph_vec3_add(b->torque, nph_vec3_cross(nph_dvec3_relative(point, b->center), force));
    NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
}

void nph_physics_body_apply_force_at_center(NphPhysicsWorld *world, NphPhysicsBody body,
                                            NphVec3 force)
{
    PhysBody *b = dynamic_body(world, body);
    if (b == NULL) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(force));
    b->force = nph_vec3_add(b->force, force);
    NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
}

void nph_physics_body_apply_torque(NphPhysicsWorld *world, NphPhysicsBody body, NphVec3 torque)
{
    PhysBody *b = dynamic_body(world, body);
    if (b == NULL) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(torque));
    b->torque = nph_vec3_add(b->torque, torque);
    NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
}

// The world inverse inertia: R I^-1 R^T.
static NphMat3 world_inv_inertia(const PhysBody *b)
{
    const NphMat3 r = nph_mat3_from_quat(b->rotation);
    return nph_mat3_mul(nph_mat3_mul(r, b->inv_inertia), nph_mat3_transpose(r));
}

void nph_physics_body_apply_linear_impulse(NphPhysicsWorld *world, NphPhysicsBody body,
                                           NphVec3 impulse, NphDVec3 point)
{
    PhysBody *b = dynamic_body(world, body);
    if (b == NULL) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(impulse));
    b->linear_velocity = nph_vec3_mul_add(b->linear_velocity, impulse, b->inv_mass);
    const NphVec3 r = nph_dvec3_relative(point, b->center);
    b->angular_velocity = nph_vec3_add(
        b->angular_velocity, nph_mat3_mul_vec3(world_inv_inertia(b), nph_vec3_cross(r, impulse)));
    NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
}

void nph_physics_body_apply_angular_impulse(NphPhysicsWorld *world, NphPhysicsBody body,
                                            NphVec3 impulse)
{
    PhysBody *b = dynamic_body(world, body);
    if (b == NULL) {
        return;
    }
    NPH_ASSERT(nph_vec3_is_finite(impulse));
    b->angular_velocity =
        nph_vec3_add(b->angular_velocity, nph_mat3_mul_vec3(world_inv_inertia(b), impulse));
    NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
}

// ---------------------------------------------------------------------------
// Mass
// ---------------------------------------------------------------------------

void phys_body_update_mass(NphPhysicsWorld *w, PhysBody *b)
{
    NphPhysicsMassData total;
    memset(&total, 0, sizeof total);
    f32 extent = 0.0F;
    if ((b->flags & PHYS_BODY_MASS_SET) == 0U) {
        for (u32 s = b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
            const PhysShape *shape = &w->shapes.data[s];
            if (shape->density <= 0.0F) {
                continue;
            }
            const NphPhysicsMassData m = nph_physics_mass_transform(
                nph_physics_geometry_mass(&shape->geometry, shape->density), shape->pose);
            total = nph_physics_mass_add(total, m);
        }
        b->mass = total.mass;
        b->local_center = total.center;
        b->inertia = total.inertia;
    } else {
        total.mass = b->mass;
        total.center = b->local_center;
        total.inertia = b->inertia;
    }
    f32 inner = FLT_MAX;
    for (u32 s = b->shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
        const PhysShape *shape = &w->shapes.data[s];
        const f32 d = nph_vec3_distance(shape->pose.position, b->local_center) +
                      nph_physics_geometry_outer_radius(&shape->geometry);
        extent = nph_f32_max(extent, d);
        inner = nph_f32_min(inner, nph_physics_geometry_inner_radius(&shape->geometry));
    }
    b->max_extent = extent;
    b->min_extent = inner;
    if (b->type == NPH_PHYSICS_BODY_DYNAMIC) {
        if (b->mass > 0.0F) {
            b->inv_mass = 1.0F / b->mass;
        } else {
            // A dynamic body needs a mass to move: a unit point-like mass.
            b->mass = 1.0F;
            b->inv_mass = 1.0F;
            b->inertia = nph_mat3_from_diagonal(nph_vec3_splat(0.4F));
            b->local_center = nph_vec3_zero();
        }
        if (!nph_mat3_inverse(b->inertia, &b->inv_inertia)) {
            // A tensor without inverse (a zero-volume shape): rotate like a
            // ball of the body's extent.
            const f32 k = 0.4F * b->mass * nph_f32_max(extent * extent, 1e-4F);
            b->inertia = nph_mat3_from_diagonal(nph_vec3_splat(k));
            b->inv_inertia = nph_mat3_from_diagonal(nph_vec3_splat(1.0F / k));
        }
    } else {
        b->inv_mass = 0.0F;
        b->inv_inertia = nph_mat3_zero();
    }
    b->center = nph_dvec3_add_vec3(b->origin, nph_quat_rotate(b->rotation, b->local_center));
}

NphPhysicsMassData nph_physics_body_mass_data(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    NphPhysicsMassData m;
    memset(&m, 0, sizeof m);
    const PhysBody *b = checked(world, body);
    if (b != NULL) {
        m.mass = b->mass;
        m.center = b->local_center;
        m.inertia = b->inertia;
    }
    return m;
}

void nph_physics_body_set_mass_data(NphPhysicsWorld *world, NphPhysicsBody body,
                                    const NphPhysicsMassData *mass)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    if (b == NULL || mass == NULL) {
        return;
    }
    if (!(mass->mass > 0.0F)) {
        b->flags &= ~(u32)PHYS_BODY_MASS_SET;
    } else {
        NPH_ASSERT(nph_f32_is_finite(mass->mass) && nph_vec3_is_finite(mass->center));
        b->flags |= PHYS_BODY_MASS_SET;
        b->mass = mass->mass;
        b->local_center = mass->center;
        b->inertia = mass->inertia;
    }
    phys_body_update_mass(world, b);
}

bool nph_physics_body_is_bullet(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL && (b->flags & PHYS_BODY_BULLET) != 0U;
}

void nph_physics_body_set_bullet(NphPhysicsWorld *world, NphPhysicsBody body, bool bullet)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    if (b == NULL || b->type != NPH_PHYSICS_BODY_DYNAMIC ||
        ((b->flags & PHYS_BODY_BULLET) != 0U) == bullet) {
        return;
    }
    if (bullet) {
        b->flags |= PHYS_BODY_BULLET;
        world->bullets += 1U;
    } else {
        b->flags &= ~(u32)PHYS_BODY_BULLET;
        world->bullets -= 1U;
    }
}

void nph_physics_body_set_damping(NphPhysicsWorld *world, NphPhysicsBody body, f32 linear,
                                  f32 angular)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    NPH_ASSERT(finite_nonneg(linear) && finite_nonneg(angular));
    if (b == NULL || !finite_nonneg(linear) || !finite_nonneg(angular)) {
        return;
    }
    b->linear_damping = linear;
    b->angular_damping = angular;
}

void nph_physics_body_set_gravity_scale(NphPhysicsWorld *world, NphPhysicsBody body, f32 scale)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    NPH_ASSERT(nph_f32_is_finite(scale));
    if (b == NULL || !nph_f32_is_finite(scale) || b->gravity_scale == scale) {
        return;
    }
    b->gravity_scale = scale;
    if (b->type == NPH_PHYSICS_BODY_DYNAMIC) {
        NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
    }
}

void nph_physics_body_set_locks(NphPhysicsWorld *world, NphPhysicsBody body, u32 locks)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    NPH_ASSERT((locks & ~0x3FU) == 0U);
    if (b == NULL || (locks & ~0x3FU) != 0U) {
        return;
    }
    b->locks = locks;
}

void nph_physics_body_set_sleep(NphPhysicsWorld *world, NphPhysicsBody body, bool enable,
                                f32 threshold)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    NPH_ASSERT(finite_nonneg(threshold));
    if (b == NULL || !finite_nonneg(threshold)) {
        return;
    }
    b->sleep_threshold = threshold;
    if (enable) {
        b->flags |= PHYS_BODY_ENABLE_SLEEP;
        return;
    }
    b->flags &= ~(u32)PHYS_BODY_ENABLE_SLEEP;
    b->sleep_time = 0.0F;
    if (b->type != NPH_PHYSICS_BODY_STATIC) {
        NPH_IGNORE_RESULT(phys_wake_body(world, phys_handle_slot(body.value)));
    }
}

bool nph_physics_body_is_awake(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL && (b->flags & PHYS_BODY_AWAKE) != 0U;
}

void nph_physics_body_set_awake(NphPhysicsWorld *world, NphPhysicsBody body, bool awake)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: body changed during a step");
    PhysBody *b = checked(world, body);
    if (b == NULL || b->type == NPH_PHYSICS_BODY_STATIC) {
        return;
    }
    const u32 slot = phys_handle_slot(body.value);
    if (awake) {
        NPH_IGNORE_RESULT(phys_wake_body(world, slot));
        return;
    }
    if ((b->flags & PHYS_BODY_AWAKE) == 0U) {
        return;
    }
    // Asleep on request: the whole island (bodies touching it would keep
    // it awake otherwise), and a kinematic body alone.
    if (b->island != PHYS_NULL) {
        NPH_IGNORE_RESULT(phys_island_sleep_now(world, phys_island_root(world, b->island)));
    } else {
        NPH_IGNORE_RESULT(phys_body_set_awake_list(world, slot, false));
    }
}

u64 nph_physics_body_user_data(const NphPhysicsWorld *world, NphPhysicsBody body)
{
    const PhysBody *b = checked(world, body);
    return b != NULL ? b->user_data : 0U;
}

void nph_physics_body_set_user_data(NphPhysicsWorld *world, NphPhysicsBody body, u64 user_data)
{
    PhysBody *b = checked(world, body);
    if (b != NULL) {
        b->user_data = user_data;
    }
}

u32 nph_physics_body_shapes(const NphPhysicsWorld *world, NphPhysicsBody body, NphPhysicsShape *out,
                            u32 capacity)
{
    const PhysBody *b = checked(world, body);
    if (b == NULL) {
        return 0U;
    }
    u32 n = 0U;
    for (u32 s = b->shape_head; s != PHYS_NULL; s = world->shapes.data[s].next) {
        if (n < capacity && out != NULL) {
            out[n] = phys_shape_handle(world, s);
        }
        n += 1U;
    }
    return n;
}
