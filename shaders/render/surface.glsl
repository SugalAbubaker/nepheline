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
// ambient occlusion (Lagarde & de Rousiers, "Moving Frostbite to PBR",
// SIGGRAPH 2014): the material's, times the production path's screen-space
// occlusion where the includer defines NPH_SURFACE_AO (vis_resolve.glsl,
// from ao.comp), which the diffuse light takes with the light its occluders
// reflect back (Jimenez, Wu, Pesce & Jarabo, "Practical Realtime Strategies
// for Accurate Indirect Occlusion", 2016, Eq. 10).
//
// The includer says how a material texture is sampled at the surface by
// defining NPH_SURFACE_SAMPLE(tex, smp, s) before including this file: the
// forward pass with the rasteriser's implicit derivatives, the resolve
// with the analytic ones it computes (textureGrad). The surface carries
// its screen-space derivatives for the same reason.
//
// The reference evaluates every light for every pixel. The production
// path's view carries a light grid (NphRenderLightGrid) instead, and a
// pixel evaluates only the lights its distance bin and screen tile may
// meet. A light skipped there contributes exactly nothing: its windowed
// falloff or its cone is zero at the pixel. A shader that always has the
// grid defines NPH_SURFACE_GRID_LIGHTS and leaves the reference's loop out.
// Each invocation walks its own lights: on the reference GPU, invocations
// of a subgroup that load the same light share the load anyway, and
// walking the subgroup's lights together (Drobot 2017) measured slower
// for the registers it holds (docs/adr/0031).
//
// The diffuse indirect light takes the environment's irradiance, or, where
// the includer defines NPH_SURFACE_GI while NPH_SURFACE_GI_ACTIVE, the
// production path's radiance cache's: NPH_SURFACE_GI is the cache's cell
// at the surface (gi_common.glsl gi_cell: its irradiance, and how far it is
// trusted, 0 to 1), and the environment's irradiance makes up what it does
// not trust yet.
//
// The environment and the emission are evaluated before the punctual
// lights, so that across the light loop, where a shader's registers peak,
// only the point's description and one sum stay live.
//
// Shadows: a light whose record says it casts them
// (NphRenderLightData.color.w) is weighed by its visibility in [0, 1] from
// the lit point `p` (an NphLitPoint, whose geometric normal offsets the
// query off its surface), when the includer defines how:
//   - NPH_SURFACE_VSM(p), for the production path's sun (color.w 2) while
//     NPH_SURFACE_VSM_ACTIVE: its virtual shadow map (vsm_common.glsl),
//     looked up once per point before the light loop, whose registers the
//     lookup would otherwise add to;
//   - NPH_SURFACE_SHADOW(p, l, distance), for the others (and the sun
//     otherwise): a shadow ray toward the light in direction `l`,
//     `distance` away (rt_common.glsl).
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
    vec3 face_normal; // the triangle's geometric normal, either way round
    vec2 pixel;       // the pixel's position in the target (a light tile is found from it)
    float depth; // its reverse-Z depth (znear over the view distance)
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

// What the lights need to know about a shaded point.
struct NphLitPoint {
    vec3 position;
    vec3 n;
    vec3 v;
    float nov;
    float a; // GGX alpha
    vec3 base;
    float metallic;
    float energy; // 1 / Ess - 1: the specular lobe's multiple-scattering compensation is
                  // 1 + F0 * energy
    vec3 face_normal; // the surface's geometric normal, either way round
#ifdef NPH_SURFACE_VSM
    float sun_visibility; // NPH_SURFACE_VSM's, or 1 when it is not active
#endif
};

// The radiance one punctual light reflects from the point, before exposure.
vec3 nph_lit_light(NphLitPoint p, NphRenderLightData light)
{
    vec3 l;
    vec3 e = nph_light_illuminance(light, p.position, l);
    const float nol = dot(p.n, l);
    if (nol <= 0.0 || (e.r == 0.0 && e.g == 0.0 && e.b == 0.0)) {
        return vec3(0.0);
    }
#if defined(NPH_SURFACE_VSM) || defined(NPH_SURFACE_SHADOW)
    if (light.color.w > 0.0) {
        float visibility = 1.0;
#ifdef NPH_SURFACE_VSM
        if (light.color.w > 1.5 && NPH_SURFACE_VSM_ACTIVE) {
            visibility = p.sun_visibility;
        } else
#endif
        {
#ifdef NPH_SURFACE_SHADOW
            // Directional lights are unlimited; the others end at the light.
            const float distance = uint(light.direction.w) == NPH_RENDER_LIGHT_DIRECTIONAL
                                       ? 1.0e6
                                       : length(light.position.xyz - p.position);
            visibility = NPH_SURFACE_SHADOW(p, l, distance);
#endif
        }
        e *= visibility;
        if (e.r == 0.0 && e.g == 0.0 && e.b == 0.0) {
            return vec3(0.0);
        }
    }
#endif
    const vec3 h = normalize(p.v + l);
    const float noh = clamp(dot(p.n, h), 0.0, 1.0);
    const float voh = clamp(dot(p.v, h), 0.0, 1.0);
    // F0 and the diffuse colour again from the base colour: fewer values
    // live across the loop for a few multiplies per light.
    const vec3 f0 = mix(vec3(0.04), p.base, p.metallic);
    const vec3 spec = nph_d_ggx(noh, p.a) * nph_v_smith_ggx_correlated(p.nov, nol, p.a) *
                      nph_f_schlick(voh, f0) * (vec3(1.0) + f0 * p.energy);
    return (p.base * (1.0 - p.metallic) * NPH_INV_PI + spec) * e * nol;
}

// The bits of light word `w` whose indices lie in [first, last]; the word
// must overlap that range.
uint nph_light_bits(uint w, uint first, uint last)
{
    const uint lo = w * 32u;
    const uint a = first > lo ? first - lo : 0u;
    const uint b = min(last - lo, 31u);
    return (0xFFFFFFFFu >> (31u - b)) & (0xFFFFFFFFu << a);
}

// The direct light the production path's grid holds for the point: the
// lights that reach every pixel, then the sorted local lights in the range
// of the point's distance bin whose bit its tile's mask sets, in index
// order.
vec3 nph_lit_grid(NphLitPoint p, NphRenderLightGrid g, vec2 pixel, float depth)
{
    vec3 direct = vec3(0.0);
    WordsRef counts = WordsRef(g.counts);
    LightsRef globals = LightsRef(g.globals);
    const uint global_count = counts.w[0];
    for (uint i = 0u; i < global_count; ++i) {
        direct += nph_lit_light(p, globals.l[i]);
    }
    const uint range = WordsRef(g.zbins).w[nph_zbin(g.znear / depth, g.znear, g.zbin_scale)];
    const uint first = range & 0xFFFFu;
    const uint last = range >> 16u;
    const uvec2 tile = uvec2(pixel) / uint(NPH_RENDER_LIGHT_TILE);
    const uint at = tile.y * g.tiles_x + tile.x;
    WordsRef tiles = WordsRef(g.tiles);
    LightsRef locals = LightsRef(g.locals);
    if (first <= last) {
        for (uint w = first >> 5u; w <= (last >> 5u); ++w) {
            uint bits = tiles.w[w * g.tile_count + at] & nph_light_bits(w, first, last);
            while (bits != 0u) {
                const uint b = findLSB(bits);
                bits &= bits - 1u;
                direct += nph_lit_light(p, locals.l[w * 32u + b]);
            }
        }
    }
    return direct;
}

// The irradiance debug view (NPH_RENDER_DEBUG_IRRADIANCE): the diffuse
// irradiance at the surface's interpolated normal (facing the viewer, as
// the shading's) over pi, pre-exposed, before any occlusion: the
// environment's, or as far as `cell.w` trusts it the radiance cache's
// (gi_common.glsl gi_cell; zero without one). Includers branch to it instead
// of nph_surface_shade: as a case of that function's debug views it kept a
// value live that cost every frame (0.05 ms in a 4K resolve).
vec3 nph_surface_irradiance_view(ViewRef view, NphSurface s, vec4 cell)
{
    const vec3 n = normalize(s.normal * (s.front_facing ? 1.0 : -1.0));
    vec3 e = vec3(0.0);
    if ((view.v.flags & NPH_RENDER_VIEW_IBL) != 0u) {
        e = nph_sh9_irradiance(ShRef(view.v.sh_address), n) * view.v.ibl_intensity;
    }
    e = mix(e, cell.rgb, cell.w);
    return min(e * (NPH_INV_PI * view.v.exposure), vec3(NPH_RENDER_HALF_MAX));
}

// The pre-exposed radiance leaving the surface towards the camera (at the
// origin), or for an unlit material its colour as authored. `base` is
// nph_surface_base_color's result. The irradiance debug view is
// nph_surface_irradiance_view's.
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
#ifdef NPH_SURFACE_AO
    const float screen_occlusion = NPH_SURFACE_AO;
#else
    const float screen_occlusion = 1.0;
#endif

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
            // MikkTSpace's frame (glTF 2.0 §3.7.2.1) in its normalised
            // variant: the interpolated normal and tangent at unit length,
            // the bitangent their cross product with the stored sign. The
            // unnormalised variant assumes both arrive as interpolated unit
            // vectors, which a transform that scales breaks: it stretches
            // the tangent by its scale and shrinks the normal by it (Bistro,
            // scaled by 100, shaded in the surface's plane). The variants
            // differ to second order in the frame's spread across a triangle.
            const vec3 t = normalize(s.tangent.xyz) * side;
            tbn = mat3(t, cross(n, t) * (s.tangent.w * side), n);
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

    // Image-based lighting, and the production path's radiance cache where
    // the includer defines NPH_SURFACE_GI (vis_resolve.glsl, from
    // gi_update.comp), whose irradiance the diffuse light takes instead of
    // the environment's: it holds the environment's light too, occluded by
    // the scene, and the light the scene reflects (with no environment, the
    // sun's alone).
    const bool ibl = (view.v.flags & NPH_RENDER_VIEW_IBL) != 0u;
#ifdef NPH_SURFACE_GI
    const bool gi = NPH_SURFACE_GI_ACTIVE;
#else
    const bool gi = false;
#endif
    vec3 indirect = vec3(0.0);
    if (ibl || gi) {
        vec3 irradiance = vec3(0.0);
        if (ibl) {
            irradiance = nph_sh9_irradiance(ShRef(view.v.sh_address), n) * view.v.ibl_intensity;
        }
#ifdef NPH_SURFACE_GI
        if (gi) {
            const vec4 cell = NPH_SURFACE_GI;
            irradiance = mix(irradiance, cell.rgb, cell.w);
        }
#endif
        const vec3 r = reflect(-v, n);
        const vec3 radiance =
            ibl ? textureLod(samplerCube(nph_textures_cube[nonuniformEXT(view.v.specular_index)],
                                         nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                             r, roughness * view.v.specular_max_lod)
                          .rgb *
                      view.v.ibl_intensity
                : vec3(0.0);
        // Fdez-Agüera 2019: single- and multiple-scattering specular, and
        // the diffuse energy that remains.
        const vec3 fr = max(vec3(1.0 - roughness), f0) - f0;
        const vec3 k_s = f0 + fr * pow(1.0 - nov, 5.0);
        const vec3 fss_ess = k_s * dfg.x + dfg.y;
        const float ems = 1.0 - (dfg.x + dfg.y);
        const vec3 f_avg = f0 + (vec3(1.0) - f0) / 21.0;
        const vec3 fms_ems = fss_ess * f_avg / (vec3(1.0) - ems * f_avg) * ems;
        const vec3 k_d = diffuse_color * (vec3(1.0) - fss_ess - fms_ems);
        const float ao = occlusion * screen_occlusion;
        const float spec_occlusion = clamp(pow(nov + ao, exp2(-16.0 * a - 1.0)) - 1.0 + ao, 0.0, 1.0);
        // The diffuse light under the screen-space occlusion A, with what its
        // occluders of the surface's albedo reflect back: Jimenez et al.'s
        // cubic fit (Eq. 10), at least A (the fit falls below it for dark
        // albedos) and at most 1, where A = 1 leaves the light as it was.
#ifdef NPH_SURFACE_AO
        const vec3 fit_a = 2.0404 * diffuse_color - 0.3324;
        const vec3 fit_b = -4.7951 * diffuse_color + 0.6417;
        const vec3 fit_c = 2.7552 * diffuse_color + 0.6903;
        const float sa = screen_occlusion;
        const vec3 bounce = clamp(((fit_a * sa + fit_b) * sa + fit_c) * sa, vec3(sa), vec3(1.0));
#else
        const vec3 bounce = vec3(1.0);
#endif
        indirect = fss_ess * radiance * spec_occlusion +
                   (fms_ems + k_d) * irradiance * NPH_INV_PI * (occlusion * bounce);
    }

    vec3 emissive = m.emissive.rgb;
    if (nph_material_has(m, NPH_RENDER_TEX_EMISSIVE)) {
        emissive *= nph_surface_sample(m, NPH_RENDER_TEX_EMISSIVE, s).rgb;
    }

    // The debug views that need no punctual light.
    switch (view.v.debug_mode) {
    case NPH_RENDER_DEBUG_BASE_COLOR:
        return min(base.rgb, vec3(NPH_RENDER_HALF_MAX));
    case NPH_RENDER_DEBUG_NORMAL:
        return n * 0.5 + 0.5;
    case NPH_RENDER_DEBUG_ROUGHNESS:
        return vec3(roughness);
    case NPH_RENDER_DEBUG_METALLIC:
        return vec3(metallic);
    case NPH_RENDER_DEBUG_OCCLUSION:
        return vec3(occlusion * screen_occlusion);
    case NPH_RENDER_DEBUG_INDIRECT:
        return min(indirect * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
    default:
        break;
    }
    const vec3 rest = indirect + emissive;

    // Punctual lights.
    NphLitPoint lp;
    lp.position = s.position;
    lp.n = n;
    lp.v = v;
    lp.nov = nov;
    lp.a = a;
    lp.base = base.rgb;
    lp.metallic = metallic;
    lp.energy = 1.0 / ess - 1.0;
    lp.face_normal = s.face_normal;
#ifdef NPH_SURFACE_VSM
    lp.sun_visibility = NPH_SURFACE_VSM_ACTIVE ? NPH_SURFACE_VSM(lp) : 1.0;
#endif
#ifdef NPH_SURFACE_GRID_LIGHTS
    const vec3 direct = nph_lit_grid(lp, LightGridRef(view.v.light_grid).g, s.pixel, s.depth);
#else
    vec3 direct = vec3(0.0);
    if (view.v.light_grid != 0ul) {
        direct = nph_lit_grid(lp, LightGridRef(view.v.light_grid).g, s.pixel, s.depth);
    } else {
        LightsRef lights = LightsRef(view.v.lights_address);
        for (uint i = 0u; i < view.v.light_count; ++i) {
            direct += nph_lit_light(lp, lights.l[i]);
        }
    }
#endif
    if (view.v.debug_mode == NPH_RENDER_DEBUG_DIRECT) {
        return min(direct * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
    }
    return min((direct + rest) * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
}

#endif
