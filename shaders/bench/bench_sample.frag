// SPDX-License-Identifier: Apache-2.0
// Texture sampling at native resolution: one trilinear sample per pixel of
// a texture mapped one texel per pixel.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant, scalar) uniform Push { NphBenchPush pc; };

void main()
{
    out_color = nph_sample_2d(pc.texture_index, pc.sampler_index, in_uv);
}
