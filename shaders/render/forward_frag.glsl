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

// The production path's blended surfaces read the sun's virtual shadow map
// where the view has one (vsm_common.glsl); their pipelines are
// specialised on it (the reference's never are).
#define NPH_VSM_SAMPLING
#include "vsm_common.glsl"
layout(constant_id = 0) const bool k_vsm = false;
VsmViewRef g_vsm; // set first thing in main()
#define NPH_SURFACE_VSM_ACTIVE k_vsm
#define NPH_SURFACE_VSM(p)     vsm_shadow(g_vsm, (p).position, (p).face_normal, (p).n)

#if NPH_FORWARD_ALPHA_MODE == 2
// The production path's blended surfaces have no depth attachment: the
// opaque surfaces' depth is the visibility buffer's, which they test
// themselves (pc.visbuffer_index, its sampled view: the depth's bits in y),
// as the reference's GREATER test does in hardware. A fragment behind is
// demoted, not discarded, so that it still serves its quad's derivatives;
// it writes nothing. Their pipelines are specialised on it.
layout(constant_id = 1) const bool k_vis_depth = false;
#endif

// The *_rt variants trace shadow rays (rt_common.glsl).
#ifdef NPH_RAY_QUERY
#include "rt_common.glsl"
uint64_t g_tlas;     // the view's top-level structure, set first thing in main(),
vec3 g_tlas_offset; // and the camera's place in it
#define NPH_SURFACE_SHADOW(p, l, distance)                                                         \
    nph_shadow_ray(g_tlas, (p).position + g_tlas_offset, (p).face_normal, (l), (distance))
#endif

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
#if NPH_FORWARD_ALPHA_MODE == 2
    if (k_vis_depth &&
        gl_FragCoord.z <= uintBitsToFloat(texelFetch(nph_utextures_2d[pc.visbuffer_index],
                                                     ivec2(gl_FragCoord.xy), 0)
                                              .y)) {
        demote;
    }
#endif
    ViewRef view = ViewRef(pc.view_address);
#ifdef NPH_RAY_QUERY
    g_tlas = view.v.tlas;
    g_tlas_offset = view.v.tlas_offset.xyz;
#endif
    g_vsm = VsmViewRef(view.v.vsm);
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
    // The triangle's plane, from the position's screen derivatives (only
    // its line matters; a degenerate one falls back on the vertex normal).
    const vec3 face = cross(dFdx(in_position), dFdy(in_position));
    s.face_normal = dot(face, face) > 1e-30 ? normalize(face) : normalize(in_normal);
    s.pixel = gl_FragCoord.xy;
    s.depth = gl_FragCoord.z;

    const vec4 base = nph_surface_base_color(m, s);
#if NPH_FORWARD_ALPHA_MODE == 1
    if (base.a < m.alpha_cutoff) {
        discard;
    }
#endif
    vec3 color;
    if (view.v.debug_mode == NPH_RENDER_DEBUG_IRRADIANCE) {
        color = nph_surface_irradiance_view(view, s, vec4(0.0));
    } else {
        color = nph_surface_shade(view, m, s, base);
    }
    out_color = vec4(color, NPH_FORWARD_ALPHA_MODE == 2 ? base.a : 1.0);
}
