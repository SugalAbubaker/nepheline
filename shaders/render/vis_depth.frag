// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's depth as a depth attachment (docs/render.md
// "Transparency"): the transparent pass draws blended surfaces with the
// reference renderer's forward pipelines, whose depth test needs a depth
// buffer. Written only on frames that draw blended surfaces. arg0 is the
// visibility buffer's storage index.
#version 460
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

layout(location = 0) in vec2 in_ndc;

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    gl_FragDepth = vis_depth(
        imageLoad(nph_u64images_2d[nonuniformEXT(pc.arg0)], ivec2(gl_FragCoord.xy)).x);
}
