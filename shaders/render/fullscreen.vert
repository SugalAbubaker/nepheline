// SPDX-License-Identifier: Apache-2.0
// One triangle that covers the viewport (vertices 0, 1, 2 at (-1, -1),
// (3, -1), (-1, 3)), at depth 0: the far plane under reverse-Z, so a
// GREATER_OR_EQUAL test passes only where nothing was drawn.
#version 460

layout(location = 0) out vec2 out_ndc;

void main()
{
    const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    out_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
