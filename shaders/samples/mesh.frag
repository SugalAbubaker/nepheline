// SPDX-License-Identifier: Apache-2.0
// gpu_mesh: a bindless texture lit by one directional light.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(push_constant, scalar) uniform Push { NphMeshPush pc; };

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    const vec3 light = normalize(vec3(0.4, 0.8, 0.5));
    const float diffuse = max(dot(normalize(in_normal), light), 0.0) * 0.85 + 0.15;
    const vec4 albedo = nph_sample_2d(pc.texture_index, pc.sampler_index, in_uv);
    out_color = vec4(albedo.rgb * diffuse, 1.0);
}
