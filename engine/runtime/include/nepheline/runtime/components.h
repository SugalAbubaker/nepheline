// SPDX-License-Identifier: Apache-2.0
#pragma once

// The runtime's components: what an entity looks like to the renderer
// (docs/runtime.md "Components").
//
//   MeshRenderer  a cooked mesh and the material of each of its slots,
//                 by GUID (scene files keep the references; the scene
//                 renderer, scene_render.h, loads and draws them)
//   MeshMaterials the material slots past a MeshRenderer's eight, for the
//                 rare mesh with more
//   Light         a punctual light at the entity, shining along its -Z
//   Camera        a perspective camera at the entity, looking along -Z;
//                 the first active one is the game's view
//   Environment   the sky that lights the world (the first one found)
//   RigidBody     a body the physics world moves while the world plays
//   Collider      the shape of the entity's body, a part of the body above
//                 it, or a static body of its own
//   Joint         a joint between the entity's body and another's
//   AudioSource   a sound the entity plays while the world plays
//   AudioListener where spatial sounds are heard from
//   WorldCell     a cell of a world partition: the mesh instances that name
//                 it, and the proxy that stands for them from afar
//
// Every world the engine creates registers them right after the built-in
// components (scene/world.h), in this order, so their ids are the same in
// every world of an engine and in every scene file its games save. Asset
// references are GUID fields: a mesh or material moved or renamed keeps
// its identity (core/guid.h).
//
// Thread-safety: the descriptors are immutable statics (NPH_THREADSAFE);
// registration is a change to the world.

#include <nepheline/asset/asset.h>
#include <nepheline/math/mat.h>
#include <nepheline/physics/geometry.h>
#include <nepheline/scene/world.h>

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_MESH_RENDERER_MATERIALS 8U

// Material slot i of the mesh draws with material<i>; a null GUID draws with the
// default material, and so does a slot past the eighth, unless the entity has a
// MeshMaterials for it. `cell`: the id of the WorldCell it belongs to (0: none),
// whose proxy stands for it from afar.
#define NPH_MESH_RENDERER_FIELDS(F)                                                                \
    F(GUID, mesh, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MESH))                                            \
    F(GUID, material0, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material1, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material2, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material3, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material4, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material5, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material6, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material7, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(BOOL, hidden, NPH_FIELD_NO_HINTS)                                                            \
    F(U32, cell, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphMeshRenderer, NPH_MESH_RENDERER_FIELDS);
NPH_COMPONENT_DECLARE(NphMeshRenderer);

// The material GUID of slot `slot` (NPH_GUID_NULL past the last). The
// eight fields are consecutive GUIDs, read through the object's bytes.
NPH_INLINE NphGuid nph_mesh_renderer_material(const NphMeshRenderer *m, u32 slot)
{
    NphGuid g = NPH_GUID_NULL;
    if (slot < NPH_MESH_RENDERER_MATERIALS) {
        memcpy(&g,
               (const u8 *)m + offsetof(NphMeshRenderer, material0) + (u64)slot * sizeof(NphGuid),
               sizeof g);
    }
    return g;
}

// Sets the material GUID of slot `slot` (ignored past the last).
NPH_INLINE void nph_mesh_renderer_set_material(NphMeshRenderer *m, u32 slot, NphGuid material)
{
    if (slot < NPH_MESH_RENDERER_MATERIALS) {
        memcpy((u8 *)m + offsetof(NphMeshRenderer, material0) + (u64)slot * sizeof(NphGuid),
               &material, sizeof material);
    }
}

NPH_STATIC_ASSERT(sizeof(NphMeshRenderer) == 152U, "MeshRenderer is 152 bytes");
NPH_STATIC_ASSERT(offsetof(NphMeshRenderer, material7) ==
                      offsetof(NphMeshRenderer, material0) + 7U * sizeof(NphGuid),
                  "the material GUIDs are consecutive");

// The slots from the ninth on, for a mesh of many materials: most have one or
// two, and every entity of a large world carries a MeshRenderer. Read when the
// MeshRenderer's mesh or materials are (a change here alone goes unseen: touch
// the MeshRenderer too).
#define NPH_MESH_MATERIALS_EXTRA 24U
#define NPH_MESH_MATERIALS_FIELDS(F)                                                               \
    F(GUID, material8, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material9, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                   \
    F(GUID, material10, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material11, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material12, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material13, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material14, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material15, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material16, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material17, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material18, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material19, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material20, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material21, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material22, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material23, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material24, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material25, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material26, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material27, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material28, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material29, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material30, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                                  \
    F(GUID, material31, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))
NPH_COMPONENT_STRUCT(NphMeshMaterials, NPH_MESH_MATERIALS_FIELDS);
NPH_COMPONENT_DECLARE(NphMeshMaterials);

NPH_STATIC_ASSERT(sizeof(NphMeshMaterials) == NPH_MESH_MATERIALS_EXTRA * sizeof(NphGuid),
                  "the extra material GUIDs are consecutive");

// The GUID of slot `slot` from its first extra slot on (NPH_GUID_NULL past the last).
NPH_INLINE NphGuid nph_mesh_materials_get(const NphMeshMaterials *m, u32 slot)
{
    NphGuid g = NPH_GUID_NULL;
    if (slot < NPH_MESH_MATERIALS_EXTRA) {
        memcpy(&g, (const u8 *)m + (u64)slot * sizeof(NphGuid), sizeof g);
    }
    return g;
}

NPH_INLINE void nph_mesh_materials_set(NphMeshMaterials *m, u32 slot, NphGuid material)
{
    if (slot < NPH_MESH_MATERIALS_EXTRA) {
        memcpy((u8 *)m + (u64)slot * sizeof(NphGuid), &material, sizeof material);
    }
}

typedef enum NphLightType {
    NPH_LIGHT_DIRECTIONAL = 0, // intensity in lux
    NPH_LIGHT_POINT = 1,       // intensity in candela
    NPH_LIGHT_SPOT = 2,        // intensity in candela, within the cones
} NphLightType;

// KHR_lights_punctual's model (render/renderer.h NphRenderLightDesc): the
// light sits at the entity and points along the entity's -Z axis.
// `shadows`: it casts ray-traced shadows where the renderer has ray tracing
// (docs/render.md "Shadows"). `near_range`: above 0, a point light is one of the
// many lights (render/renderer.h NphRenderManyLight): per pixel within it, through
// the radiance cache beyond, and `shadows` does not apply.
#define NPH_LIGHT_FIELDS(F)                                                                        \
    F(U32, type, NPH_FIELD_ENUM("directional|point|spot"))                                         \
    F(VEC3, color, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_COLOR))                                          \
    F(F32, intensity, NPH_FIELD_NO_HINTS)                                                          \
    F(F32, range, NPH_FIELD_NO_HINTS)                                                              \
    F(F32, inner_cone,                                                                             \
      NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, 0.0F, 1.5707F))                 \
    F(F32, outer_cone,                                                                             \
      NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, 0.0F, 1.5708F))                 \
    F(BOOL, shadows, NPH_FIELD_NO_HINTS)                                                           \
    F(F32, near_range, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphLight, NPH_LIGHT_FIELDS);
NPH_COMPONENT_DECLARE(NphLight);

// A perspective camera along the entity's -Z with +Y up (ADR-0008), with
// its exposure (EV100), tonemapper, shutter, and lens (render/renderer.h
// NphRenderView): how long the shutter is open in seconds, which the motion
// blur spans (0: none; 1/480 is a 180-degree shutter at 240 frames a
// second; the r.motion_blur cvar turns it off); and the lens's f-number
// (`aperture`, 0: a pinhole, everything sharp), the distance in metres it
// focuses at, and its sensor's height in metres (0: full frame, 0.024),
// which with yfov give its focal length. The depth of field they make
// (r.dof turns it off) needs the focus beyond the focal length: short of it
// the lens is a pinhole.
#define NPH_CAMERA_FIELDS(F)                                                                       \
    F(F32, yfov, NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, 0.1F, 2.8F))         \
    F(F32, znear, NPH_FIELD_RANGE(0.001F, 10.0F))                                                  \
    F(F32, ev100, NPH_FIELD_RANGE(-6.0F, 20.0F))                                                   \
    F(U32, tonemapper, NPH_FIELD_ENUM("none|ACES|AgX"))                                            \
    F(BOOL, active, NPH_FIELD_NO_HINTS)                                                            \
    F(F32, shutter, NPH_FIELD_RANGE(0.0F, 0.1F))                                                   \
    F(F32, aperture, NPH_FIELD_RANGE(0.0F, 32.0F))                                                 \
    F(F32, focus_distance, NPH_FIELD_RANGE(0.05F, 1000.0F))                                        \
    F(F32, sensor_height, NPH_FIELD_RANGE(0.0F, 0.1F))
NPH_COMPONENT_STRUCT(NphCamera, NPH_CAMERA_FIELDS);
NPH_COMPONENT_DECLARE(NphCamera);

// The procedural sky (render/renderer.h NphRenderSkyDesc): each colour is
// scaled by `luminance` (cd/m^2) into radiance; `intensity` scales the
// image-based lighting it produces.
#define NPH_ENVIRONMENT_FIELDS(F)                                                                  \
    F(VEC3, zenith, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_COLOR))                                         \
    F(VEC3, horizon, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_COLOR))                                        \
    F(VEC3, ground, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_COLOR))                                         \
    F(F32, luminance, NPH_FIELD_RANGE(0.0F, 20000.0F))                                             \
    F(F32, intensity, NPH_FIELD_RANGE(0.0F, 4.0F))
NPH_COMPONENT_STRUCT(NphEnvironment, NPH_ENVIRONMENT_FIELDS);
NPH_COMPONENT_DECLARE(NphEnvironment);

// ---------------------------------------------------------------------------
// Physics (docs/runtime.md "Physics"; docs/adr/0050-physics-in-the-engine.md)
// ---------------------------------------------------------------------------
//
// While the world plays, the engine gives every entity with a RigidBody or
// a Collider a body in its physics world (physics/world.h), placed where
// the entity is and shaped by its Collider: a Collider without a RigidBody
// is a static body, and a RigidBody without a Collider a point of 1 kg
// (or of `mass`). A Collider without a RigidBody below an entity with one
// (no closer one between them) is a part of that entity's body instead: a
// compound, each part placed where its entity is in the body and scaled by
// its own world scale (ADR-0108). A dynamic body moves its entity: each step writes its
// pose into the entity's LocalTransform and its velocities into its
// RigidBody. The game moves bodies by writing their components:
//   - new velocities, or any other RigidBody field, change the body;
//   - a new LocalTransform teleports a dynamic or static body there, and
//     moves a kinematic one there over the step (pushing what it meets on
//     the way), after which it stops unless moved again or given a
//     velocity;
//   - a new Collider makes the body's shape again (and its mass), and a
//     part's new LocalTransform places it again in its body.
// Bodies move in the world: a dynamic body's parent moving does not carry
// it (its LocalTransform is written relative to wherever the parent is), a
// static or kinematic one follows its parent. An entity with a
// CharacterController is a character instead (below), whose RigidBody and
// Collider make no body.

typedef enum NphRigidBodyType {
    NPH_RIGID_BODY_STATIC = 0,    // stays where the game puts it: the level
    NPH_RIGID_BODY_KINEMATIC = 1, // moved by the game, pushes, is never pushed: platforms, doors
    NPH_RIGID_BODY_DYNAMIC = 2,   // moved by gravity, forces, and contacts
} NphRigidBodyType;

// `mass`: kg, or 0 for the Collider's (its volume times its density).
// Velocities: of the centre of mass, in world axes (radians per second for
// the angular one). Damping: per second (physics/body.h). `locks`: the
// world axes the body does not move along or turn about
// (NPH_PHYSICS_LOCK_* bits). `bullet`: a fast projectile, swept against
// moving bodies too, not only static ones. `can_sleep`: the body may fall
// asleep when it comes to rest, which makes it free until something wakes
// it.
#define NPH_RIGID_BODY_FIELDS(F)                                                                   \
    F(U32, type, NPH_FIELD_ENUM("static|kinematic|dynamic"))                                       \
    F(F32, mass, NPH_FIELD_RANGE(0.0F, 100000.0F))                                                 \
    F(VEC3, linear_velocity, NPH_FIELD_NO_HINTS)                                                   \
    F(VEC3, angular_velocity, NPH_FIELD_NO_HINTS)                                                  \
    F(F32, linear_damping, NPH_FIELD_RANGE(0.0F, 100.0F))                                          \
    F(F32, angular_damping, NPH_FIELD_RANGE(0.0F, 100.0F))                                         \
    F(F32, gravity_scale, NPH_FIELD_RANGE(-10.0F, 10.0F))                                          \
    F(U32, locks, NPH_FIELD_BITS("linear x|linear y|linear z|angular x|angular y|angular z"))      \
    F(BOOL, bullet, NPH_FIELD_NO_HINTS)                                                            \
    F(BOOL, can_sleep, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphRigidBody, NPH_RIGID_BODY_FIELDS);
NPH_COMPONENT_DECLARE(NphRigidBody);

typedef enum NphColliderShape {
    NPH_COLLIDER_SPHERE = 0,
    NPH_COLLIDER_CAPSULE = 1, // its core along the entity's Y
    NPH_COLLIDER_BOX = 2,
    NPH_COLLIDER_ASSET = 3, // every shape of the collision asset `collision`
} NphColliderShape;

// A sphere (`radius`), a capsule (`radius` around a core 2 * `half_height`
// long), or a box (`half_extents`), at `center` and `rotation` in the
// entity's frame, scaled by the entity's world scale: a box along each of
// its axes, a sphere by the largest, a capsule's core by Y and its radius
// by the larger of X and Z. Or (ASSET; ADR-0108) every shape of the
// collision asset `collision` (asset/collision.h: the cooker's convex hulls
// and triangle meshes, in the frame of what collides), turned by `rotation`,
// then scaled by the entity's world scale along each of its axes and
// mirrored where it mirrors, at `center`; a mesh only on a static or
// kinematic body. Its surface: `density` in kg/m^3, `friction`, and
// `restitution`, each combined with a touching shape's by default
// (physics/shape.h NphPhysicsCombine); or (ADR-0108) those of the physics
// material `physics_material` (asset/physics_material.h, a `.npmat` source),
// with its combine modes: a surface made once and changed once for every
// Collider naming it. Its shapes wait for it while it loads, as an ASSET
// Collider's for its asset; one that cannot be read leaves the Collider its
// own values (logged). Its filter (physics/shape.h NphPhysicsFilter): two
// shapes touch when each one's category bits meet the other's mask, unless
// they share a group (negative: never; positive: always). `hit_events`:
// report hits above the world's threshold (physics/world.h). `trigger`
// (ADR-0108): its shapes touch nothing and report what enters and leaves
// them (game.h nph_game_physics_triggers): sensors (physics/shape.h), which
// see moving shapes whose category meets their mask; convex only, so an
// ASSET trigger leaves its meshes out.
#define NPH_COLLIDER_FIELDS(F)                                                                     \
    F(U32, shape, NPH_FIELD_ENUM("sphere|capsule|box|asset"))                                      \
    F(F32, radius, NPH_FIELD_RANGE(0.001F, 100.0F))                                                \
    F(F32, half_height, NPH_FIELD_RANGE(0.0F, 100.0F))                                             \
    F(VEC3, half_extents, NPH_FIELD_NO_HINTS)                                                      \
    F(VEC3, center, NPH_FIELD_NO_HINTS)                                                            \
    F(QUAT, rotation, NPH_FIELD_NO_HINTS)                                                          \
    F(F32, density, NPH_FIELD_RANGE(0.0F, 20000.0F))                                               \
    F(F32, friction, NPH_FIELD_RANGE(0.0F, 2.0F))                                                  \
    F(F32, restitution, NPH_FIELD_RANGE(0.0F, 1.0F))                                               \
    F(GUID, physics_material, NPH_FIELD_ASSET(NPH_ASSET_TYPE_PHYSICS_MATERIAL))                    \
    F(U64, category, NPH_FIELD_BITS(NULL))                                                         \
    F(U64, mask, NPH_FIELD_BITS(NULL))                                                             \
    F(I32, group, NPH_FIELD_NO_HINTS)                                                              \
    F(BOOL, hit_events, NPH_FIELD_NO_HINTS)                                                        \
    F(GUID, collision, NPH_FIELD_ASSET(NPH_ASSET_TYPE_COLLISION))                                  \
    F(BOOL, trigger, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphCollider, NPH_COLLIDER_FIELDS);
NPH_COMPONENT_DECLARE(NphCollider);

NPH_STATIC_ASSERT(sizeof(NphRigidBody) == 56U, "RigidBody is 56 bytes");
NPH_STATIC_ASSERT(sizeof(NphCollider) == 128U, "Collider is 128 bytes");

// A joint (ADR-0108; physics/joint.h) between the body of the entity (its
// RigidBody's or Collider's, or the nearest body above it) and the body of
// `other` (the null entity: the world). It is made once both bodies exist,
// at the frames `anchor` and `axis` (in the entity's frame) give in the
// world then, and made again when the component changes or a body is made
// again. Its kind:
//   ball      the anchor held together, any turn; with `limit`, the turn
//             kept within a cone of `upper` radians about `axis`
//   hinge     a turn about `axis` alone: from `lower` to `upper` radians
//             with `limit`, the motor's speed in rad/s and its most torque in
//             N m, the spring's target in radians (a door: its limits its
//             swing, its spring what closes it)
//   fixed     the two welded
//   slider    a slide along `axis` alone: from `lower` to `upper` metres,
//             the motor in m/s and N, the spring's target in metres
//   distance  `anchor` held as far from `other_anchor` (in `other`'s frame,
//             or the world's) as when made: with `limit`, anywhere from
//             `lower` to `upper` metres apart (a rope: 0 to its length);
//             with `spring`, pulled softly to that length; the motor in m/s
//             (positive apart) and N
// The spring: a frequency in Hz and a damping ratio, whatever the masses,
// for the joint's own turn or slide as if the body turned about its centre:
// a door, turning about its edge, swings slower and less damped than they
// say (half of each for a plain slab).
// `collide_connected`: the two bodies' shapes touch each other.
// `break_force` (N; 0: never): past it the joint breaks, `broken` is
// written true, and the joint stays broken until the component changes (a
// game joins it again by writing `broken` false). One of the two bodies at
// least must be dynamic; a joint that cannot be made is logged and tried
// again when the component or a body changes.
typedef enum NphJointKind {
    NPH_JOINT_BALL = 0,
    NPH_JOINT_HINGE = 1,
    NPH_JOINT_FIXED = 2,
    NPH_JOINT_SLIDER = 3,
    NPH_JOINT_DISTANCE = 4,
} NphJointKind;

#define NPH_JOINT_FIELDS(F)                                                                        \
    F(U32, kind, NPH_FIELD_ENUM("ball|hinge|fixed|slider|distance"))                               \
    F(ENTITY, other, NPH_FIELD_NO_HINTS)                                                           \
    F(VEC3, anchor, NPH_FIELD_NO_HINTS)                                                            \
    F(VEC3, axis, NPH_FIELD_NO_HINTS)                                                              \
    F(VEC3, other_anchor, NPH_FIELD_NO_HINTS)                                                      \
    F(BOOL, limit, NPH_FIELD_NO_HINTS)                                                             \
    F(F32, lower, NPH_FIELD_NO_HINTS)                                                              \
    F(F32, upper, NPH_FIELD_NO_HINTS)                                                              \
    F(BOOL, motor, NPH_FIELD_NO_HINTS)                                                             \
    F(F32, motor_speed, NPH_FIELD_NO_HINTS)                                                        \
    F(F32, motor_force, NPH_FIELD_RANGE(0.0F, 1.0e9F))                                             \
    F(BOOL, spring, NPH_FIELD_NO_HINTS)                                                            \
    F(F32, spring_hertz, NPH_FIELD_RANGE(0.0F, 1000.0F))                                           \
    F(F32, spring_damping, NPH_FIELD_RANGE(0.0F, 10.0F))                                           \
    F(F32, spring_target, NPH_FIELD_NO_HINTS)                                                      \
    F(BOOL, collide_connected, NPH_FIELD_NO_HINTS)                                                 \
    F(F32, break_force, NPH_FIELD_RANGE(0.0F, 1.0e9F))                                             \
    F(BOOL, broken, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY))
NPH_COMPONENT_STRUCT(NphJoint, NPH_JOINT_FIELDS);
NPH_COMPONENT_DECLARE(NphJoint);

NPH_STATIC_ASSERT(sizeof(NphJoint) == 96U, "Joint is 96 bytes");

// A character controller (ADR-0108; physics/character.h): a capsule `height`
// tall and `radius` round (a ball when the height is under twice the radius),
// upright, standing on the entity's origin, which the engine moves every step
// before the physics step as a person walks: along what it meets, up steps
// as high as `step_height`, never up slopes steeper than `max_slope`
// (radians), and kept to the ground over drops as small as `snap_distance`.
//   - The game drives it by `move`, a velocity along the ground in world axes
//     (its part along +Y is ignored), and `jump`, an upward speed taken when
//     it stands and cleared at every step, taken or not.
//   - Off the ground the world's gravity times `gravity_scale` pulls it, and
//     it keeps the velocity it left the ground with (a platform's too); on
//     the ground, what it stands on carries it.
//   - What it walks into is pushed as by a body of `push_mass` kg (0: nothing
//     is pushed).
//   - After each step the engine writes the entity's LocalTransform's
//     translation (its rotation stays the game's: the capsule never turns,
//     and the entity's scale does not size it), and, for the game to read,
//     `velocity` (m/s, what it moved over the step), `grounded` (on ground no
//     steeper than `max_slope`), and `ground` (the entity of what it stands
//     on, or the null entity). A game that writes the translation puts it
//     there, at rest: a respawn (game.h nph_game_reset_interpolation draws
//     it there at once).
// Its body is kinematic, its capsule a ghost (physics/shape.h): it touches
// nothing, so bodies do not collide with it (the next move pushes it out of
// what moved into it), but rays and triggers see it, by its filter
// (`category`, `mask`), which is also what its moves meet. The entity's
// RigidBody and Collider, if it has them, make no body.
#define NPH_CHARACTER_CONTROLLER_FIELDS(F)                                                         \
    F(F32, radius, NPH_FIELD_RANGE(0.01F, 10.0F))                                                  \
    F(F32, height, NPH_FIELD_RANGE(0.02F, 20.0F))                                                  \
    F(F32, step_height, NPH_FIELD_RANGE(0.0F, 2.0F))                                               \
    F(F32, max_slope, NPH_FIELD_HINTS(NPH_FIELD_FLAG_RANGE | NPH_FIELD_FLAG_ANGLE, 0.0F, 1.5533F)) \
    F(F32, snap_distance, NPH_FIELD_RANGE(0.0F, 2.0F))                                             \
    F(F32, push_mass, NPH_FIELD_RANGE(0.0F, 10000.0F))                                             \
    F(F32, gravity_scale, NPH_FIELD_RANGE(-10.0F, 10.0F))                                          \
    F(VEC3, move, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_TRANSIENT))                                       \
    F(F32, jump, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_TRANSIENT))                                        \
    F(VEC3, velocity, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY | NPH_FIELD_FLAG_TRANSIENT))         \
    F(BOOL, grounded, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY | NPH_FIELD_FLAG_TRANSIENT))         \
    F(ENTITY, ground, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_READONLY | NPH_FIELD_FLAG_TRANSIENT))         \
    F(U64, category, NPH_FIELD_BITS(NULL))                                                         \
    F(U64, mask, NPH_FIELD_BITS(NULL))
NPH_COMPONENT_STRUCT(NphCharacterController, NPH_CHARACTER_CONTROLLER_FIELDS);
NPH_COMPONENT_DECLARE(NphCharacterController);

NPH_STATIC_ASSERT(sizeof(NphCharacterController) == 80U, "CharacterController is 80 bytes");

// ---------------------------------------------------------------------------
// Audio (docs/runtime.md "Audio"; docs/adr/0058-audio-in-the-engine.md)
// ---------------------------------------------------------------------------
//
// While the world plays, an entity with an AudioSource plays its sound
// (asset/sound.h, by GUID) through the engine's mixer: from memory, or
// streamed from storage when the cooker made it so. The game starts and
// stops it by writing `playing`; the engine writes it false when the sound
// ends, and true when `autoplay` starts it (when play begins, or when the
// entity appears while playing). A spatial source sounds from where its
// entity is in the world, around the listener: the first active
// AudioListener, else the view the host set (the editor's camera), else the
// first active Camera. Changes to any other field apply to the sound
// playing.

// The engine's buses, under the master; each has its volume cvar
// (audio.volume_<name>), and every voice also sends to the reverberation.
typedef enum NphAudioSourceBus {
    NPH_AUDIO_SOURCE_BUS_EFFECTS = 0,
    NPH_AUDIO_SOURCE_BUS_MUSIC = 1,
    NPH_AUDIO_SOURCE_BUS_DIALOGUE = 2,
    NPH_AUDIO_SOURCE_BUS_AMBIENCE = 3,
    NPH_AUDIO_SOURCE_BUS_UI = 4,
} NphAudioSourceBus;

#define NPH_AUDIO_SOURCE_BUS_COUNT 5U

// `volume` is linear and `pitch` a playback rate (1: as recorded). A
// spatial source falls off with distance by `rolloff` (audio/spatial.h
// NphAudioRolloff: 0 inverse, at `rolloff_factor`'s steepness, full within
// `min_distance` and falling no further past `max_distance`; 1 linear,
// silent from `max_distance`; 2 the curve through four points, distances
// in metres rising in `curve_distances` and gains in dB in `curve_gains`;
// 3 none), spreads over its `size` (m) when near, pitches by `doppler` (0
// none, 1 physical), and, with `occlusion`, is darkened and quietened by
// what lies between it and the listener (rays through the physics world).
// `reverb` is its send to the room's reverberation (0 .. 1); `priority`
// keeps its voice real over lower ones (0 .. 255).
#define NPH_AUDIO_SOURCE_FIELDS(F)                                                                 \
    F(GUID, sound, NPH_FIELD_ASSET(NPH_ASSET_TYPE_SOUND))                                          \
    F(U32, bus, NPH_FIELD_ENUM("effects|music|dialogue|ambience|ui"))                              \
    F(F32, volume, NPH_FIELD_RANGE(0.0F, 4.0F))                                                    \
    F(F32, pitch, NPH_FIELD_RANGE(0.125F, 8.0F))                                                   \
    F(BOOL, loop, NPH_FIELD_NO_HINTS)                                                              \
    F(BOOL, autoplay, NPH_FIELD_NO_HINTS)                                                          \
    F(BOOL, playing, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_TRANSIENT))                                    \
    F(BOOL, spatial, NPH_FIELD_NO_HINTS)                                                           \
    F(U32, rolloff, NPH_FIELD_ENUM("inverse|linear|curve|none"))                                   \
    F(F32, min_distance, NPH_FIELD_RANGE(0.01F, 1000.0F))                                          \
    F(F32, max_distance, NPH_FIELD_RANGE(0.01F, 100000.0F))                                        \
    F(F32, rolloff_factor, NPH_FIELD_RANGE(0.0F, 10.0F))                                           \
    F(VEC4, curve_distances, NPH_FIELD_NO_HINTS)                                                   \
    F(VEC4, curve_gains, NPH_FIELD_NO_HINTS)                                                       \
    F(F32, doppler, NPH_FIELD_RANGE(0.0F, 4.0F))                                                   \
    F(F32, size, NPH_FIELD_RANGE(0.0F, 100.0F))                                                    \
    F(BOOL, occlusion, NPH_FIELD_NO_HINTS)                                                         \
    F(F32, reverb, NPH_FIELD_RANGE(0.0F, 1.0F))                                                    \
    F(U32, priority, NPH_FIELD_RANGE(0.0F, 255.0F))
NPH_COMPONENT_STRUCT(NphAudioSource, NPH_AUDIO_SOURCE_FIELDS);
NPH_COMPONENT_DECLARE(NphAudioSource);

// The listener the engine places spatial sounds around: at the entity, its
// -Z ahead and +Y up, as a Camera looks. The first active one is the
// listener.
#define NPH_AUDIO_LISTENER_FIELDS(F) F(BOOL, active, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphAudioListener, NPH_AUDIO_LISTENER_FIELDS);
NPH_COMPONENT_DECLARE(NphAudioListener);

NPH_STATIC_ASSERT(sizeof(NphAudioSource) == 112U, "AudioSource is 112 bytes");

// How a body takes the frame of an entity under a parent (ADR-0108): from its
// world transform's axes, the rotation of their unmirrored twin with their
// scale and shear taken out (Gram-Schmidt from X), and their lengths, Z's
// negative where the axes mirror (the rotation leaves the mirror out, the
// scale puts it back). A root's body takes its LocalTransform's rotation and
// scale as they are instead. The engine's bodies and the editor's outlines
// of them both take frames so.
NPH_INLINE NphQuat nph_body_axes_rotation(NphVec3 x_axis, NphVec3 y_axis)
{
    const NphVec3 x = nph_vec3_normalize_or(x_axis, nph_vec3_unit_x());
    NphVec3 y = nph_vec3_sub(y_axis, nph_vec3_scale(x, nph_vec3_dot(y_axis, x)));
    y = nph_vec3_normalize_or(y, nph_vec3_perpendicular(x));
    const NphVec3 z = nph_vec3_cross(x, y);
    return nph_quat_normalize(nph_quat_from_mat3(nph_mat3_from_cols(x, y, z)));
}

NPH_INLINE NphVec3 nph_body_axes_scale(NphVec3 x_axis, NphVec3 y_axis, NphVec3 z_axis)
{
    const f32 sz = nph_vec3_length(z_axis);
    return nph_vec3(nph_vec3_length(x_axis), nph_vec3_length(y_axis),
                    nph_vec3_dot(nph_vec3_cross(x_axis, y_axis), z_axis) < 0.0F ? -sz : sz);
}

// The shape a Collider makes on an entity of world scale `scale` (per
// local axis, negative where the entity mirrors): its geometry, sized by
// the scale's magnitude, and its pose in the entity's frame (the centre
// scaled, the rotation unit). Negative and non-finite sizes count as zero.
// False when that is no valid shape (a sphere of radius 0, an ASSET
// Collider, whose shapes come from its asset, an unknown `shape`); the
// outputs are then unspecified.
bool nph_collider_shape(const NphCollider *collider, NphVec3 scale,
                        NphPhysicsGeometry *out_geometry, NphPhysicsPose *out_pose);

// The ids of the runtime's components in a world.
// A cell of a world partition (ADR-0062, ADR-0072; docs/runtime.md "World
// partition"). `id`, nonzero and unique in its world, is what MeshRenderers
// name it by. Its box and its proxy's place are relative to the entity, whose
// world transform may only move them. `proxy` is its proxy's mesh, a head of
// levels apart that the scene renderer streams, or a mesh cooked whole (null:
// no proxy, and its instances draw as themselves), drawn with `proxy_material`
// from `proxy_distance` on; `proxy_cell` is the error of its finest level.
#define NPH_WORLD_CELL_FIELDS(F)                                                                   \
    F(U32, id, NPH_FIELD_NO_HINTS)                                                                 \
    F(VEC3, bounds_min, NPH_FIELD_NO_HINTS)                                                        \
    F(VEC3, bounds_max, NPH_FIELD_NO_HINTS)                                                        \
    F(GUID, proxy, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MESH))                                           \
    F(GUID, proxy_material, NPH_FIELD_ASSET(NPH_ASSET_TYPE_MATERIAL))                              \
    F(VEC3, proxy_origin, NPH_FIELD_NO_HINTS)                                                      \
    F(F32, proxy_cell, NPH_FIELD_NO_HINTS)                                                         \
    F(F32, proxy_distance, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphWorldCell, NPH_WORLD_CELL_FIELDS);
NPH_COMPONENT_DECLARE(NphWorldCell);

typedef struct NphRuntimeComponents {
    NphComponentId mesh_renderer;
    NphComponentId light;
    NphComponentId camera;
    NphComponentId environment;
    NphComponentId rigid_body;
    NphComponentId collider;
    NphComponentId audio_source;
    NphComponentId audio_listener;
    NphComponentId world_cell;
    NphComponentId mesh_materials;
    NphComponentId joint;
    NphComponentId character_controller;
} NphRuntimeComponents;

#define NPH_RUNTIME_COMPONENT_COUNT 12U

// Registers the runtime's components (idempotent: a world that has them
// returns the same ids). Errors of nph_world_register_component.
NPH_MUST_USE NphResult nph_runtime_register_components(NphWorld *world, NphRuntimeComponents *out);

#ifdef __cplusplus
}
#endif
