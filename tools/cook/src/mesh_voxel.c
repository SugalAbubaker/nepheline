// SPDX-License-Identifier: Apache-2.0
// The voxelizer's materials (cook/voxel.h; ADR-0061): images as RGBA8 mip
// chains, trilinear lookups, and the sampler over a quantised mesh.
#include <nepheline/core/assert.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>

#include <cook/voxel.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// A channel of the source image's texel in linear light; `srgb_lut` decodes
// an sRGB byte.
static f32 texel_value(const CookImage *image, u32 x, u32 y, u32 c, b32 srgb, const f32 *srgb_lut)
{
    const u64 i = ((u64)y * image->width + x) * 4U + c;
    if (image->format == COOK_IMAGE_RGBA32F) {
        return nph_f32_saturate(((const f32 *)image->pixels)[i]);
    }
    const u8 v = ((const u8 *)image->pixels)[i];
    return srgb && c < 3U ? srgb_lut[v] : (f32)v / 255.0F;
}

static u8 encode(f32 v, b32 srgb, u32 c)
{
    const f32 x =
        srgb && c < 3U ? nph_render_ref_linear_to_srgb(nph_f32_saturate(v)) : nph_f32_saturate(v);
    return (u8)nph_f32_round(x * 255.0F);
}

NphResult cook_voxel_image_init(CookVoxelImage *out, NphAllocator *alloc, const CookImage *image,
                                b32 srgb)
{
    NPH_ASSERT(out != NULL && alloc != NULL && image != NULL);
    memset(out, 0, sizeof *out);
    if (image->width == 0U || image->height == 0U || image->pixels == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    f32 srgb_lut[256];
    for (u32 i = 0; i < 256U; ++i) {
        srgb_lut[i] = nph_render_ref_srgb_to_linear((f32)i / 255.0F);
        out->decode[i] = srgb ? srgb_lut[i] : (f32)i / 255.0F;
    }
    // Level 0: the image halved (box-filtered) until it fits.
    u32 shift = 0U;
    while ((image->width >> shift) > COOK_VOXEL_IMAGE_MAX ||
           (image->height >> shift) > COOK_VOXEL_IMAGE_MAX) {
        ++shift;
    }
    const u32 w0 = nph_max_u32(image->width >> shift, 1U);
    const u32 h0 = nph_max_u32(image->height >> shift, 1U);
    u32 levels = 1U;
    u64 size = (u64)w0 * h0 * 4U;
    out->offset[0] = 0U;
    while (levels < COOK_VOXEL_IMAGE_LEVELS &&
           ((w0 >> (levels - 1U)) > 1U || (h0 >> (levels - 1U)) > 1U)) {
        out->offset[levels] = size;
        size += (u64)nph_max_u32(w0 >> levels, 1U) * nph_max_u32(h0 >> levels, 1U) * 4U;
        ++levels;
    }
    u8 *texels = (u8 *)NPH_ALLOC(alloc, size, 16U);
    f32 *linear = NPH_ALLOC_ARRAY(alloc, f32, (u64)w0 * h0 * 4U);
    if (texels == NULL || linear == NULL) {
        if (texels != NULL) {
            NPH_FREE(alloc, texels, size, 16U);
        }
        if (linear != NULL) {
            NPH_FREE_ARRAY(alloc, f32, linear, (u64)w0 * h0 * 4U);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    // Level 0 in linear light, each texel the mean of its 2^shift square.
    const u32 span = 1U << shift;
    for (u32 y = 0; y < h0; ++y) {
        for (u32 x = 0; x < w0; ++x) {
            for (u32 c = 0; c < 4U; ++c) {
                f64 sum = 0.0;
                u32 n = 0U;
                for (u32 sy = 0; sy < span; ++sy) {
                    for (u32 sx = 0; sx < span; ++sx) {
                        const u32 px = nph_min_u32(x * span + sx, image->width - 1U);
                        const u32 py = nph_min_u32(y * span + sy, image->height - 1U);
                        sum += (f64)texel_value(image, px, py, c, srgb, srgb_lut);
                        ++n;
                    }
                }
                linear[((u64)y * w0 + x) * 4U + c] = (f32)(sum / (f64)n);
            }
        }
    }
    // Each level from the one before, still in linear light.
    u32 w = w0;
    u32 h = h0;
    for (u32 l = 0; l < levels; ++l) {
        u8 *dst = texels + out->offset[l];
        for (u64 i = 0; i < (u64)w * h; ++i) {
            for (u32 c = 0; c < 4U; ++c) {
                dst[i * 4U + c] = encode(linear[i * 4U + c], srgb, c);
            }
        }
        if (l + 1U == levels) {
            break;
        }
        const u32 nw = nph_max_u32(w >> 1U, 1U);
        const u32 nh = nph_max_u32(h >> 1U, 1U);
        for (u32 y = 0; y < nh; ++y) {
            for (u32 x = 0; x < nw; ++x) {
                for (u32 c = 0; c < 4U; ++c) {
                    f32 sum = 0.0F;
                    for (u32 k = 0; k < 4U; ++k) {
                        const u32 sx = nph_min_u32(x * 2U + (k & 1U), w - 1U);
                        const u32 sy = nph_min_u32(y * 2U + (k >> 1U), h - 1U);
                        sum += linear[((u64)sy * w + sx) * 4U + c];
                    }
                    // In place: row y of the next level reads rows 2y and
                    // 2y + 1 of this one, which no earlier row overwrote.
                    linear[((u64)y * nw + x) * 4U + c] = 0.25F * sum;
                }
            }
        }
        w = nw;
        h = nh;
    }
    NPH_FREE_ARRAY(alloc, f32, linear, (u64)w0 * h0 * 4U);
    out->alloc = alloc;
    out->width = w0;
    out->height = h0;
    out->levels = levels;
    out->srgb = srgb;
    out->texels = texels;
    out->size = size;
    return NPH_RESULT_OK;
}

void cook_voxel_image_free(CookVoxelImage *img)
{
    if (img == NULL || img->texels == NULL) {
        return;
    }
    NPH_FREE(img->alloc, img->texels, img->size, 16U);
    memset(img, 0, sizeof *img);
}

// Texel coordinate `i` of an axis `n` texels long under a wrap mode.
static u32 wrap(i64 i, u32 n, u32 mode)
{
    const i64 len = (i64)n;
    if (mode == NPH_MATERIAL_WRAP_CLAMP) {
        return (u32)(i < 0 ? 0 : (i >= len ? len - 1 : i));
    }
    if (mode == NPH_MATERIAL_WRAP_MIRROR) {
        i64 m = i % (2 * len);
        m = m < 0 ? m + 2 * len : m;
        return (u32)(m < len ? m : 2 * len - 1 - m);
    }
    i64 m = i % len;
    return (u32)(m < 0 ? m + len : m);
}

static NphVec4 fetch(const CookVoxelImage *img, u32 level, u32 w, u32 x, u32 y)
{
    const u8 *t = img->texels + img->offset[level] + ((u64)y * w + x) * 4U;
    return nph_vec4(img->decode[t[0]], img->decode[t[1]], img->decode[t[2]], (f32)t[3] / 255.0F);
}

static NphVec4 bilinear(const CookVoxelImage *img, u32 level, NphVec2 uv, u32 wrap_u, u32 wrap_v)
{
    const u32 w = nph_max_u32(img->width >> level, 1U);
    const u32 h = nph_max_u32(img->height >> level, 1U);
    const f64 fx = (f64)uv.x * w - 0.5;
    const f64 fy = (f64)uv.y * h - 0.5;
    const f64 x0 = floor(fx);
    const f64 y0 = floor(fy);
    const f32 ax = (f32)(fx - x0);
    const f32 ay = (f32)(fy - y0);
    const i64 ix = (i64)x0;
    const i64 iy = (i64)y0;
    const u32 xa = wrap(ix, w, wrap_u);
    const u32 xb = wrap(ix + 1, w, wrap_u);
    const u32 ya = wrap(iy, h, wrap_v);
    const u32 yb = wrap(iy + 1, h, wrap_v);
    const NphVec4 a = nph_vec4_lerp(fetch(img, level, w, xa, ya), fetch(img, level, w, xb, ya), ax);
    const NphVec4 b = nph_vec4_lerp(fetch(img, level, w, xa, yb), fetch(img, level, w, xb, yb), ax);
    return nph_vec4_lerp(a, b, ay);
}

NphVec4 cook_voxel_image_sample(const CookVoxelImage *img, NphVec2 uv, f32 footprint, u32 wrap_u,
                                u32 wrap_v)
{
    NPH_ASSERT(img != NULL && img->texels != NULL);
    if (!nph_f32_is_finite(uv.x) || !nph_f32_is_finite(uv.y)) {
        uv = nph_vec2_zero();
    }
    const f32 texels = nph_f32_max(footprint, 0.0F) * (f32)nph_max_u32(img->width, img->height);
    const f32 lod =
        nph_f32_clamp(texels > 1.0F ? nph_f32_log2(texels) : 0.0F, 0.0F, (f32)(img->levels - 1U));
    const u32 l0 = (u32)lod;
    const u32 l1 = nph_min_u32(l0 + 1U, img->levels - 1U);
    const f32 t = lod - (f32)l0;
    const NphVec4 a = bilinear(img, l0, uv, wrap_u, wrap_v);
    return t > 0.0F ? nph_vec4_lerp(a, bilinear(img, l1, uv, wrap_u, wrap_v), t) : a;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

void cook_voxel_material_init(CookVoxelMaterial *out, const NphMaterialHeader *h)
{
    NPH_ASSERT(out != NULL && h != NULL);
    memset(out, 0, sizeof *out);
    const bool emissive = (h->emissive_factor.x > 0.0F || h->emissive_factor.y > 0.0F ||
                           h->emissive_factor.z > 0.0F) &&
                          h->emissive_strength > 0.0F;
    if (h->alpha_mode == NPH_MATERIAL_ALPHA_BLEND || emissive ||
        (h->flags & (u32)NPH_MATERIAL_FLAG_UNLIT) != 0U) {
        out->surface = NPH_RENDER_VOXEL_SURFACE_NONE;
    } else {
        out->surface = h->alpha_mode == NPH_MATERIAL_ALPHA_MASK ? NPH_RENDER_VOXEL_SURFACE_MASKED
                                                                : NPH_RENDER_VOXEL_SURFACE_OPAQUE;
    }
    out->base_color = h->base_color_factor;
    out->alpha_cutoff = h->alpha_cutoff;
    out->metallic = h->metallic_factor;
    out->roughness = h->roughness_factor;
    out->normal_scale = h->textures[NPH_MATERIAL_TEXTURE_NORMAL].scale;
    out->occlusion_strength = h->textures[NPH_MATERIAL_TEXTURE_OCCLUSION].scale;
}

// ---------------------------------------------------------------------------
// The sampler
// ---------------------------------------------------------------------------

static const CookVoxelMaterial *material_of(const CookVoxelSampler *s, u32 submesh)
{
    if (submesh >= s->mesh->submeshes.len) {
        return NULL;
    }
    const u32 slot = s->mesh->submeshes.data[submesh].material_slot;
    return slot < s->materials->slot_count ? &s->materials->slots[slot] : NULL;
}

// The point's attributes: texture coordinates, vertex colour, normal,
// tangent (handedness in w), and the triangle's texture coordinates a mesh
// unit spans there.
typedef struct Point {
    NphVec2 uv;
    NphVec4 color;
    NphVec3 normal;
    NphVec4 tangent;
    f32 uv_per_unit;
} Point;

static Point point(const CookVoxelSampler *s, const u32 v[3], NphVec3 b)
{
    Point p;
    memset(&p, 0, sizeof p);
    const f32 w[3] = {b.x, b.y, b.z};
    NphVec2 uv[3];
    NphVec3 n = nph_vec3_zero();
    NphVec3 t = nph_vec3_zero();
    f32 hand = 0.0F;
    for (u32 k = 0; k < 3U; ++k) {
        const NphMeshAttrib *a = &s->attribs[v[k]];
        uv[k] = nph_unpack_f16x2(a->uv0);
        p.uv = nph_vec2_add(p.uv, nph_vec2_scale(uv[k], w[k]));
        p.color = nph_vec4_add(p.color, nph_vec4_scale(nph_unpack_unorm8x4(a->color), w[k]));
        n = nph_vec3_add(n, nph_vec3_scale(nph_oct_decode_snorm16(a->normal), w[k]));
        const NphVec4 tt = nph_unpack_snorm8x4(a->tangent);
        t = nph_vec3_add(t, nph_vec3_scale(nph_vec4_xyz(tt), w[k]));
        hand += (tt.w < 0.0F ? -1.0F : 1.0F) * w[k];
    }
    p.normal = nph_vec3_normalize_or(n, nph_vec3_unit_z());
    p.tangent = nph_vec4_from_vec3(t, hand < 0.0F ? -1.0F : 1.0F);
    const NphVec3 p0 = s->positions[v[0]];
    const f32 world = nph_vec3_length(
        nph_vec3_cross(nph_vec3_sub(s->positions[v[1]], p0), nph_vec3_sub(s->positions[v[2]], p0)));
    const f32 tex =
        nph_f32_abs(nph_vec2_cross(nph_vec2_sub(uv[1], uv[0]), nph_vec2_sub(uv[2], uv[0])));
    p.uv_per_unit = world > 0.0F ? nph_f32_sqrt(tex / world) : 0.0F;
    return p;
}

static NphVec4 lookup(const CookVoxelSlot *slot, const Point *p, f32 footprint)
{
    return cook_voxel_image_sample(slot->image, p->uv, footprint * p->uv_per_unit, slot->wrap_u,
                                   slot->wrap_v);
}

static NphVec4 base_color(const CookVoxelMaterial *m, const Point *p, f32 footprint)
{
    NphVec4 c = nph_vec4_mul(m->base_color, p->color);
    if (m->base_color_image.image != NULL) {
        c = nph_vec4_mul(c, lookup(&m->base_color_image, p, footprint));
    }
    return c;
}

static void sample(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                   NphRenderVoxelSample *out)
{
    const CookVoxelSampler *s = (const CookVoxelSampler *)user;
    const CookVoxelMaterial *m = material_of(s, submesh);
    const Point p = point(s, v, b);
    memset(out, 0, sizeof *out);
    out->normal = p.normal;
    if (m == NULL) {
        out->base_color = nph_vec3_one();
        out->roughness = 1.0F;
        out->occlusion = 1.0F;
        return;
    }
    out->base_color = nph_vec4_xyz(base_color(m, &p, footprint));
    out->roughness = m->roughness;
    out->metallic = m->metallic;
    if (m->metallic_roughness_image.image != NULL) {
        const NphVec4 mr = lookup(&m->metallic_roughness_image, &p, footprint);
        out->roughness *= mr.y;
        out->metallic *= mr.z;
    }
    out->occlusion = 1.0F;
    if (m->occlusion_image.image != NULL) {
        out->occlusion =
            1.0F + m->occlusion_strength * (lookup(&m->occlusion_image, &p, footprint).x - 1.0F);
    }
    if (m->normal_image.image != NULL) {
        // The tangent frame of glTF (the bitangent cross(n, t) times the
        // handedness), the map's xy scaled.
        const NphVec4 nm = lookup(&m->normal_image, &p, footprint);
        const NphVec3 tn = nph_vec3((nm.x * 2.0F - 1.0F) * m->normal_scale,
                                    (nm.y * 2.0F - 1.0F) * m->normal_scale, nm.z * 2.0F - 1.0F);
        const NphVec3 t = nph_vec4_xyz(p.tangent);
        if (nph_vec3_length_sq(t) > 1e-12F) {
            const NphVec3 tu = nph_vec3_normalize(t);
            const NphVec3 bv = nph_vec3_scale(nph_vec3_cross(p.normal, tu), p.tangent.w);
            out->normal = nph_vec3_normalize_or(
                nph_vec3_add(nph_vec3_add(nph_vec3_scale(tu, tn.x), nph_vec3_scale(bv, tn.y)),
                             nph_vec3_scale(p.normal, tn.z)),
                p.normal);
        }
    }
}

static bool covers(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint)
{
    const CookVoxelSampler *s = (const CookVoxelSampler *)user;
    const CookVoxelMaterial *m = material_of(s, submesh);
    if (m == NULL) {
        return true;
    }
    const Point p = point(s, v, b);
    return base_color(m, &p, footprint).w >= m->alpha_cutoff;
}

void cook_voxel_sampler_init(CookVoxelSampler *s, const CookMesh *mesh,
                             const CookVoxelMaterials *materials, const NphVec3 *positions,
                             const NphMeshAttrib *attribs, u8 *surfaces, NphRenderVoxelSampler *out)
{
    NPH_ASSERT(s != NULL && mesh != NULL && materials != NULL && out != NULL);
    s->mesh = mesh;
    s->materials = materials;
    s->positions = positions;
    s->attribs = attribs;
    for (u32 i = 0; i < mesh->submeshes.len; ++i) {
        const CookVoxelMaterial *m = material_of(s, i);
        surfaces[i] = m != NULL ? m->surface : (u8)NPH_RENDER_VOXEL_SURFACE_NONE;
    }
    memset(out, 0, sizeof *out);
    out->user = s;
    out->submesh_surface = surfaces;
    out->sample = sample;
    out->covers = covers;
}
