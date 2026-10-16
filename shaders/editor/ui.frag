// SPDX-License-Identifier: Apache-2.0
// The editor UI: the vertex colour times the draw's texture. Colours are
// written as they are into a UNORM target: ImGui's are already encoded.
#version 460
#include "ui_common.glsl"

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec4 t = nph_sample_2d(pc.texture_index, pc.sampler_index, in_uv);
    if ((pc.flags & UI_FLAG_OPAQUE) != 0u) {
        t.a = 1.0;
    }
    out_color = in_color * t;
}
