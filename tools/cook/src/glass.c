#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>

#include <cook/glass.h>
#include <string.h>

static u8 unorm8(f32 v)
{
    return (u8)(nph_f32_saturate(v) * 255.0F + 0.5F);
}

NphResult cook_glass_texture(NphAllocator *alloc, const CookImage *base,
                             const CookImage *transmission, f32 factor, CookImage *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (transmission == NULL || transmission->format != COOK_IMAGE_RGBA8 ||
        (base != NULL && base->format != COOK_IMAGE_RGBA8) || !nph_f32_is_finite(factor)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const f32 f = nph_f32_saturate(factor);
    const u32 w = nph_max_u32(base != NULL ? base->width : 1U, transmission->width);
    const u32 h = nph_max_u32(base != NULL ? base->height : 1U, transmission->height);
    // The base colour in linear light (alpha as stored), the transmission as stored: glTF's
    // transmission texture is linear.
    CookImage b_lin;
    CookImage t_lin;
    memset(&b_lin, 0, sizeof b_lin);
    memset(&t_lin, 0, sizeof t_lin);
    NphResult r = NPH_RESULT_OK;
    if (base != NULL) {
        r = cook_image_to_f32(alloc, base, true, &b_lin);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_image_to_f32(alloc, transmission, false, &t_lin);
    }
    if (r == NPH_RESULT_OK) {
        r = cook_image_create(alloc, w, h, COOK_IMAGE_RGBA8, out);
    }
    if (r == NPH_RESULT_OK) {
        // A base colour of the output's extent keeps its colour's bytes as they were.
        const bool base_whole = base != NULL && base->width == w && base->height == h;
        u8 *px = (u8 *)out->pixels;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                f32 bt[4] = {1.0F, 1.0F, 1.0F, 1.0F};
                f32 tt[4];
                if (base != NULL) {
                    cook_image_sample_grid(&b_lin, x, y, w, h, bt);
                }
                cook_image_sample_grid(&t_lin, x, y, w, h, tt);
                const f32 keep = 1.0F - nph_f32_saturate(f * tt[0]);
                u8 *o = px + ((u64)y * w + x) * 4U;
                if (base_whole) {
                    const u8 *src = (const u8 *)base->pixels + ((u64)y * w + x) * 4U;
                    o[0] = src[0];
                    o[1] = src[1];
                    o[2] = src[2];
                } else {
                    o[0] = unorm8(cook_linear_to_srgb(bt[0]));
                    o[1] = unorm8(cook_linear_to_srgb(bt[1]));
                    o[2] = unorm8(cook_linear_to_srgb(bt[2]));
                }
                o[3] = unorm8(bt[3] * keep);
            }
        }
        out->source_channels = 4U;
    }
    cook_image_free(&b_lin);
    cook_image_free(&t_lin);
    if (r != NPH_RESULT_OK) {
        cook_image_free(out);
    }
    return r;
}
