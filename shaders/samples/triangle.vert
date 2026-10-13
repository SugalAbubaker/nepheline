// SPDX-License-Identifier: Apache-2.0
// gpu_triangle: the bring-up triangle (CLAUDE.md §8.6 stage 1), colours
// from the vertex index, no buffers at all.
#version 460
#include "nph_bindless.glsl"

layout(location = 0) out vec3 out_color;

void main()
{
    const vec2 positions[3] = vec2[3](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
    const vec3 colors[3] = vec3[3](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.2, 1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
    out_color = colors[gl_VertexIndex];
}
