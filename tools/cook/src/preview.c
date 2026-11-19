// SPDX-License-Identifier: Apache-2.0
// Material previews (preview.h): the studio, the images a preview reads, the sphere, and the
// cache file.
#include <nepheline/asset/format.h>
#include <nepheline/asset/texture.h>
#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/render/reference.h>

#include <cook/preview.h>
#include <cook/texture_import.h>
#include <stdio.h>
#include <string.h>

// The view's half-width in sphere radii: the sphere and a margin for its edge's filtering.
#define VIEW_RADIUS 1.04F
#define DFG_SAMPLES 128U // per entry of the DFG table, as brdf_lut.comp's quality
#define SKY_CUBE    16U  // texels a face of the cube the sky's irradiance is projected from
// The key light's highlight stays finite on a mirror (the renderer's lights clamp it too).
#define MIN_KEY_ROUGHNESS 0.05F
#define PATH_MAX_PREVIEW  4096U
#define SAMPLES           4U
// What a white diffuse sphere's brightest point shows as, linear (sRGB about 210): light, with
// room above it for highlights, and short of where AgX takes bright colours toward white.
#define WHITE_PEAK 0.65F

static f32 luminance(NphVec3 c)
{
    return 0.2126F * c.x + 0.7152F * c.y + 0.0722F * c.z;
}

// ---------------------------------------------------------------------------
// The studio
// ---------------------------------------------------------------------------

void cook_preview_studio_init(CookPreviewStudio *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    // The renderer's sky model, made a neutral studio: grey, brighter overhead, the ground a
    // little warm and dark. The default sky's blue would tint every material it shows.
    nph_render_sky_desc_default(&out->sky);
    out->sky.zenith = nph_vec3(5000.0F, 5000.0F, 5000.0F);
    out->sky.horizon = nph_vec3(3800.0F, 3800.0F, 3800.0F);
    out->sky.ground = nph_vec3(900.0F, 850.0F, 800.0F);
    nph_render_ref_sh9_sky(&out->sky, SKY_CUBE, out->sh);
    for (u32 r = 0; r < COOK_PREVIEW_DFG; ++r) {
        for (u32 n = 0; n < COOK_PREVIEW_DFG; ++n) {
            const f32 nov = ((f32)n + 0.5F) / (f32)COOK_PREVIEW_DFG;
            const f32 roughness = ((f32)r + 0.5F) / (f32)COOK_PREVIEW_DFG;
            out->dfg[r * COOK_PREVIEW_DFG + n] = nph_render_ref_dfg(nov, roughness, DFG_SAMPLES);
        }
    }
    // A white key light from above, to the left and in front, twice the sky's light on a face
    // turned up.
    out->key_direction = nph_vec3_normalize(nph_vec3(-0.45F, 0.65F, 0.6F));
    const f32 sky_up = luminance(nph_render_ref_sh9_irradiance(out->sh, nph_vec3_unit_y()));
    out->key_illuminance = nph_vec3_splat(2.0F * sky_up);
    // A white diffuse sphere at its brightest, where it faces the key, comes out of AgX at
    // WHITE_PEAK: the input that does, found by bisection on the curve (it rises), over that
    // point's radiance.
    const NphVec3 peak =
        nph_vec3_scale(nph_vec3_add(nph_render_ref_sh9_irradiance(out->sh, out->key_direction),
                                    out->key_illuminance),
                       NPH_INV_PI);
    f32 lo = 0.0F;
    f32 hi = 64.0F;
    for (u32 i = 0; i < 40U; ++i) {
        const f32 mid = 0.5F * (lo + hi);
        const f32 shown =
            luminance(nph_render_ref_tonemap(nph_vec3_splat(mid), NPH_RENDER_TONEMAPPER_AGX));
        if (shown < WHITE_PEAK) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    out->exposure = 0.5F * (lo + hi) / luminance(peak);
    // An emission of 1 at about half the white peak's input: bright, and still its colour (AgX
    // takes what nears white toward white).
    out->emission = 0.25F * (lo + hi);
}

// The split sum's terms for `nov` and `roughness`, bilinear in the table.
static NphVec2 dfg_at(const CookPreviewStudio *st, f32 nov, f32 roughness)
{
    const f32 x =
        nph_f32_clamp(nov * (f32)COOK_PREVIEW_DFG - 0.5F, 0.0F, (f32)(COOK_PREVIEW_DFG - 1U));
    const f32 y =
        nph_f32_clamp(roughness * (f32)COOK_PREVIEW_DFG - 0.5F, 0.0F, (f32)(COOK_PREVIEW_DFG - 1U));
    const u32 x0 = (u32)x;
    const u32 y0 = (u32)y;
    const u32 x1 = nph_min_u32(x0 + 1U, COOK_PREVIEW_DFG - 1U);
    const u32 y1 = nph_min_u32(y0 + 1U, COOK_PREVIEW_DFG - 1U);
    const f32 fx = x - (f32)x0;
    const f32 fy = y - (f32)y0;
    const NphVec2 a =
        nph_vec2_lerp(st->dfg[y0 * COOK_PREVIEW_DFG + x0], st->dfg[y0 * COOK_PREVIEW_DFG + x1], fx);
    const NphVec2 b =
        nph_vec2_lerp(st->dfg[y1 * COOK_PREVIEW_DFG + x0], st->dfg[y1 * COOK_PREVIEW_DFG + x1], fx);
    return nph_vec2_lerp(a, b, fy);
}

// ---------------------------------------------------------------------------
// The images
// ---------------------------------------------------------------------------

NphResult cook_preview_reduce(NphAllocator *alloc, const CookImage *src, bool srgb, u32 max_extent,
                              CookImage *out)
{
    NPH_ASSERT(alloc != NULL && src != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (max_extent == 0U || src->width == 0U || src->height == 0U || src->pixels == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 w = src->width;
    const u32 h = src->height;
    // Blocks of f x f texels, the last of a row or column cut short where the image ends.
    const u32 longest = nph_max_u32(w, h);
    const u32 f = (longest + max_extent - 1U) / max_extent;
    const u32 tw = (w + f - 1U) / f;
    const u32 th = (h + f - 1U) / f;
    NPH_TRY(cook_image_create(alloc, tw, th, COOK_IMAGE_RGBA32F, out));
    f32 decode[256];
    for (u32 i = 0; i < 256U; ++i) {
        const f32 v = (f32)i / 255.0F;
        decode[i] = srgb ? cook_srgb_to_linear(v) : v;
    }
    const bool bytes = src->format == COOK_IMAGE_RGBA8;
    f32 *dst = (f32 *)out->pixels;
    for (u32 y = 0; y < th; ++y) {
        const u32 y0 = y * f;
        const u32 y1 = nph_min_u32(y0 + f, h);
        for (u32 x = 0; x < tw; ++x) {
            const u32 x0 = x * f;
            const u32 x1 = nph_min_u32(x0 + f, w);
            f64 sum[4] = {0.0, 0.0, 0.0, 0.0};
            for (u32 sy = y0; sy < y1; ++sy) {
                const u64 row = (u64)sy * w;
                for (u32 sx = x0; sx < x1; ++sx) {
                    if (bytes) {
                        const u8 *p = (const u8 *)src->pixels + (row + sx) * 4U;
                        sum[0] += (f64)decode[p[0]];
                        sum[1] += (f64)decode[p[1]];
                        sum[2] += (f64)decode[p[2]];
                        sum[3] += (f64)p[3] / 255.0;
                    } else {
                        // Float images are linear already (HDR, BC6H).
                        const f32 *p = (const f32 *)src->pixels + (row + sx) * 4U;
                        for (u32 c = 0; c < 4U; ++c) {
                            sum[c] += (f64)p[c];
                        }
                    }
                }
            }
            const f64 n = (f64)(x1 - x0) * (f64)(y1 - y0);
            f32 *d = dst + ((u64)y * tw + x) * 4U;
            for (u32 c = 0; c < 4U; ++c) {
                d[c] = (f32)(sum[c] / n);
            }
        }
    }
    return NPH_RESULT_OK;
}

void cook_preview_images_free(CookPreviewImages *images)
{
    if (images == NULL) {
        return;
    }
    for (u32 s = 0; s < NPH_MATERIAL_TEXTURE_COUNT; ++s) {
        for (u32 l = 0; l < COOK_PREVIEW_IMAGE_LEVELS; ++l) {
            cook_image_free(&images->slots[s].levels[l]);
        }
        images->slots[s].level_count = 0U;
    }
}

// A normal map's green flipped (DirectX's convention to the renderer's).
static void flip_green(CookImage *img)
{
    f32 *p = (f32 *)img->pixels;
    const u64 n = (u64)img->width * img->height;
    for (u64 i = 0; i < n; ++i) {
        p[i * 4U + 1U] = 1.0F - p[i * 4U + 1U];
    }
}

// Slot `s`'s image from its file: reduced, then its levels. False, and the slot empty, for one
// that does not decode; the allocator's failure in `out_r`.
static bool make_slot(NphAllocator *alloc, const CookPreviewFiles *files, u32 s,
                      CookPreviewImage *img, NphResult *out_r)
{
    *out_r = NPH_RESULT_OK;
    CookImage decoded;
    NphResult r = cook_image_decode(alloc, files->image[s], files->image_size[s], &decoded);
    if (r != NPH_RESULT_OK) {
        *out_r = r == NPH_RESULT_OUT_OF_MEMORY ? r : NPH_RESULT_OK;
        return false;
    }
    CookTextureImport settings;
    cook_texture_import_default(&settings);
    if (files->settings[s] != NULL) {
        r = cook_texture_import_read(
            alloc, nph_str((const char *)files->settings[s], files->settings_size[s]), &settings);
        if (r == NPH_RESULT_OUT_OF_MEMORY) {
            cook_image_free(&decoded);
            *out_r = r;
            return false;
        }
        if (r != NPH_RESULT_OK) {
            cook_texture_import_default(&settings); // as the image holds it
        }
    }
    const bool colour =
        s == (u32)NPH_MATERIAL_TEXTURE_BASE_COLOR || s == (u32)NPH_MATERIAL_TEXTURE_EMISSIVE;
    const bool normal = s == (u32)NPH_MATERIAL_TEXTURE_NORMAL;
    r = cook_preview_reduce(alloc, &decoded, colour, COOK_PREVIEW_IMAGE_EXTENT, &img->levels[0]);
    cook_image_free(&decoded);
    if (r != NPH_RESULT_OK) {
        *out_r = r;
        return false;
    }
    img->level_count = 1U;
    if (normal) {
        if (settings.green_down) {
            flip_green(&img->levels[0]);
        }
        cook_image_renormalize_normals(&img->levels[0]);
    }
    while (img->level_count < COOK_PREVIEW_IMAGE_LEVELS) {
        const CookImage *last = &img->levels[img->level_count - 1U];
        if (last->width == 1U && last->height == 1U) {
            break;
        }
        r = cook_image_downsample(alloc, last, &img->levels[img->level_count]);
        if (r != NPH_RESULT_OK) {
            *out_r = r;
            return false;
        }
        if (normal) {
            cook_image_renormalize_normals(&img->levels[img->level_count]);
        }
        ++img->level_count;
    }
    return true;
}

NphResult cook_preview_images_make(NphAllocator *alloc, const CookPreviewFiles *files,
                                   CookPreviewImages *out)
{
    NPH_ASSERT(alloc != NULL && files != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    for (u32 s = 0; s < NPH_MATERIAL_TEXTURE_COUNT; ++s) {
        if (files->image[s] == NULL) {
            continue;
        }
        NphResult r = NPH_RESULT_OK;
        if (!make_slot(alloc, files, s, &out->slots[s], &r)) {
            for (u32 l = 0; l < COOK_PREVIEW_IMAGE_LEVELS; ++l) {
                cook_image_free(&out->slots[s].levels[l]);
            }
            out->slots[s].level_count = 0U;
        }
        if (r != NPH_RESULT_OK) {
            cook_preview_images_free(out);
            return r;
        }
    }
    return NPH_RESULT_OK;
}

void cook_preview_files_free(CookPreviewFiles *f)
{
    if (f == NULL || f->alloc == NULL) {
        return;
    }
    for (u32 s = 0; s < NPH_MATERIAL_TEXTURE_COUNT; ++s) {
        if (f->image[s] != NULL) {
            nph_file_free_all(f->alloc, f->image[s], f->image_size[s]);
        }
        if (f->settings[s] != NULL) {
            nph_file_free_all(f->alloc, f->settings[s], f->settings_size[s]);
        }
    }
    memset(f, 0, sizeof *f);
}

NphResult cook_preview_files_read(NphAllocator *alloc, const char *root,
                                  const CookMaterialSource *m, CookPreviewFiles *out)
{
    NPH_ASSERT(alloc != NULL && root != NULL && m != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    for (u32 s = 0; s < NPH_MATERIAL_TEXTURE_COUNT; ++s) {
        const char *key = m->textures[s].image;
        if (key[0] == '\0') {
            continue;
        }
        char path[PATH_MAX_PREVIEW];
        int n = snprintf(path, sizeof path, "%s/%s", root, key);
        if (n < 0 || (u64)n >= sizeof path) {
            cook_preview_files_free(out);
            return NPH_RESULT_OVERFLOW;
        }
        NphResult r = nph_file_read_all(path, alloc, &out->image[s], &out->image_size[s]);
        if (r != NPH_RESULT_OK) {
            out->image[s] = NULL;
            out->image_size[s] = 0U;
            if (r == NPH_RESULT_OUT_OF_MEMORY) {
                cook_preview_files_free(out);
                return r;
            }
            continue; // drawn without it
        }
        n = snprintf(path, sizeof path, "%s/%s.import.json", root, key);
        if (n < 0 || (u64)n >= sizeof path) {
            cook_preview_files_free(out);
            return NPH_RESULT_OVERFLOW;
        }
        r = nph_file_read_all(path, alloc, &out->settings[s], &out->settings_size[s]);
        if (r != NPH_RESULT_OK) {
            out->settings[s] = NULL;
            out->settings_size[s] = 0U;
            if (r == NPH_RESULT_OUT_OF_MEMORY) {
                cook_preview_files_free(out);
                return r;
            }
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

static i64 wrap_index(i64 i, u32 n, u32 mode)
{
    const i64 size = (i64)n;
    if (mode == (u32)NPH_MATERIAL_WRAP_CLAMP) {
        return i < 0 ? 0 : (i >= size ? size - 1 : i);
    }
    if (mode == (u32)NPH_MATERIAL_WRAP_MIRROR) {
        i64 m = i % (2 * size);
        m = m < 0 ? m + 2 * size : m;
        return m < size ? m : 2 * size - 1 - m;
    }
    i64 m = i % size;
    return m < 0 ? m + size : m;
}

static NphVec4 texel(const CookImage *img, i64 x, i64 y)
{
    const f32 *p = (const f32 *)img->pixels + ((u64)y * img->width + (u64)x) * 4U;
    return nph_vec4(p[0], p[1], p[2], p[3]);
}

// Level `level` of `img` at `uv`, bilinear or nearest, with the slot's wraps.
static NphVec4 sample_level(const CookImage *img, const CookMaterialTexture *t, NphVec2 uv)
{
    // Far texture coordinates (a large tiling) stay exact enough in texels: f64.
    const f64 x = (f64)uv.x * (f64)img->width - 0.5;
    const f64 y = (f64)uv.y * (f64)img->height - 0.5;
    if (t->nearest != 0U) {
        const i64 ix = wrap_index((i64)nph_f64_floor(x + 0.5), img->width, t->wrap_u);
        const i64 iy = wrap_index((i64)nph_f64_floor(y + 0.5), img->height, t->wrap_v);
        return texel(img, ix, iy);
    }
    const f64 fx0 = nph_f64_floor(x);
    const f64 fy0 = nph_f64_floor(y);
    const f32 fx = (f32)(x - fx0);
    const f32 fy = (f32)(y - fy0);
    const i64 x0 = wrap_index((i64)fx0, img->width, t->wrap_u);
    const i64 x1 = wrap_index((i64)fx0 + 1, img->width, t->wrap_u);
    const i64 y0 = wrap_index((i64)fy0, img->height, t->wrap_v);
    const i64 y1 = wrap_index((i64)fy0 + 1, img->height, t->wrap_v);
    const NphVec4 a = nph_vec4_lerp(texel(img, x0, y0), texel(img, x1, y0), fx);
    const NphVec4 b = nph_vec4_lerp(texel(img, x0, y1), texel(img, x1, y1), fx);
    return nph_vec4_lerp(a, b, fy);
}

// The slot's image at `uv`, at the level its footprint (the coordinates' change across a pixel,
// `dx` and `dy`) asks for: trilinear, or the nearest level of a nearest-filtered slot.
static NphVec4 sample(const CookPreviewImage *img, const CookMaterialTexture *t, NphVec2 uv,
                      NphVec2 dx, NphVec2 dy)
{
    const CookImage *top = &img->levels[0];
    const f32 w = (f32)top->width;
    const f32 h = (f32)top->height;
    const f32 rx = nph_f32_sqrt(nph_f32_square(dx.x * w) + nph_f32_square(dx.y * h));
    const f32 ry = nph_f32_sqrt(nph_f32_square(dy.x * w) + nph_f32_square(dy.y * h));
    const f32 rho = nph_f32_max(rx, ry);
    const f32 last = (f32)(img->level_count - 1U);
    const f32 lod = rho > 1.0F ? nph_f32_min(nph_f32_log2(rho), last) : 0.0F;
    if (t->nearest != 0U) {
        return sample_level(&img->levels[(u32)nph_f32_round(lod)], t, uv);
    }
    const u32 l0 = (u32)lod;
    const u32 l1 = nph_min_u32(l0 + 1U, img->level_count - 1U);
    const NphVec4 a = sample_level(&img->levels[l0], t, uv);
    if (l1 == l0) {
        return a;
    }
    return nph_vec4_lerp(a, sample_level(&img->levels[l1], t, uv), lod - (f32)l0);
}

// ---------------------------------------------------------------------------
// The sphere
// ---------------------------------------------------------------------------

typedef struct Surface {
    NphVec3 normal;
    NphVec3 base; // linear
    f32 alpha;
    f32 metallic;
    f32 roughness;
    f32 occlusion;
    NphVec3 emissive; // the tonemapper's input, by the studio's emission scale
} Surface;

// The material at sphere point `n` (unit, facing the viewer), from view-space position `p` and
// the size of a pixel there, `pixel`.
static void surface_at(const CookMaterialSource *m, const CookPreviewImages *images, NphVec3 n,
                       f32 pixel, const NphMaterialUvTransform *t, Surface *out)
{
    // Longitude and latitude, and the texture coordinates: one tile across the front.
    const f32 phi = nph_f32_atan2(n.x, n.z);
    const f32 theta = nph_f32_asin(nph_f32_clamp(n.y, -1.0F, 1.0F));
    const NphVec2 uv = nph_vec2(0.5F + phi * NPH_INV_PI, 0.5F - theta * NPH_INV_PI);
    // Their change across a pixel (the view is orthographic: x and y are the point's own).
    const f32 z = nph_f32_max(n.z, 1e-4F);
    const f32 xz = nph_f32_max(n.x * n.x + z * z, 1e-8F);
    const f32 cos_theta = nph_f32_max(nph_f32_sqrt(nph_f32_max(1.0F - n.y * n.y, 0.0F)), 1e-4F);
    const NphVec2 duv_dx = nph_vec2(z / xz * NPH_INV_PI * pixel, 0.0F);
    const NphVec2 duv_dy =
        nph_vec2(n.x * n.y / (z * xz) * NPH_INV_PI * pixel, -NPH_INV_PI / cos_theta * pixel);
    // Through the material's transform, as the renderer's coordinates and derivatives are.
    const NphVec2 tuv =
        nph_vec2(t->m00 * uv.x + t->m01 * uv.y + t->m02, t->m10 * uv.x + t->m11 * uv.y + t->m12);
    const NphVec2 tdx =
        nph_vec2(t->m00 * duv_dx.x + t->m01 * duv_dx.y, t->m10 * duv_dx.x + t->m11 * duv_dx.y);
    const NphVec2 tdy =
        nph_vec2(t->m00 * duv_dy.x + t->m01 * duv_dy.y, t->m10 * duv_dy.x + t->m11 * duv_dy.y);
    NphVec4 tex[NPH_MATERIAL_TEXTURE_COUNT];
    for (u32 s = 0; s < NPH_MATERIAL_TEXTURE_COUNT; ++s) {
        const CookPreviewImage *img = images != NULL ? &images->slots[s] : NULL;
        tex[s] = img != NULL && img->level_count != 0U ? sample(img, &m->textures[s], tuv, tdx, tdy)
                                                       : nph_vec4(1.0F, 1.0F, 1.0F, 1.0F);
    }
    const NphVec4 base = tex[NPH_MATERIAL_TEXTURE_BASE_COLOR];
    out->base =
        nph_vec3(m->base_color.x * base.x, m->base_color.y * base.y, m->base_color.z * base.z);
    out->alpha = m->base_color.w * base.w;
    const NphVec4 mr = tex[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS];
    out->roughness = nph_f32_saturate(m->roughness * mr.y);
    out->metallic = nph_f32_saturate(m->metallic * mr.z);
    out->occlusion = 1.0F + m->occlusion_strength * (tex[NPH_MATERIAL_TEXTURE_OCCLUSION].x - 1.0F);
    const NphVec4 e = tex[NPH_MATERIAL_TEXTURE_EMISSIVE];
    out->emissive =
        nph_vec3_scale(nph_vec3(m->emissive.x * e.x, m->emissive.y * e.y, m->emissive.z * e.z),
                       m->emissive_strength);
    out->normal = n;
    const CookPreviewImage *ni =
        images != NULL ? &images->slots[NPH_MATERIAL_TEXTURE_NORMAL] : NULL;
    if (ni != NULL && ni->level_count != 0U) {
        // The sphere's own frame: along its longitude, and toward its north (green up).
        const f32 sp = nph_f32_sin(phi);
        const f32 cp = nph_f32_cos(phi);
        const f32 st = nph_f32_sin(theta);
        const f32 ct = nph_f32_cos(theta);
        const NphVec3 tangent = nph_vec3(cp, 0.0F, -sp);
        const NphVec3 bitangent = nph_vec3(-st * sp, ct, -st * cp);
        const NphVec4 c = tex[NPH_MATERIAL_TEXTURE_NORMAL];
        const f32 tx = (2.0F * c.x - 1.0F) * m->normal_scale;
        const f32 ty = (2.0F * c.y - 1.0F) * m->normal_scale;
        const f32 tz = 2.0F * c.z - 1.0F;
        const NphVec3 bent =
            nph_vec3_add(nph_vec3_add(nph_vec3_scale(tangent, tx), nph_vec3_scale(bitangent, ty)),
                         nph_vec3_scale(n, tz));
        out->normal = nph_vec3_normalize_or(bent, n);
    }
}

// The light leaving surface `s` toward `v`, before the exposure.
static NphVec3 shade(const CookPreviewStudio *st, const Surface *s, NphVec3 v)
{
    const NphVec3 n = s->normal;
    const NphVec3 diffuse = nph_vec3_scale(s->base, 1.0F - s->metallic);
    const NphVec3 f0 = nph_vec3_lerp(nph_vec3_splat(0.04F), s->base, s->metallic);
    const f32 nov = nph_f32_max(nph_vec3_dot(n, v), 1e-4F);
    // The sky: its irradiance, the occlusion's share of it with the bounce, and its radiance in
    // the reflected direction, blurred toward its irradiance with roughness, by the split sum.
    const NphVec3 irradiance =
        nph_vec3_max(nph_render_ref_sh9_irradiance(st->sh, n), nph_vec3_zero());
    const NphVec3 share = nph_render_ref_ao_bounce(nph_f32_saturate(s->occlusion), diffuse);
    NphVec3 c = nph_vec3_mul(nph_vec3_mul(diffuse, nph_vec3_scale(irradiance, NPH_INV_PI)), share);
    const NphVec3 r = nph_vec3_sub(nph_vec3_scale(n, 2.0F * nov), v);
    const NphVec3 rn = nph_vec3_normalize_or(r, n);
    const NphVec3 sharp = nph_render_ref_sky(&st->sky, rn);
    const NphVec3 blurred = nph_vec3_scale(
        nph_vec3_max(nph_render_ref_sh9_irradiance(st->sh, rn), nph_vec3_zero()), NPH_INV_PI);
    const NphVec3 env = nph_vec3_lerp(sharp, blurred, s->roughness);
    const NphVec2 dfg = dfg_at(st, nov, s->roughness);
    const NphVec3 spec_env = nph_vec3_add(nph_vec3_scale(f0, dfg.x), nph_vec3_splat(dfg.y));
    c = nph_vec3_add(c,
                     nph_vec3_scale(nph_vec3_mul(env, spec_env), nph_f32_saturate(s->occlusion)));
    // The key light.
    const NphVec3 l = st->key_direction;
    const f32 nol = nph_vec3_dot(n, l);
    if (nol > 0.0F) {
        const f32 a = nph_f32_square(nph_f32_max(s->roughness, MIN_KEY_ROUGHNESS));
        const NphVec3 h = nph_vec3_normalize_or(nph_vec3_add(l, v), n);
        const f32 noh = nph_f32_saturate(nph_vec3_dot(n, h));
        const f32 voh = nph_f32_saturate(nph_vec3_dot(v, h));
        const f32 dv = nph_render_ref_d_ggx(noh, a) * nph_render_ref_v_smith(nov, nol, a);
        const NphVec3 f =
            nph_vec3(nph_render_ref_f_schlick(voh, f0.x), nph_render_ref_f_schlick(voh, f0.y),
                     nph_render_ref_f_schlick(voh, f0.z));
        const NphVec3 brdf =
            nph_vec3_add(nph_vec3_scale(diffuse, NPH_INV_PI), nph_vec3_scale(f, dv));
        c = nph_vec3_add(c, nph_vec3_scale(nph_vec3_mul(brdf, st->key_illuminance), nol));
    }
    return c;
}

// The rotated grid's offsets within a pixel.
static const f32 k_offsets[SAMPLES][2] = {
    {-0.125F, -0.375F}, {0.375F, -0.125F}, {0.125F, 0.375F}, {-0.375F, 0.125F}};

static u8 to_byte(f32 v)
{
    return (u8)nph_f32_round(nph_f32_saturate(v) * 255.0F);
}

void cook_preview_render(const CookPreviewStudio *studio, const CookMaterialSource *m,
                         const CookPreviewImages *images, u8 *out)
{
    NPH_ASSERT(studio != NULL && m != NULL && out != NULL);
    const NphMaterialUvTransform t = cook_material_source_uv(m);
    const f32 pixel = 2.0F * VIEW_RADIUS / (f32)COOK_PREVIEW_SIZE;
    const NphVec3 v = nph_vec3_unit_z();
    const bool unlit = m->unlit != 0;
    for (u32 py = 0; py < COOK_PREVIEW_SIZE; ++py) {
        for (u32 px = 0; px < COOK_PREVIEW_SIZE; ++px) {
            // Non-premultiplied: the covered samples' colour weighed by their alpha.
            NphVec3 colour = nph_vec3_zero();
            f32 weight = 0.0F;
            for (u32 i = 0; i < SAMPLES; ++i) {
                const f32 x = ((f32)px + 0.5F + k_offsets[i][0]) * pixel - VIEW_RADIUS;
                const f32 y = VIEW_RADIUS - ((f32)py + 0.5F + k_offsets[i][1]) * pixel;
                const f32 r2 = x * x + y * y;
                if (r2 >= 1.0F) {
                    continue; // the background
                }
                const NphVec3 n = nph_vec3(x, y, nph_f32_sqrt(1.0F - r2));
                Surface s;
                surface_at(m, images, n, pixel, &t, &s);
                f32 alpha = 1.0F;
                if (m->alpha == (u32)NPH_MATERIAL_ALPHA_MASK) {
                    if (s.alpha < m->alpha_cutoff) {
                        continue; // cut out
                    }
                } else if (m->alpha == (u32)NPH_MATERIAL_ALPHA_BLEND) {
                    alpha = nph_f32_saturate(s.alpha);
                }
                NphVec3 c;
                if (unlit) {
                    c = nph_vec3_add(s.base, nph_vec3_scale(s.emissive, studio->emission));
                } else {
                    c = nph_vec3_add(nph_vec3_scale(shade(studio, &s, v), studio->exposure),
                                     nph_vec3_scale(s.emissive, studio->emission));
                    c = nph_render_ref_tonemap(c, NPH_RENDER_TONEMAPPER_AGX);
                }
                c = nph_vec3_clamp(c, nph_vec3_zero(), nph_vec3_one());
                colour = nph_vec3_add(colour, nph_vec3_scale(c, alpha));
                weight += alpha;
            }
            u8 *p = out + ((u64)py * COOK_PREVIEW_SIZE + px) * 4U;
            if (weight <= 0.0F) {
                memset(p, 0, 4U);
                continue;
            }
            const NphVec3 c = nph_vec3_scale(colour, 1.0F / weight);
            p[0] = to_byte(nph_render_ref_linear_to_srgb(c.x));
            p[1] = to_byte(nph_render_ref_linear_to_srgb(c.y));
            p[2] = to_byte(nph_render_ref_linear_to_srgb(c.z));
            p[3] = to_byte(weight / (f32)SAMPLES);
        }
    }
}

// ---------------------------------------------------------------------------
// The cache file
// ---------------------------------------------------------------------------

NphResult cook_preview_save(NphAllocator *alloc, const char *path, NphGuid guid, u64 key,
                            const u8 *pixels)
{
    NPH_ASSERT(alloc != NULL && path != NULL && pixels != NULL);
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION, guid, key);
    nph_asset_writer_set_lz_level(&w, NPH_LZ_LEVEL_FAST);
    NphTextureHeader h;
    memset(&h, 0, sizeof h);
    h.width = COOK_PREVIEW_SIZE;
    h.height = COOK_PREVIEW_SIZE;
    h.mip_count = 1U;
    h.layer_count = 1U;
    h.format = (u32)NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB;
    h.kind = (u32)NPH_TEXTURE_2D;
    NphResult r = nph_asset_writer_add_blob(&w, NPH_TEXTURE_TAG_HEADER, 0U, &h, sizeof h,
                                            NPH_ASSET_CODEC_NONE, 0U);
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(&w, NPH_TEXTURE_TAG_MIP, 0U, pixels, COOK_PREVIEW_BYTES,
                                      NPH_ASSET_CODEC_LZ, 0U);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_write_file(&w, path);
    }
    nph_asset_writer_free(&w);
    return r;
}

NphResult cook_preview_read(const void *bytes, u64 size, u64 key, u8 *out)
{
    NPH_ASSERT(out != NULL);
    NphAssetFileInfo info;
    if (bytes == NULL || nph_asset_file_validate(bytes, size, &info) != NPH_RESULT_OK ||
        info.header.type != (u32)NPH_ASSET_TYPE_TEXTURE ||
        info.header.type_version != NPH_TEXTURE_TYPE_VERSION || info.blob_count != 2U) {
        return NPH_RESULT_CORRUPT;
    }
    const NphAssetBlobEntry *head = nph_asset_file_find(&info, NPH_TEXTURE_TAG_HEADER, 0U);
    const NphAssetBlobEntry *mip = nph_asset_file_find(&info, NPH_TEXTURE_TAG_MIP, 0U);
    // The checksums too, as a loader that verifies does: the table's checks do not reach the
    // blobs' bytes.
    if (head == NULL || mip == NULL || head->raw_size != sizeof(NphTextureHeader) ||
        mip->raw_size != COOK_PREVIEW_BYTES || !nph_asset_file_verify_blob(bytes, head) ||
        !nph_asset_file_verify_blob(bytes, mip)) {
        return NPH_RESULT_CORRUPT;
    }
    NphTextureHeader h;
    if (nph_asset_file_unpack_blob(bytes, head, &h) != NPH_RESULT_OK) {
        return NPH_RESULT_CORRUPT;
    }
    bool reserved = true;
    for (u32 i = 0; i < NPH_ARRAY_COUNT(h.reserved); ++i) {
        reserved = reserved && h.reserved[i] == 0U;
    }
    if (h.width != COOK_PREVIEW_SIZE || h.height != COOK_PREVIEW_SIZE || h.mip_count != 1U ||
        h.layer_count != 1U || h.format != (u32)NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB ||
        h.kind != (u32)NPH_TEXTURE_2D || h.flags != 0U || !reserved) {
        return NPH_RESULT_CORRUPT;
    }
    if (info.header.content_hash != key) {
        return NPH_RESULT_NOT_FOUND; // made for another version of the material
    }
    return nph_asset_file_unpack_blob(bytes, mip, out) == NPH_RESULT_OK ? NPH_RESULT_OK
                                                                        : NPH_RESULT_CORRUPT;
}
