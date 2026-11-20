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
// The production path draws them all with one indirect draw of draws the GPU
// listed (vis_blend.comp; ADR-0097; NPH_FORWARD_INDIRECT, which
// forward_blend_indirect.frag sets), so with one pipeline, which culls back
// faces counter-clockwise, from index lists wound for it: what a fragment
// needs of its draw's record comes from the vertex stage (forward.vert
// out_draw), and its facing from the list. A one-sided draw's fragments are
// all of front faces (its list is turned where its transform mirrors). A
// two-sided draw's list holds each triangle and then the same turned, of
// which the rasteriser draws the one that winds counter-clockwise on screen:
// an even primitive is the triangle as made, which is a front face unless the
// transform mirrors.
#ifdef NPH_FORWARD_INDIRECT
layout(location = 5) flat in uvec2 in_draw;
#endif
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

// Whether a material reads the second texture coordinate set (ADR-0107; forward.vert's
// k_uv1): only then does a slot test which set it reads, and the stage read the second.
layout(constant_id = 5) const bool k_uv1 = false;
#define NPH_SURFACE_UV1_ACTIVE k_uv1
// Whether the material's F0 is read (ADR-0107): only in the variants drawn while a material needs
// one other than 0.04 (renderer.c render_forward_pipeline's `f0`, and its `tint`, which reads it
// too). The others shade with 0.04, which the compiler folds: a read F0 cost the blended pass a
// tenth of its time where its surfaces lie many deep.
layout(constant_id = 7) const bool k_f0 = false;
#define NPH_SURFACE_F0(m) (k_f0 ? (m).emissive.w : 0.04)
#include "surface.glsl"

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;
layout(location = 6) in vec2 in_uv1;
layout(location = 0) out vec4 out_color;
#if NPH_FORWARD_ALPHA_MODE == 2
// What of the light behind the surface passes, channel by channel: the blend's second source
// (dual-source blending; ADR-0107), the destination's factor, written in the variants that blend
// so (k_tint): those drawn while a tinted material is live (renderer.c render_forward_pipeline's
// `tint`). The others blend "over" by the first output's alpha, as before.
layout(location = 0, index = 1) out vec4 out_transmit;
layout(constant_id = 6) const bool k_tint = false;
#endif

#if NPH_FORWARD_ALPHA_MODE == 2
// A blended surface's colour as the pipeline composites it, "over" with the colour multiplied
// by its alpha here: alpha is a coverage, and all the surface's light scales with it. Glass
// (NPH_RENDER_MATERIAL_TRANSMISSIVE) is whole, and thin: `alpha` is the share of the light
// behind it that its body stops, its base colour came scaled by it, and what it reflects is its
// surface's, added as shaded; what is behind it shows less by what the surface turns back too,
// a dielectric's share at the angle it is seen from (Schlick, "An Inexpensive BRDF Model for
// Physically-Based Rendering", 1994; 4 % head on).
vec4 forward_blended(NphRenderMaterialData m, NphSurface s, vec3 color, float alpha)
{
    if ((m.flags & NPH_RENDER_MATERIAL_TRANSMISSIVE) == 0u) {
        return vec4(color * alpha, alpha);
    }
    const vec3 n = normalize(s.face_normal);
    const vec3 v = normalize(-s.position);
    const float turned = 0.04 + 0.96 * pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 5.0);
    return vec4(color, 1.0 - (1.0 - alpha) * (1.0 - turned));
}

// The k_tint variants' outputs, over forward_blended's (ADR-0107). Glass turns back its own F0's
// share. The light behind passes by the second output, channel by channel: one minus the first's
// alpha, as "over" composites, but where glass is tinted (NPH_RENDER_MATERIAL_TINTED), whose share
// is then times its tint, its vertex colour and its base colour texture (glTF tints what glass
// lets through by its base colour), and so coloured.
void forward_blended_tinted(NphRenderMaterialData m, NphSurface s, vec3 color, float alpha)
{
    if ((m.flags & NPH_RENDER_MATERIAL_TRANSMISSIVE) == 0u) {
        out_transmit = vec4(vec3(1.0 - out_color.a), 0.0);
        return;
    }
    const vec3 n = normalize(s.face_normal);
    const vec3 v = normalize(-s.position);
    const float f0 = m.emissive.w;
    const float turned = f0 + (1.0 - f0) * pow(1.0 - clamp(abs(dot(n, v)), 0.0, 1.0), 5.0);
    out_color = vec4(color, 1.0 - (1.0 - alpha) * (1.0 - turned));
    out_transmit = vec4(vec3(1.0 - out_color.a), 0.0);
    if ((m.flags & NPH_RENDER_MATERIAL_TINTED) != 0u) {
        vec3 tint = m.transmission.rgb * s.color.rgb;
        if (nph_material_has(m, NPH_RENDER_TEX_BASE_COLOR)) {
            tint *= nph_surface_sample(m, NPH_RENDER_TEX_BASE_COLOR, s).rgb;
        }
        out_transmit.rgb = (1.0 - alpha) * (1.0 - turned) * tint;
    }
}
#endif

void main()
{
#ifdef NPH_FORWARD_INDIRECT
    const bool front_facing =
        (in_draw.y & 2u) != 0u || ((gl_PrimitiveID & 1) == 0) != ((in_draw.y & 1u) != 0u);
#else
    const bool front_facing = gl_FrontFacing;
#endif
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
    s.uv1 = vec2(0.0);
    s.uv1_dx = vec2(0.0);
    s.uv1_dy = vec2(0.0);
    if (k_uv1) {
        s.uv1 = in_uv1;
        s.uv1_dx = dFdx(in_uv1);
        s.uv1_dy = dFdy(in_uv1);
    }
    // Mirrored transforms are drawn with clockwise front faces, so the
    // rasteriser's facing already accounts for them (an indirect draw's is its
    // list's, above).
    s.front_facing = front_facing;
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
#ifdef NPH_FORWARD_INDIRECT
    const uint material_index = in_draw.x;
#else
    const uint material_index = DrawsRef(view.v.draws_address).d[pc.draw_index].material_index;
#endif
    const NphRenderMaterialData m = MaterialsRef(view.v.materials_address).m[material_index];
    s.pixel = gl_FragCoord.xy;
    s.depth = gl_FragCoord.z;
    // Where the view pages textures, a fragment of each 8 x 8 tile a frame
    // asks for the pages its footprint covers (its derivatives taken above,
    // with the quad's).
    if (view.v.paging != 0ul &&
        paging_asks(uvec2(gl_FragCoord.xy), PagingViewRef(view.v.paging).v.feedback_phase)) {
        PagingViewRef paging = PagingViewRef(view.v.paging);
        for (int slot = 0; slot < NPH_RENDER_TEX_COUNT; ++slot) {
            const bool second = k_uv1 && nph_material_slot_uv1(m.wrap, uint(slot));
            paging_request(paging, m.textures[slot], (m.wrap >> (4 * slot)) & 15u,
                           second ? s.uv1 : s.uv, second ? s.uv1_dx : s.uv_dx,
                           second ? s.uv1_dy : s.uv_dy);
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
            g_gi_cell = gi_surface_cell(GiViewRef(view.v.gi), in_position, s.face_normal);
        }
    }
#endif
    vec3 color;
#if NPH_FORWARD_ALPHA_MODE == 2
    // Glass whose transmission varies per texel (NPH_RENDER_MATERIAL_TRANSMISSION_TEXELS;
    // ADR-0107): its colour scaled by its alpha here, texel by texel, as the cooker scales the
    // factor of glass whose share is one. Only the k_tint variants draw such glass
    // (render_material_tinted).
    vec4 lit = base;
    if (k_tint && (m.flags & NPH_RENDER_MATERIAL_TRANSMISSION_TEXELS) != 0u) {
        lit.rgb *= base.a;
    }
#define NPH_FORWARD_LIT lit
#else
#define NPH_FORWARD_LIT base
#endif
#if NPH_FORWARD_ALPHA_MODE == 2
    if (k_cache_only) {
        const float metallic = m.metallic;
        vec3 albedo = base.rgb * (1.0 - metallic) * 0.96 + mix(vec3(0.04), base.rgb, metallic);
        if (k_f0) {
            // The material's F0 (ADR-0107), and glass's colour as `lit` scales it per texel.
            const float f0 = m.emissive.w;
            albedo = lit.rgb * (1.0 - metallic) * (1.0 - f0) + mix(vec3(f0), lit.rgb, metallic);
        }
        vec3 emissive = m.emissive.rgb;
        if (nph_material_has(m, NPH_RENDER_TEX_EMISSIVE)) {
            emissive *= nph_surface_sample(m, NPH_RENDER_TEX_EMISSIVE, s).rgb;
        }
        color = min((albedo * g_gi_cell.rgb * (NPH_INV_PI * g_gi_cell.w) + emissive) * view.v.exposure,
                    vec3(NPH_RENDER_HALF_MAX));
        out_color = forward_blended(m, s, color, base.a);
        if (k_tint) {
            forward_blended_tinted(m, s, color, base.a);
        }
        return;
    }
#endif
    if (view.v.debug_mode == NPH_RENDER_DEBUG_IRRADIANCE) {
        color = nph_surface_irradiance_view(view, s, vec4(0.0));
    } else {
        color = nph_surface_shade(view, m, s, NPH_FORWARD_LIT);
    }
#if NPH_FORWARD_ALPHA_MODE == 2
    out_color = forward_blended(m, s, color, base.a);
    if (k_tint) {
        forward_blended_tinted(m, s, color, base.a);
    }
#else
    out_color = vec4(color, 1.0);
#endif
}
