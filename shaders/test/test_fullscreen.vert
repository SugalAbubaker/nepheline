// SPDX-License-Identifier: Apache-2.0
// One triangle covering the whole viewport (Sousa / Hoffman style).
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) out vec2 out_uv;

void main()
{
    const vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    out_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
