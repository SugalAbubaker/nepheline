// SPDX-License-Identifier: Apache-2.0
// Shared by the renderer's shaders (engine/render; docs/render.md): buffer
// references to the records of nph_render.h, vertex decoding, colour
// encodings, the BRDF terms, sampling sequences, and cube-face mapping.
// Every function here has a C twin in engine/render/src/reference.c that
// the render tests compare against what the GPU computes.
#ifndef NPH_RENDER_COMMON_GLSL
#define NPH_RENDER_COMMON_GLSL

#include "nph_bindless.glsl"
#include "nph_render.h"

#define NPH_PI     3.14159265358979323846
#define NPH_INV_PI 0.31830988618379067154

// The smallest perceptual roughness shaded: below it GGX's peak overflows
// half floats and specular aliasing explodes (Guy & Agopian, "Physically
// Based Rendering in Filament", 2018, §4.8.3.3).
#define NPH_MIN_ROUGHNESS 0.045

NPH_BUFFER_REF(ViewRef) { NphRenderViewData v; };
NPH_BUFFER_REF(DrawsRef) { NphRenderDrawData d[]; };
NPH_BUFFER_REF(MaterialsRef) { NphRenderMaterialData m[]; };
NPH_BUFFER_REF(LightsRef) { NphRenderLightData l[]; };
// One light's record: LightRef(lights)[i] is the reference to lights[i]
// (GL_EXT_buffer_reference2), whose fields then load at fixed offsets from
// one address, where indexing LightsRef recomputes the address for each.
NPH_BUFFER_REF(LightRef) { NphRenderLightData l; };
NPH_BUFFER_REF(ShRef) { vec4 c[9]; };
NPH_BUFFER_REF(PositionsRef) { uvec2 p[]; };
NPH_BUFFER_REF(AttribsRef) { uvec4 a[]; };
NPH_BUFFER_REF(DebugRef) { NphRenderDebugVertex v[]; };
NPH_BUFFER_REF(SkyRef) { NphRenderSkyData s; };
NPH_BUFFER_REF(LightGridRef) { NphRenderLightGrid g; };
NPH_BUFFER_REF(WordsRef) { uint w[]; };

// Interleaved gradient noise in [0, 1) at a pixel (Jimenez, "Next
// Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH
// 2014): cheap, and without the low frequencies that read as blotches.
float nph_gradient_noise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The colour rounded to B10G11R11's grid (mantissas of 6 bits for red and
// green, 5 for blue) up or down at random, with the odds that keep its
// expected value: the format's coarse steps then show as fine noise
// rather than bands (Gjøl & Svendsen, "The Rendering of INSIDE", GDC 2016).
// The result is exactly representable, so the device's own conversion,
// which Vulkan lets round either to nearest or toward zero (the RTX 3060
// Ti truncates), never applies. `u` is noise in [0, 1).
vec3 nph_dither_b10g11r11(vec3 c, float u)
{
    // The grid's spacing, 2^(exponent - mantissa bits), and its reciprocal
    // as exact powers of two built from the exponent's bits (a reciprocal
    // power of two has the biased exponent 254 - e).
    const uvec3 e = (floatBitsToUint(max(c, vec3(1e-20))) & 0x7F800000u) -
                    (uvec3(6u, 6u, 5u) << 23u);
    const vec3 spacing = uintBitsToFloat(e);
    const vec3 inverse = uintBitsToFloat((254u << 23u) - e);
    return floor(c * inverse + u) * spacing;
}

// The distance bin of view distance `z` in a light grid
// (NphRenderLightGrid): bins evenly spaced in log2 from the near plane,
// the last one open-ended. C twin: nph_render_ref_zbin.
uint nph_zbin(float z, float znear, float scale)
{
    return uint(clamp(log2(z / znear) * scale, 0.0, float(NPH_RENDER_ZBINS - 1)));
}

// ---------------------------------------------------------------------------
// Vertex decoding (engine/render primitives.h; the cooker's mesh streams)
// ---------------------------------------------------------------------------

// The inverse of math/pack.c nph_oct_encode (Cigolle et al., "A Survey of
// Efficient Representations for Independent Unit Vectors", JCGT 2014).
vec3 nph_oct_decode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        const float x = (1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        const float y = (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = vec2(x, y);
    }
    return normalize(n);
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

// The sRGB transfer functions (IEC 61966-2-1).
float nph_srgb_to_linear1(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
vec3 nph_srgb_to_linear(vec3 c)
{
    return vec3(nph_srgb_to_linear1(c.r), nph_srgb_to_linear1(c.g), nph_srgb_to_linear1(c.b));
}
float nph_linear_to_srgb1(float c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
vec3 nph_linear_to_srgb(vec3 c)
{
    return vec3(nph_linear_to_srgb1(c.r), nph_linear_to_srgb1(c.g), nph_linear_to_srgb1(c.b));
}

// ---------------------------------------------------------------------------
// The BRDF (Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013;
// Burley, "Physically-Based Shading at Disney", 2012; Heitz,
// "Understanding the Masking-Shadowing Function in Microfacet-Based
// BRDFs", JCGT 2014). `a` is GGX alpha = perceptual roughness squared.
// ---------------------------------------------------------------------------

// The GGX / Trowbridge-Reitz normal distribution.
float nph_d_ggx(float noh, float a)
{
    const float a2 = a * a;
    const float d = (noh * a2 - noh) * noh + 1.0;
    return a2 / (NPH_PI * d * d);
}

// Smith's height-correlated masking-shadowing for GGX, folded with the
// 1 / (4 NoV NoL) of the microfacet BRDF: V = G2 / (4 NoV NoL) (Heitz 2014,
// eq. 99; the form of Lagarde & de Rousiers, "Moving Frostbite to PBR",
// SIGGRAPH 2014).
float nph_v_smith_ggx_correlated(float nov, float nol, float a)
{
    const float a2 = a * a;
    const float gv = nol * sqrt(nov * nov * (1.0 - a2) + a2);
    const float gl = nov * sqrt(nol * nol * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-8);
}

// Schlick's Fresnel approximation.
vec3 nph_f_schlick(float voh, vec3 f0)
{
    const float f = pow(1.0 - voh, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

// The Hammersley point set: i / n and the base-2 radical inverse of i
// (Dammertz, "Hammersley Points on the Hemisphere", 2012).
vec2 nph_hammersley(uint i, uint n)
{
    return vec2(float(i) / float(n), float(bitfieldReverse(i)) * 2.3283064365386963e-10);
}

// A GGX-distributed half vector around +Z for the sample `xi`: the
// importance sampling of Karis 2013 (pdf(h) = D(h) NoH).
vec3 nph_importance_sample_ggx(vec2 xi, float a)
{
    const float phi = 2.0 * NPH_PI * xi.x;
    // For a near-mirror lobe the ratio rounds to slightly above 1 in single
    // precision; unclamped, the sine below would be NaN.
    const float cos_theta = min(sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y)), 1.0);
    const float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
    return vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
}

// An orthonormal basis around a unit vector (Duff et al., "Building an
// Orthonormal Basis, Revisited", JCGT 2017).
void nph_basis(vec3 n, out vec3 t, out vec3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// ---------------------------------------------------------------------------
// Cube maps (Vulkan spec, "Cube Map Face Selection"): face order +X -X +Y
// -Y +Z -Z; `st` in [0, 1] across the face, t down.
// ---------------------------------------------------------------------------

vec3 nph_cube_direction(uint face, vec2 st)
{
    const float u = 2.0 * st.x - 1.0;
    const float v = 2.0 * st.y - 1.0;
    vec3 d;
    if (face == 0u) {
        d = vec3(1.0, -v, -u);
    } else if (face == 1u) {
        d = vec3(-1.0, -v, u);
    } else if (face == 2u) {
        d = vec3(u, 1.0, v);
    } else if (face == 3u) {
        d = vec3(u, -1.0, -v);
    } else if (face == 4u) {
        d = vec3(u, -v, 1.0);
    } else {
        d = vec3(-u, -v, -1.0);
    }
    return normalize(d);
}

// The solid angle of a cube-face texel at face coordinates `uv` in [-1, 1]
// with half-size `h` (texel spacing 2h): the integral of the projected
// area element 1 / (1 + u^2 + v^2)^1.5 over the texel (Öhrström; the
// arctangent form of Driscoll's cube-map texel solid angle).
float nph_cube_area_element(float x, float y)
{
    return atan(x * y, sqrt(x * x + y * y + 1.0));
}
float nph_cube_texel_solid_angle(vec2 uv, float h)
{
    const float x0 = uv.x - h;
    const float y0 = uv.y - h;
    const float x1 = uv.x + h;
    const float y1 = uv.y + h;
    return nph_cube_area_element(x0, y0) - nph_cube_area_element(x0, y1) -
           nph_cube_area_element(x1, y0) + nph_cube_area_element(x1, y1);
}

// ---------------------------------------------------------------------------
// Spherical harmonics: the real SH basis to band 2, the constants of
// Ramamoorthi & Hanrahan, "An Efficient Representation for Irradiance
// Environment Maps", SIGGRAPH 2001.
// ---------------------------------------------------------------------------

void nph_sh9_basis(vec3 d, out float y[9])
{
    y[0] = 0.282095;
    y[1] = 0.488603 * d.y;
    y[2] = 0.488603 * d.z;
    y[3] = 0.488603 * d.x;
    y[4] = 1.092548 * d.x * d.y;
    y[5] = 1.092548 * d.y * d.z;
    y[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    y[7] = 1.092548 * d.x * d.z;
    y[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

// Irradiance in direction `n` from cosine-convolved coefficients.
vec3 nph_sh9_irradiance(ShRef sh, vec3 n)
{
    float y[9];
    nph_sh9_basis(n, y);
    vec3 e = vec3(0.0);
    for (int i = 0; i < 9; ++i) {
        e += sh.c[i].rgb * y[i];
    }
    return max(e, vec3(0.0));
}

// ---------------------------------------------------------------------------
// The procedural sky (NphRenderSkyData; render/reference.h nph_render_ref_sky)
// ---------------------------------------------------------------------------

vec3 nph_sky_radiance(NphRenderSkyData s, vec3 d)
{
    const float h = d.y;
    if (h >= 0.0) {
        return mix(s.horizon.rgb, s.zenith.rgb, pow(h, s.params.x));
    }
    return mix(s.horizon.rgb, s.ground.rgb, clamp(-h * s.params.y, 0.0, 1.0));
}

#endif
