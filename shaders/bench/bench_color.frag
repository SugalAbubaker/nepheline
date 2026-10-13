// SPDX-License-Identifier: Apache-2.0
// Raster fill: a constant colour (varied per instance so compression and
// culling cannot skip the writes).
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant, scalar) uniform Push { NphBenchPush pc; };

void main()
{
    out_color = pc.color + vec4(in_uv * 0.001, 0.0, 0.0);
}
