// SPDX-License-Identifier: Apache-2.0
// Tonemapping (docs/render.md "Tonemapping"): maps pre-exposed HDR colour
// to the display. Shared by the tonemap pass (tonemap.frag), the
// production path's temporal resolve that ends in the display colour
// (taa.frag), and its microbenchmark. The curves have C twins in
// engine/render/src/reference.c (nph_render_ref_tonemap), which the render
// tests compare texel by texel.
#ifndef NPH_TONEMAP_GLSL
#define NPH_TONEMAP_GLSL

#include "render_common.glsl"

// Stephen Hill's fit of the ACES reference rendering transform and sRGB
// output transform ("BakingLab", 2016, after the Academy's ACES 1.0):
// sRGB -> AP1 with the RRT saturation, a rational curve, AP1 -> sRGB with
// the ODT saturation. Matrices as rows.
vec3 tonemap_aces(vec3 c)
{
    const vec3 i0 = vec3(0.59719, 0.35458, 0.04823);
    const vec3 i1 = vec3(0.07600, 0.90834, 0.01566);
    const vec3 i2 = vec3(0.02840, 0.13383, 0.83777);
    const vec3 o0 = vec3(1.60475, -0.53108, -0.07367);
    const vec3 o1 = vec3(-0.10208, 1.10813, -0.00605);
    const vec3 o2 = vec3(-0.00327, -0.07276, 1.07602);
    const vec3 v = vec3(dot(i0, c), dot(i1, c), dot(i2, c));
    const vec3 a = v * (v + 0.0245786) - 0.000090537;
    const vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    const vec3 f = a / b;
    return clamp(vec3(dot(o0, f), dot(o1, f), dot(o2, f)), 0.0, 1.0);
}

// AgX (Troy Sobotka, 2022, as adopted by Blender 4.0) with its base
// contrast look, using the inset/outset matrices of Blender's
// configuration and the sixth-order polynomial fit of the sigmoid by
// Benjamin Wrensch ("Minimal AgX Implementation", 2023). Matrices as rows.
vec3 tonemap_agx(vec3 c)
{
    const vec3 i0 = vec3(0.842479062253094, 0.0784335999999992, 0.0792237451477643);
    const vec3 i1 = vec3(0.0423282422610123, 0.878468636469772, 0.0791661274605434);
    const vec3 i2 = vec3(0.0423756549057051, 0.0784336, 0.879142973793104);
    const vec3 o0 = vec3(1.19687900512017, -0.0980208811401368, -0.0990297440797205);
    const vec3 o1 = vec3(-0.0528968517574562, 1.15190312990417, -0.0989611768448433);
    const vec3 o2 = vec3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    vec3 v = vec3(dot(i0, c), dot(i1, c), dot(i2, c));
    v = clamp(log2(max(v, vec3(1e-10))), min_ev, max_ev);
    v = (v - min_ev) / (max_ev - min_ev);
    const vec3 x2 = v * v;
    const vec3 x4 = x2 * x2;
    v = 15.5 * x4 * x2 - 40.14 * x4 * v + 31.96 * x4 - 6.868 * x2 * v + 0.4298 * x2 + 0.1191 * v -
        0.00232;
    v = vec3(dot(o0, v), dot(o1, v), dot(o2, v));
    // The curve's output is display encoded (a 2.2 power); return linear
    // light, which the target's sRGB encoding then re-encodes.
    return pow(clamp(v, 0.0, 1.0), vec3(2.2));
}

// The display colour of pre-exposed HDR colour `hdr` with tonemapper
// NPH_RENDER_TONEMAP_*, sRGB encoded when `encode_srgb` is set (a UNORM
// target; an sRGB target encodes on store).
vec3 nph_tonemap(vec3 hdr, uint tonemapper, bool encode_srgb)
{
    vec3 ldr;
    if (tonemapper == NPH_RENDER_TONEMAP_ACES) {
        ldr = tonemap_aces(hdr);
    } else if (tonemapper == NPH_RENDER_TONEMAP_AGX) {
        ldr = tonemap_agx(hdr);
    } else {
        ldr = clamp(hdr, 0.0, 1.0);
    }
    return encode_srgb ? nph_linear_to_srgb(ldr) : ldr;
}

#endif
