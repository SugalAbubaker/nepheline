// SPDX-License-Identifier: Apache-2.0
// asset_viewer: the material's base colour (factor times texture) lit by
// the scene's directional light, with a constant ambient term.
#version 460
#include "nph_bindless.glsl"
#include "nph_shared.h"

NPH_BUFFER_REF(CookedDraws) { NphCookedDraw d[]; };

layout(push_constant, scalar) uniform Push { NphCookedPush pc; };

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    const NphCookedDraw draw = CookedDraws(pc.draws_address).d[pc.draw_index];
    const vec4 albedo = nph_sample_2d(draw.texture_index, pc.sampler_index, in_uv) * draw.base_color;
    const float diffuse = max(dot(normalize(in_normal), pc.light_dir.xyz), 0.0) * 0.8 + 0.2;
    out_color = vec4(albedo.rgb * diffuse, 1.0);
}
