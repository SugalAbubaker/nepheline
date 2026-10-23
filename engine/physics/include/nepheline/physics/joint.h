// SPDX-License-Identifier: Apache-2.0
#pragma once

// Joints: constraints between two bodies (docs/physics.md "Joints").
//
// A joint holds a frame on each body, given in the body's frame: an anchor
// and three axes, of which X is the hinge or twist axis. The joint is
// satisfied when the two frames coincide (less the motion it allows).
// `nph_physics_joint_frames_at` makes frames that coincide at a world point
// for the bodies' current poses, which is how most joints are made.
//
//   ball   the anchors meet; rotation is free, within an optional cone for
//          the X axis (elliptical: half angles about Y and Z) and an
//          optional range of twist about it; optional friction against any
//          rotation. Shoulders, hips, ropes and chains.
//   hinge  the anchors meet and the X axes align; rotation about X is free,
//          within an optional range, with an optional motor (a target speed
//          up to a torque) and spring (towards an angle). Doors, wheels,
//          elbows, knees.
//   fixed  the frames coincide. Welds.
//
// Joints are solved with the contacts, in the same colours and substeps
// (ADR-0044): softly with the world's joint stiffness, then rigidly. A
// joint with at least one dynamic body is required. Bodies joined to each
// other sleep and wake together, and their shapes do not touch unless the
// joint says so. Creating, changing, or destroying a joint wakes its
// bodies. Destroying a body destroys its joints.
//
// Thread-safety: as the world's (world.h).

#include <nepheline/physics/body.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphPhysicsJoint {
    u32 value;
} NphPhysicsJoint;

#define NPH_PHYSICS_JOINT_NULL ((NphPhysicsJoint){0U})

NPH_INLINE bool nph_physics_joint_is_null(NphPhysicsJoint j)
{
    return j.value == 0U;
}

typedef enum NphPhysicsJointType {
    NPH_PHYSICS_JOINT_BALL = 0,
    NPH_PHYSICS_JOINT_HINGE = 1,
    NPH_PHYSICS_JOINT_FIXED = 2,
    NPH_PHYSICS_JOINT_TYPE_COUNT = 3,
} NphPhysicsJointType;

// A ball joint's limits. Angles in radians.
typedef struct NphPhysicsBallJoint {
    // The X axis of B's frame stays in a cone around A's: half angles of
    // rotation about A's Y and Z axes, each in (0, pi).
    b32 enable_swing_limit;
    f32 swing_y;
    f32 swing_z;
    // B's twist about its X axis relative to A's frame, in [-pi, pi].
    b32 enable_twist_limit;
    f32 twist_lower;
    f32 twist_upper;
    // Friction against any relative rotation, up to this torque (N m):
    // a limp ragdoll's joints. 0: none.
    f32 friction_torque;
} NphPhysicsBallJoint;

// A hinge joint's limits, motor, and spring, about the frames' X axes. The
// angle is B's rotation from A's frame, in [-pi, pi].
typedef struct NphPhysicsHingeJoint {
    b32 enable_limit;
    f32 lower; // radians
    f32 upper;
    // Drives the relative angular speed to `motor_speed` (rad/s) with at most
    // `max_motor_torque` (N m); a speed of 0 is friction.
    b32 enable_motor;
    f32 motor_speed;
    f32 max_motor_torque;
    // Pulls towards `spring_target` (radians) like a damped spring of the
    // given frequency and damping ratio, whatever the bodies' masses.
    b32 enable_spring;
    f32 spring_hertz;
    f32 spring_damping_ratio;
    f32 spring_target;
} NphPhysicsHingeJoint;

typedef struct NphPhysicsJointDesc {
    NphPhysicsJointType type;
    NphPhysicsBody body_a;
    NphPhysicsBody body_b;  // not body_a; at least one of the two is dynamic
    NphPhysicsPose frame_a; // in body A's frame (its origin, not its centre of mass)
    NphPhysicsPose frame_b; // in body B's frame
    b32 collide_connected;  // the two bodies' shapes touch each other
    u64 user_data;
    union {
        NphPhysicsBallJoint ball;
        NphPhysicsHingeJoint hinge;
    };
} NphPhysicsJointDesc;

// A joint of `type` between `a` and `b` at identity frames, its limits,
// motor, and spring off, not colliding. Set the frames before creating
// (nph_physics_joint_frames_at).
NphPhysicsJointDesc nph_physics_joint_desc_default(NphPhysicsJointType type, NphPhysicsBody a,
                                                   NphPhysicsBody b);

// Frames on `a` and `b` that coincide at the world point `anchor` with X
// along the unit `axis` (Y and Z chosen perpendicular), for the bodies'
// current poses: a joint made from them starts satisfied. Asserts on stale
// bodies.
void nph_physics_joint_frames_at(const NphPhysicsWorld *world, NphPhysicsBody a, NphPhysicsBody b,
                                 NphDVec3 anchor, NphVec3 axis, NphPhysicsPose *frame_a,
                                 NphPhysicsPose *frame_b);

// NPH_RESULT_INVALID_ARGUMENT for a bad type, frames that are not finite
// with unit rotations, the same body twice, two bodies neither of which is
// dynamic, or limits out of range (swing angles in (0, pi), lower <= upper
// within [-pi, pi], non-negative torques, a positive spring frequency);
// NPH_RESULT_NOT_FOUND for a stale body; NPH_RESULT_FULL past
// NPH_PHYSICS_MAX_OBJECTS; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_physics_joint_create(NphPhysicsWorld *world,
                                                const NphPhysicsJointDesc *desc,
                                                NphPhysicsJoint *out);

// Destroys the joint and wakes its bodies. A stale handle is ignored
// (asserted in development builds).
void nph_physics_joint_destroy(NphPhysicsWorld *world, NphPhysicsJoint joint);

bool nph_physics_joint_is_valid(const NphPhysicsWorld *world, NphPhysicsJoint joint);

// The description the joint was made from, with its current settings.
NphPhysicsJointDesc nph_physics_joint_desc(const NphPhysicsWorld *world, NphPhysicsJoint joint);

// Changes a ball joint's limits and friction, or a hinge's limit, motor,
// and spring (as validated by nph_physics_joint_create), and wakes the
// bodies. Asserts on the wrong joint type or out-of-range values.
void nph_physics_joint_set_ball(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                const NphPhysicsBallJoint *ball);
void nph_physics_joint_set_hinge(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                 const NphPhysicsHingeJoint *hinge);

// A hinge's angle (radians, B's rotation about A's X axis from A's frame,
// in [-pi, pi]) and relative angular speed about it (rad/s), now.
f32 nph_physics_joint_hinge_angle(const NphPhysicsWorld *world, NphPhysicsJoint joint);
f32 nph_physics_joint_hinge_speed(const NphPhysicsWorld *world, NphPhysicsJoint joint);

// The force and torque the joint applied to body B in the last step (A got
// their opposites), world axes, about B's centre of mass: what a breakable
// joint compares with its strength.
void nph_physics_joint_reaction(const NphPhysicsWorld *world, NphPhysicsJoint joint, NphVec3 *force,
                                NphVec3 *torque);

u64 nph_physics_joint_user_data(const NphPhysicsWorld *world, NphPhysicsJoint joint);
void nph_physics_joint_set_user_data(NphPhysicsWorld *world, NphPhysicsJoint joint, u64 user_data);

// The body's joints, through `out` (at most `capacity`); returns the count.
u32 nph_physics_body_joints(const NphPhysicsWorld *world, NphPhysicsBody body, NphPhysicsJoint *out,
                            u32 capacity);

#ifdef __cplusplus
}
#endif
