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

#if NPH_FORWARD_ALPHA_MODE == 2
// The production path's blended surfaces have no depth attachment: the
// opaque surfaces' depth is the visibility buffer's, which they test
// themselves (pc.visbuffer_index, its sampled view: the depth's bits in y),
// as the reference's GREATER test does in hardware. Their pipelines are
// specialised on it.
layout(constant_id = 1) const bool k_vis_depth = false;
// They sample by the derivatives taken at the start: a helper's part, and a
// hidden fragment's, ends with those (main()), so a fragment's quad may be gone
// by the time it samples. A sheet of dirt in front of a wall is mostly clear,
// and much of it hidden: those fragments cost nearly nothing this way.
#define NPH_SURFACE_SAMPLE(tex, smp, s)                                                            \
    (k_vis_depth ? nph_sample_2d_grad((tex), (smp), (s).uv, (s).uv_dx, (s).uv_dy)                  \
                 : nph_sample_2d((tex), (smp), (s).uv))
// And they take the radiance cache's light where the frame has it, as the
// resolve's surfaces do (vis_resolve.glsl).
#include "gi_common.glsl"
layout(constant_id = 2) const bool k_gi = false;
vec4 g_gi_cell; // set in main() where k_gi
// A frame whose only light is the cache's (no lights, no sun, no sky): all a blended surface
// shows of it is its colour times what the cell gathered, the specular's share in that
// colour (a cell's light arrives as if evenly from everywhere: it has no highlight, and a
// rough lobe integrates to about the base colour).
layout(constant_id = 3) const bool k_cache_only = false;
#define NPH_SURFACE_GI_ACTIVE k_gi
#define NPH_SURFACE_GI        g_gi_cell
#define NPH_SURFACE_GI_FAR_SPECULAR
#else
#define NPH_SURFACE_SAMPLE(tex, smp, s) nph_sample_2d((tex), (smp), (s).uv)
#endif
// Paged textures' fragments ask for their pages (texture_paging.glsl;
// ADR-0065).
#include "texture_paging.glsl"
// With the production path's light grid, the surfaces drawn here lie in
// front of the visibility buffer's (or of the background): the grid's
// masks for blended surfaces.
#define NPH_SURFACE_GRID_TILES(g) (g).tiles_blended

// The production path's blended surfaces read the sun's virtual shadow map
// where the view has one (vsm_common.glsl); their pipelines are
// specialised on it (the reference's never are).
#define NPH_VSM_SAMPLING
#include "vsm_common.glsl"
layout(constant_id = 0) const bool k_vsm = false;
VsmViewRef g_vsm; // set first thing in main()
#define NPH_SURFACE_VSM_ACTIVE k_vsm
#define NPH_SURFACE_VSM(p)     vsm_shadow(g_vsm, (p).position, (p).face_normal, (p).n)

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
    // A fragment behind is demoted, not discarded, so that it still serves its
    // quad's derivatives; it writes nothing.
    if (k_vis_depth &&
        gl_FragCoord.z <= uintBitsToFloat(texelFetch(nph_utextures_2d[pc.visbuffer_index],
                                                     ivec2(gl_FragCoord.xy), 0)
                                              .y)) {
        demote;
    }
#endif
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
#if NPH_FORWARD_ALPHA_MODE == 2
    // A helper's part ends here, a hidden fragment's with it: all the rest is its own.
    if (k_vis_depth && helperInvocationEXT()) {
        return;
    }
#endif
    s.face_normal = dot(face, face) > 1e-30 ? normalize(face) : normalize(in_normal);
    ViewRef view = ViewRef(pc.view_address);
#ifdef NPH_RAY_QUERY
    g_tlas = view.v.tlas;
    g_tlas_offset = view.v.tlas_offset.xyz;
#endif
    g_vsm = VsmViewRef(view.v.vsm);
    const NphRenderDrawData draw = DrawsRef(view.v.draws_address).d[pc.draw_index];
    const NphRenderMaterialData m = MaterialsRef(view.v.materials_address).m[draw.material_index];
    s.pixel = gl_FragCoord.xy;
    s.depth = gl_FragCoord.z;
    // Where the view pages textures, a fragment of each 8 x 8 tile a frame
    // asks for the pages its footprint covers (its derivatives taken above,
    // with the quad's).
    if (view.v.paging != 0ul &&
        paging_asks(uvec2(gl_FragCoord.xy), PagingViewRef(view.v.paging).v.feedback_phase)) {
        PagingViewRef paging = PagingViewRef(view.v.paging);
        for (int slot = 0; slot < NPH_RENDER_TEX_COUNT; ++slot) {
            paging_request(paging, m.textures[slot], (m.wrap >> (4 * slot)) & 15u, s.uv, s.uv_dx,
                           s.uv_dy);
        }
    }

    const vec4 base = nph_surface_base_color(m, s);
#if NPH_FORWARD_ALPHA_MODE == 1
    if (base.a < m.alpha_cutoff) {
        discard;
    }
#endif
#if NPH_FORWARD_ALPHA_MODE == 2
    if (k_vis_depth) {
        // Under the target's steps whatever it is shaded to.
        if (base.a < 1.0 / 255.0) {
            discard;
        }
        if (k_gi) {
            g_gi_cell = gi_surface_cell(GiViewRef(view.v.gi), ivec2(gl_FragCoord.xy), in_position,
                                        s.face_normal);
        }
    }
#endif
    vec3 color;
#if NPH_FORWARD_ALPHA_MODE == 2
    if (k_cache_only) {
        const float metallic = m.metallic;
        const vec3 albedo = base.rgb * (1.0 - metallic) * 0.96 + mix(vec3(0.04), base.rgb, metallic);
        vec3 emissive = m.emissive.rgb;
        if (nph_material_has(m, NPH_RENDER_TEX_EMISSIVE)) {
            emissive *= nph_surface_sample(m, NPH_RENDER_TEX_EMISSIVE, s).rgb;
        }
        color = min((albedo * g_gi_cell.rgb * (NPH_INV_PI * g_gi_cell.w) + emissive) * view.v.exposure,
                    vec3(NPH_RENDER_HALF_MAX));
        out_color = vec4(color, base.a);
        return;
    }
#endif
    if (view.v.debug_mode == NPH_RENDER_DEBUG_IRRADIANCE) {
        color = nph_surface_irradiance_view(view, s, vec4(0.0));
    } else {
        color = nph_surface_shade(view, m, s, base);
    }
    out_color = vec4(color, NPH_FORWARD_ALPHA_MODE == 2 ? base.a : 1.0);
}
