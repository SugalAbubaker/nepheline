// SPDX-License-Identifier: Apache-2.0
#pragma once

// The runtime's components: what an entity looks like to the renderer
// (docs/runtime.md "Components").
//
//   MeshRenderer  a cooked mesh and the material of each of its slots,
//                 by GUID (scene files keep the references; the scene
//                 renderer, scene_render.h, loads and draws them)
//   Light         a punctual light at the entity, shining along its -Z
//   Camera        a perspective camera at the entity, looking along -Z;
//                 the first active one is the game's view
//   Environment   the sky that lights the world (the first one found)
//   RigidBody     a body the physics world moves while the world plays
//   Collider      the shape of the entity's body, or a static body of its
//                 own
//
// Every world the engine creates registers them right after the built-in
// components (scene/world.h), in this order, so their ids are the same in
// every world of an engine and in every scene file its games save. Asset
// references are GUID fields: a mesh or material moved or renamed keeps
// its identity (core/guid.h).
//
// Thread-safety: the descriptors are immutable statics (NPH_THREADSAFE);
// registration is a change to the world.

#include <nepheline/scene/world.h>

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_MESH_RENDERER_MATERIALS 8U

// Material slot i of the mesh draws with material<i>; a null GUID, or a
// slot past the eighth, draws with the default material.
#define NPH_MESH_RENDERER_FIELDS(F)                                                                \
    F(GUID, mesh, NPH_FIELD_NO_HINTS)                                                              \
    F(GUID, material0, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material1, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material2, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material3, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material4, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material5, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material6, NPH_FIELD_NO_HINTS)                                                         \
    F(GUID, material7, NPH_FIELD_NO_HINTS)                                                         \
    F(BOOL, hidden, NPH_FIELD_NO_HINTS)
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

typedef enum NphLightType {
    NPH_LIGHT_DIRECTIONAL = 0, // intensity in lux
    NPH_LIGHT_POINT = 1,       // intensity in candela
    NPH_LIGHT_SPOT = 2,        // intensity in candela, within the cones
} NphLightType;

// KHR_lights_punctual's model (render/renderer.h NphRenderLightDesc): the
// light sits at the entity and points along the entity's -Z axis.
// `shadows`: it casts ray-traced shadows where the renderer has ray tracing
// (docs/render.md "Shadows").
#define NPH_LIGHT_FIELDS(F)                                                                        \
    F(U32, type, NPH_FIELD_RANGE(0.0F, 2.0F))                                                      \
    F(VEC3, color, NPH_FIELD_FLAGS(NPH_FIELD_FLAG_COLOR))                                          \
    F(F32, intensity, NPH_FIELD_NO_HINTS)                                                          \
    F(F32, range, NPH_FIELD_NO_HINTS)                                                              \
    F(F32, inner_cone,                                                                             \
      NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, 0.0F, 1.5707F))                 \
    F(F32, outer_cone,                                                                             \
      NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, 0.0F, 1.5708F))                 \
    F(BOOL, shadows, NPH_FIELD_NO_HINTS)
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
    F(U32, tonemapper, NPH_FIELD_RANGE(0.0F, 2.0F))                                                \
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
// (or of `mass`). A dynamic body moves its entity: each step writes its
// pose into the entity's LocalTransform and its velocities into its
// RigidBody. The game moves bodies by writing their components:
//   - new velocities, or any other RigidBody field, change the body;
//   - a new LocalTransform teleports a dynamic or static body there, and
//     moves a kinematic one there over the step (pushing what it meets on
//     the way), after which it stops unless moved again or given a
//     velocity;
//   - a new Collider makes the body's shape again (and its mass).
// Bodies move in the world: a dynamic body's parent moving does not carry
// it (its LocalTransform is written relative to wherever the parent is), a
// static or kinematic one follows its parent.

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
    F(U32, type, NPH_FIELD_RANGE(0.0F, 2.0F))                                                      \
    F(F32, mass, NPH_FIELD_RANGE(0.0F, 100000.0F))                                                 \
    F(VEC3, linear_velocity, NPH_FIELD_NO_HINTS)                                                   \
    F(VEC3, angular_velocity, NPH_FIELD_NO_HINTS)                                                  \
    F(F32, linear_damping, NPH_FIELD_RANGE(0.0F, 100.0F))                                          \
    F(F32, angular_damping, NPH_FIELD_RANGE(0.0F, 100.0F))                                         \
    F(F32, gravity_scale, NPH_FIELD_RANGE(-10.0F, 10.0F))                                          \
    F(U32, locks, NPH_FIELD_RANGE(0.0F, 63.0F))                                                    \
    F(BOOL, bullet, NPH_FIELD_NO_HINTS)                                                            \
    F(BOOL, can_sleep, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphRigidBody, NPH_RIGID_BODY_FIELDS);
NPH_COMPONENT_DECLARE(NphRigidBody);

typedef enum NphColliderShape {
    NPH_COLLIDER_SPHERE = 0,
    NPH_COLLIDER_CAPSULE = 1, // its core along the entity's Y
    NPH_COLLIDER_BOX = 2,
} NphColliderShape;

// A sphere (`radius`), a capsule (`radius` around a core 2 * `half_height`
// long), or a box (`half_extents`), at `center` and `rotation` in the
// entity's frame, scaled by the entity's world scale: a box along each of
// its axes, a sphere by the largest, a capsule's core by Y and its radius
// by the larger of X and Z. Its material: `density` in kg/m^3, `friction`,
// and `restitution`. Its filter (physics/shape.h NphPhysicsFilter): two
// shapes touch when each one's category bits meet the other's mask, unless
// they share a group (negative: never; positive: always). `hit_events`:
// report hits above the world's threshold (physics/world.h).
#define NPH_COLLIDER_FIELDS(F)                                                                     \
    F(U32, shape, NPH_FIELD_RANGE(0.0F, 2.0F))                                                     \
    F(F32, radius, NPH_FIELD_RANGE(0.001F, 100.0F))                                                \
    F(F32, half_height, NPH_FIELD_RANGE(0.0F, 100.0F))                                             \
    F(VEC3, half_extents, NPH_FIELD_NO_HINTS)                                                      \
    F(VEC3, center, NPH_FIELD_NO_HINTS)                                                            \
    F(QUAT, rotation, NPH_FIELD_NO_HINTS)                                                          \
    F(F32, density, NPH_FIELD_RANGE(0.0F, 20000.0F))                                               \
    F(F32, friction, NPH_FIELD_RANGE(0.0F, 2.0F))                                                  \
    F(F32, restitution, NPH_FIELD_RANGE(0.0F, 1.0F))                                               \
    F(U64, category, NPH_FIELD_NO_HINTS)                                                           \
    F(U64, mask, NPH_FIELD_NO_HINTS)                                                               \
    F(I32, group, NPH_FIELD_NO_HINTS)                                                              \
    F(BOOL, hit_events, NPH_FIELD_NO_HINTS)
NPH_COMPONENT_STRUCT(NphCollider, NPH_COLLIDER_FIELDS);
NPH_COMPONENT_DECLARE(NphCollider);

NPH_STATIC_ASSERT(sizeof(NphRigidBody) == 56U, "RigidBody is 56 bytes");
NPH_STATIC_ASSERT(sizeof(NphCollider) == 88U, "Collider is 88 bytes");

// The ids of the runtime's components in a world.
typedef struct NphRuntimeComponents {
    NphComponentId mesh_renderer;
    NphComponentId light;
    NphComponentId camera;
    NphComponentId environment;
    NphComponentId rigid_body;
    NphComponentId collider;
} NphRuntimeComponents;

#define NPH_RUNTIME_COMPONENT_COUNT 6U

// Registers the runtime's components (idempotent: a world that has them
// returns the same ids). Errors of nph_world_register_component.
NPH_MUST_USE NphResult nph_runtime_register_components(NphWorld *world, NphRuntimeComponents *out);

#ifdef __cplusplus
}
#endif
