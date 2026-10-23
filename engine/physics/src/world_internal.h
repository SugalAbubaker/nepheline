// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal to engine/physics: the world's records and the solver's step
// data (docs/physics.md "The world"; ADR-0043).

#include <nepheline/core/array.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/bitset.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/physics/aabb_tree.h>
#include <nepheline/physics/joint.h>
#include <nepheline/physics/shape.h>

#include "physics_internal.h"

#define PHYS_NULL NPH_U32_MAX

// Handles: 20 bits of slot, 12 of generation (1 to 4095, never 0).
#define PHYS_SLOT_BITS 20U
#define PHYS_SLOT_MASK ((1U << PHYS_SLOT_BITS) - 1U)
#define PHYS_GEN_MAX   4095U

NPH_INLINE u32 phys_handle(u32 slot, u32 generation)
{
    return (generation << PHYS_SLOT_BITS) | slot;
}
NPH_INLINE u32 phys_handle_slot(u32 handle)
{
    return handle & PHYS_SLOT_MASK;
}

// Slots for one kind of object: the generation each slot hands out next,
// and the free slots (last in, first out).
typedef struct PhysSlots {
    NphArrayU32 generation;
    NphArrayU32 free;
} PhysSlots;

// The broadphase's three trees: static shapes are queried by moving ones
// and never query, kinematic shapes only meet dynamic ones.
enum {
    PHYS_TREE_STATIC = 0,
    PHYS_TREE_KINEMATIC = 1,
    PHYS_TREE_DYNAMIC = 2,
    PHYS_TREE_COUNT = 3,
};

NPH_INLINE u32 phys_proxy_key(u32 proxy, u32 tree)
{
    return (proxy << 2U) | tree;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

enum {
    PHYS_BODY_AWAKE = 1U << 0U,
    PHYS_BODY_ENABLE_SLEEP = 1U << 1U,
    PHYS_BODY_GYROSCOPIC = 1U << 2U,
    PHYS_BODY_MASS_SET = 1U << 3U, // mass properties set explicitly
    PHYS_BODY_ENLARGED = 1U << 4U, // a shape's proxy grew this step
};

typedef struct PhysBody {
    u32 handle;           // 0 for a free slot
    u32 type;             // NphPhysicsBodyType
    u32 flags;            // PHYS_BODY_*
    u32 locks;            // NPH_PHYSICS_LOCK_*
    NphDVec3 origin;      // the body frame's origin in the world
    NphQuat rotation;     // unit
    NphDVec3 center;      // the centre of mass in the world
    NphVec3 local_center; // in the body frame
    NphVec3 linear_velocity;
    NphVec3 angular_velocity;
    NphVec3 force; // accumulated for the next step, at the centre of mass
    NphVec3 torque;
    f32 mass;
    f32 inv_mass;    // 0 for static and kinematic bodies
    NphMat3 inertia; // about the centre of mass, body axes
    NphMat3 inv_inertia;
    f32 linear_damping;
    f32 angular_damping;
    f32 gravity_scale;
    f32 sleep_threshold;
    f32 sleep_time;
    f32 max_extent; // the farthest shape point from the centre of mass
    u32 shape_head;
    u32 shape_count;
    u32 contact_head; // a contact key (contact << 1 | side), or PHYS_NULL
    u32 contact_count;
    u32 joint_head; // a joint key (joint << 1 | side), or PHYS_NULL
    u32 joint_count;
    u32 island; // dynamic bodies only; PHYS_NULL otherwise
    u32 island_prev;
    u32 island_next;
    u32 awake_slot; // in world->awake_bodies; PHYS_NULL when asleep or static
    u32 move_event; // its move event of the last step
    u64 user_data;
} PhysBody;

enum {
    PHYS_SHAPE_CONTACT_EVENTS = 1U << 0U,
    PHYS_SHAPE_HIT_EVENTS = 1U << 1U,
    PHYS_SHAPE_ENLARGED = 1U << 2U, // its proxy grew this step
    PHYS_SHAPE_IN_MOVE_BUFFER = 1U << 3U,
};

typedef struct PhysShape {
    u32 handle; // 0 for a free slot
    u32 body;   // slot
    u32 next;   // the body's next shape
    u32 flags;  // PHYS_SHAPE_*
    NphPhysicsGeometry geometry;
    NphPhysicsPose pose; // in the body
    f32 density;
    f32 friction;
    f32 restitution;
    NphPhysicsFilter filter;
    u32 tree;         // PHYS_TREE_*
    u32 proxy;        // in its tree
    NphAABB aabb;     // tight, world, 32-bit rounded outward
    NphAABB fat_aabb; // the proxy's box
    u64 user_data;
} PhysShape;

typedef struct PhysContactPoint {
    NphVec3 anchor_a; // from A's centre of mass, world axes
    NphVec3 anchor_b; // from B's centre of mass
    f32 separation;
    u32 id;
    f32 normal_impulse;
    f32 max_normal_impulse;  // the largest this step (for hit events)
    NphVec3 tangent_impulse; // world: kept across bases (tangents rotate)
    f32 relative_velocity;   // normal, at the step's start (restitution)
} PhysContactPoint;

enum {
    PHYS_CONTACT_TOUCHING = 1U << 0U,
    PHYS_CONTACT_EVENTS = 1U << 1U,     // either shape reports begin and end
    PHYS_CONTACT_HIT_EVENTS = 1U << 2U, // either shape reports hits
    PHYS_CONTACT_ISLAND = 1U << 3U,     // linked in an island (two dynamic bodies)
};

// What the parallel narrow phase found, for the serial update after it.
enum {
    PHYS_CHANGE_STARTED = 1U,
    PHYS_CHANGE_STOPPED = 2U,
    PHYS_CHANGE_DISJOINT = 4U,
};

typedef struct PhysContact {
    u32 shape_a; // slots
    u32 shape_b;
    u32 body_a;
    u32 body_b;
    u32 flags;  // PHYS_CONTACT_*
    u32 change; // PHYS_CHANGE_*, written by the narrow phase
    // The bodies' contact lists: keys (contact << 1 | side).
    u32 prev[2];
    u32 next[2];
    u32 color;      // constraint graph colour (PHYS_NULL: not in the graph)
    u32 color_slot; // position in the colour's array
    u32 island;
    u32 island_prev;
    u32 island_next;
    u32 awake_slot; // in world->awake_contacts; PHYS_NULL when asleep
    f32 friction;
    f32 restitution;
    NphPhysicsCollideCache cache;
    NphVec3 normal; // from A to B
    u32 point_count;
    PhysContactPoint points[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
} PhysContact;

// A soft constraint's coefficients (Catto, "Solver2D"; the implicit spring
// of "Soft Constraints", GDC 2011): impulse = -mass * mass_scale * (Cdot +
// bias_rate * C) - impulse_scale * accumulated.
typedef struct PhysSoftness {
    f32 bias_rate;
    f32 mass_scale;
    f32 impulse_scale;
} PhysSoftness;

// For frequency `hertz` (0: rigid, no bias), damping ratio `zeta`, and
// time step `h`.
PhysSoftness phys_make_soft(f32 hertz, f32 zeta, f32 h);

enum {
    PHYS_JOINT_COLLIDE_CONNECTED = 1U << 0U,
    PHYS_JOINT_ISLAND = 1U << 1U, // linked in an island (two dynamic bodies)
};

enum {
    PHYS_ROW_LOCK = 0, // or the lower limit
    PHYS_ROW_UPPER = 1,
    PHYS_ROW_MOTOR = 2,
    PHYS_ROW_SPRING = 3,
};

typedef struct PhysJoint {
    u32 handle; // 0 for a free slot
    u32 type;   // NphPhysicsJointType
    u32 body_a; // slots
    u32 body_b;
    u32 flags; // PHYS_JOINT_*
    // The bodies' joint lists: keys (joint << 1 | side).
    u32 prev[2];
    u32 next[2];
    u32 color;      // constraint graph colour (PHYS_NULL: not in the graph)
    u32 color_slot; // position in the colour's array
    u32 island;
    u32 island_prev;
    u32 island_next;
    NphPhysicsPose frame_a; // in body A's frame
    NphPhysicsPose frame_b; // in body B's frame
    union {
        NphPhysicsBallJoint ball;
        NphPhysicsHingeJoint hinge;
        NphPhysicsSliderJoint slider;
        NphPhysicsDistanceJoint distance;
        NphPhysicsD6Joint d6;
    };
    u64 user_data;
    // Accumulated impulses, per substep, kept between steps (warm start).
    NphVec3 linear_impulse;  // the anchors as one block, world
    NphVec3 angular_impulse; // the aligned axes (hinge) or the whole rotation (fixed), world
    // A 6-DOF joint's (and a slider's) rows per axis, and a distance joint's
    // on axis 0: the lock or lower limit, the upper limit, the motor, the
    // spring (PHYS_ROW_*).
    f32 axis_impulse[NPH_PHYSICS_D6_AXES][4];
    NphVec3 friction_impulse; // a ball's friction, world
    f32 lower_impulse;        // a hinge's limit or a ball's twist limits, about X
    f32 upper_impulse;
    f32 swing_impulse; // a ball's cone
    f32 motor_impulse;
    f32 spring_impulse;
    // The step's data (phys_joint_prepare).
    u32 solver_a; // solver body indices (the dummy for static ones)
    u32 solver_b;
    f32 inv_mass_a; // 0 unless dynamic
    f32 inv_mass_b;
    NphMat3 inv_inertia_a; // world, at the step's start
    NphMat3 inv_inertia_b;
    NphVec3 anchor_a;     // from A's centre of mass, world axes, at the step's start
    NphVec3 anchor_b;     // from B's
    NphVec3 delta_center; // B's centre of mass less A's, at the step's start
    NphQuat frame_q_a;    // the frames' rotations in the world at the step's start
    NphQuat frame_q_b;
    PhysSoftness spring_soft;
} PhysJoint;

typedef struct PhysIsland {
    u32 live;
    u32 awake;
    u32 body_head;
    u32 body_tail;
    u32 body_count;
    u32 contact_head;
    u32 contact_tail;
    u32 contact_count;
    u32 joint_head;
    u32 joint_tail;
    u32 joint_count;
    u32 parent;                  // a pending merge into another island (union-find), or PHYS_NULL
    u32 constraint_remove_count; // contacts and joints removed since the last split: it may fall
                                 // apart
    u32 awake_slot;              // in world->awake_islands
    f32 sleep_time;              // the least of its bodies', at the last step
} PhysIsland;

// ---------------------------------------------------------------------------
// The constraint graph (Catto, "Solver2D"; Box2D v3's graph colouring):
// a colour holds contacts that share no dynamic body, so a colour's
// contacts solve in parallel. Contacts with a static or kinematic body
// leave colour 0 to dynamic pairs; what fits nowhere overflows and is
// solved serially.
// ---------------------------------------------------------------------------

#define PHYS_GRAPH_COLORS         24U
#define PHYS_GRAPH_DYNAMIC_COLORS 20U
#define PHYS_GRAPH_OVERFLOW       PHYS_GRAPH_COLORS

typedef struct PhysColor {
    NphBitset bodies; // dynamic bodies with a contact or joint in this colour (by slot)
    NphArrayU32 contacts;
    NphArrayU32 joints;
} PhysColor;

// ---------------------------------------------------------------------------
// The solver's step data
// ---------------------------------------------------------------------------

#define PHYS_LANES 8U

// A body's state in the solver: one cache line of four 16-byte rows, which
// the kernels gather eight at a time by 4x4 transposes (`rows`).
typedef union PhysSolverBody {
    struct {
        NphVec3 v;
        f32 pad0;
        NphVec3 w;
        f32 pad1;
        NphVec3 dp; // displacement of the centre of mass this step
        f32 pad2;
        NphQuat dq; // rotation this step
    };
    f32 rows[4][4];
} PhysSolverBody;

NPH_STATIC_ASSERT(sizeof(PhysSolverBody) == 64U, "a solver body is one cache line");

// What integration needs about a body, fixed for the step.
typedef struct PhysSolverParams {
    NphVec3 linear_accel;  // gravity * scale + force / mass
    f32 linear_damping;    // the factor per substep, 1 / (1 + h c)
    NphVec3 angular_accel; // I^-1 torque, world
    f32 angular_damping;
    NphMat3 inv_inertia;      // world, at the step's start
    NphMat3 inertia_local;    // for the gyroscopic term
    NphQuat q0;               // the rotation at the step's start
    f32 max_angular_speed_sq; // an eighth of a turn per substep (Box2D's limit)
    f32 max_linear_speed_sq;
    u32 body;  // slot
    u32 flags; // PHYS_PARAM_*
    u32 locks;
    u32 pad;
} PhysSolverParams;

enum {
    PHYS_PARAM_DYNAMIC = 1U << 0U,
    PHYS_PARAM_GYROSCOPIC = 1U << 1U,
};

// Eight contacts, one per lane; their points in `point_count` consecutive
// PhysPointLanes. Lanes past `lane_count` (and points past a lane's own
// count) have zero masses and change nothing.
typedef struct PhysContactLanes {
    u32 body_a[PHYS_LANES]; // solver body indices (the dummy for static ones)
    u32 body_b[PHYS_LANES];
    u32 contact[PHYS_LANES]; // PHYS_NULL in empty lanes
    u32 write_mask_a;        // bit per lane: the body takes the result
    u32 write_mask_b;
    u32 lane_count;
    u32 point_count; // the largest of the lanes'
    u32 first_point; // into the step's point array
    u32 pad[3];
    f32 inv_mass_a[PHYS_LANES];
    f32 inv_mass_b[PHYS_LANES];
    f32 normal[3][PHYS_LANES];
    f32 tangent1[3][PHYS_LANES];
    f32 tangent2[3][PHYS_LANES];
    f32 friction[PHYS_LANES];
    f32 restitution[PHYS_LANES];
    f32 bias_rate[PHYS_LANES];
    f32 mass_scale[PHYS_LANES];
    f32 impulse_scale[PHYS_LANES];
} PhysContactLanes;

typedef struct PhysPointLanes {
    f32 anchor_a[3][PHYS_LANES];
    f32 anchor_b[3][PHYS_LANES];
    f32 adjusted_separation[PHYS_LANES];
    f32 normal_mass[PHYS_LANES];
    f32 normal_impulse[PHYS_LANES];
    f32 max_normal_impulse[PHYS_LANES];
    f32 relative_velocity[PHYS_LANES];
    f32 substep_velocity[PHYS_LANES]; // XPBD: the normal speed before a substep's positions
    f32 tangent_mass[3][PHYS_LANES];  // the inverse of the 2x2 effective mass: 00, 01, 11
    f32 tangent_impulse[2][PHYS_LANES];
    // I^-1 (r x axis) for each body and axis: impulses apply without the
    // inertia (Jolt's "invI r x axis" terms).
    f32 ang_a_n[3][PHYS_LANES];
    f32 ang_b_n[3][PHYS_LANES];
    f32 ang_a_t1[3][PHYS_LANES];
    f32 ang_b_t1[3][PHYS_LANES];
    f32 ang_a_t2[3][PHYS_LANES];
    f32 ang_b_t2[3][PHYS_LANES];
} PhysPointLanes;

// What prepare_group gathers for the prepare kernel besides the rows'
// anchors, raw separations, and normal impulses.
typedef struct PhysPrepareLanes {
    f32 inv_inertia_a[9][PHYS_LANES]; // world, column-major (column * 3 + row)
    f32 inv_inertia_b[9][PHYS_LANES];
    f32 tangent_impulse[NPH_PHYSICS_MANIFOLD_MAX_POINTS][3][PHYS_LANES]; // last step's, world
    f32 active[NPH_PHYSICS_MANIFOLD_MAX_POINTS][PHYS_LANES]; // 1: the lane has the point
} PhysPrepareLanes;

// The solver's stages, unrolled for a step.
typedef enum PhysStageType {
    PHYS_STAGE_PREPARE_BODIES = 0,
    PHYS_STAGE_PREPARE_CONTACTS, // and the joints
    PHYS_STAGE_INTEGRATE_VELOCITIES,
    PHYS_STAGE_WARM_START,
    PHYS_STAGE_SOLVE,
    PHYS_STAGE_INTEGRATE_POSITIONS,
    PHYS_STAGE_RELAX,
    PHYS_STAGE_RESTITUTION,
    PHYS_STAGE_STORE,
    PHYS_STAGE_FINALIZE,
    // XPBD (NPH_PHYSICS_SOLVER_XPBD): per substep, predict the bodies,
    // correct positions, derive velocities, then the velocity pass.
    PHYS_STAGE_XPBD_PREDICT,
    PHYS_STAGE_XPBD_POSITIONS,
    PHYS_STAGE_XPBD_DERIVE,
    PHYS_STAGE_XPBD_VELOCITIES,
} PhysStageType;

// A stage's blocks are claimed one by one through its claim words (in the
// world's `claims`), so each participant can start at its own share of the
// blocks and keep its data in its own cache from substep to substep.
//
// A colour's stages hold its joints and its contacts: the joints' blocks
// first (indices into the step's joint array), then the contact groups'.
typedef struct PhysStage {
    NphAtomicU32 done; // blocks finished, added once per participant
    u32 type;          // PhysStageType
    u32 first;         // the first item (contact group, body, or step joint)
    u32 count;         // items
    u32 block_size;
    u32 block_count; // all blocks, the joints' included
    u32 joint_first; // the first step joint
    u32 joint_count;
    u32 joint_block_size;
    u32 joint_blocks;
    u32 claim_first; // the stage's first claim word
    u32 pad;
} PhysStage;

// One contact kernel set per ISA, all with eight lanes and the same
// arithmetic (solver_impl.h), so the results agree to the bit.
typedef struct PhysSolverKernels {
    const char *name;
    void (*prepare)(const PhysSolverBody *bodies, const PhysContactLanes *g, PhysPointLanes *points,
                    const PhysPrepareLanes *in);
    void (*warm_start)(PhysSolverBody *bodies, PhysContactLanes *g, PhysPointLanes *points);
    void (*solve)(PhysSolverBody *bodies, PhysContactLanes *g, PhysPointLanes *points, f32 inv_h,
                  f32 push_max, f32 slop, bool use_bias);
    void (*restitution)(PhysSolverBody *bodies, PhysContactLanes *g, PhysPointLanes *points,
                        f32 threshold);
    void (*xpbd_positions)(PhysSolverBody *bodies, const PhysSolverBody *prev, PhysContactLanes *g,
                           PhysPointLanes *points);
    void (*xpbd_velocities)(PhysSolverBody *bodies, PhysContactLanes *g, PhysPointLanes *points,
                            f32 inv_h, f32 threshold);
} PhysSolverKernels;

extern const PhysSolverKernels g_phys_solver_sse4;
extern const PhysSolverKernels g_phys_solver_avx2;

typedef NPH_ARRAY(PhysBody) PhysBodyArray;
typedef NPH_ARRAY(PhysShape) PhysShapeArray;
typedef NPH_ARRAY(PhysContact) PhysContactArray;
typedef NPH_ARRAY(PhysJoint) PhysJointArray;
typedef NPH_ARRAY(PhysIsland) PhysIslandArray;
typedef NPH_ARRAY(PhysSolverBody) PhysSolverBodyArray;
typedef NPH_ARRAY(PhysSolverParams) PhysSolverParamsArray;
typedef NPH_ARRAY(PhysContactLanes) PhysContactLanesArray;
typedef NPH_ARRAY(PhysPointLanes) PhysPointLanesArray;
typedef NPH_ARRAY(PhysStage) PhysStageArray;
typedef NPH_ARRAY(NphAtomicU32) PhysClaimArray;
typedef NPH_ARRAY(NphPhysicsContactBeginEvent) PhysBeginEventArray;
typedef NPH_ARRAY(NphPhysicsContactEndEvent) PhysEndEventArray;
typedef NPH_ARRAY(NphPhysicsContactHitEvent) PhysHitEventArray;
typedef NPH_ARRAY(NphPhysicsBodyMoveEvent) PhysMoveEventArray;

typedef struct PhysPair {
    u32 shape_a;
    u32 shape_b;
} PhysPair;

typedef NPH_ARRAY(PhysPair) PhysPairArray;

struct NphPhysicsWorld {
    NphAllocator *alloc;
    NphPhysicsWorldDesc desc;
    f32 hit_threshold;
    const PhysSolverKernels *kernels;
    bool stepping;

    PhysSlots body_slots;
    PhysBodyArray bodies;
    PhysSlots shape_slots;
    PhysShapeArray shapes;
    PhysContactArray contacts;
    NphArrayU32 free_contacts;
    PhysSlots joint_slots;
    PhysJointArray joints;
    PhysIslandArray islands;
    NphArrayU32 free_islands;

    NphArrayU32 awake_bodies;   // body slots, dynamic and kinematic
    NphArrayU32 awake_contacts; // contact indices with an awake body
    NphArrayU32 awake_islands;

    NphHashMap pairs; // (shape a slot << 32 | shape b slot) -> contact index

    // Broadphase.
    NphAabbTree trees[PHYS_TREE_COUNT];
    NphArrayU32 move_buffer; // proxy keys of shapes whose proxies grew
    NphArrayU32 pair_counts; // per moved proxy, then their prefix sums
    PhysPairArray pair_buffer;

    // The constraint graph.
    PhysColor colors[PHYS_GRAPH_COLORS];
    NphArrayU32 overflow; // contacts
    NphArrayU32 overflow_joints;

    // Step data, kept between steps so steady state allocates nothing.
    PhysSolverBodyArray solver_bodies; // awake bodies, then the dummy
    PhysSolverBodyArray solver_prev;   // XPBD: their positions at the substep's start
    PhysSolverParamsArray solver_params;
    PhysContactLanesArray groups;
    PhysPointLanesArray points;
    PhysStageArray stages;
    PhysClaimArray claims; // per stage block: zero until claimed
    NphArrayU32 changed;   // contacts the narrow phase flagged
    NphArrayU32 scratch;   // island splitting, sorting
    NphArrayU32 scratch2;
    NphArrayU32 candidates; // islands ready to sleep
    u32 color_group_first[PHYS_GRAPH_COLORS + 1U];
    u32 color_group_count[PHYS_GRAPH_COLORS + 1U];
    NphArrayU32 step_joints; // the graph's joints: by colour, the overflow last
    u32 color_joint_first[PHYS_GRAPH_COLORS + 1U];
    u32 color_joint_count[PHYS_GRAPH_COLORS + 1U];

    // Events.
    PhysBeginEventArray begin_events;
    PhysEndEventArray end_events;
    PhysHitEventArray hit_events;
    PhysMoveEventArray move_events;

    NphPhysicsStats stats;
    u32 split_candidate;
    u32 forced_participants; // white-box tests: nonzero fixes the solver's thread count
    f32 last_substep;        // the last step's substep, seconds (joint reactions)
};

// ---------------------------------------------------------------------------
// Shared internals (world.c, body.c, shape.c, contact.c, island.c, solver.c)
// ---------------------------------------------------------------------------

NphResult phys_slots_alloc(PhysSlots *s, u32 *out_slot, u32 *out_generation);
void phys_slots_release(PhysSlots *s, u32 slot);

PhysBody *phys_body(const NphPhysicsWorld *w, NphPhysicsBody h);
PhysShape *phys_shape(const NphPhysicsWorld *w, NphPhysicsShape h);

NphPhysicsBody phys_body_handle(const NphPhysicsWorld *w, u32 slot);
NphPhysicsShape phys_shape_handle(const NphPhysicsWorld *w, u32 slot);

// Recomputes a body's mass properties from its shapes (unless set) and
// its centre of mass in the world.
void phys_body_update_mass(NphPhysicsWorld *w, PhysBody *b);
// The world pose of a shape's frame relative to `origin` (32-bit).
NphPhysicsPose phys_shape_pose_relative(const PhysBody *b, const PhysShape *s, NphDVec3 origin);
// Tight world box of a shape, rounded outward to 32 bits.
NphAABB phys_shape_world_aabb(const PhysBody *b, const PhysShape *s);
// Creates the shape's proxy in its body type's tree and queues it.
NphResult phys_shape_create_proxy(NphPhysicsWorld *w, u32 shape_slot);
void phys_shape_destroy_proxy(NphPhysicsWorld *w, u32 shape_slot);
NphResult phys_move_buffer_push(NphPhysicsWorld *w, u32 shape_slot);

// Contacts.
NphResult phys_contact_create(NphPhysicsWorld *w, u32 shape_a, u32 shape_b);
void phys_contact_destroy(NphPhysicsWorld *w, u32 contact, bool events);
void phys_contact_update(NphPhysicsWorld *w, u32 contact);
// Every contact and joint of a body: awake-list and graph membership after
// the body woke or fell asleep.
NphResult phys_body_constraints_sync(NphPhysicsWorld *w, u32 body);
NphResult phys_update_pairs(NphPhysicsWorld *w, NphJobSystem *js);
NphResult phys_collide(NphPhysicsWorld *w, NphJobSystem *js);
// Every contact of a shape's body that involves the shape.
void phys_shape_destroy_contacts(NphPhysicsWorld *w, u32 shape_slot);

// Joints.
PhysJoint *phys_joint(const NphPhysicsWorld *w, NphPhysicsJoint h);
// Destroys a joint; `wake` wakes its bodies.
void phys_joint_destroy(NphPhysicsWorld *w, u32 joint, bool wake);
// Graph membership after a body woke or fell asleep.
NphResult phys_joint_sync(NphPhysicsWorld *w, u32 joint);
// False when a joint between the two bodies keeps their shapes apart.
bool phys_joints_allow_contact(const NphPhysicsWorld *w, u32 body_a, u32 body_b);

// The joints' arithmetic (joint_solver.c), per joint, scalar: the same on
// every ISA.
typedef struct PhysJointStep {
    f32 h;
    f32 inv_h;
    PhysSoftness soft; // the world's joint stiffness
    u32 dummy;         // the solver body of static and sleeping-kinematic ends
} PhysJointStep;
void phys_joint_prepare(NphPhysicsWorld *w, const PhysJointStep *step, PhysJoint *j);
void phys_joint_warm_start(PhysSolverBody *bodies, PhysJoint *j);
// The joint's accumulated angular impulse as a world vector, for frames
// with world rotations `qa` and `qb`.
NphVec3 phys_joint_angular_impulse(const PhysJoint *j, NphQuat qa, NphQuat qb);
void phys_joint_solve(PhysSolverBody *bodies, const PhysJointStep *step, PhysJoint *j,
                      bool use_bias);
// XPBD (ADR-0045): the joint's position corrections, then its velocity
// rows (motor, friction, spring damping) on the derived velocities.
void phys_joint_xpbd_positions(PhysSolverBody *bodies, const PhysJointStep *step, PhysJoint *j);
void phys_joint_xpbd_velocities(PhysSolverBody *bodies, const PhysJointStep *step, PhysJoint *j);
// The joint's current angle about X and the frames' world rotations, from
// the bodies' poses (between steps).
f32 phys_joint_angle(const NphPhysicsWorld *w, const PhysJoint *j);
// The impulses of the last substep: the linear one on B (at its anchor) and
// the angular one, world, from the bodies' poses (between steps).
void phys_joint_reaction_impulses(const NphPhysicsWorld *w, const PhysJoint *j, NphVec3 *linear,
                                  NphVec3 *angular);
// A slider's position (or `speed`) along A's X axis, and any joint's
// anchors' distance, from the bodies (between steps).
f32 phys_joint_slider_value(const NphPhysicsWorld *w, const PhysJoint *j, bool speed);
f32 phys_joint_length(const NphPhysicsWorld *w, const PhysJoint *j);
// A motor driving the joint towards a non-zero speed: its bodies stay awake.
bool phys_joint_is_driven(const PhysJoint *j);

// The constraint graph: contacts and joints share the colours.
NphResult phys_graph_add(NphPhysicsWorld *w, u32 contact);
void phys_graph_remove(NphPhysicsWorld *w, u32 contact);
NphResult phys_graph_add_joint(NphPhysicsWorld *w, u32 joint);
void phys_graph_remove_joint(NphPhysicsWorld *w, u32 joint);

// Islands.
NphResult phys_island_add_body(NphPhysicsWorld *w, u32 body);
void phys_island_remove_body(NphPhysicsWorld *w, u32 body);
NphResult phys_island_link_contact(NphPhysicsWorld *w, u32 contact);
void phys_island_unlink_contact(NphPhysicsWorld *w, u32 contact);
void phys_island_link_joint(NphPhysicsWorld *w, u32 joint);
void phys_island_unlink_joint(NphPhysicsWorld *w, u32 joint);
NphResult phys_island_merge_pending(NphPhysicsWorld *w);
NphResult phys_island_wake(NphPhysicsWorld *w, u32 island);
NphResult phys_island_sleep_and_split(NphPhysicsWorld *w);
// Puts an island to sleep now (between steps; no merge may be pending).
NphResult phys_island_sleep_now(NphPhysicsWorld *w, u32 island);
u32 phys_island_root(NphPhysicsWorld *w, u32 island);
// Wakes a body: its island, or the body alone (kinematic).
NphResult phys_wake_body(NphPhysicsWorld *w, u32 body);
NphResult phys_body_set_awake_list(NphPhysicsWorld *w, u32 body, bool awake);

// The solver.
NphResult phys_solve(NphPhysicsWorld *w, f32 dt, NphJobSystem *js);
const PhysSolverKernels *phys_solver_kernels_best(void);

// Allocation through the world's arrays: the step's failure propagates.
#define PHYS_TRY(expr) NPH_TRY(expr)
