// SPDX-License-Identifier: Apache-2.0
// The tonemap pass alone (shaders/render/tonemap.frag): one fetch of the
// HDR colour per pixel, the ACES fit, the sRGB curve, one 32-bit write. The
// production path's cost without temporal anti-aliasing, and the second
// pass of bench_taa.comp's form.
#version 460
#include "../render/tonemap.glsl"
#include "nph_shared.h"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant, scalar) uniform Push { NphBenchPush pc; };

void main()
{
    const vec3 hdr = texelFetch(nph_textures_2d[pc.texture_index], ivec2(gl_FragCoord.xy), 0).rgb;
    out_color = vec4(nph_tonemap(hdr, NPH_RENDER_TONEMAP_ACES, true), 1.0);
}
