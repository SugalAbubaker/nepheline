// SPDX-License-Identifier: Apache-2.0
#pragma once

// The character controller (CLAUDE.md §8.9; docs/physics.md "The character
// controller"; docs/adr/0049-physics-character-controller.md): a convex
// shape the game moves by displacements, which slides along what it meets,
// climbs steps, keeps off slopes too steep to walk, stays on the ground
// going down, and reports what it stands on and what it pushed.
//
// A character is not a body: the world does not know it. A move only
// queries the world (query.h), so it costs no allocation and no lock, and
// any number of characters may move at once, on any threads, between steps
// (NPH_THREADSAFE then; never during one). Bodies feel a character only
// through nph_physics_character_push, which the game calls with a move's
// hits, between steps.
//
// A move:
//   1. slides: casts the shape, rounded by the skin, along what is left of
//      the displacement, stops a skin off what it meets, and keeps the part
//      of the motion along the surfaces met (along the crease of two,
//      nothing at three), up to `iterations` times;
//   2. when walking (on walkable ground at the start and not moving up),
//      walks along the ground, its pace across kept up slopes, treats
//      surfaces steeper than `max_slope` as walls (it slides down them,
//      never up), climbs steps up to `step_height` (up, forward, and down
//      onto ground to stand on), and keeps to the ground over drops up to
//      `snap_distance` (slopes and stairs down);
//   3. pushes the shape out of anything nearer than the skin (what moved
//      into it since its last move, or a narrow corner's other wall), along
//      the contacts' normals;
//   4. finds the ground under the shape: walkable (by the contact's normal
//      or the surface's own: a capsule perched on a step's edge stands on
//      the step), too steep, or none, its body, normal, and the body's
//      velocity there (a platform's: the game adds it to the next
//      displacement to ride along).
//
// Positions are 64-bit, as bodies' are: a character anywhere moves as it
// would at the origin.

#include <nepheline/physics/query.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphPhysicsCharacterDesc {
    // Convex, valid; a capsule along the local Y axis is typical (its
    // rounded bottom slides over edges and steps).
    NphPhysicsGeometry shape;
    NphVec3 up;        // unit
    f32 max_slope;     // radians, in [0, pi/2): steeper surfaces are walls
    f32 step_height;   // metres, >= 0: climbed while walking
    f32 snap_distance; // metres, >= 0: kept to the ground over drops this small
    f32 skin;          // metres, > 0: the gap kept from every surface
    u32 iterations;    // slides a move may take, >= 1
    // What the character meets (its own body, if the game gave it one, as
    // `ignore`).
    NphPhysicsQueryFilter filter;
} NphPhysicsCharacterDesc;

// `shape`; up +Y; a 45 degree slope; steps of 0.3 m; snapping over 0.3 m; a
// 5 mm skin; 4 slides; the default query filter.
NphPhysicsCharacterDesc nph_physics_character_desc_default(NphPhysicsGeometry shape);

// NPH_RESULT_OK, or NPH_RESULT_INVALID_ARGUMENT for a NULL description, a
// shape that is not convex or not valid, an up that is not unit, or a
// parameter out of its range.
NphResult nph_physics_character_desc_validate(const NphPhysicsCharacterDesc *desc);

// What a character stands on.
typedef enum NphPhysicsCharacterGround {
    NPH_PHYSICS_CHARACTER_AIRBORNE = 0, // nothing under it, within twice the skin
    NPH_PHYSICS_CHARACTER_WALKABLE = 1, // ground no steeper than max_slope
    NPH_PHYSICS_CHARACTER_STEEP = 2,    // ground too steep to stand on: it slides down
} NphPhysicsCharacterGround;

// A character's state: the game owns it and passes it to each move.
typedef struct NphPhysicsCharacter {
    NphDVec3 position; // the shape's frame in the world
    NphQuat rotation;  // unit; moves never turn it
    // Found by the last move (and nph_physics_character_init):
    NphPhysicsCharacterGround ground;
    NphVec3 ground_normal;      // unit, out of the ground (up when airborne)
    NphDVec3 ground_point;      // where it touches the ground
    NphPhysicsBody ground_body; // NPH_PHYSICS_BODY_NULL when airborne
    NphPhysicsShape ground_shape;
    NphVec3 ground_velocity; // the ground body's at ground_point
} NphPhysicsCharacter;

// A surface a move ran into: for pushing bodies (nph_physics_character_push)
// and gameplay.
typedef struct NphPhysicsCharacterHit {
    NphPhysicsBody body;
    NphPhysicsShape shape;
    NphDVec3 point; // on the surface, in the world
    NphVec3 normal; // unit, out of the surface
    // The part of the displacement the surface took away (along -normal).
    NphVec3 blocked;
} NphPhysicsCharacterHit;

// Places a character at `position` with `rotation` and finds its ground.
// NPH_THREADSAFE between steps.
NphResult nph_physics_character_init(const NphPhysicsWorld *world,
                                     const NphPhysicsCharacterDesc *desc, NphDVec3 position,
                                     NphQuat rotation, NphPhysicsCharacter *out);

// Moves `character` by `displacement` (what the game wants this frame:
// velocity times the time step, gravity included), as the header's comment
// says, and finds its ground. Up to `max_hits` of the surfaces it ran into
// go to `hits` (NULL for none), their number to `*hit_count` (may be NULL).
// The displacement must be finite.
//
// NPH_RESULT_OK; NPH_RESULT_INVALID_ARGUMENT for NULL arguments, an invalid
// description, or a displacement that is not finite.
//
// NPH_THREADSAFE between steps: each character's state belongs to one
// thread at a time.
NphResult nph_physics_character_move(const NphPhysicsWorld *world,
                                     const NphPhysicsCharacterDesc *desc,
                                     NphPhysicsCharacter *character, NphVec3 displacement,
                                     NphPhysicsCharacterHit *hits, u32 max_hits, u32 *hit_count);

// Pushes the dynamic bodies among `hits` (a move's) as a body of `mass`
// (kg, > 0) moving at `velocity` (the character's this frame, m/s) would,
// inelastically: each takes, at its hit point and along the surface's normal
// into it, the impulse that brings the two to one speed there (their
// reduced mass, by the body's mass alone, times the speed they close at).
// The character is not slowed: the game drives it. A box pushed by a
// walking character comes up to its speed and goes no faster. Static and
// kinematic bodies are left alone.
// NPH_MAIN_THREAD_ONLY, between steps (it changes the world).
void nph_physics_character_push(NphPhysicsWorld *world, const NphPhysicsCharacterHit *hits,
                                u32 count, f32 mass, NphVec3 velocity);

#ifdef __cplusplus
}
#endif
