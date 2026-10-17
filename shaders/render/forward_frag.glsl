// SPDX-License-Identifier: Apache-2.0
// The forward reference renderer's fragment stage (docs/render.md "Forward
// pass"), included by forward_opaque.frag, forward_mask.frag, and
// forward_blend.frag with NPH_FORWARD_ALPHA_MODE set to 0, 1, or 2. The
// shading itself is surface.glsl's, shared with the visibility-buffer
// resolve; here material textures are sampled with the rasteriser's
// implicit derivatives.
//
// The output is pre-exposed radiance, clamped to the largest half float.
#include "render_common.glsl"

#ifndef NPH_FORWARD_ALPHA_MODE
#error "include through forward_opaque.frag, forward_mask.frag, or forward_blend.frag"
#endif

#define NPH_SURFACE_SAMPLE(tex, smp, s) nph_sample_2d((tex), (smp), (s).uv)
#include "surface.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main()
{
    ViewRef view = ViewRef(pc.view_address);
    const NphRenderDrawData draw = DrawsRef(view.v.draws_address).d[pc.draw_index];
    const NphRenderMaterialData m = MaterialsRef(view.v.materials_address).m[draw.material_index];

    NphSurface s;
    s.position = in_position;
    s.normal = in_normal;
    s.tangent = in_tangent;
    s.uv = in_uv;
    s.color = in_color;
    s.position_dx = dFdx(in_position);
    s.position_dy = dFdy(in_position);
    s.uv_dx = dFdx(in_uv);
    s.uv_dy = dFdy(in_uv);
    // Mirrored transforms are drawn with clockwise front faces, so the
    // rasteriser's facing already accounts for them.
    s.front_facing = gl_FrontFacing;
    s.pixel = gl_FragCoord.xy;
    s.depth = gl_FragCoord.z;

    const vec4 base = nph_surface_base_color(m, s);
#if NPH_FORWARD_ALPHA_MODE == 1
    if (base.a < m.alpha_cutoff) {
        discard;
    }
#endif
    const vec3 color = nph_surface_shade(view, m, s, base);
    out_color = vec4(color, NPH_FORWARD_ALPHA_MODE == 2 ? base.a : 1.0);
}
