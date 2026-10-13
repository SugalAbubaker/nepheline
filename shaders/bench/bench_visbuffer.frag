// SPDX-License-Identifier: Apache-2.0
// Visibility-buffer write: 8 bytes per pixel (depth bits + id), the
// §8.6.4 layout, into an R32G32_UINT target.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out uvec2 out_vis;

layout(push_constant, scalar) uniform Push { NphBenchPush pc; };

void main()
{
    out_vis = uvec2(floatBitsToUint(gl_FragCoord.z), uint(in_uv.x * 65535.0) | (pc.iterations << 16u));
}
