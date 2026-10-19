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

NphResult nph_runtime_register_components(NphWorld *world, NphRuntimeComponents *out)
{
    NPH_ASSERT(world != NULL && out != NULL);
    NPH_TRY(nph_world_register_component(world, NphMeshRenderer_component(), &out->mesh_renderer));
    NPH_TRY(nph_world_register_component(world, NphLight_component(), &out->light));
    NPH_TRY(nph_world_register_component(world, NphCamera_component(), &out->camera));
    return nph_world_register_component(world, NphEnvironment_component(), &out->environment);
}
