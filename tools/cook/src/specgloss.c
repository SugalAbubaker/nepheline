// SPDX-License-Identifier: Apache-2.0
// Specular-glossiness materials to metallic-roughness (cook/specgloss.h).
#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>

#include <cook/specgloss.h>
#include <string.h>

// A dielectric's reflectance at normal incidence, as metallic-roughness
// fixes it, and the conversion's guard against dividing by zero.
#define DIELECTRIC 0.04F
#define EPSILON    1e-6F

static f32 perceived_brightness(f32 r, f32 g, f32 b)
{
    return nph_f32_sqrt(0.299F * r * r + 0.587F * g * g + 0.114F * b * b);
}

// The metallic share whose blend of a dielectric and a metal gives these
// brightnesses: the positive root of
// a m^2 + b m + c = 0 (a = 0.04, b = d s / 0.96 + p - 0.08, c = 0.04 - p, for
// the diffuse brightness d, the specular brightness p, and s one minus the
// specular colour's largest channel), 0 below a dielectric's specular.
static f32 solve_metallic(f32 diffuse, f32 specular, f32 one_minus_specular_strength)
{
    if (specular < DIELECTRIC) {
        return 0.0F;
    }
    const f32 a = DIELECTRIC;
    const f32 b =
        diffuse * one_minus_specular_strength / (1.0F - DIELECTRIC) + specular - 2.0F * DIELECTRIC;
    const f32 c = DIELECTRIC - specular;
    const f32 d = b * b - 4.0F * a * c; // c <= 0, so d >= b^2
    return nph_f32_saturate((-b + nph_f32_sqrt(nph_f32_max(d, 0.0F))) / (2.0F * a));
}

CookMetalRough cook_specgloss_to_metalrough(CookSpecGloss sg)
{
    const f32 diffuse[4] = {nph_f32_saturate(sg.diffuse.x), nph_f32_saturate(sg.diffuse.y),
                            nph_f32_saturate(sg.diffuse.z), nph_f32_saturate(sg.diffuse.w)};
    const f32 specular[3] = {nph_f32_saturate(sg.specular.x), nph_f32_saturate(sg.specular.y),
                             nph_f32_saturate(sg.specular.z)};
    const f32 strength = nph_f32_max(specular[0], nph_f32_max(specular[1], specular[2]));
    const f32 one_minus_strength = 1.0F - strength;
    const f32 metallic = solve_metallic(perceived_brightness(diffuse[0], diffuse[1], diffuse[2]),
                                        perceived_brightness(specular[0], specular[1], specular[2]),
                                        one_minus_strength);
    // The base colour as the diffuse reads it (what is left of the diffuse
    // light once the specular took its share, over a dielectric's) and as
    // the specular reads it (a metal's specular is its base colour), blended
    // by the metallic share squared.
    const f32 from_diffuse =
        one_minus_strength / (1.0F - DIELECTRIC) / nph_f32_max(1.0F - metallic, EPSILON);
    const f32 from_specular = 1.0F / nph_f32_max(metallic, EPSILON);
    const f32 t = metallic * metallic;
    f32 base[3];
    for (u32 ch = 0; ch < 3U; ++ch) {
        const f32 d = diffuse[ch] * from_diffuse;
        const f32 s = (specular[ch] - DIELECTRIC * (1.0F - metallic)) * from_specular;
        base[ch] = nph_f32_saturate(d * (1.0F - t) + s * t);
    }
    CookMetalRough out;
    out.base_color = nph_vec4(base[0], base[1], base[2], diffuse[3]);
    out.metallic = metallic;
    out.roughness = 1.0F - nph_f32_saturate(sg.glossiness);
    return out;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static u8 unorm8(f32 v)
{
    return (u8)(nph_f32_saturate(v) * 255.0F + 0.5F);
}

NphResult cook_specgloss_textures(NphAllocator *alloc, const CookImage *diffuse,
                                  const CookImage *specgloss, const CookSpecGloss *factors,
                                  CookImage *out_base, CookImage *out_mr)
{
    NPH_ASSERT(alloc != NULL && factors != NULL && out_base != NULL && out_mr != NULL);
    memset(out_base, 0, sizeof *out_base);
    memset(out_mr, 0, sizeof *out_mr);
    if ((diffuse == NULL && specgloss == NULL) ||
        (diffuse != NULL && diffuse->format != COOK_IMAGE_RGBA8) ||
        (specgloss != NULL && specgloss->format != COOK_IMAGE_RGBA8)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 w = nph_max_u32(diffuse != NULL ? diffuse->width : 1U,
                              specgloss != NULL ? specgloss->width : 1U);
    const u32 h = nph_max_u32(diffuse != NULL ? diffuse->height : 1U,
                              specgloss != NULL ? specgloss->height : 1U);
    // Both inputs in linear light (colour decoded, alpha as stored).
    CookImage d_lin;
    CookImage s_lin;
    memset(&d_lin, 0, sizeof d_lin);
    memset(&s_lin, 0, sizeof s_lin);
    NphResult r = NPH_RESULT_OK;
    if (diffuse != NULL) {
        r = cook_image_to_f32(alloc, diffuse, true, &d_lin);
    }
    if (r == NPH_RESULT_OK && specgloss != NULL) {
        r = cook_image_to_f32(alloc, specgloss, true, &s_lin);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_image_create(alloc, w, h, COOK_IMAGE_RGBA8, out_base);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_image_create(alloc, w, h, COOK_IMAGE_RGBA8, out_mr);
    }
    if (r == NPH_RESULT_OK) {
        u8 *base = (u8 *)out_base->pixels;
        u8 *mr = (u8 *)out_mr->pixels;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                f32 dt[4] = {1.0F, 1.0F, 1.0F, 1.0F};
                f32 st[4] = {1.0F, 1.0F, 1.0F, 1.0F};
                if (diffuse != NULL) {
                    cook_image_sample_grid(&d_lin, x, y, w, h, dt);
                }
                if (specgloss != NULL) {
                    cook_image_sample_grid(&s_lin, x, y, w, h, st);
                }
                CookSpecGloss sg;
                sg.diffuse = nph_vec4(dt[0] * factors->diffuse.x, dt[1] * factors->diffuse.y,
                                      dt[2] * factors->diffuse.z, dt[3] * factors->diffuse.w);
                sg.specular = nph_vec3(st[0] * factors->specular.x, st[1] * factors->specular.y,
                                       st[2] * factors->specular.z);
                sg.glossiness = st[3] * factors->glossiness;
                const CookMetalRough m = cook_specgloss_to_metalrough(sg);
                u8 *b = base + ((u64)y * w + x) * 4U;
                b[0] = unorm8(cook_linear_to_srgb(m.base_color.x));
                b[1] = unorm8(cook_linear_to_srgb(m.base_color.y));
                b[2] = unorm8(cook_linear_to_srgb(m.base_color.z));
                b[3] = unorm8(m.base_color.w);
                u8 *t = mr + ((u64)y * w + x) * 4U;
                t[0] = 255U;
                t[1] = unorm8(m.roughness);
                t[2] = unorm8(m.metallic);
                t[3] = 255U;
            }
        }
        out_base->source_channels = 4U;
        out_mr->source_channels = 3U;
    }
    cook_image_free(&d_lin);
    cook_image_free(&s_lin);
    if (r != NPH_RESULT_OK) {
        cook_image_free(out_base);
        cook_image_free(out_mr);
    }
    return r;
}
