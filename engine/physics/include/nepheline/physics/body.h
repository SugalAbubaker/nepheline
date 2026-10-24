// SPDX-License-Identifier: Apache-2.0
#pragma once

// Rigid bodies (docs/physics.md "Bodies").
//
// A body has a 64-bit position (the origin of its frame), a rotation, and
// the shapes attached to it (shape.h). Its mass properties come from its
// shapes' densities unless set explicitly. Three types:
//
//   static     never moves (but can be teleported); infinite mass; the level
//   kinematic  moves by its velocity only, pushes dynamic bodies, is never
//              pushed back; infinite mass; platforms, doors
//   dynamic    moves under forces, gravity, and contacts
//
// Velocities are of the centre of mass, in world axes; the angular velocity
// is in radians per second.
//
// Calls that change how a body moves (velocity, forces, impulses, a
// teleport) wake its island. Thread-safety: as the world's (world.h).

#include <nepheline/physics/world.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphPhysicsBodyType {
    NPH_PHYSICS_BODY_STATIC = 0,
    NPH_PHYSICS_BODY_KINEMATIC = 1,
    NPH_PHYSICS_BODY_DYNAMIC = 2,
} NphPhysicsBodyType;

// Axes along (or about) which a dynamic body does not move: a 2.5D game
// locks Z and the rotations about X and Y; a character locks every rotation.
// World axes.
enum {
    NPH_PHYSICS_LOCK_LINEAR_X = 1U << 0U,
    NPH_PHYSICS_LOCK_LINEAR_Y = 1U << 1U,
    NPH_PHYSICS_LOCK_LINEAR_Z = 1U << 2U,
    NPH_PHYSICS_LOCK_ANGULAR_X = 1U << 3U,
    NPH_PHYSICS_LOCK_ANGULAR_Y = 1U << 4U,
    NPH_PHYSICS_LOCK_ANGULAR_Z = 1U << 5U,
};

typedef struct NphPhysicsBodyDesc {
    NphPhysicsBodyType type;
    NphDVec3 position;
    NphQuat rotation; // unit
    NphVec3 linear_velocity;
    NphVec3 angular_velocity;
    // Damping per second: v' = v / (1 + h c) each substep (a Pade fit of
    // exp(-c h)).
    f32 linear_damping;
    f32 angular_damping;
    f32 gravity_scale;
    // Below this speed, m/s (angular speed times the body's extent counts),
    // the body is still for sleeping.
    f32 sleep_threshold;
    u32 locks; // NPH_PHYSICS_LOCK_* bits
    b32 enable_sleep;
    b32 awake;
    // Integrate the gyroscopic torque implicitly (Catto, "Numerical
    // Methods", GDC 2015): spinning asymmetric bodies precess and tumble as
    // they should (the Dzhanibekov effect) without the explicit term's
    // instability.
    b32 gyroscopic;
    // A dynamic body swept, when fast, against kinematic and dynamic bodies
    // too, not only static ones (Box2D's bullets; world.h "Continuous
    // collision"): projectiles. Bullets do not sweep against each other.
    b32 bullet;
    u64 user_data;
} NphPhysicsBodyDesc;

// A dynamic body at the origin, at rest: damping 0, gravity scale 1, sleep
// threshold 0.05 m/s, sleep allowed, awake, gyroscopic, not a bullet.
NphPhysicsBodyDesc nph_physics_body_desc_default(void);

// NPH_RESULT_INVALID_ARGUMENT for a non-finite pose or velocity, a rotation
// that is not unit within 1e-3, or a bad type; NPH_RESULT_FULL past
// NPH_PHYSICS_MAX_OBJECTS; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_physics_body_create(NphPhysicsWorld *world,
                                               const NphPhysicsBodyDesc *desc, NphPhysicsBody *out);

// Destroys the body, its shapes, and their contacts (end events are not
// reported). A stale handle is ignored (asserted in development builds).
void nph_physics_body_destroy(NphPhysicsWorld *world, NphPhysicsBody body);

bool nph_physics_body_is_valid(const NphPhysicsWorld *world, NphPhysicsBody body);

NphPhysicsBodyType nph_physics_body_type(const NphPhysicsWorld *world, NphPhysicsBody body);

NphDVec3 nph_physics_body_position(const NphPhysicsWorld *world, NphPhysicsBody body);
NphQuat nph_physics_body_rotation(const NphPhysicsWorld *world, NphPhysicsBody body);
// The centre of mass in the world.
NphDVec3 nph_physics_body_center_of_mass(const NphPhysicsWorld *world, NphPhysicsBody body);

// Teleports the body (no velocity is implied). Its shapes' proxies move at
// once; contacts update at the next step.
void nph_physics_body_set_transform(NphPhysicsWorld *world, NphPhysicsBody body, NphDVec3 position,
                                    NphQuat rotation);

NphVec3 nph_physics_body_linear_velocity(const NphPhysicsWorld *world, NphPhysicsBody body);
NphVec3 nph_physics_body_angular_velocity(const NphPhysicsWorld *world, NphPhysicsBody body);
void nph_physics_body_set_linear_velocity(NphPhysicsWorld *world, NphPhysicsBody body,
                                          NphVec3 velocity);
void nph_physics_body_set_angular_velocity(NphPhysicsWorld *world, NphPhysicsBody body,
                                           NphVec3 velocity);

// Forces and torques accumulate until the next step, which applies and
// clears them. A force at a point also torques about the centre of mass.
// Dynamic bodies only (ignored on others).
void nph_physics_body_apply_force(NphPhysicsWorld *world, NphPhysicsBody body, NphVec3 force,
                                  NphDVec3 point);
void nph_physics_body_apply_force_at_center(NphPhysicsWorld *world, NphPhysicsBody body,
                                            NphVec3 force);
void nph_physics_body_apply_torque(NphPhysicsWorld *world, NphPhysicsBody body, NphVec3 torque);

// Impulses change the velocity now. Dynamic bodies only.
void nph_physics_body_apply_linear_impulse(NphPhysicsWorld *world, NphPhysicsBody body,
                                           NphVec3 impulse, NphDVec3 point);
void nph_physics_body_apply_angular_impulse(NphPhysicsWorld *world, NphPhysicsBody body,
                                            NphVec3 impulse);

// The mass properties in the body's frame: from the shapes' densities, or
// as set. Static and kinematic bodies report their shapes' mass but move as
// if infinitely heavy.
NphPhysicsMassData nph_physics_body_mass_data(const NphPhysicsWorld *world, NphPhysicsBody body);

// Overrides the mass properties (body frame; the tensor about the centre).
// A non-positive mass returns the body to its shapes' mass. Asserts on a
// non-finite value or a tensor that is not positive definite.
void nph_physics_body_set_mass_data(NphPhysicsWorld *world, NphPhysicsBody body,
                                    const NphPhysicsMassData *mass);

bool nph_physics_body_is_bullet(const NphPhysicsWorld *world, NphPhysicsBody body);
// Makes a dynamic body a bullet or not (ignored on static and kinematic
// bodies), from the next step.
void nph_physics_body_set_bullet(NphPhysicsWorld *world, NphPhysicsBody body, bool bullet);

// How the body moves, as its description set it (NphPhysicsBodyDesc),
// changed in place from the next step: what the runtime does when a game
// edits a RigidBody component while it plays. Asserted and ignored: a
// non-finite value, a negative damping or threshold, a lock outside
// NPH_PHYSICS_LOCK_*. A new gravity scale wakes the body, which now falls
// differently, as does turning its sleep off; damping, locks, and the
// threshold take effect when it next moves.
void nph_physics_body_set_damping(NphPhysicsWorld *world, NphPhysicsBody body, f32 linear,
                                  f32 angular);
void nph_physics_body_set_gravity_scale(NphPhysicsWorld *world, NphPhysicsBody body, f32 scale);
void nph_physics_body_set_locks(NphPhysicsWorld *world, NphPhysicsBody body, u32 locks);
void nph_physics_body_set_sleep(NphPhysicsWorld *world, NphPhysicsBody body, bool enable,
                                f32 threshold);

bool nph_physics_body_is_awake(const NphPhysicsWorld *world, NphPhysicsBody body);
// Wakes the body's island, or puts it to sleep (dynamic and kinematic
// bodies; ignored on static ones).
void nph_physics_body_set_awake(NphPhysicsWorld *world, NphPhysicsBody body, bool awake);

u64 nph_physics_body_user_data(const NphPhysicsWorld *world, NphPhysicsBody body);
void nph_physics_body_set_user_data(NphPhysicsWorld *world, NphPhysicsBody body, u64 user_data);

// The body's shapes, through `out` (at most `capacity`); returns the count.
u32 nph_physics_body_shapes(const NphPhysicsWorld *world, NphPhysicsBody body, NphPhysicsShape *out,
                            u32 capacity);

#ifdef __cplusplus
}
#endif
