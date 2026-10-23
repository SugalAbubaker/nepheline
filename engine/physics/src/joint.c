// SPDX-License-Identifier: Apache-2.0
// Joints: creation, destruction, their bodies' lists, graph membership, and
// the public queries (joint.h; docs/physics.md "Joints"). The arithmetic is
// in joint_solver.c.

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <string.h>

static bool body_awake(const PhysBody *b)
{
    return (b->flags & PHYS_BODY_AWAKE) != 0U;
}

PhysJoint *phys_joint(const NphPhysicsWorld *w, NphPhysicsJoint h)
{
    const u32 slot = phys_handle_slot(h.value);
    if (h.value == 0U || slot >= w->joints.len || w->joints.data[slot].handle != h.value) {
        return NULL;
    }
    return &w->joints.data[slot];
}

static PhysJoint *checked(const NphPhysicsWorld *w, NphPhysicsJoint h)
{
    PhysJoint *j = phys_joint(w, h);
    NPH_ASSERT_MSG(j != NULL, "physics: stale joint handle 0x%08x", h.value);
    return j;
}

// ---------------------------------------------------------------------------
// Descriptions
// ---------------------------------------------------------------------------

NphPhysicsJointDesc nph_physics_joint_desc_default(NphPhysicsJointType type, NphPhysicsBody a,
                                                   NphPhysicsBody b)
{
    NphPhysicsJointDesc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.body_a = a;
    d.body_b = b;
    d.frame_a = nph_physics_pose_identity();
    d.frame_b = nph_physics_pose_identity();
    if (type == NPH_PHYSICS_JOINT_BALL) {
        d.ball.swing_y = 0.25F * 3.14159265F;
        d.ball.swing_z = 0.25F * 3.14159265F;
        d.ball.twist_lower = -0.25F * 3.14159265F;
        d.ball.twist_upper = 0.25F * 3.14159265F;
    } else if (type == NPH_PHYSICS_JOINT_HINGE) {
        d.hinge.lower = -0.5F * 3.14159265F;
        d.hinge.upper = 0.5F * 3.14159265F;
        d.hinge.spring_hertz = 1.0F;
        d.hinge.spring_damping_ratio = 1.0F;
    } else if (type == NPH_PHYSICS_JOINT_SLIDER) {
        d.slider.lower = -1.0F;
        d.slider.upper = 1.0F;
        d.slider.spring_hertz = 1.0F;
        d.slider.spring_damping_ratio = 1.0F;
    } else if (type == NPH_PHYSICS_JOINT_DISTANCE) {
        d.distance.length = 1.0F;
        d.distance.max_length = 1.0F;
        d.distance.spring_hertz = 1.0F;
        d.distance.spring_damping_ratio = 1.0F;
    } else if (type == NPH_PHYSICS_JOINT_D6) {
        for (u32 i = 0U; i < NPH_PHYSICS_D6_AXES; ++i) {
            NphPhysicsD6Axis *axis = &d.d6.axes[i];
            const f32 range = i < 3U ? 1.0F : 0.25F * 3.14159265F;
            axis->lower = -range;
            axis->upper = range;
            axis->spring_hertz = 1.0F;
            axis->spring_damping_ratio = 1.0F;
        }
    }
    return d;
}

void nph_physics_joint_frames_at(const NphPhysicsWorld *world, NphPhysicsBody a, NphPhysicsBody b,
                                 NphDVec3 anchor, NphVec3 axis, NphPhysicsPose *frame_a,
                                 NphPhysicsPose *frame_b)
{
    const PhysBody *ba = phys_body(world, a);
    const PhysBody *bb = phys_body(world, b);
    NPH_ASSERT_MSG(ba != NULL && bb != NULL, "nph_physics_joint_frames_at: stale body");
    if (ba == NULL || bb == NULL) {
        *frame_a = nph_physics_pose_identity();
        *frame_b = nph_physics_pose_identity();
        return;
    }
    // The world frame: X along the axis, Y and Z perpendicular.
    const NphVec3 x = nph_vec3_normalize_or(axis, nph_vec3(1.0F, 0.0F, 0.0F));
    const NphVec3 y = nph_vec3_perpendicular(x);
    const NphVec3 z = nph_vec3_cross(x, y);
    const NphQuat q = nph_quat_normalize(nph_quat_from_mat3(nph_mat3_from_cols(x, y, z)));
    frame_a->rotation = nph_quat_normalize(nph_quat_mul(nph_quat_conjugate(ba->rotation), q));
    frame_b->rotation = nph_quat_normalize(nph_quat_mul(nph_quat_conjugate(bb->rotation), q));
    frame_a->position =
        nph_quat_rotate_inverse(ba->rotation, nph_dvec3_relative(anchor, ba->origin));
    frame_b->position =
        nph_quat_rotate_inverse(bb->rotation, nph_dvec3_relative(anchor, bb->origin));
}

static bool pose_valid(NphPhysicsPose p)
{
    return nph_vec3_is_finite(p.position) && nph_f32_is_finite(p.rotation.x) &&
           nph_f32_is_finite(p.rotation.y) && nph_f32_is_finite(p.rotation.z) &&
           nph_f32_is_finite(p.rotation.w) && nph_quat_is_normalized(p.rotation, 1e-3F);
}

static bool finite_nonneg(f32 v)
{
    return nph_f32_is_finite(v) && v >= 0.0F;
}

static bool angle_range_valid(f32 lower, f32 upper)
{
    const f32 pi = 3.14159265F;
    return nph_f32_is_finite(lower) && nph_f32_is_finite(upper) && lower <= upper && lower >= -pi &&
           upper <= pi;
}

static bool ball_valid(const NphPhysicsBallJoint *b)
{
    const f32 pi = 3.14159265F;
    if (b->enable_swing_limit != 0 &&
        !(b->swing_y > 0.0F && b->swing_y < pi && b->swing_z > 0.0F && b->swing_z < pi)) {
        return false;
    }
    if (b->enable_twist_limit != 0 && !angle_range_valid(b->twist_lower, b->twist_upper)) {
        return false;
    }
    return finite_nonneg(b->friction_torque);
}

static bool drive_valid(b32 enable_motor, f32 speed, f32 max_force, b32 enable_spring, f32 hertz,
                        f32 zeta, f32 target)
{
    if (enable_motor != 0 && (!nph_f32_is_finite(speed) || !finite_nonneg(max_force))) {
        return false;
    }
    if (enable_spring != 0 && (!(hertz > 0.0F) || !nph_f32_is_finite(hertz) ||
                               !finite_nonneg(zeta) || !nph_f32_is_finite(target))) {
        return false;
    }
    return true;
}

static bool slider_valid(const NphPhysicsSliderJoint *s)
{
    if (s->enable_limit != 0 &&
        !(nph_f32_is_finite(s->lower) && nph_f32_is_finite(s->upper) && s->lower <= s->upper)) {
        return false;
    }
    return drive_valid(s->enable_motor, s->motor_speed, s->max_motor_force, s->enable_spring,
                       s->spring_hertz, s->spring_damping_ratio, s->spring_target);
}

static bool distance_valid(const NphPhysicsDistanceJoint *d)
{
    if (!finite_nonneg(d->length)) {
        return false;
    }
    if (d->enable_limit != 0 && !(finite_nonneg(d->min_length) && finite_nonneg(d->max_length) &&
                                  d->min_length <= d->max_length)) {
        return false;
    }
    return drive_valid(d->enable_motor, d->motor_speed, d->max_motor_force, d->enable_spring,
                       d->spring_hertz, d->spring_damping_ratio, 0.0F);
}

static bool d6_valid(const NphPhysicsD6Joint *d)
{
    const f32 pi = 3.14159265F;
    for (u32 i = 0U; i < NPH_PHYSICS_D6_AXES; ++i) {
        const NphPhysicsD6Axis *a = &d->axes[i];
        if ((u32)a->motion > (u32)NPH_PHYSICS_AXIS_FREE) {
            return false;
        }
        if (a->motion == NPH_PHYSICS_AXIS_LIMITED) {
            if (!(nph_f32_is_finite(a->lower) && nph_f32_is_finite(a->upper) &&
                  a->lower <= a->upper)) {
                return false;
            }
            // Angles stay inside the decomposition's range: twist within
            // [-pi, pi], swings within (-pi, pi).
            if (i == NPH_PHYSICS_D6_TWIST && !(a->lower >= -pi && a->upper <= pi)) {
                return false;
            }
            if (i > NPH_PHYSICS_D6_TWIST && !(a->lower > -pi && a->upper < pi)) {
                return false;
            }
        }
        if (!drive_valid(a->enable_motor, a->motor_speed, a->max_motor_force, a->enable_spring,
                         a->spring_hertz, a->spring_damping_ratio, a->spring_target)) {
            return false;
        }
    }
    return true;
}

static bool hinge_valid(const NphPhysicsHingeJoint *h)
{
    if (h->enable_limit != 0 && !angle_range_valid(h->lower, h->upper)) {
        return false;
    }
    if (h->enable_motor != 0 &&
        (!nph_f32_is_finite(h->motor_speed) || !finite_nonneg(h->max_motor_torque))) {
        return false;
    }
    if (h->enable_spring != 0 &&
        (!(h->spring_hertz > 0.0F) || !nph_f32_is_finite(h->spring_hertz) ||
         !finite_nonneg(h->spring_damping_ratio) || !nph_f32_is_finite(h->spring_target))) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lists and membership
// ---------------------------------------------------------------------------

static void list_insert(NphPhysicsWorld *w, u32 joint, u32 side, u32 body)
{
    PhysBody *b = &w->bodies.data[body];
    PhysJoint *j = &w->joints.data[joint];
    const u32 key = (joint << 1U) | side;
    j->prev[side] = PHYS_NULL;
    j->next[side] = b->joint_head;
    if (b->joint_head != PHYS_NULL) {
        w->joints.data[b->joint_head >> 1U].prev[b->joint_head & 1U] = key;
    }
    b->joint_head = key;
    b->joint_count += 1U;
}

static void list_remove(NphPhysicsWorld *w, u32 joint, u32 side, u32 body)
{
    PhysBody *b = &w->bodies.data[body];
    PhysJoint *j = &w->joints.data[joint];
    const u32 prev = j->prev[side];
    const u32 next = j->next[side];
    if (prev != PHYS_NULL) {
        w->joints.data[prev >> 1U].next[prev & 1U] = next;
    } else {
        b->joint_head = next;
    }
    if (next != PHYS_NULL) {
        w->joints.data[next >> 1U].prev[next & 1U] = prev;
    }
    b->joint_count -= 1U;
}

static bool body_solvable(const PhysBody *b)
{
    return body_awake(b) || b->type != NPH_PHYSICS_BODY_DYNAMIC;
}

NphResult phys_joint_sync(NphPhysicsWorld *w, u32 joint)
{
    PhysJoint *j = &w->joints.data[joint];
    const PhysBody *a = &w->bodies.data[j->body_a];
    const PhysBody *b = &w->bodies.data[j->body_b];
    const bool in_graph = (body_awake(a) || body_awake(b)) && body_solvable(a) && body_solvable(b);
    if (in_graph && j->color == PHYS_NULL) {
        return phys_graph_add_joint(w, joint);
    }
    if (!in_graph && j->color != PHYS_NULL) {
        phys_graph_remove_joint(w, joint);
    }
    return NPH_RESULT_OK;
}

bool phys_joints_allow_contact(const NphPhysicsWorld *w, u32 body_a, u32 body_b)
{
    const PhysBody *a = &w->bodies.data[body_a];
    const PhysBody *b = &w->bodies.data[body_b];
    // The shorter list.
    const u32 body = a->joint_count <= b->joint_count ? body_a : body_b;
    const u32 other = body == body_a ? body_b : body_a;
    for (u32 key = w->bodies.data[body].joint_head; key != PHYS_NULL;) {
        const PhysJoint *j = &w->joints.data[key >> 1U];
        const u32 far = (key & 1U) == 0U ? j->body_b : j->body_a;
        if (far == other && (j->flags & PHYS_JOINT_COLLIDE_CONNECTED) == 0U) {
            return false;
        }
        key = j->next[key & 1U];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

static void store_settings(PhysJoint *j, const NphPhysicsJointDesc *desc)
{
    switch (desc->type) {
    case NPH_PHYSICS_JOINT_BALL:
        j->ball = desc->ball;
        break;
    case NPH_PHYSICS_JOINT_HINGE:
        j->hinge = desc->hinge;
        break;
    case NPH_PHYSICS_JOINT_SLIDER:
        j->slider = desc->slider;
        break;
    case NPH_PHYSICS_JOINT_DISTANCE:
        j->distance = desc->distance;
        break;
    case NPH_PHYSICS_JOINT_D6:
        j->d6 = desc->d6;
        break;
    case NPH_PHYSICS_JOINT_FIXED:
    case NPH_PHYSICS_JOINT_TYPE_COUNT:
    default:
        break;
    }
}

static bool settings_valid(const NphPhysicsJointDesc *desc)
{
    switch (desc->type) {
    case NPH_PHYSICS_JOINT_BALL:
        return ball_valid(&desc->ball);
    case NPH_PHYSICS_JOINT_HINGE:
        return hinge_valid(&desc->hinge);
    case NPH_PHYSICS_JOINT_SLIDER:
        return slider_valid(&desc->slider);
    case NPH_PHYSICS_JOINT_DISTANCE:
        return distance_valid(&desc->distance);
    case NPH_PHYSICS_JOINT_D6:
        return d6_valid(&desc->d6);
    case NPH_PHYSICS_JOINT_FIXED:
        return true;
    case NPH_PHYSICS_JOINT_TYPE_COUNT:
    default:
        return false;
    }
}

NphResult nph_physics_joint_create(NphPhysicsWorld *world, const NphPhysicsJointDesc *desc,
                                   NphPhysicsJoint *out)
{
    NPH_ASSERT(world != NULL && out != NULL);
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_joint_create during a step");
    if (desc == NULL || (u32)desc->type >= (u32)NPH_PHYSICS_JOINT_TYPE_COUNT ||
        !pose_valid(desc->frame_a) || !pose_valid(desc->frame_b) ||
        desc->body_a.value == desc->body_b.value || !settings_valid(desc)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const PhysBody *ba = phys_body(world, desc->body_a);
    const PhysBody *bb = phys_body(world, desc->body_b);
    if (ba == NULL || bb == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    if (ba->type != NPH_PHYSICS_BODY_DYNAMIC && bb->type != NPH_PHYSICS_BODY_DYNAMIC) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 slot_a = phys_handle_slot(desc->body_a.value);
    const u32 slot_b = phys_handle_slot(desc->body_b.value);
    u32 slot = 0U;
    u32 generation = 0U;
    NPH_TRY(phys_slots_alloc(&world->joint_slots, &slot, &generation));
    NphResult r = NPH_RESULT_OK;
    if (slot >= world->joints.len) {
        r = NPH_ARRAY_RESIZE(&world->joints, slot + 1U);
    }
    if (r == NPH_RESULT_OK) {
        // Releasing the slot later cannot fail.
        r = NPH_ARRAY_RESERVE(&world->joint_slots.free, world->joint_slots.generation.len);
    }
    if (r != NPH_RESULT_OK) {
        phys_slots_release(&world->joint_slots, slot);
        return r;
    }
    PhysJoint *j = &world->joints.data[slot];
    memset(j, 0, sizeof *j);
    j->type = (u32)desc->type;
    j->body_a = slot_a;
    j->body_b = slot_b;
    j->flags = desc->collide_connected != 0 ? PHYS_JOINT_COLLIDE_CONNECTED : 0U;
    j->color = PHYS_NULL;
    j->color_slot = PHYS_NULL;
    j->island = PHYS_NULL;
    j->island_prev = PHYS_NULL;
    j->island_next = PHYS_NULL;
    j->frame_a = desc->frame_a;
    j->frame_b = desc->frame_b;
    j->frame_a.rotation = nph_quat_normalize(desc->frame_a.rotation);
    j->frame_b.rotation = nph_quat_normalize(desc->frame_b.rotation);
    store_settings(j, desc);
    j->user_data = desc->user_data;
    j->handle = phys_handle(slot, generation);
    list_insert(world, slot, 0U, slot_a);
    list_insert(world, slot, 1U, slot_b);
    // Joined bodies keep apart unless asked: their contacts go now (the
    // touching ones report their end).
    if (desc->collide_connected == 0) {
        for (u32 key = world->bodies.data[slot_a].contact_head; key != PHYS_NULL;) {
            const u32 c = key >> 1U;
            const PhysContact *contact = &world->contacts.data[c];
            const u32 other = (key & 1U) == 0U ? contact->body_b : contact->body_a;
            key = contact->next[key & 1U];
            if (other == slot_b) {
                phys_contact_destroy(world, c, true);
            }
        }
    }
    // Both ends awake, their islands linked (merged at the step's end).
    r = phys_wake_body(world, slot_a);
    if (r == NPH_RESULT_OK) {
        r = phys_wake_body(world, slot_b);
    }
    if (r == NPH_RESULT_OK && ba->type == NPH_PHYSICS_BODY_DYNAMIC &&
        bb->type == NPH_PHYSICS_BODY_DYNAMIC) {
        phys_island_link_joint(world, slot);
    }
    if (r == NPH_RESULT_OK) {
        r = phys_joint_sync(world, slot);
    }
    if (r != NPH_RESULT_OK) {
        phys_joint_destroy(world, slot, false);
        return r;
    }
    *out = (NphPhysicsJoint){j->handle};
    return NPH_RESULT_OK;
}

void phys_joint_destroy(NphPhysicsWorld *w, u32 joint, bool wake)
{
    PhysJoint *j = &w->joints.data[joint];
    const u32 a = j->body_a;
    const u32 b = j->body_b;
    if (j->color != PHYS_NULL) {
        phys_graph_remove_joint(w, joint);
    }
    if ((j->flags & PHYS_JOINT_ISLAND) != 0U) {
        phys_island_unlink_joint(w, joint);
    }
    list_remove(w, joint, 0U, a);
    list_remove(w, joint, 1U, b);
    if ((j->flags & PHYS_JOINT_COLLIDE_CONNECTED) == 0U) {
        // The shapes may touch now: A's shapes look for pairs next step.
        for (u32 s = w->bodies.data[a].shape_head; s != PHYS_NULL; s = w->shapes.data[s].next) {
            NPH_IGNORE_RESULT(phys_move_buffer_push(w, s));
        }
    }
    j->handle = 0U;
    phys_slots_release(&w->joint_slots, joint);
    if (wake) {
        NPH_IGNORE_RESULT(phys_wake_body(w, a));
        NPH_IGNORE_RESULT(phys_wake_body(w, b));
    }
}

void nph_physics_joint_destroy(NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_joint_destroy during a step");
    if (checked(world, joint) == NULL) {
        return;
    }
    phys_joint_destroy(world, phys_handle_slot(joint.value), true);
}

bool nph_physics_joint_is_valid(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    return phys_joint(world, joint) != NULL;
}

NphPhysicsJointDesc nph_physics_joint_desc(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    NphPhysicsJointDesc d;
    memset(&d, 0, sizeof d);
    const PhysJoint *j = checked(world, joint);
    if (j == NULL) {
        return d;
    }
    d.type = (NphPhysicsJointType)j->type;
    d.body_a = phys_body_handle(world, j->body_a);
    d.body_b = phys_body_handle(world, j->body_b);
    d.frame_a = j->frame_a;
    d.frame_b = j->frame_b;
    d.collide_connected = (j->flags & PHYS_JOINT_COLLIDE_CONNECTED) != 0U ? 1 : 0;
    d.user_data = j->user_data;
    switch ((NphPhysicsJointType)j->type) {
    case NPH_PHYSICS_JOINT_BALL:
        d.ball = j->ball;
        break;
    case NPH_PHYSICS_JOINT_HINGE:
        d.hinge = j->hinge;
        break;
    case NPH_PHYSICS_JOINT_SLIDER:
        d.slider = j->slider;
        break;
    case NPH_PHYSICS_JOINT_DISTANCE:
        d.distance = j->distance;
        break;
    case NPH_PHYSICS_JOINT_D6:
        d.d6 = j->d6;
        break;
    case NPH_PHYSICS_JOINT_FIXED:
    case NPH_PHYSICS_JOINT_TYPE_COUNT:
    default:
        break;
    }
    return d;
}

static void wake_ends(NphPhysicsWorld *w, const PhysJoint *j)
{
    NPH_IGNORE_RESULT(phys_wake_body(w, j->body_a));
    NPH_IGNORE_RESULT(phys_wake_body(w, j->body_b));
}

void nph_physics_joint_set_ball(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                const NphPhysicsBallJoint *ball)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: joint changed during a step");
    PhysJoint *j = checked(world, joint);
    if (j == NULL) {
        return;
    }
    NPH_ASSERT_MSG(j->type == NPH_PHYSICS_JOINT_BALL, "nph_physics_joint_set_ball: not a ball");
    NPH_ASSERT_MSG(ball_valid(ball), "nph_physics_joint_set_ball: settings out of range");
    if (j->type != NPH_PHYSICS_JOINT_BALL || !ball_valid(ball)) {
        return;
    }
    j->ball = *ball;
    wake_ends(world, j);
}

void nph_physics_joint_set_hinge(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                 const NphPhysicsHingeJoint *hinge)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: joint changed during a step");
    PhysJoint *j = checked(world, joint);
    if (j == NULL) {
        return;
    }
    NPH_ASSERT_MSG(j->type == NPH_PHYSICS_JOINT_HINGE, "nph_physics_joint_set_hinge: not a hinge");
    NPH_ASSERT_MSG(hinge_valid(hinge), "nph_physics_joint_set_hinge: settings out of range");
    if (j->type != NPH_PHYSICS_JOINT_HINGE || !hinge_valid(hinge)) {
        return;
    }
    j->hinge = *hinge;
    wake_ends(world, j);
}

// The joint of `type` behind a handle for a setter, or NULL (asserted).
static PhysJoint *settable(NphPhysicsWorld *world, NphPhysicsJoint joint, NphPhysicsJointType type,
                           const char *what)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: joint changed during a step");
    NPH_UNUSED(what); // in the assert's message only
    PhysJoint *j = checked(world, joint);
    if (j == NULL) {
        return NULL;
    }
    NPH_ASSERT_MSG(j->type == (u32)type, "%s: the wrong joint type", what);
    return j->type == (u32)type ? j : NULL;
}

void nph_physics_joint_set_slider(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                  const NphPhysicsSliderJoint *slider)
{
    PhysJoint *j = settable(world, joint, NPH_PHYSICS_JOINT_SLIDER, "nph_physics_joint_set_slider");
    NPH_ASSERT_MSG(slider_valid(slider), "nph_physics_joint_set_slider: settings out of range");
    if (j == NULL || !slider_valid(slider)) {
        return;
    }
    j->slider = *slider;
    wake_ends(world, j);
}

void nph_physics_joint_set_distance(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                    const NphPhysicsDistanceJoint *distance)
{
    PhysJoint *j =
        settable(world, joint, NPH_PHYSICS_JOINT_DISTANCE, "nph_physics_joint_set_distance");
    NPH_ASSERT_MSG(distance_valid(distance),
                   "nph_physics_joint_set_distance: settings out of range");
    if (j == NULL || !distance_valid(distance)) {
        return;
    }
    j->distance = *distance;
    wake_ends(world, j);
}

void nph_physics_joint_set_d6(NphPhysicsWorld *world, NphPhysicsJoint joint,
                              const NphPhysicsD6Joint *d6)
{
    PhysJoint *j = settable(world, joint, NPH_PHYSICS_JOINT_D6, "nph_physics_joint_set_d6");
    NPH_ASSERT_MSG(d6_valid(d6), "nph_physics_joint_set_d6: settings out of range");
    if (j == NULL || !d6_valid(d6)) {
        return;
    }
    j->d6 = *d6;
    wake_ends(world, j);
}

f32 nph_physics_joint_slider_position(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    const PhysJoint *j = checked(world, joint);
    return j != NULL ? phys_joint_slider_value(world, j, false) : 0.0F;
}

f32 nph_physics_joint_slider_speed(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    const PhysJoint *j = checked(world, joint);
    return j != NULL ? phys_joint_slider_value(world, j, true) : 0.0F;
}

f32 nph_physics_joint_length(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    const PhysJoint *j = checked(world, joint);
    return j != NULL ? phys_joint_length(world, j) : 0.0F;
}

f32 nph_physics_joint_hinge_angle(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    const PhysJoint *j = checked(world, joint);
    return j != NULL ? phys_joint_angle(world, j) : 0.0F;
}

f32 nph_physics_joint_hinge_speed(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    const PhysJoint *j = checked(world, joint);
    if (j == NULL) {
        return 0.0F;
    }
    const PhysBody *a = &world->bodies.data[j->body_a];
    const PhysBody *b = &world->bodies.data[j->body_b];
    const NphVec3 axis = nph_quat_axis_x(nph_quat_mul(a->rotation, j->frame_a.rotation));
    return nph_vec3_dot(nph_vec3_sub(b->angular_velocity, a->angular_velocity), axis);
}

void nph_physics_joint_reaction(const NphPhysicsWorld *world, NphPhysicsJoint joint, NphVec3 *force,
                                NphVec3 *torque)
{
    *force = nph_vec3_zero();
    *torque = nph_vec3_zero();
    const PhysJoint *j = checked(world, joint);
    if (j == NULL || !(world->last_substep > 0.0F)) {
        return;
    }
    // The accumulated impulses are per substep: over the substep's length,
    // they are the last substep's force and torque.
    const f32 inv_h = 1.0F / world->last_substep;
    const PhysBody *b = &world->bodies.data[j->body_b];
    NphVec3 linear;
    NphVec3 angular;
    phys_joint_reaction_impulses(world, j, &linear, &angular);
    *force = nph_vec3_scale(linear, inv_h);
    const NphVec3 rb =
        nph_quat_rotate(b->rotation, nph_vec3_sub(j->frame_b.position, b->local_center));
    *torque = nph_vec3_add(nph_vec3_scale(angular, inv_h), nph_vec3_cross(rb, *force));
}

u64 nph_physics_joint_user_data(const NphPhysicsWorld *world, NphPhysicsJoint joint)
{
    const PhysJoint *j = checked(world, joint);
    return j != NULL ? j->user_data : 0U;
}

void nph_physics_joint_set_user_data(NphPhysicsWorld *world, NphPhysicsJoint joint, u64 user_data)
{
    PhysJoint *j = checked(world, joint);
    if (j != NULL) {
        j->user_data = user_data;
    }
}

u32 nph_physics_body_joints(const NphPhysicsWorld *world, NphPhysicsBody body, NphPhysicsJoint *out,
                            u32 capacity)
{
    const PhysBody *b = phys_body(world, body);
    NPH_ASSERT_MSG(b != NULL, "physics: stale body handle 0x%08x", body.value);
    if (b == NULL) {
        return 0U;
    }
    u32 n = 0U;
    for (u32 key = b->joint_head; key != PHYS_NULL;) {
        const PhysJoint *j = &world->joints.data[key >> 1U];
        if (n < capacity && out != NULL) {
            out[n] = (NphPhysicsJoint){j->handle};
        }
        n += 1U;
        key = j->next[key & 1U];
    }
    return n;
}
