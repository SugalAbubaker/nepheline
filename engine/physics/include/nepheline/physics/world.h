// SPDX-License-Identifier: Apache-2.0
#pragma once

// The physics world (CLAUDE.md §8.9; docs/physics.md "The world"; docs/adr/0043-physics-world.md):
// bodies with shapes, stepped at a fixed rate on the job system.
//
// A step (nph_physics_world_step):
//
//   1. pairs: every shape whose broadphase box grew last step queries the
//      trees (static, kinematic, dynamic), and new overlapping pairs become
//      contacts;
//   2. collide: every contact of an awake body updates its manifold in
//      parallel; contacts that began or stopped touching join or leave the
//      constraint graph and the islands, in contact order;
//   3. solve: the soft-step solver (Catto, "Solver2D", 2024, after Macklin
//      et al., "Small Steps in Physics Simulation", SCA 2019): per substep,
//      integrate velocities, warm start, solve with soft contacts, integrate
//      positions, relax without softness; then restitution; colour by colour,
//      eight contacts per instruction, the colours' contacts in parallel;
//   4. finalize: positions in 64 bits, shapes' boxes, the boxes that left
//      their fattened proxies grow in the trees, sleep timers;
//   5. islands that stayed still long enough fall asleep, and cost nothing
//      until something touches them.
//
// Determinism: the same calls give the same world, bit for bit, whatever
// the job system's worker count (or none): every parallel piece writes only
// its own data, and every merge happens in a fixed order.
//
// Costs (§1.4): creation allocates from the description's allocator; the
// world's arrays grow from it when bodies, shapes, or contacts outnumber
// their capacity (the step grows them only between its parallel stages),
// and keep their size, so a scene in steady state allocates nothing. Each
// step with a job system runs one batch of helper jobs, as many as its work
// pays for, that spin through the solver's stages (the calling thread works
// too).
//
// Thread-safety: a world belongs to one thread at a time. The step uses the
// job system internally; nothing may call into the world while it runs.

#include <nepheline/jobs/jobs.h>
#include <nepheline/physics/geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphPhysicsWorld NphPhysicsWorld;

// A body or a shape: a 32-bit generational handle (20 bits of slot, 12 of
// generation), stale once the object is destroyed. Zero is never valid.
typedef struct NphPhysicsBody {
    u32 value;
} NphPhysicsBody;

typedef struct NphPhysicsShape {
    u32 value;
} NphPhysicsShape;

#define NPH_PHYSICS_BODY_NULL  ((NphPhysicsBody){0U})
#define NPH_PHYSICS_SHAPE_NULL ((NphPhysicsShape){0U})

NPH_INLINE bool nph_physics_body_is_null(NphPhysicsBody b)
{
    return b.value == 0U;
}
NPH_INLINE bool nph_physics_shape_is_null(NphPhysicsShape s)
{
    return s.value == 0U;
}

// The most bodies or shapes a world holds (the slot bits of a handle).
#define NPH_PHYSICS_MAX_OBJECTS (1U << 20U)

// The constraint solver (ADR-0045; docs/physics.md "Choosing a solver"):
// the soft step by default, TGS and XPBD for their trade-offs. Each
// substeps the step and solves contacts and joints in the constraint
// graph's colours on the same threads, from the same collision.
typedef enum NphPhysicsSolver {
    // Catto's soft step (Box2D v3): per substep, a soft solve towards the
    // contacts' and joints' springs, then a rigid relax.
    NPH_PHYSICS_SOLVER_SOFT_STEP = 0,
    // Temporal Gauss-Seidel (PhysX 4): per substep, a rigid solve with a
    // Baumgarte bias on the error since the step began; one velocity pass
    // without bias after the substeps. 9 to 16 % cheaper; joints under
    // heavy loads do not converge.
    NPH_PHYSICS_SOLVER_TGS = 1,
    // Extended position-based dynamics (Mueller et al., SCA 2020): per
    // substep, positions corrected by rigid (or compliant) constraints,
    // velocities derived from them, then a velocity pass for friction,
    // restitution, and motors. The stiffest joints (with 16 substeps); it
    // cannot stack, and its friction and energy are approximate.
    NPH_PHYSICS_SOLVER_XPBD = 2,
    NPH_PHYSICS_SOLVER_COUNT = 3,
} NphPhysicsSolver;

typedef struct NphPhysicsWorldDesc {
    NphAllocator *alloc; // required: everything the world keeps
    NphVec3 gravity;     // m/s^2
    NphPhysicsSolver solver;
    // Solver substeps per step: each integrates, solves softly, and relaxes
    // rigidly. Four at 240 Hz is 960 Hz of solver rate. With the default
    // stiffnesses (below), the one quality setting: twice the substeps,
    // twice the stiffness, a quarter of the give.
    u32 substeps;
    // The contacts' spring: stiffness (Hz; 0, the default: an eighth of the
    // substep rate, Box2D's ratio; at most a quarter of it; contacts with
    // static and kinematic bodies are twice as stiff) and damping ratio.
    // Soft contacts push overlaps out smoothly instead of exploding (Catto
    // 2024), and hold a load at a depth of load / mass * g / (2 pi hertz)^2.
    f32 contact_hertz;
    f32 contact_damping_ratio;
    // The fastest an overlap is pushed out, m/s.
    f32 contact_push_max_velocity;
    // Normal approach speed below which contacts do not bounce, m/s.
    f32 restitution_threshold;
    // Clamp for bodies' speeds, m/s.
    f32 max_linear_speed;
    // The joints' spring (joint.h): stiffness (Hz; 0, the default: a quarter
    // of the substep rate, also the most) and damping ratio of the soft solve
    // that pulls a joint back together; the relax pass then holds it
    // rigidly.
    f32 joint_hertz;
    f32 joint_damping_ratio;
    // Bodies whose islands stay below their sleep threshold this long, in
    // seconds, fall asleep.
    f32 time_to_sleep;
    b32 enable_sleep;
    // Initial capacities (the arrays grow past them).
    u32 body_capacity;
    u32 shape_capacity;
    u32 contact_capacity;
} NphPhysicsWorldDesc;

// The defaults: gravity (0, -9.81, 0), the soft step with 4 substeps,
// contacts at an eighth of the substep rate (120 Hz at 4 substeps, Box2D's
// 30 Hz at its 240) with damping ratio 10, joints at a quarter of it
// (240 Hz) with damping ratio 2, a 3 m/s push, a 1 m/s restitution
// threshold, 400 m/s, half a second to sleep, sleep on, and small initial
// capacities. Set `alloc` before creating.
NphPhysicsWorldDesc nph_physics_world_desc_default(void);

// NPH_RESULT_INVALID_ARGUMENT for a NULL allocator, zero substeps, or a
// non-finite or negative setting; NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_physics_world_create(const NphPhysicsWorldDesc *desc,
                                                NphPhysicsWorld **out);

// Destroys the world with every body, shape, and contact. Safe on NULL.
void nph_physics_world_destroy(NphPhysicsWorld *world);

// Advances the world by `dt` seconds (> 0; the engine steps at a fixed
// 1/240). `jobs` may be NULL: the step then runs on the calling thread
// alone, with the same result. NPH_RESULT_INVALID_ARGUMENT for a bad dt;
// NPH_RESULT_OUT_OF_MEMORY when growing an array failed (the step is
// abandoned between stages; the world stays consistent but is not advanced).
NPH_MUST_USE NphResult nph_physics_world_step(NphPhysicsWorld *world, f32 dt, NphJobSystem *jobs);

void nph_physics_world_set_gravity(NphPhysicsWorld *world, NphVec3 gravity);
NphVec3 nph_physics_world_gravity(const NphPhysicsWorld *world);

// ---------------------------------------------------------------------------
// Events: what the last step did, valid until the next step or destroy.
// ---------------------------------------------------------------------------

typedef struct NphPhysicsContactBeginEvent {
    NphPhysicsShape shape_a;
    NphPhysicsShape shape_b;
} NphPhysicsContactBeginEvent;

typedef struct NphPhysicsContactEndEvent {
    NphPhysicsShape shape_a;
    NphPhysicsShape shape_b;
} NphPhysicsContactEndEvent;

// A contact that began touching at speed: for sounds and effects.
typedef struct NphPhysicsContactHitEvent {
    NphPhysicsShape shape_a;
    NphPhysicsShape shape_b;
    NphDVec3 point;     // where, in the world
    NphVec3 normal;     // from shape A towards shape B
    f32 approach_speed; // m/s along the normal, > the hit threshold
} NphPhysicsContactHitEvent;

// A body that moved during the step (every awake body), with its new pose:
// how the runtime mirrors physics into the scene.
typedef struct NphPhysicsBodyMoveEvent {
    NphPhysicsBody body;
    u32 fell_asleep; // the body's island fell asleep at the end of this step
    u64 user_data;
    NphDVec3 position;
    NphQuat rotation;
} NphPhysicsBodyMoveEvent;

typedef struct NphPhysicsEvents {
    const NphPhysicsContactBeginEvent *begin;
    u32 begin_count;
    const NphPhysicsContactEndEvent *end;
    u32 end_count;
    const NphPhysicsContactHitEvent *hit;
    u32 hit_count;
    const NphPhysicsBodyMoveEvent *move;
    u32 move_count;
} NphPhysicsEvents;

// Contact events are reported for shapes with `contact_events`, hit events
// for shapes with `hit_events` (shape.h), and move events for every awake
// body. The arrays are the world's; they are overwritten by the next step.
NphPhysicsEvents nph_physics_world_events(const NphPhysicsWorld *world);

// The approach speed above which touching contacts report hits (default
// 1 m/s).
void nph_physics_world_set_hit_threshold(NphPhysicsWorld *world, f32 speed);

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

typedef struct NphPhysicsStats {
    u32 bodies;
    u32 awake_bodies;
    u32 shapes;
    u32 contacts;          // pairs whose fattened boxes overlap
    u32 touching_contacts; // of those, with manifold points
    u32 manifold_points;
    u32 joints;
    u32 islands;
    u32 awake_islands;
    u32 colors_used;          // constraint-graph colours holding contacts
    u32 overflow_constraints; // contacts no colour could take, solved serially
    u32 moved_proxies;        // broadphase proxies that grew last step
    u32 new_pairs;
    u32 workers; // threads that worked the solver's stages
    u64 steps;
    // The last step's wall time and its stages', in milliseconds.
    f64 step_ms;
    f64 pairs_ms;
    f64 collide_ms;
    f64 solve_ms;
    f64 finalize_ms;
    f64 sleep_ms;
} NphPhysicsStats;

NphPhysicsStats nph_physics_world_stats(const NphPhysicsWorld *world);

// A hash of every body's pose and velocities (their bits), for determinism
// tests: two worlds stepped the same way hash the same.
u64 nph_physics_world_hash(const NphPhysicsWorld *world);

// Checks the world's internal invariants (lists, islands, the constraint
// graph, the trees). Slow; for tests.
bool nph_physics_world_validate(const NphPhysicsWorld *world);

// ---------------------------------------------------------------------------
// Debug drawing
// ---------------------------------------------------------------------------

enum {
    NPH_PHYSICS_DRAW_SHAPES = 1U << 0U,
    NPH_PHYSICS_DRAW_AABBS = 1U << 1U,    // the broadphase's fattened boxes
    NPH_PHYSICS_DRAW_CONTACTS = 1U << 2U, // points and normals
    NPH_PHYSICS_DRAW_MASS = 1U << 3U,     // centres of mass
    NPH_PHYSICS_DRAW_SLEEP = 1U << 4U,    // sleeping bodies' shapes in their own colour
    NPH_PHYSICS_DRAW_JOINTS = 1U << 5U,   // anchors joined to the centres, and the frames' X axes
};

// Lines in 32-bit coordinates relative to `origin` (the renderer's debug
// origin), so drawing keeps its precision far from the world's.
typedef struct NphPhysicsDebugDraw {
    void (*line)(void *user, NphVec3 a, NphVec3 b, u32 rgba);
    void *user;
    NphDVec3 origin;
    u32 flags; // NPH_PHYSICS_DRAW_*
} NphPhysicsDebugDraw;

// Draws the world through `draw`. Reads only; between steps.
void nph_physics_world_draw(const NphPhysicsWorld *world, const NphPhysicsDebugDraw *draw);

#ifdef __cplusplus
}
#endif
