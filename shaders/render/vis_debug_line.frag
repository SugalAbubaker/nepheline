// SPDX-License-Identifier: Apache-2.0
// Depth-tested debug lines on the production path (render/debug_draw.h):
// the test is the reference's GREATER_OR_EQUAL against the scene's depth,
// read from the visibility buffer instead of a depth attachment (its
// sampled view: the depth's bits in y), so the frame needs no depth buffer
// for them.
#version 460
#include "render_common.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main()
{
    if (gl_FragCoord.z < uintBitsToFloat(texelFetch(nph_utextures_2d[pc.visbuffer_index],
                                                    ivec2(gl_FragCoord.xy), 0)
                                             .y)) {
        discard;
    }
    out_color = in_color;
}
