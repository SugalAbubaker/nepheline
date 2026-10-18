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
// its exposure (EV100) and tonemapper (render/renderer.h).
#define NPH_CAMERA_FIELDS(F)                                                                       \
    F(F32, yfov, NPH_FIELD_HINTS(NPH_FIELD_FLAG_ANGLE | NPH_FIELD_FLAG_RANGE, 0.1F, 2.8F))         \
    F(F32, znear, NPH_FIELD_RANGE(0.001F, 10.0F))                                                  \
    F(F32, ev100, NPH_FIELD_RANGE(-6.0F, 20.0F))                                                   \
    F(U32, tonemapper, NPH_FIELD_RANGE(0.0F, 2.0F))                                                \
    F(BOOL, active, NPH_FIELD_NO_HINTS)
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

// The ids of the runtime's components in a world.
typedef struct NphRuntimeComponents {
    NphComponentId mesh_renderer;
    NphComponentId light;
    NphComponentId camera;
    NphComponentId environment;
} NphRuntimeComponents;

#define NPH_RUNTIME_COMPONENT_COUNT 4U

// Registers the runtime's components (idempotent: a world that has them
// returns the same ids). Errors of nph_world_register_component.
NPH_MUST_USE NphResult nph_runtime_register_components(NphWorld *world, NphRuntimeComponents *out);

#ifdef __cplusplus
}
#endif
