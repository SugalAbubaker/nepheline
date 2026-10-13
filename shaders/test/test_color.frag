// SPDX-License-Identifier: Apache-2.0
// Writes the push-constant colour.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant, scalar) uniform Push { NphTestPush pc; };

void main()
{
    out_color = pc.color;
}
