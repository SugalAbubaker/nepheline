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
//   slider B's anchor moves along A's X axis only, its rotation locked to
//          A's, within an optional range, with an optional motor (a speed
//          up to a force) and spring (towards a position). Pistons, drawers,
//          elevators.
//   distance the anchors stay `length` apart (a rod), or spring towards it,
//          within an optional [min, max] (a rope: 0 to its length), with an
//          optional motor along the line. Rotation is free.
//   d6     six axes of A's frame chosen one by one, each locked, limited, or
//          free, with an optional motor and spring: translation along X, Y,
//          and Z; twist about X and swing about Y and Z (swing-twist
//          decomposition, limited per axis: a pyramid where the ball joint's
//          cone is an ellipse). Vehicle suspensions, rigs, anything else.
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
    NPH_PHYSICS_JOINT_SLIDER = 3,
    NPH_PHYSICS_JOINT_DISTANCE = 4,
    NPH_PHYSICS_JOINT_D6 = 5,
    NPH_PHYSICS_JOINT_TYPE_COUNT = 6,
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

// A slider's range, motor, and spring along A's X axis. The position is B's
// anchor's offset from A's along that axis, in metres.
typedef struct NphPhysicsSliderJoint {
    b32 enable_limit;
    f32 lower;
    f32 upper;
    // Drives the relative speed along the axis to `motor_speed` (m/s) with
    // at most `max_motor_force` (N).
    b32 enable_motor;
    f32 motor_speed;
    f32 max_motor_force;
    // Pulls towards `spring_target` (m) like a damped spring of the given
    // frequency and damping ratio, whatever the bodies' masses.
    b32 enable_spring;
    f32 spring_hertz;
    f32 spring_damping_ratio;
    f32 spring_target;
} NphPhysicsSliderJoint;

// A distance joint: the anchors held `length` apart (m), rigidly, unless the
// spring is on, which pulls towards it softly; the limits then bound the
// spring's range (and alone, without the spring, make a rope or a strut:
// [0, length] lets the anchors close but not part).
typedef struct NphPhysicsDistanceJoint {
    f32 length;
    b32 enable_limit;
    f32 min_length;
    f32 max_length;
    b32 enable_spring;
    f32 spring_hertz;
    f32 spring_damping_ratio;
    // Drives the rate the anchors part at to `motor_speed` (m/s, positive
    // apart) with at most `max_motor_force` (N).
    b32 enable_motor;
    f32 motor_speed;
    f32 max_motor_force;
} NphPhysicsDistanceJoint;

// How a 6-DOF joint's axis moves.
typedef enum NphPhysicsAxisMotion {
    NPH_PHYSICS_AXIS_LOCKED = 0,
    NPH_PHYSICS_AXIS_LIMITED = 1, // within [lower, upper]
    NPH_PHYSICS_AXIS_FREE = 2,
} NphPhysicsAxisMotion;

// One axis of a 6-DOF joint: metres and m/s and N along a linear axis,
// radians and rad/s and N m about an angular one.
typedef struct NphPhysicsD6Axis {
    NphPhysicsAxisMotion motion;
    f32 lower;
    f32 upper;
    b32 enable_motor;
    f32 motor_speed;
    f32 max_motor_force;
    b32 enable_spring;
    f32 spring_hertz;
    f32 spring_damping_ratio;
    f32 spring_target;
} NphPhysicsD6Axis;

// A 6-DOF joint's axes in A's frame: translation along X, Y, Z (B's
// anchor's offset from A's), then twist about X and swing about Y and Z (of
// B's frame from A's; swings within (-pi, pi), twist within [-pi, pi]).
enum {
    NPH_PHYSICS_D6_LINEAR_X = 0,
    NPH_PHYSICS_D6_LINEAR_Y = 1,
    NPH_PHYSICS_D6_LINEAR_Z = 2,
    NPH_PHYSICS_D6_TWIST = 3,
    NPH_PHYSICS_D6_SWING_Y = 4,
    NPH_PHYSICS_D6_SWING_Z = 5,
    NPH_PHYSICS_D6_AXES = 6,
};

typedef struct NphPhysicsD6Joint {
    NphPhysicsD6Axis axes[NPH_PHYSICS_D6_AXES];
} NphPhysicsD6Joint;

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
        NphPhysicsSliderJoint slider;
        NphPhysicsDistanceJoint distance;
        NphPhysicsD6Joint d6;
    };
} NphPhysicsJointDesc;

// A joint of `type` between `a` and `b` at identity frames, its limits,
// motor, and spring off, not colliding (a distance joint 1 m long; a 6-DOF
// joint with every axis locked). Set the frames before creating
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
// dynamic, or settings out of range (finite values; swing angles in (0, pi);
// angle ranges lower <= upper within [-pi, pi], swings' within (-pi, pi);
// a distance's lengths >= 0 with min <= max; non-negative forces and
// torques; a positive spring frequency);
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

// Changes a joint's type-specific settings (as validated by
// nph_physics_joint_create) and wakes the bodies. Asserts on the wrong joint
// type or out-of-range values.
void nph_physics_joint_set_ball(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                const NphPhysicsBallJoint *ball);
void nph_physics_joint_set_slider(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                  const NphPhysicsSliderJoint *slider);
void nph_physics_joint_set_distance(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                    const NphPhysicsDistanceJoint *distance);
void nph_physics_joint_set_d6(NphPhysicsWorld *world, NphPhysicsJoint joint,
                              const NphPhysicsD6Joint *d6);
void nph_physics_joint_set_hinge(NphPhysicsWorld *world, NphPhysicsJoint joint,
                                 const NphPhysicsHingeJoint *hinge);

// A hinge's angle (radians, B's rotation about A's X axis from A's frame,
// in [-pi, pi]) and relative angular speed about it (rad/s), now.
f32 nph_physics_joint_hinge_angle(const NphPhysicsWorld *world, NphPhysicsJoint joint);
f32 nph_physics_joint_hinge_speed(const NphPhysicsWorld *world, NphPhysicsJoint joint);

// A slider's position along A's X axis (m) and its speed (m/s), now.
f32 nph_physics_joint_slider_position(const NphPhysicsWorld *world, NphPhysicsJoint joint);
f32 nph_physics_joint_slider_speed(const NphPhysicsWorld *world, NphPhysicsJoint joint);

// The distance between a joint's anchors (m), now (any joint type).
f32 nph_physics_joint_length(const NphPhysicsWorld *world, NphPhysicsJoint joint);

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
