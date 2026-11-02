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
// The tiles' masks are culled against the depths of the tile's opaque
// surfaces (the visibility buffer's); a shader that draws others in front
// of them defines NPH_SURFACE_GRID_TILES(g) as the grid's tiles_blended.
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
//
// Voxels (render/voxel.h; docs/render.md "Voxels"; ADR-0061): an includer
// that defines NPH_SURFACE_VOXEL (vis_voxel_resolve.glsl) shades with
// nph_surface_shade_voxel, a point whose normals spread as a two-sided von
// Mises-Fisher lobe, integrated analytically: the diffuse light averaged over
// the part of the lobe the viewer sees (Hill's fit of its clamped cosine), the
// lobe's variance added to GGX's, and its convolution of the environment's
// spherical harmonics. Its point reflects light from behind its lobe's axis
// too, so the lights' tests of N.L allow for the lobe.
#ifndef NPH_SURFACE_GLSL
#define NPH_SURFACE_GLSL

#include "render_common.glsl"

#ifndef NPH_SURFACE_SAMPLE
#error "define NPH_SURFACE_SAMPLE(tex, smp, s) before including surface.glsl"
#endif
#ifndef NPH_SURFACE_GRID_TILES
#define NPH_SURFACE_GRID_TILES(g) (g).tiles
#endif
// The view's debug view (NPH_RENDER_DEBUG_*): a shader whose pipelines are
// specialised on whether they draw the debug views defines it so that the
// others compile without their branches.
#ifndef NPH_SURFACE_DEBUG_MODE
#define NPH_SURFACE_DEBUG_MODE(view) (view).v.debug_mode
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

// What of a positional light's intensity reaches a surface facing it, from
// its direction `l` (unit) and the squared distance `d2` (at least 1e-8) and
// its reciprocal square root `inv_d` that gave it: the illuminance over the
// light's colour.
float nph_light_attenuation(NphRenderLightData light, vec3 l, float d2, float inv_d)
{
    // Inverse-square falloff, windowed to reach zero at the range (Karis
    // 2013); a range of 0 is unlimited. 1 / max(d2, 1e-4), from the
    // direction's reciprocal square root.
    float attenuation = min(inv_d * inv_d, 1e4);
    if (light.position.w > 0.0) {
        const float x = d2 * light.position.w;
        const float window = clamp(1.0 - x * x, 0.0, 1.0);
        attenuation *= window * window;
    }
    if (uint(light.direction.w) == NPH_RENDER_LIGHT_SPOT) {
        // KHR_lights_punctual: a smooth step between the cone angles.
        const float cd = dot(light.direction.xyz, -l);
        const float s = clamp(cd * light.spot.x + light.spot.y, 0.0, 1.0);
        attenuation *= s * s;
    }
    return attenuation;
}

// The illuminance a punctual light delivers at `p` on a surface facing it,
// and the direction towards the light.
vec3 nph_light_illuminance(NphRenderLightData light, vec3 p, out vec3 l)
{
    if (uint(light.direction.w) == NPH_RENDER_LIGHT_DIRECTIONAL) {
        l = -light.direction.xyz;
        return light.color.rgb;
    }
    const vec3 to_light = light.position.xyz - p;
    const float d2 = max(dot(to_light, to_light), 1e-8);
    const float inv_d = inversesqrt(d2);
    l = to_light * inv_d;
    return light.color.rgb * nph_light_attenuation(light, l, d2, inv_d);
}

// What the lights need to know about a shaded point: besides the point, the
// terms of its BRDF that no light changes, computed once here rather than
// once per light in the loop (the same arithmetic either way).
struct NphLitPoint {
    vec3 position;
    vec3 n;
    vec3 v;
    float nov;     // N.V, at least 1e-4
    float nov_raw; // N.V as it is
    float a2;      // GGX alpha squared
    float smith_v; // the height-correlated Smith visibility's view half, sqrt(nov^2 (1 - a2) + a2)
    vec3 diffuse;  // the Lambertian lobe: the diffuse colour over pi
    // The specular lobe's Fresnel and multiple-scattering compensation
    // together: energy (F0 + (1 - F0) x^5) = spec_base + spec_grazing x^5,
    // energy 1 + F0 (1 / Ess - 1) (Schlick's two terms each scaled once).
    vec3 spec_base;    // energy F0
    vec3 spec_grazing; // energy (1 - F0)
    vec3 face_normal; // the surface's geometric normal, either way round
#ifdef NPH_SURFACE_VSM
    float sun_visibility; // NPH_SURFACE_VSM's, or 1 when it is not active
#endif
#ifdef NPH_SURFACE_VOXEL
    // The visible lobe's axis and Hill's fit's terms (nph_voxel_visible).
    vec3 lobe_axis;
    float lobe_scale;
    float lobe_bias;
    float lobe_x;
#endif
};

#ifdef NPH_SURFACE_VOXEL
// The diffuse response of a voxel's lobe toward the light in unit direction
// `l` (render/voxel.h nph_render_voxel_diffuse): the mean clamped cosine
// toward it over the normals the viewer sees, by Hill's fit (Pettineo, "SG
// Series Part 3", 2016), which is (n.l)+ for a lobe of one normal.
float nph_voxel_diffuse(NphLitPoint p, vec3 l)
{
    const float c0 = 0.36;
    const float c1 = 1.0 / (4.0 * c0);
    const float mu = dot(p.lobe_axis, l);
    const float x0 = c0 * mu;
    const float x1 = c1 * p.lobe_x;
    float y = clamp(mu, 0.0, 1.0);
    if (abs(x0) <= x1) {
        y = (x0 + x1) * (x0 + x1) / p.lobe_x;
    }
    return max(p.lobe_scale * y + p.lobe_bias, 0.0);
}

// nph_lit_reflect for a voxel: the diffuse lobe takes the lobe's response
// `response` in place of N.L, and the specular lobe (GGX of the spread's
// alpha) reflects only above the axis's horizon.
vec3 nph_lit_reflect_voxel(NphLitPoint p, vec3 l, float nol, float response, vec3 e)
{
    vec3 spec = vec3(0.0);
    if (nol > 0.0) {
        const float vol = dot(p.v, l);
        const float inv_h = inversesqrt(max(2.0 + 2.0 * vol, 1e-12));
        const float noh = clamp((p.nov_raw + nol) * inv_h, 0.0, 1.0);
        const float voh = clamp((1.0 + vol) * inv_h, 0.0, 1.0);
        const float a2 = p.a2;
        const float d = (noh * a2 - noh) * noh + 1.0;
        const float gv = nol * p.smith_v;
        const float gl = p.nov * sqrt(nol * nol * (1.0 - a2) + a2);
        const float dv = 0.5 * a2 / (NPH_PI * d * d * max(gv + gl, 1e-8));
        const float x = 1.0 - voh;
        const float x2 = x * x;
        spec = dv * (p.spec_base + p.spec_grazing * (x2 * x2 * x)) * nol;
    }
    return (p.diffuse * response + spec) * e;
}
#endif

// The radiance the point reflects of light arriving from unit direction `l`
// at N.L `nol` (above 0) with illuminance `e`, before exposure. The terms of
// Karis 2013 rearranged: the GGX distribution and the Smith visibility share
// one division, Schlick's fifth power is three multiplies, and the half
// vector's length is |v + l| = sqrt(2 + 2 v.l) for unit v and l, so N.H and
// V.H need no vector: one reciprocal square root, one square root and one
// division a light where the textbook order takes six transcendentals. The
// algebra is exact; the rounding moves results by a few ulps.
vec3 nph_lit_reflect(NphLitPoint p, vec3 l, float nol, vec3 e)
{
    const float vol = dot(p.v, l);
    const float inv_h = inversesqrt(max(2.0 + 2.0 * vol, 1e-12));
    const float noh = clamp((p.nov_raw + nol) * inv_h, 0.0, 1.0);
    const float voh = clamp((1.0 + vol) * inv_h, 0.0, 1.0);
    const float a2 = p.a2;
    const float d = (noh * a2 - noh) * noh + 1.0;
    const float gv = nol * p.smith_v;
    const float gl = p.nov * sqrt(nol * nol * (1.0 - a2) + a2);
    const float dv = 0.5 * a2 / (NPH_PI * d * d * max(gv + gl, 1e-8));
    const float x = 1.0 - voh;
    const float x2 = x * x;
    const vec3 spec = dv * (p.spec_base + p.spec_grazing * (x2 * x2 * x));
    return (p.diffuse + spec) * e * nol;
}

// The radiance one punctual light reflects from the point, before exposure.
vec3 nph_lit_light(NphLitPoint p, NphRenderLightData light)
{
    vec3 l;
    vec3 e = nph_light_illuminance(light, p.position, l);
    const float nol = dot(p.n, l);
#ifdef NPH_SURFACE_VOXEL
    const float response = nph_voxel_diffuse(p, l);
    if ((nol <= 0.0 && response <= 0.0) || (e.r == 0.0 && e.g == 0.0 && e.b == 0.0)) {
        return vec3(0.0);
    }
#else
    if (nol <= 0.0 || (e.r == 0.0 && e.g == 0.0 && e.b == 0.0)) {
        return vec3(0.0);
    }
#endif
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
#ifdef NPH_SURFACE_VOXEL
    return nph_lit_reflect_voxel(p, l, nol, response, e);
#else
    return nph_lit_reflect(p, l, nol, e);
#endif
}

// nph_lit_light for the local light `light` (a reference to its record, so
// its fields load at fixed offsets from one address) whose vector from the
// point the caller has (with its squared length): N.L is tested from the
// position alone, before the rest of the record loads, and a light its
// window or cone leaves dark at the point returns before the BRDF (the same
// early returns as nph_lit_light's, the second tested on the attenuation
// instead of the illuminance's three channels: a light of colour zero,
// which it lets through, reflects zero all the same).
vec3 nph_lit_local(NphLitPoint p, LightRef light, vec3 to_light, float d2_raw)
{
    const float d2 = max(d2_raw, 1e-8);
    const float inv_d = inversesqrt(d2);
    const vec3 l = to_light * inv_d;
    const float nol = dot(p.n, l);
#ifdef NPH_SURFACE_VOXEL
    const float response = nph_voxel_diffuse(p, l);
    if (nol <= 0.0 && response <= 0.0) {
        return vec3(0.0);
    }
#else
    if (nol <= 0.0) {
        return vec3(0.0);
    }
#endif
    const NphRenderLightData record = light.l;
    const float attenuation = nph_light_attenuation(record, l, d2, inv_d);
    if (attenuation == 0.0) {
        return vec3(0.0);
    }
    vec3 e = record.color.rgb * attenuation;
#ifdef NPH_SURFACE_SHADOW
    if (record.color.w > 0.0) {
        e *= NPH_SURFACE_SHADOW(p, l, length(record.position.xyz - p.position));
        if (e.r == 0.0 && e.g == 0.0 && e.b == 0.0) {
            return vec3(0.0);
        }
    }
#endif
#ifdef NPH_SURFACE_VOXEL
    return nph_lit_reflect_voxel(p, l, nol, response, e);
#else
    return nph_lit_reflect(p, l, nol, e);
#endif
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
    WordsRef tiles = WordsRef(NPH_SURFACE_GRID_TILES(g));
    if (first <= last) {
        for (uint w = first >> 5u; w <= (last >> 5u); ++w) {
            uint bits = tiles.w[w * g.tile_count + at] & nph_light_bits(w, first, last);
            while (bits != 0u) {
                const uint b = findLSB(bits);
                bits &= bits - 1u;
                // Some of the lights a pixel's bin and tile hand it do not
                // reach it (Bistro's street: 5.7 a pixel, 5.0 in range): the
                // range test first, from the position alone (w: 1 / range^2).
                LightRef light = LightRef(g.locals)[w * 32u + b];
                const vec4 at_light = light.l.position;
                const vec3 to_light = at_light.xyz - p.position;
                const float d2 = dot(to_light, to_light);
                if (d2 * at_light.w < 1.0) {
                    direct += nph_lit_local(p, light, to_light, d2);
                }
            }
        }
    }
    return direct;
}

#ifdef NPH_SURFACE_MANY_ACTIVE
// The many lights' near fields at the point (docs/render.md "Many lights"): the lights
// its world cell lists, each by the share of its light within its near range (a
// range's window, nph_light_attenuation), without shadows.
vec3 nph_lit_many(NphLitPoint p, ManyViewRef view)
{
    const NphRenderManyView m = view.m;
    const vec3 x = p.position + m.to_origin.xyz;
    const uvec3 c = uvec3(ivec3(floor(x * m.to_origin.w)));
    const uint entry = WordsRef(m.table).w[NPH_RENDER_MANY_HASH(c.x, c.y, c.z) & m.table_mask];
    const uint first = entry & uint(NPH_RENDER_MANY_FIRST_MASK);
    const uint end = first + (entry >> uint(NPH_RENDER_MANY_COUNT_SHIFT));
    WordsRef indices = WordsRef(m.indices);
    vec3 direct = vec3(0.0);
    for (uint i = first; i < end; ++i) {
        ManyLightRef light = ManyLightRef(m.records)[indices.w[i]];
        const vec4 at = light.l.position;
        const vec3 to_light = at.xyz - x;
        const float d2_raw = dot(to_light, to_light);
        const float k = d2_raw * at.w;
        if (k < 1.0) {
            const float d2 = max(d2_raw, 1e-8);
            const float inv_d = inversesqrt(d2);
            const vec3 l = to_light * inv_d;
            const float nol = dot(p.n, l);
            if (nol > 0.0) {
                const vec4 color = light.l.color;
                const float near_window = 1.0 - k * k;
                const float y = d2 * color.w;
                const float range_window = clamp(1.0 - y * y, 0.0, 1.0);
                const float attenuation = min(inv_d * inv_d, 1e4) * (near_window * near_window) *
                                          (range_window * range_window);
                direct += nph_lit_reflect(p, l, nol, color.rgb * attenuation);
            }
        }
    }
    return direct;
}
#endif

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
        vec3 radiance =
            ibl ? textureLod(samplerCube(nph_textures_cube[nonuniformEXT(view.v.specular_index)],
                                         nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                             r, roughness * view.v.specular_max_lod)
                          .rgb *
                      view.v.ibl_intensity
                : vec3(0.0);
#ifdef NPH_SURFACE_MANY_ACTIVE
        // The many lights' far fields have no highlights of their own: what the cell
        // gathered, as if arriving evenly from everywhere, gives the specular its
        // energy, which the diffuse below gives up.
        if (NPH_SURFACE_MANY_ACTIVE && gi) {
            const vec4 far_cell = NPH_SURFACE_GI;
            radiance += far_cell.rgb * (NPH_INV_PI * far_cell.w);
        }
#endif
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
    switch (NPH_SURFACE_DEBUG_MODE(view)) {
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
    lp.nov_raw = dot(n, v);
    lp.a2 = a * a;
    lp.smith_v = sqrt(nov * nov * (1.0 - lp.a2) + lp.a2);
    lp.diffuse = diffuse_color * NPH_INV_PI;
    lp.spec_base = energy * f0;
    lp.spec_grazing = energy - lp.spec_base;
    lp.face_normal = s.face_normal;
#ifdef NPH_SURFACE_VSM
    lp.sun_visibility = NPH_SURFACE_VSM_ACTIVE ? NPH_SURFACE_VSM(lp) : 1.0;
#endif
#ifdef NPH_SURFACE_GRID_LIGHTS
#ifdef NPH_SURFACE_MANY_ACTIVE
    vec3 direct = nph_lit_grid(lp, LightGridRef(view.v.light_grid).g, s.pixel, s.depth);
    if (NPH_SURFACE_MANY_ACTIVE) {
        direct += nph_lit_many(lp, ManyViewRef(view.v.many));
    }
#else
    const vec3 direct = nph_lit_grid(lp, LightGridRef(view.v.light_grid).g, s.pixel, s.depth);
#endif
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
    if (NPH_SURFACE_DEBUG_MODE(view) == NPH_RENDER_DEBUG_DIRECT) {
        return min(direct * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
    }
    return min((direct + rest) * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
}

#ifdef NPH_SURFACE_VOXEL
// A(kappa) = coth(kappa) - 1/kappa, the mean cosine of a vMF lobe about its
// axis (voxel.c vmf_mean_cosine): its series near 0, where the difference
// cancels in single precision.
float nph_voxel_mean_cosine(float kappa)
{
    if (kappa < 0.1) {
        const float k2 = kappa * kappa;
        return kappa * (1.0 / 3.0 - k2 * (1.0 / 45.0 - k2 * (2.0 / 945.0)));
    }
    if (kappa > 40.0) {
        return 1.0 - 1.0 / kappa;
    }
    return 1.0 / tanh(kappa) - 1.0 / kappa;
}

// The diffuse irradiance at a lobe of normals of axis `n` and concentration
// `kappa`: the environment's cosine-convolved harmonics convolved with the
// lobe too, its bands 1 and 2 scaled by the means of P1 and P2 over it,
// A(kappa) and 1 - 3 A(kappa) / kappa (Han et al., "Frequency Domain Normal
// Map Filtering", SIGGRAPH 2007; render/voxel.h nph_render_voxel_sh_band1).
vec3 nph_voxel_sh_irradiance(ShRef sh, vec3 n, float kappa)
{
    const float a = nph_voxel_mean_cosine(kappa);
    const float band2 = kappa < 1e-3 ? kappa * kappa / 15.0 : 1.0 - 3.0 * a / kappa;
    float y[9];
    nph_sh9_basis(n, y);
    vec3 e = sh.c[0].rgb * y[0];
    for (int i = 1; i < 4; ++i) {
        e += sh.c[i].rgb * (y[i] * a);
    }
    for (int i = 4; i < 9; ++i) {
        e += sh.c[i].rgb * (y[i] * band2);
    }
    return max(e, vec3(0.0));
}

// The pre-exposed radiance leaving a voxel toward the camera (at the origin):
// albedo, perceptual roughness, metallic, and the material's occlusion, its
// lobe of normals of axis `axis` (unit, either way round: it is turned to the
// viewer) and concentration `kappa`, at camera-relative `position` (its rays
// and lookups start there) of geometric normal `face_normal`, seen at pixel
// centre `pixel` at reverse-Z `depth`. The surface model of
// nph_surface_shade without textures (a voxel's attributes are filtered at
// its scale) and with the lobe's integrals; `irradiance_view` gives the
// irradiance debug view's value instead.
vec3 nph_surface_shade_voxel(ViewRef view, vec3 albedo, float roughness_in, float metallic_in,
                             float occlusion, vec3 axis, float kappa, vec3 position,
                             vec3 face_normal, vec2 pixel, float depth, bool irradiance_view)
{
    const float metallic = clamp(metallic_in, 0.0, 1.0);
#ifdef NPH_SURFACE_AO
    const float screen_occlusion = NPH_SURFACE_AO;
#else
    const float screen_occlusion = 1.0;
#endif
    const vec3 v = normalize(-position);
    const vec3 n = dot(axis, v) < 0.0 ? -axis : axis;
    // GGX's alpha squared with the lobe's spread added, alpha^2 + 2 / kappa
    // (render/voxel.h nph_render_voxel_alpha2), and the perceptual roughness
    // it stands for, which the lookups take.
    const float r0 = clamp(roughness_in, NPH_MIN_ROUGHNESS, 1.0);
    const float a2 = kappa > 0.0 ? min(r0 * r0 * r0 * r0 + 2.0 / kappa, 1.0) : 1.0;
    const float roughness = sqrt(sqrt(a2));
    const float a = roughness * roughness;
    const float nov = max(dot(n, v), 1e-4);
    const vec3 f0 = mix(vec3(0.04), albedo, metallic);
    const vec3 diffuse_color = albedo * (1.0 - metallic);
    const vec2 dfg = textureLod(sampler2D(nph_textures_2d[nonuniformEXT(view.v.brdf_lut_index)],
                                          nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                                vec2(nov, roughness), 0.0).xy;
    const float ess = max(dfg.x + dfg.y, 1e-4);
    const vec3 energy = vec3(1.0) + f0 * (1.0 / ess - 1.0);

    const bool ibl = (view.v.flags & NPH_RENDER_VIEW_IBL) != 0u;
#ifdef NPH_SURFACE_GI
    const bool gi = NPH_SURFACE_GI_ACTIVE;
#else
    const bool gi = false;
#endif
    vec3 irradiance = vec3(0.0);
    if (ibl) {
        irradiance = nph_voxel_sh_irradiance(ShRef(view.v.sh_address), n, kappa) *
                     view.v.ibl_intensity;
    }
#ifdef NPH_SURFACE_GI
    if (gi) {
        const vec4 cell = NPH_SURFACE_GI;
        irradiance = mix(irradiance, cell.rgb, cell.w);
    }
#endif
    if (irradiance_view) {
        return min(irradiance * (NPH_INV_PI * view.v.exposure), vec3(NPH_RENDER_HALF_MAX));
    }
    vec3 indirect = vec3(0.0);
    if (ibl || gi) {
        const vec3 r = reflect(-v, n);
        const vec3 radiance =
            ibl ? textureLod(samplerCube(nph_textures_cube[nonuniformEXT(view.v.specular_index)],
                                         nph_samplers[nonuniformEXT(view.v.clamp_sampler)]),
                             r, roughness * view.v.specular_max_lod)
                          .rgb *
                      view.v.ibl_intensity
                : vec3(0.0);
        const vec3 fr = max(vec3(1.0 - roughness), f0) - f0;
        const vec3 k_s = f0 + fr * pow(1.0 - nov, 5.0);
        const vec3 fss_ess = k_s * dfg.x + dfg.y;
        const float ems = 1.0 - (dfg.x + dfg.y);
        const vec3 f_avg = f0 + (vec3(1.0) - f0) / 21.0;
        const vec3 fms_ems = fss_ess * f_avg / (vec3(1.0) - ems * f_avg) * ems;
        const vec3 k_d = diffuse_color * (vec3(1.0) - fss_ess - fms_ems);
        const float ao = occlusion * screen_occlusion;
        const float spec_occlusion = clamp(pow(nov + ao, exp2(-16.0 * a - 1.0)) - 1.0 + ao, 0.0, 1.0);
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

    switch (NPH_SURFACE_DEBUG_MODE(view)) {
    case NPH_RENDER_DEBUG_BASE_COLOR:
        return min(albedo, vec3(NPH_RENDER_HALF_MAX));
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

    NphLitPoint lp;
    lp.position = position;
    lp.n = n;
    lp.v = v;
    lp.nov = nov;
    lp.nov_raw = dot(n, v);
    lp.a2 = a2;
    lp.smith_v = sqrt(nov * nov * (1.0 - a2) + a2);
    lp.diffuse = diffuse_color * NPH_INV_PI;
    lp.spec_base = energy * f0;
    lp.spec_grazing = energy - lp.spec_base;
    lp.face_normal = face_normal;
    // The visible lobe: the vMF lobe times the viewer's cosine as a spherical
    // Gaussian of sharpness 2.8 (render/voxel.h
    // NPH_RENDER_VOXEL_VIEW_SHARPNESS), itself a lobe (Wang et al. 2009), and
    // the terms of Hill's fit for its sharpness.
    const vec3 rl = n * max(kappa, 0.0) + v * 2.8;
    const float lambda = length(rl);
    lp.lobe_axis = rl / lambda;
    const float eml = exp(-lambda);
    const float em2l = eml * eml;
    const float inv_l = 1.0 / lambda;
    lp.lobe_scale = 1.0 + 2.0 * em2l - inv_l;
    lp.lobe_bias = (eml - em2l) * inv_l - em2l;
    lp.lobe_x = sqrt(max(inv_l - 2.0 * em2l, 1e-12)); // sqrt(1 - scale), without the cancellation
#ifdef NPH_SURFACE_VSM
    lp.sun_visibility = NPH_SURFACE_VSM_ACTIVE ? NPH_SURFACE_VSM(lp) : 1.0;
#endif
#ifdef NPH_SURFACE_GRID_LIGHTS
    const vec3 direct = nph_lit_grid(lp, LightGridRef(view.v.light_grid).g, pixel, depth);
#else
    vec3 direct = vec3(0.0);
    if (view.v.light_grid != 0ul) {
        direct = nph_lit_grid(lp, LightGridRef(view.v.light_grid).g, pixel, depth);
    } else {
        LightsRef lights = LightsRef(view.v.lights_address);
        for (uint i = 0u; i < view.v.light_count; ++i) {
            direct += nph_lit_light(lp, lights.l[i]);
        }
    }
#endif
    if (NPH_SURFACE_DEBUG_MODE(view) == NPH_RENDER_DEBUG_DIRECT) {
        return min(direct * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
    }
    return min((direct + indirect) * view.v.exposure, vec3(NPH_RENDER_HALF_MAX));
}
#endif

#endif
