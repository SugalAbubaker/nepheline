// SPDX-License-Identifier: Apache-2.0
// Surface shading shared by the forward reference renderer
// (forward_frag.glsl) and the visibility-buffer resolve (vis_resolve.comp):
// the metallic-roughness model of glTF 2.0 with a Lambertian diffuse lobe
// and a GGX specular lobe, height-correlated Smith visibility, and Schlick
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
// The includer says how a material texture is sampled at the surface by
// defining NPH_SURFACE_SAMPLE(tex, smp, s) before including this file: the
// forward pass with the rasteriser's implicit derivatives, the resolve
// with the analytic ones it computes (textureGrad). The surface carries
// its screen-space derivatives for the same reason.
#ifndef NPH_SURFACE_GLSL
#define NPH_SURFACE_GLSL

#include "render_common.glsl"

#ifndef NPH_SURFACE_SAMPLE
#error "define NPH_SURFACE_SAMPLE(tex, smp, s) before including surface.glsl"
#endif

// What shading needs to know about a point of a surface.
struct NphSurface {
    vec3 position;    // camera-relative world space
    vec3 normal;      // the interpolated vertex normal (not normalised)
    vec4 tangent;     // xyz: the interpolated tangent; w: bitangent sign
    vec2 uv;
    vec4 color;       // vertex colour
    vec3 position_dx; // screen-space derivatives (one pixel right, one down)
    vec3 position_dy;
    vec2 uv_dx;
    vec2 uv_dy;
    bool front_facing;
};

vec4 nph_surface_sample(NphRenderMaterialData m, int slot, NphSurface s)
{
    return NPH_SURFACE_SAMPLE(m.textures[slot], m.samplers[slot], s);
}

bool nph_material_has(NphRenderMaterialData m, int slot)
{
    return m.textures[slot] != NPH_RENDER_NO_TEXTURE;
}

// The base colour: the factor, the vertex colour, and the texture (sRGB
// formats where glTF says so; the hardware decodes them). Alpha is the
// coverage MASK tests and BLEND composites with.
vec4 nph_surface_base_color(NphRenderMaterialData m, NphSurface s)
{
    vec4 base = m.base_color * s.color;
    if (nph_material_has(m, NPH_RENDER_TEX_BASE_COLOR)) {
        base *= nph_surface_sample(m, NPH_RENDER_TEX_BASE_COLOR, s);
    }
    return base;
}

// A tangent frame from screen-space derivatives for meshes without
// tangents (Schüler, "Normal Mapping without Precomputed Tangents",
// ShaderX5, 2006). The bitangent follows decreasing v, as glTF's do.
mat3 nph_derivative_frame(vec3 n, NphSurface s)
{
    const vec3 dp1 = s.position_dx;
    const vec3 dp2 = s.position_dy;
    const vec2 duv1 = s.uv_dx;
    const vec2 duv2 = s.uv_dy;
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
vec3 nph_light_illuminance(NphRenderLightData light, vec3 p, out vec3 l)
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

// The pre-exposed radiance leaving the surface towards the camera (at the
// origin), or for an unlit material its colour as authored. `base` is
// nph_surface_base_color's result.
vec3 nph_surface_shade(ViewRef view, NphRenderMaterialData m, NphSurface s, vec4 base)
{
    if ((m.flags & NPH_RENDER_MATERIAL_UNLIT) != 0u) {
        // KHR_materials_unlit: the colour as authored, outside exposure.
        return base.rgb;
    }

    float roughness = m.roughness;
    float metallic = m.metallic;
    if (nph_material_has(m, NPH_RENDER_TEX_METALLIC_ROUGHNESS)) {
        const vec4 mr = nph_surface_sample(m, NPH_RENDER_TEX_METALLIC_ROUGHNESS, s);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, NPH_MIN_ROUGHNESS, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    float occlusion = 1.0;
    if (nph_material_has(m, NPH_RENDER_TEX_OCCLUSION)) {
        occlusion =
            1.0 + m.occlusion_strength * (nph_surface_sample(m, NPH_RENDER_TEX_OCCLUSION, s).r - 1.0);
    }

    // The shading normal: back faces of double-sided materials face the
    // viewer (glTF 2.0 §3.9.2); the whole tangent frame flips with them.
    const float side = s.front_facing ? 1.0 : -1.0;
    const vec3 vn = s.normal * side;
    vec3 n = normalize(vn);
    if (nph_material_has(m, NPH_RENDER_TEX_NORMAL)) {
        vec3 tn;
        const vec4 ns = nph_surface_sample(m, NPH_RENDER_TEX_NORMAL, s);
        tn.xy = ns.xy * 2.0 - 1.0;
        tn.z = (m.flags & NPH_RENDER_MATERIAL_NORMAL_TWO_CHANNEL) != 0u
                   ? sqrt(clamp(1.0 - dot(tn.xy, tn.xy), 0.0, 1.0))
                   : ns.z * 2.0 - 1.0;
        tn.xy *= m.normal_scale;
        mat3 tbn;
        if (dot(s.tangent.xyz, s.tangent.xyz) > 1e-12) {
            // MikkTSpace convention: the interpolated, unnormalised frame
            // (glTF 2.0 §3.7.2.1).
            const vec3 t = s.tangent.xyz * side;
            tbn = mat3(t, cross(vn, t) * (s.tangent.w * side), vn);
        } else {
            tbn = nph_derivative_frame(n, s);
        }
        n = normalize(tbn * tn);
    }

    const vec3 v = normalize(-s.position); // the camera is at the origin
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
        const vec3 e = nph_light_illuminance(lights.l[i], s.position, l);
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
        const vec3 irradiance =
            nph_sh9_irradiance(ShRef(view.v.sh_address), n) * view.v.ibl_intensity;
        const vec3 r = reflect(-v, n);
        const vec3 radiance =
            textureLod(samplerCube(nph_textures_cube[nonuniformEXT(view.v.specular_index)],
                                   nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                       r, roughness * view.v.specular_max_lod).rgb *
            view.v.ibl_intensity;
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
    if (nph_material_has(m, NPH_RENDER_TEX_EMISSIVE)) {
        emissive *= nph_surface_sample(m, NPH_RENDER_TEX_EMISSIVE, s).rgb;
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
    return min(color, vec3(NPH_RENDER_HALF_MAX));
}

#endif
