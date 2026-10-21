// SPDX-License-Identifier: Apache-2.0
// Writes each pixel's x and its y plus the push-constant value into an
// unsigned two-channel target.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out uvec2 out_value;

layout(push_constant, scalar) uniform Push { NphTestPush pc; };

void main()
{
    out_value = uvec2(uint(gl_FragCoord.x), uint(gl_FragCoord.y) + pc.value);
}
