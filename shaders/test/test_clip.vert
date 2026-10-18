// SPDX-License-Identifier: Apache-2.0
// A triangle over the whole viewport, no vertex buffer, whose right half
// (x > 0 in NDC) a clip distance cuts away when the specialization constant
// asks for it: the gpu tests' check of graphics specialization constants
// and of gl_ClipDistance.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(constant_id = 0) const bool k_clip = false;

layout(location = 0) out vec2 out_uv;

void main()
{
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    out_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.5, 1.0);
    gl_ClipDistance[0] = k_clip ? -p.x : 1.0;
}
