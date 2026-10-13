// SPDX-License-Identifier: Apache-2.0
// A triangle covering the middle of the viewport, no vertex buffer.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) out vec2 out_uv;

void main()
{
    const vec2 positions[3] = vec2[3](vec2(-0.5, 0.5), vec2(0.5, 0.5), vec2(0.0, -0.5));
    const vec2 p = positions[gl_VertexIndex];
    out_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.5, 1.0);
}
