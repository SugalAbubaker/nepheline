// SPDX-License-Identifier: Apache-2.0
// Paged textures' requests (docs/adr/0065-virtual-texturing.md;
// docs/render.md "Virtual textures").
//
// A paged texture is a sparse image (gpu/sparse.h) whose levels are resident
// whole or not at all: from the first under a page always (its mip tail, or
// those levels' own pages on a device that keeps none), and above them the
// levels from the finest its pixels asked for. Its material slots sample it
// with a sampler whose minimum level of detail is its first resident level
// (render_material_sampler_level), so the shaders sample as they do any
// texture: where the hardware's λ is at that level or above, the sample is the
// whole texture's, byte for byte; below, it takes the level itself, a blurrier
// texture while the finer levels come. Every level from there is resident
// whole, so no sample reads a page that is not, whatever its footprint,
// anisotropy or addressing. (Testing the pages each footprint covers cost the
// resolve 0.2 to 0.7 ms in registers and latency, and the MinLod operand 0.09
// ms: ADR-0065, stage 2.)
//
// Feedback: one pixel of each 8 x 8 tile a frame (the view's
// feedback_phase, in the order of an 8 x 8 Bayer matrix: paging.c) sets
// the request bits of the pages its footprint covers, for each paged
// texture, at the level the hardware wants: floor(λ), λ = log2(ρmax / N),
// N = min(ceil(ρmax / ρmin), maxAnisotropy) (Vulkan, "Texel Anisotropic
// Filtering"), its probes within the parallelogram the gradients span (±
// half of each), each one bilinear (± 1 texel). Pages are 128 to 512
// texels a side, so a tile's pixels mostly ask for the same ones; one a
// frame asks for them from the first frame something comes into view. The
// production path's opaque pixels ask in a pass of their own
// (vis_page_requests.comp: in the resolve, the request's code cost it 0.25
// ms though it never ran); the forward pass's fragments ask as they shade.
// paging.c reads the bits back NPH_GPU_FRAMES_IN_FLIGHT frames later.

#ifndef NPH_TEXTURE_PAGING_GLSL
#define NPH_TEXTURE_PAGING_GLSL

#include "render_common.glsl"

NPH_BUFFER_REF(PagingRecordsRef) { NphRenderTexturePaging r[]; };
NPH_BUFFER_REF(PageRequestsRef) { uint w[]; };
NPH_BUFFER_REF(PagingViewRef) { NphRenderPagingView v; };

// Whether pixel `p` asks for pages this frame (`phase`: the view's
// feedback_phase).
bool paging_asks(uvec2 p, uint phase)
{
    return (p.x & 7U) == (phase & 7U) && (p.y & 7U) == (phase >> 3U);
}

uint paging_tail(NphRenderTexturePaging p)
{
    return (p.shape >> 8U) & 15U;
}

uvec2 paging_page_log2(NphRenderTexturePaging p)
{
    return uvec2(p.shape & 15U, (p.shape >> 4U) & 15U);
}

uvec2 paging_extent(NphRenderTexturePaging p)
{
    return uvec2(p.extent & 0xFFFFU, p.extent >> 16U);
}

uint paging_level_bit(NphRenderTexturePaging p, uint level)
{
    const uint pair = level < 2U   ? p.levels01
                      : level < 4U ? p.levels23
                      : level < 6U ? p.levels45
                                   : p.levels67;
    return (level & 1U) == 0U ? (pair & 0xFFFFU) : (pair >> 16U);
}

// λ for gradients dx, dy (texture coordinates a pixel) on `size` texels.
float paging_lod(vec2 size, vec2 dx, vec2 dy, float max_aniso)
{
    const vec2 tx = dx * size;
    const vec2 ty = dy * size;
    const float px = dot(tx, tx);
    const float py = dot(ty, ty);
    const float pmax = max(max(px, py), 1.0e-30);
    const float pmin = max(min(px, py), 1.0e-30);
    const float n = min(ceil(sqrt(pmax / pmin)), max(max_aniso, 1.0));
    return 0.5 * log2(pmax) - log2(n);
}

// The first and last texel a box covers along an axis of a level `texels`
// wide, addressed as its sampler addresses (`wrap`: 0 repeat, 1 clamp to
// the edge, 2 mirrored repeat), as the first and last of the pages of
// 2^page_log2 texels they fall in.
uvec2 paging_axis(float lo, float hi, uint texels, uint page_log2, uint wrap)
{
    // Float arithmetic: a GPU divides integers slowly.
    const float n = float(texels);
    float a = floor(lo);
    float b = floor(hi);
    if (wrap == 1U) {
        a = clamp(a, 0.0, n - 1.0);
        b = clamp(b, 0.0, n - 1.0);
    } else if (wrap == 2U) {
        // A mirror's period is twice the level; the box, far narrower than a
        // page at the paged levels, folds at most once, and the pages it
        // covers lie between its folded ends.
        const float period = 2.0 * n;
        a -= period * floor(a / period);
        b -= period * floor(b / period);
        a = a < n ? a : period - 1.0 - a;
        b = b < n ? b : period - 1.0 - b;
        const float lower = min(a, b);
        b = max(a, b);
        a = lower;
    } else {
        // Repeat: a box across the edge has its ends on either side, the
        // pages between them untouched.
        a -= n * floor(a / n);
        b -= n * floor(b / n);
    }
    return uvec2(uint(clamp(a, 0.0, n - 1.0)) >> page_log2, uint(clamp(b, 0.0, n - 1.0)) >> page_log2);
}

// The bit of page (x, y) of `level`, among all of the texture's words.
uint paging_bit(NphRenderTexturePaging p, uint level, uint x, uint y, uint pages_x)
{
    return (p.first_word + 1U) * 32U + paging_level_bit(p, level) + y * pages_x + x;
}

// Sets request bit `bit`. A page is asked for by many pixels, and atomics
// on one word queue behind each other, so a bit already set (as read) is
// left alone: the atomics cost the request pass 0.1 to 0.17 ms on Bistro,
// the rest of it 0.007. Where the includer defines PAGING_COMBINE (a
// compute shader: a fragment shader's helper lanes could be elected, and
// they do not write), a subgroup whose lanes all set bits of one word sets
// them together.
void paging_request_bit(PageRequestsRef requests, uint bit)
{
    const uint word = bit >> 5U;
    uint mask = 1U << (bit & 31U);
#ifdef PAGING_COMBINE
    if (subgroupAllEqual(word)) {
        mask = subgroupOr(mask);
        if (!subgroupElect()) {
            return;
        }
    }
#endif
    if ((requests.w[word] & mask) != mask) {
        atomicOr(requests.w[word], mask);
    }
}

// Asks for the pages that a footprint (centre `uv`, gradients dx, dy) of
// texture `tex` covers at the level the hardware wants, the texture
// addressed as `wrap` (its slot's 4 bits of NphRenderMaterialData.wrap).
// Nothing for a texture resident whole, nor at the levels resident from
// creation.
void paging_request(PagingViewRef view, uint tex, uint wrap, vec2 uv, vec2 dx, vec2 dy)
{
    if (tex == NPH_RENDER_NO_TEXTURE) {
        return;
    }
    const NphRenderTexturePaging p = PagingRecordsRef(view.v.records).r[tex];
    if (p.shape == 0U) {
        return;
    }
    const uvec2 extent0 = paging_extent(p);
    const float lod = paging_lod(vec2(extent0), dx, dy, view.v.max_anisotropy);
    if (lod >= float(paging_tail(p))) {
        return;
    }
    const uint level = uint(max(floor(lod), 0.0));
    const uvec2 extent = max(extent0 >> level, uvec2(1U));
    const uvec2 page_log2 = paging_page_log2(p);
    const vec2 size = vec2(extent);
    const vec2 centre = uv * size - 0.5;
    const vec2 reach = 0.5 * (abs(dx) + abs(dy)) * size + 1.0;
    const uvec2 xs = paging_axis(centre.x - reach.x, centre.x + reach.x, extent.x, page_log2.x,
                                 wrap & 3U);
    const uvec2 ys = paging_axis(centre.y - reach.y, centre.y + reach.y, extent.y, page_log2.y,
                                 wrap >> 2U);
    const uint pages_x = (extent.x + (1U << page_log2.x) - 1U) >> page_log2.x;
    PageRequestsRef requests = PageRequestsRef(view.v.requests);
    // One or two pages a side; each distinct one once.
    for (uint j = 0U; j < (ys.x == ys.y ? 1U : 2U); ++j) {
        const uint y = j == 0U ? ys.x : ys.y;
        for (uint i = 0U; i < (xs.x == xs.y ? 1U : 2U); ++i) {
            paging_request_bit(requests, paging_bit(p, level, i == 0U ? xs.x : xs.y, y, pages_x));
        }
    }
}

#endif
