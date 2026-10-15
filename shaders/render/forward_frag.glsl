// SPDX-License-Identifier: Apache-2.0
// The forward reference renderer's fragment stage (docs/render.md "Forward
// pass"), included by forward_opaque.frag, forward_mask.frag, and
// forward_blend.frag with NPH_FORWARD_ALPHA_MODE set to 0, 1, or 2.
//
// The metallic-roughness model of glTF 2.0: a Lambertian diffuse lobe and
// a GGX specular lobe with height-correlated Smith visibility and Schlick
// Fresnel (Karis 2013; Burley 2012; Heitz 2014), lit by
//   - punctual lights in KHR_lights_punctual units with the windowed
//     inverse-square falloff of Karis 2013,
//   - the environment through the split-sum approximation (Karis 2013):
//     a GGX-prefiltered cube for the specular term and band-2 spherical
//     harmonics for the irradiance (Ramamoorthi & Hanrahan 2001),
// with multiple-scattering energy compensation for both (Fdez-Agüera, "A
// Multiple-Scattering Microfacet Model for Real-Time Image-based
// Lighting", JCGT 2019; Kulla & Conty, "Revisiting Physically Based
// Shading at Imageworks", SIGGRAPH 2017) and specular occlusion from the
// material's ambient occlusion (Lagarde & de Rousiers, "Moving Frostbite
// to PBR", SIGGRAPH 2014).
//
// The output is pre-exposed radiance, clamped to the largest half float.
#include "render_common.glsl"

#ifndef NPH_FORWARD_ALPHA_MODE
#error "include through forward_opaque.frag, forward_mask.frag, or forward_blend.frag"
#endif

layout(push_constant, scalar) uniform Push { NphRenderPush pc; };

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_color;
layout(location = 0) out vec4 out_color;

vec4 material_sample(NphRenderMaterialData m, int slot)
{
    return nph_sample_2d(m.textures[slot], m.samplers[slot], in_uv);
}

bool material_has(NphRenderMaterialData m, int slot)
{
    return m.textures[slot] != NPH_RENDER_NO_TEXTURE;
}

// A tangent frame from screen-space derivatives for meshes without
// tangents (Schüler, "Normal Mapping without Precomputed Tangents",
// ShaderX5, 2006). The bitangent follows decreasing v, as glTF's do.
mat3 derivative_frame(vec3 n, vec3 p, vec2 uv)
{
    const vec3 dp1 = dFdx(p);
    const vec3 dp2 = dFdy(p);
    const vec2 duv1 = dFdx(uv);
    const vec2 duv2 = dFdy(uv);
    const vec3 dp2perp = cross(dp2, n);
    const vec3 dp1perp = cross(n, dp1);
    const vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    const vec3 b = -(dp2perp * duv1.y + dp1perp * duv2.y);
    const float m = max(dot(t, t), dot(b, b));
    const float inv = m > 0.0 ? inversesqrt(m) : 0.0;
    return mat3(t * inv, b * inv, n);
}

// The illuminance a punctual light delivers at `p` on a surface facing it,
// and the direction towards the light.
vec3 light_illuminance(NphRenderLightData light, vec3 p, out vec3 l)
{
    const uint type = uint(light.direction.w);
    if (type == NPH_RENDER_LIGHT_DIRECTIONAL) {
        l = -light.direction.xyz;
        return light.color.rgb;
    }
    const vec3 to_light = light.position.xyz - p;
    const float d2 = max(dot(to_light, to_light), 1e-8);
    l = to_light * inversesqrt(d2);
    // Inverse-square falloff, windowed to reach zero at the range (Karis
    // 2013); a range of 0 is unlimited.
    float attenuation = 1.0 / max(d2, 1e-4);
    if (light.position.w > 0.0) {
        const float x = d2 * light.position.w;
        const float window = clamp(1.0 - x * x, 0.0, 1.0);
        attenuation *= window * window;
    }
    if (type == NPH_RENDER_LIGHT_SPOT) {
        // KHR_lights_punctual: a smooth step between the cone angles.
        const float cd = dot(light.direction.xyz, -l);
        const float s = clamp(cd * light.spot.x + light.spot.y, 0.0, 1.0);
        attenuation *= s * s;
    }
    return light.color.rgb * attenuation;
}

void main()
{
    ViewRef view = ViewRef(pc.view_address);
    const NphRenderDrawData draw = DrawsRef(view.v.draws_address).d[pc.draw_index];
    const NphRenderMaterialData m = MaterialsRef(view.v.materials_address).m[draw.material_index];

    // Base colour (texture formats are sRGB where glTF says so; the
    // hardware decodes them).
    vec4 base = m.base_color * in_color;
    if (material_has(m, NPH_RENDER_TEX_BASE_COLOR)) {
        base *= material_sample(m, NPH_RENDER_TEX_BASE_COLOR);
    }
#if NPH_FORWARD_ALPHA_MODE == 1
    if (base.a < m.alpha_cutoff) {
        discard;
    }
#endif
    if ((m.flags & NPH_RENDER_MATERIAL_UNLIT) != 0u) {
        // KHR_materials_unlit: the colour as authored, outside exposure.
        out_color = vec4(base.rgb, NPH_FORWARD_ALPHA_MODE == 2 ? base.a : 1.0);
        return;
    }

    float roughness = m.roughness;
    float metallic = m.metallic;
    if (material_has(m, NPH_RENDER_TEX_METALLIC_ROUGHNESS)) {
        const vec4 mr = material_sample(m, NPH_RENDER_TEX_METALLIC_ROUGHNESS);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, NPH_MIN_ROUGHNESS, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    float occlusion = 1.0;
    if (material_has(m, NPH_RENDER_TEX_OCCLUSION)) {
        occlusion = 1.0 + m.occlusion_strength * (material_sample(m, NPH_RENDER_TEX_OCCLUSION).r - 1.0);
    }

    // The shading normal: back faces of double-sided materials face the
    // viewer (glTF 2.0 §3.9.2); the whole tangent frame flips with them.
    const float side = gl_FrontFacing ? 1.0 : -1.0;
    const vec3 vn = in_normal * side;
    vec3 n = normalize(vn);
    if (material_has(m, NPH_RENDER_TEX_NORMAL)) {
        vec3 tn;
        const vec4 s = material_sample(m, NPH_RENDER_TEX_NORMAL);
        tn.xy = s.xy * 2.0 - 1.0;
        tn.z = (m.flags & NPH_RENDER_MATERIAL_NORMAL_TWO_CHANNEL) != 0u
                   ? sqrt(clamp(1.0 - dot(tn.xy, tn.xy), 0.0, 1.0))
                   : s.z * 2.0 - 1.0;
        tn.xy *= m.normal_scale;
        mat3 tbn;
        if (dot(in_tangent.xyz, in_tangent.xyz) > 1e-12) {
            // MikkTSpace convention: the interpolated, unnormalised frame
            // (glTF 2.0 §3.7.2.1).
            const vec3 t = in_tangent.xyz * side;
            tbn = mat3(t, cross(vn, t) * (in_tangent.w * side), vn);
        } else {
            tbn = derivative_frame(n, in_position, in_uv);
        }
        n = normalize(tbn * tn);
    }

    const vec3 v = normalize(-in_position); // the camera is at the origin
    const float nov = max(dot(n, v), 1e-4);
    const vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
    const vec3 diffuse_color = base.rgb * (1.0 - metallic);
    const float a = roughness * roughness;

    // The split-sum DFG terms (brdf_lut.comp): F0 * x + y is the
    // directional albedo of the specular lobe.
    const vec2 dfg = textureLod(sampler2D(nph_textures_2d[nonuniformEXT(view.v.brdf_lut_index)],
                                          nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                                vec2(nov, roughness), 0.0).xy;
    const float ess = max(dfg.x + dfg.y, 1e-4);
    const vec3 energy = vec3(1.0) + f0 * (1.0 / ess - 1.0);

    // Punctual lights.
    vec3 direct = vec3(0.0);
    LightsRef lights = LightsRef(view.v.lights_address);
    for (uint i = 0u; i < view.v.light_count; ++i) {
        vec3 l;
        const vec3 e = light_illuminance(lights.l[i], in_position, l);
        const float nol = dot(n, l);
        if (nol <= 0.0) {
            continue;
        }
        const vec3 h = normalize(v + l);
        const float noh = clamp(dot(n, h), 0.0, 1.0);
        const float voh = clamp(dot(v, h), 0.0, 1.0);
        const vec3 spec = nph_d_ggx(noh, a) * nph_v_smith_ggx_correlated(nov, nol, a) *
                          nph_f_schlick(voh, f0) * energy;
        direct += (diffuse_color * NPH_INV_PI + spec) * e * nol;
    }

    // Image-based lighting.
    vec3 indirect = vec3(0.0);
    if ((view.v.flags & NPH_RENDER_VIEW_IBL) != 0u) {
        const vec3 irradiance = nph_sh9_irradiance(ShRef(view.v.sh_address), n) * view.v.ibl_intensity;
        const vec3 r = reflect(-v, n);
        const vec3 radiance =
            textureLod(samplerCube(nph_textures_cube[nonuniformEXT(view.v.specular_index)],
                                   nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                       r, roughness * view.v.specular_max_lod).rgb * view.v.ibl_intensity;
        // Fdez-Agüera 2019: single- and multiple-scattering specular, and
        // the diffuse energy that remains.
        const vec3 fr = max(vec3(1.0 - roughness), f0) - f0;
        const vec3 k_s = f0 + fr * pow(1.0 - nov, 5.0);
        const vec3 fss_ess = k_s * dfg.x + dfg.y;
        const float ems = 1.0 - (dfg.x + dfg.y);
        const vec3 f_avg = f0 + (vec3(1.0) - f0) / 21.0;
        const vec3 fms_ems = fss_ess * f_avg / (vec3(1.0) - ems * f_avg) * ems;
        const vec3 k_d = diffuse_color * (vec3(1.0) - fss_ess - fms_ems);
        const float spec_occlusion =
            clamp(pow(nov + occlusion, exp2(-16.0 * a - 1.0)) - 1.0 + occlusion, 0.0, 1.0);
        indirect = fss_ess * radiance * spec_occlusion +
                   (fms_ems + k_d) * irradiance * NPH_INV_PI * occlusion;
    }

    vec3 emissive = m.emissive.rgb;
    if (material_has(m, NPH_RENDER_TEX_EMISSIVE)) {
        emissive *= material_sample(m, NPH_RENDER_TEX_EMISSIVE).rgb;
    }

    vec3 color = (direct + indirect + emissive) * view.v.exposure;
    switch (view.v.debug_mode) {
    case NPH_RENDER_DEBUG_BASE_COLOR:
        color = base.rgb;
        break;
    case NPH_RENDER_DEBUG_NORMAL:
        color = n * 0.5 + 0.5;
        break;
    case NPH_RENDER_DEBUG_ROUGHNESS:
        color = vec3(roughness);
        break;
    case NPH_RENDER_DEBUG_METALLIC:
        color = vec3(metallic);
        break;
    case NPH_RENDER_DEBUG_OCCLUSION:
        color = vec3(occlusion);
        break;
    case NPH_RENDER_DEBUG_DIRECT:
        color = direct * view.v.exposure;
        break;
    case NPH_RENDER_DEBUG_INDIRECT:
        color = indirect * view.v.exposure;
        break;
    default:
        break;
    }
    out_color = vec4(min(color, vec3(NPH_RENDER_HALF_MAX)), NPH_FORWARD_ALPHA_MODE == 2 ? base.a : 1.0);
}
