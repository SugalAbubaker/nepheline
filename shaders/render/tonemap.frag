// SPDX-License-Identifier: Apache-2.0
// The tonemap pass (docs/render.md "Tonemapping"): maps the pre-exposed HDR
// colour to the display through tonemap.glsl.
#version 460
#include "tonemap.glsl"

layout(push_constant, scalar) uniform Push { NphRenderTonemapPush pc; };

layout(location = 0) in vec2 in_ndc;
layout(location = 0) out vec4 out_color;

void main()
{
    const vec3 hdr =
        texelFetch(nph_textures_2d[nonuniformEXT(pc.hdr_index)], ivec2(gl_FragCoord.xy), 0).rgb;
    out_color = vec4(nph_tonemap(hdr, pc.tonemapper, pc.encode_srgb != 0u), 1.0);
}
