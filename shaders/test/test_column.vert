// SPDX-License-Identifier: Apache-2.0
// A quad over column gl_InstanceIndex of an 8-column split of the target,
// six vertices, no vertex buffer (gl_VertexIndex modulo 6, so an index
// buffer of 0 to 5 draws it too): the gpu tests' indirect draws, which put
// each record's quad in the column its first instance names.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0),
                                    vec2(1.0, 1.0), vec2(0.0, 1.0));
    const vec2 c = corners[gl_VertexIndex % 6];
    const float x0 = -1.0 + 2.0 * float(gl_InstanceIndex) / 8.0;
    gl_Position = vec4(x0 + c.x * (2.0 / 8.0), c.y * 2.0 - 1.0, 0.5, 1.0);
}
