// SPDX-License-Identifier: Apache-2.0
// The tonemap pass (docs/render.md "Tonemapping"): maps the pre-exposed HDR
// colour, with the bloom mixed in (bloom.comp), to the display through
// tonemap.glsl.
#version 460
#include "tonemap.glsl"

layout(push_constant, scalar) uniform Push { NphRenderTonemapPush pc; };

layout(location = 0) in vec2 in_ndc;
layout(location = 0) out vec4 out_color;

void main()
{
    vec3 hdr =
        texelFetch(nph_textures_2d[nonuniformEXT(pc.hdr_index)], ivec2(gl_FragCoord.xy), 0).rgb;
    if (pc.bloom > 0.0) {
        // The bloom chain's mip 0 holds the sum of its levels' blurs.
        const vec3 glare = textureLod(sampler2D(nph_textures_2d[pc.bloom_index],
                                                nph_samplers[pc.sampler_index]),
                                      gl_FragCoord.xy * pc.bloom_uv, 0.0)
                               .rgb *
                           pc.bloom_scale;
        hdr = mix(hdr, glare, pc.bloom);
    }
    out_color = vec4(nph_tonemap(hdr, pc.tonemapper, pc.encode_srgb != 0u), 1.0);
}
