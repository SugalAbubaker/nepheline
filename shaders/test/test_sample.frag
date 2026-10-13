// SPDX-License-Identifier: Apache-2.0
// Samples a bindless texture with a bindless sampler at the interpolated UV.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant, scalar) uniform Push { NphTestPush pc; };

void main()
{
    out_color = nph_sample_2d(pc.texture_index, pc.sampler_index, in_uv);
}
