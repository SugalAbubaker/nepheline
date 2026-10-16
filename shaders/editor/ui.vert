// SPDX-License-Identifier: Apache-2.0
// The editor UI: display coordinates (pixels, y down) to clip space, which
// Vulkan also has y down.
#version 460
#include "ui_common.glsl"

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;

void main()
{
    const UiVertex v = UiVertices(pc.vertices).v[gl_VertexIndex];
    out_color = unpackUnorm4x8(v.color);
    out_uv = v.uv;
    gl_Position = vec4(v.pos * pc.scale + pc.translate, 0.0, 1.0);
}
