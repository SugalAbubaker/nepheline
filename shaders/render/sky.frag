// SPDX-License-Identifier: Apache-2.0
// The background where no geometry was drawn (docs/render.md "Sky"): the
// environment cube seen along the view ray, or a solid colour, pre-exposed
// like the forward pass's output.
#version 460
#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) in vec2 in_ndc;
layout(location = 0) out vec4 out_color;

void main()
{
    ViewRef view = ViewRef(pc.view_address);
    vec3 color;
    if (uint(view.v.background.w) == NPH_RENDER_BACKGROUND_COLOR) {
        color = view.v.background.rgb;
    } else {
        // A point on the near plane (depth 1 under reverse-Z) gives the
        // ray; the camera sits at the origin.
        const vec4 p = view.v.inv_view_proj * vec4(in_ndc, 1.0, 1.0);
        const vec3 dir = normalize(p.xyz / p.w);
        color = textureLod(samplerCube(nph_textures_cube[nonuniformEXT(view.v.env_index)],
                                       nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                           dir, 0.0).rgb * view.v.ibl_intensity;
    }
    out_color = vec4(min(color * view.v.exposure, vec3(NPH_RENDER_HALF_MAX)), 1.0);
}
