// SPDX-License-Identifier: Apache-2.0
// The runtime's components. See components.h.
#include <nepheline/core/assert.h>
#include <nepheline/runtime/components.h>

#include "runtime_internal.h"

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_RUNTIME, "runtime", NPH_LOG_LEVEL_INFO);

static const NphMeshRenderer g_mesh_renderer_defaults = {
    {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, {0U, 0U}, 0};
NPH_COMPONENT_DEFINE(NphMeshRenderer, NPH_MESH_RENDERER_FIELDS, "MeshRenderer", 0U,
                     &g_mesh_renderer_defaults)

// A 1000 cd white point light: a bright bulb at a few metres, casting no
// shadows.
static const NphLight g_light_defaults = {
    (u32)NPH_LIGHT_POINT, {1.0F, 1.0F, 1.0F}, 1000.0F, 0.0F, 0.4F, 0.6F, 0};
NPH_COMPONENT_DEFINE(NphLight, NPH_LIGHT_FIELDS, "Light", 0U, &g_light_defaults)

// About 52 degrees of vertical field, a 10 cm near plane, sunny-day
// exposure, ACES, no motion blur, and a pinhole, which an aperture turns
// into a full-frame lens focused 10 m away (render/renderer.h
// nph_render_view_default).
static const NphCamera g_camera_defaults = {0.9F, 0.1F, 15.0F, 1U, 1, 0.0F, 0.0F, 10.0F, 0.0F};
NPH_COMPONENT_DEFINE(NphCamera, NPH_CAMERA_FIELDS, "Camera", 0U, &g_camera_defaults)

// The renderer's default clear sky (nph_render_sky_desc_default): colours
// normalised by the brightest horizon channel, 8000 cd/m^2.
static const NphEnvironment g_environment_defaults = {
    {0.2F, 0.4F, 0.8F}, {0.75F, 0.875F, 1.0F}, {0.1875F, 0.1625F, 0.1375F}, 8000.0F, 1.0F};
NPH_COMPONENT_DEFINE(NphEnvironment, NPH_ENVIRONMENT_FIELDS, "Environment", 0U,
                     &g_environment_defaults)

// A dynamic body with its Collider's mass, undamped, under gravity, free to
// sleep.
static const NphRigidBody g_rigid_body_defaults = {(u32)NPH_RIGID_BODY_DYNAMIC,
                                                   0.0F,
                                                   {0.0F, 0.0F, 0.0F},
                                                   {0.0F, 0.0F, 0.0F},
                                                   0.0F,
                                                   0.0F,
                                                   1.0F,
                                                   0U,
                                                   0,
                                                   1};
NPH_COMPONENT_DEFINE(NphRigidBody, NPH_RIGID_BODY_FIELDS, "RigidBody", 0U, &g_rigid_body_defaults)

// The unit cube the editor's primitive is, of water, with the physics
// module's default friction and filter (physics/shape.h).
static const NphCollider g_collider_defaults = {(u32)NPH_COLLIDER_BOX,
                                                0.5F,
                                                0.5F,
                                                {0.5F, 0.5F, 0.5F},
                                                {0.0F, 0.0F, 0.0F},
                                                {0.0F, 0.0F, 0.0F, 1.0F},
                                                1000.0F,
                                                0.6F,
                                                0.0F,
                                                1U,
                                                NPH_U64_MAX,
                                                0,
                                                0};
NPH_COMPONENT_DEFINE(NphCollider, NPH_COLLIDER_FIELDS, "Collider", 0U, &g_collider_defaults)

// An effect at full volume and as recorded, not playing until the game
// says so, in 3D with the mixer's default fall-off (inverse, physical,
// full within 1 m, down to 50 m; the curve, if chosen, from 0 dB at 1 m to
// -40 dB at 50 m), physical Doppler, a point source, occluded, a little
// of the room, middling priority.
static const NphAudioSource g_audio_source_defaults = {{0U, 0U},
                                                       (u32)NPH_AUDIO_SOURCE_BUS_EFFECTS,
                                                       1.0F,
                                                       1.0F,
                                                       0,
                                                       0,
                                                       0,
                                                       1,
                                                       0U,
                                                       1.0F,
                                                       50.0F,
                                                       1.0F,
                                                       {1.0F, 5.0F, 20.0F, 50.0F},
                                                       {0.0F, -14.0F, -26.0F, -40.0F},
                                                       1.0F,
                                                       0.0F,
                                                       1,
                                                       0.25F,
                                                       128U};
NPH_COMPONENT_DEFINE(NphAudioSource, NPH_AUDIO_SOURCE_FIELDS, "AudioSource", 0U,
                     &g_audio_source_defaults)

static const NphAudioListener g_audio_listener_defaults = {1};
NPH_COMPONENT_DEFINE(NphAudioListener, NPH_AUDIO_LISTENER_FIELDS, "AudioListener", 0U,
                     &g_audio_listener_defaults)

static f32 size_of(f32 v)
{
    return nph_f32_is_finite(v) && v > 0.0F ? v : 0.0F;
}

bool nph_collider_shape(const NphCollider *c, NphVec3 scale, NphPhysicsGeometry *out_geometry,
                        NphPhysicsPose *out_pose)
{
    NPH_ASSERT(c != NULL && out_geometry != NULL && out_pose != NULL);
    const NphVec3 s = nph_vec3(size_of(scale.x), size_of(scale.y), size_of(scale.z));
    switch (c->shape) {
    case NPH_COLLIDER_SPHERE:
        *out_geometry = nph_physics_geometry_sphere(size_of(c->radius) *
                                                    nph_f32_max(s.x, nph_f32_max(s.y, s.z)));
        break;
    case NPH_COLLIDER_CAPSULE:
        *out_geometry = nph_physics_geometry_capsule(size_of(c->half_height) * s.y,
                                                     size_of(c->radius) * nph_f32_max(s.x, s.z));
        break;
    case NPH_COLLIDER_BOX:
        *out_geometry = nph_physics_geometry_box(nph_vec3(size_of(c->half_extents.x) * s.x,
                                                          size_of(c->half_extents.y) * s.y,
                                                          size_of(c->half_extents.z) * s.z));
        break;
    default:
        return false;
    }
    const NphVec3 center = nph_vec3_mul(c->center, s);
    out_pose->position = nph_vec3_is_finite(center) ? center : nph_vec3_zero();
    // A rotation as it is when unit to rounding, normalised otherwise (the
    // identity for zero or non-finite).
    const NphQuat q = c->rotation;
    const bool finite = nph_f32_is_finite(q.x) && nph_f32_is_finite(q.y) &&
                        nph_f32_is_finite(q.z) && nph_f32_is_finite(q.w);
    out_pose->rotation = !finite                            ? nph_quat_identity()
                         : nph_quat_is_normalized(q, 1e-6F) ? q
                                                            : nph_quat_normalize_or_identity(q);
    return nph_physics_geometry_is_valid(out_geometry);
}

NphResult nph_runtime_register_components(NphWorld *world, NphRuntimeComponents *out)
{
    NPH_ASSERT(world != NULL && out != NULL);
    NPH_TRY(nph_world_register_component(world, NphMeshRenderer_component(), &out->mesh_renderer));
    NPH_TRY(nph_world_register_component(world, NphLight_component(), &out->light));
    NPH_TRY(nph_world_register_component(world, NphCamera_component(), &out->camera));
    NPH_TRY(nph_world_register_component(world, NphEnvironment_component(), &out->environment));
    NPH_TRY(nph_world_register_component(world, NphRigidBody_component(), &out->rigid_body));
    NPH_TRY(nph_world_register_component(world, NphCollider_component(), &out->collider));
    NPH_TRY(nph_world_register_component(world, NphAudioSource_component(), &out->audio_source));
    return nph_world_register_component(world, NphAudioListener_component(), &out->audio_listener);
}
