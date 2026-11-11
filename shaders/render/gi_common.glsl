// SPDX-License-Identifier: Apache-2.0
// The radiance cache's cells (docs/render.md "Global illumination";
// ADR-0039): a hash grid of the irradiance arriving at the scene's surfaces,
// after Binder, Fricke & Keller, "Fast Path Space Filtering by Jittered
// Spatial Hashing" (SIGGRAPH 2018 talks) and "Massively Parallel Path Space
// Filtering" (MCQMC 2020), Gautron, "Real-Time Ray-Traced Ambient Occlusion
// of Complex Scenes using Spatial Hashing" (SIGGRAPH 2020 talks), and
// Boissé et al., "GI-1.0: A Fast and Scalable Two-Level Radiance Caching
// Scheme for Real-Time Global Illumination" (2023).
//
// A cell is a cube of the world at a level the point's distance from the
// camera chooses (cells grow with distance: 1 / (2 scale) to 1 / scale of
// it across),
// for the surfaces in it whose normals face one octant, so that the two
// sides of a wall are cells apart. Its key packs the cell's coordinates
// (their low 17 bits, wrapping: a level spans a few hundred cells), its
// level, and the octant into 64 bits; a 64-bit finaliser (MurmurHash3's
// fmix64, Appleby) mixes it into a bucket of NPH_RENDER_GI_BUCKET slots and a
// 32-bit fingerprint that the slot holds (0: free), so 32-bit atomics claim
// slots on every device. Coordinates come from the camera-relative point and
// each level's camera cell, which the CPU works out in double precision,
// so cells stay put in the world however far it reaches (CLAUDE.md §8.2).
//
// The includer enables nothing more: the fingerprint is 32 bits, the keys
// 64-bit integers (nph_bindless.glsl's arithmetic types).
#ifndef NPH_GI_COMMON_GLSL
#define NPH_GI_COMMON_GLSL

#include "render_common.glsl"

NPH_BUFFER_REF(GiViewRef) { NphRenderGiView g; };
NPH_BUFFER_REF(GiKeysRef) { uint k[]; };
NPH_BUFFER_REF(GiKeys4Ref) { uvec4 k[]; };
NPH_BUFFER_REF(GiSumsRef) { uvec4 s[]; };
NPH_BUFFER_REF(GiCellsRef) { uvec4 c[]; };
NPH_BUFFER_REF(GiMomentsRef) { uvec4 m[]; };

#define GI_NONE 0xFFFFFFFFu

// A cell's word (NPH_RENDER_GI_COUNT_BITS): its sample count, the frames since a sample arrived,
// and the many lights' far fields' share of its irradiance (as bits, and as a fraction).
uint gi_word_count(uint w)
{
    return w & ((1u << NPH_RENDER_GI_COUNT_BITS) - 1u);
}

uint gi_word_age(uint w)
{
    return (w >> NPH_RENDER_GI_COUNT_BITS) & 0xFFu;
}

uint gi_word_far(uint w)
{
    return w >> (NPH_RENDER_GI_COUNT_BITS + 8);
}

#define GI_FAR_ONE float((1u << NPH_RENDER_GI_FAR_BITS) - 1u)

float gi_word_far_share(uint w)
{
    return float(gi_word_far(w)) * (1.0 / GI_FAR_ONE);
}

uint gi_word(uint count, uint age, uint far)
{
    return count | (age << NPH_RENDER_GI_COUNT_BITS) | (far << (NPH_RENDER_GI_COUNT_BITS + 8));
}

// The level of a point `dist` metres from the camera: floor(log2(dist)),
// the float's exponent, biased and clamped.
int gi_level(float dist)
{
    const int e = (floatBitsToInt(max(dist, 1e-30)) >> 23) - 127;
    return clamp(e + NPH_RENDER_GI_LEVEL_BIAS, 0, NPH_RENDER_GI_LEVELS - 1);
}

// The size of a cell at level `level` (metres).
float gi_cell_size(GiViewRef g, int level)
{
    return g.g.levels[level].origin.w;
}

// The octant a normal faces, its components' signs (one a thousandth below
// zero still counts as zero, so a face whose normal lies in a plane of the
// axes does not flicker between octants).
uint gi_octant(vec3 n)
{
    return (n.x > -1e-3 ? 1u : 0u) | (n.y > -1e-3 ? 2u : 0u) | (n.z > -1e-3 ? 4u : 0u);
}

// The key of the cell at `c` (in the world: the level's camera cell added) of level `level`, for
// the surfaces in it that face octant `octant`.
uint64_t gi_cell_key(ivec3 c, int level, uint octant)
{
    const uint m = 0x1FFFFu;
    return uint64_t(uint(c.x) & m) | (uint64_t(uint(c.y) & m) << 17ul) |
           (uint64_t(uint(c.z) & m) << 34ul) | (uint64_t(level) << 51ul) |
           (uint64_t(octant) << 56ul);
}

// The camera's cell at `level`.
ivec3 gi_camera_cell(GiViewRef g, int level)
{
    // Read field by field: a level holds an array (docs/build.md "Shaders").
    return ivec3(uvec3(g.g.levels[level].cell[0], g.g.levels[level].cell[1],
                       g.g.levels[level].cell[2]));
}

// The key of the cell holding the camera-relative point `p` on a surface
// whose geometric normal is `n`. The point is taken half a cell out along
// the normal: a surface on a cell's face (a floor at y = 0, which the
// cells' powers of two align with) would otherwise fall either side of it
// as its points' rounding goes, and lookups (blended, or another frame's
// pixels) would miss the cells the update filled.
uint64_t gi_key(GiViewRef g, vec3 p, vec3 n)
{
    const int level = gi_level(length(p));
    const vec4 origin = g.g.levels[level].origin;
    const ivec3 c =
        gi_camera_cell(g, level) + ivec3(floor((p + 0.5 * origin.w * n + origin.xyz) / origin.w));
    return gi_cell_key(c, level, gi_octant(n));
}

// Uniform random numbers from a 3D integer hash (Jarzynski & Olano, "Hash
// Functions for GPU Rendering", JCGT 2020: pcg3d).
uvec3 gi_pcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

vec3 gi_random(uvec3 seed)
{
    return vec3(gi_pcg3d(seed) >> 8u) * (1.0 / 16777216.0);
}

// A key's hash: its two words, and a constant, through pcg3d, whose first two words make the
// bucket (low) and the fingerprint (high). Every bit of the key moves every bit of the hash, with
// 32-bit multiplies; MurmurHash3's 64-bit finaliser, which this replaced (ADR-0085), took 64-bit
// ones, which a GPU makes of several 32-bit ones, and the blended lookup hashes four keys a pixel.
uint64_t gi_mix(uint64_t k)
{
    const uvec3 h = gi_pcg3d(uvec3(uint(k), uint(k >> 32ul), 0x9E3779B9u));
    return uint64_t(h.x) | (uint64_t(h.y) << 32ul);
}

// The first slot of a key's bucket, and the fingerprint its slot holds
// (never 0, which marks a free slot).
uint gi_bucket(GiViewRef g, uint64_t h)
{
    return (uint(h) & g.g.bucket_mask) * uint(NPH_RENDER_GI_BUCKET);
}

uint gi_fingerprint(uint64_t h)
{
    return uint(h >> 32ul) | 1u;
}

// The slot holding `key` in the table of fingerprints at `keys_at`, GI_NONE if none does. Slots
// freed by the resolve leave gaps in a bucket, so the whole bucket is read (64 bytes).
uint gi_find_in(uint64_t keys_at, uint bucket_mask, uint64_t key)
{
    const uint64_t h = gi_mix(key);
    const uint base = (uint(h) & bucket_mask) * uint(NPH_RENDER_GI_BUCKET);
    const uint fp = gi_fingerprint(h);
    GiKeys4Ref keys = GiKeys4Ref(keys_at);
    for (uint i = 0u; i < uint(NPH_RENDER_GI_BUCKET) / 4u; ++i) {
        const bvec4 hit = equal(keys.k[base / 4u + i], uvec4(fp));
        if (any(hit)) {
            return base + i * 4u + (hit.x ? 0u : (hit.y ? 1u : (hit.z ? 2u : 3u)));
        }
    }
    return GI_NONE;
}

uint gi_find(GiViewRef g, uint64_t key)
{
    return gi_find_in(g.g.keys, g.g.bucket_mask, key);
}

// The slot of `key`, claimed if no slot holds it yet: its bucket's first
// free one, by compare-and-swap in order, so that two invocations claiming
// the same new key meet in the same slot. GI_NONE when the bucket is full
// (the sample is dropped). Only the update claims slots, and only the
// resolve frees them, so during the update a bucket only fills.
uint gi_claim_in(uint64_t keys_at, uint bucket_mask, uint64_t key)
{
    const uint64_t h = gi_mix(key);
    const uint base = (uint(h) & bucket_mask) * uint(NPH_RENDER_GI_BUCKET);
    const uint fp = gi_fingerprint(h);
    GiKeys4Ref keys4 = GiKeys4Ref(keys_at);
    for (uint i = 0u; i < uint(NPH_RENDER_GI_BUCKET) / 4u; ++i) {
        const bvec4 hit = equal(keys4.k[base / 4u + i], uvec4(fp));
        if (any(hit)) {
            return base + i * 4u + (hit.x ? 0u : (hit.y ? 1u : (hit.z ? 2u : 3u)));
        }
    }
    GiKeysRef keys = GiKeysRef(keys_at);
    for (uint i = 0u; i < uint(NPH_RENDER_GI_BUCKET); ++i) {
        const uint prev = atomicCompSwap(keys.k[base + i], 0u, fp);
        if (prev == 0u || prev == fp) {
            return base + i;
        }
    }
    return GI_NONE;
}

uint gi_claim(GiViewRef g, uint64_t key)
{
    return gi_claim_in(g.g.keys, g.g.bucket_mask, key);
}

// A cell's irradiance (lux, rgb) and how far it can be trusted (w: 0 for
// none to 1 from 16 samples on).
vec4 gi_cell(GiViewRef g, uint slot)
{
    const uvec4 c = GiCellsRef(g.g.cells).c[slot];
    return vec4(uintBitsToFloat(c.xyz), min(float(gi_word_count(c.w)) * (1.0 / 16.0), 1.0));
}

// The share of a cell's irradiance that is the many lights' far fields' (ADR-0078).
float gi_cell_far(GiViewRef g, uint slot)
{
    return gi_word_far_share(GiCellsRef(g.g.cells).c[slot].w);
}

// The cache's irradiance at the camera-relative point `p` (geometric normal
// `n`): `fallback` where it knows none, blended toward the cell's as it
// trusts it.
vec3 gi_irradiance(GiViewRef g, vec3 p, vec3 n, vec3 fallback)
{
    const uint slot = gi_find(g, gi_key(g, p, n));
    if (slot == GI_NONE) {
        return fallback;
    }
    const vec4 c = gi_cell(g, slot);
    return mix(fallback, c.rgb, c.w);
}

// A direction on an octahedral map, 12 bits an axis.
uvec2 gi_reflect_oct(vec3 r)
{
    vec2 o = r.xy / (abs(r.x) + abs(r.y) + abs(r.z));
    if (r.z < 0.0) {
        o = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
    }
    return uvec2((o * 0.5 + 0.5) * 4095.0 + 0.5);
}

// A roughness in 4 bits, up to where reflection cells end.
uint gi_reflect_rough(float roughness)
{
    return uint(clamp(roughness / NPH_RENDER_REFLECT_MAX_ROUGHNESS, 0.0, 1.0) * 15.0 + 0.5);
}

// The key of the reflection cell of the camera-relative point `p`, whose mirror direction is `d`
// (gi_reflect_oct), of roughness `rough` (gi_reflect_rough), ADR-0077: cells of the same grid,
// finer, for each direction of bins as wide as the surface's lobe, 128 a side for the smoothest.
uint64_t gi_reflect_key(GiViewRef g, vec3 p, uvec2 d, uint rough)
{
    const int level = max(gi_level(length(p)) - NPH_RENDER_REFLECT_LEVEL_SHIFT, 0);
    const vec4 origin = g.g.levels[level].origin;
    const ivec3 camera = ivec3(uvec3(g.g.levels[level].cell[0], g.g.levels[level].cell[1],
                                     g.g.levels[level].cell[2]));
    const ivec3 c = camera + ivec3(floor((p + origin.xyz) / origin.w));
    const uint m = 0x3FFFu;
    const uint coarse = rough >> 2u;
    const uvec2 bin = d >> (5u + coarse);
    return uint64_t(uint(c.x) & m) | (uint64_t(uint(c.y) & m) << 14ul) |
           (uint64_t(uint(c.z) & m) << 28ul) | (uint64_t(level) << 42ul) | (uint64_t(bin.x) << 47ul) |
           (uint64_t(bin.y) << 54ul) | (uint64_t(coarse) << 61ul) | (1ul << 63ul);
}

// What a smooth pixel leaves for the reflection's passes (ADR-0077): its mirror direction, 12 bits
// an axis of an octahedral map; its roughness, 4 bits; its specular colour, 10 bits a channel of its
// square root. The top bit is what tells a pixel with one from one without.
uvec2 gi_reflect_pack(vec3 r, float roughness, vec3 specular)
{
    const uvec2 d = gi_reflect_oct(r);
    const uvec3 t = uvec3(sqrt(clamp(specular, vec3(0.0), vec3(1.0))) * 1023.0 + 0.5);
    return uvec2(d.x | (d.y << 12u) | (gi_reflect_rough(roughness) << 24u),
                 t.x | (t.y << 10u) | (t.z << 20u) | 0x80000000u);
}

// The specular colour that a pack holds.
vec3 gi_reflect_specular(uint w)
{
    const vec3 t = vec3(float(w & 0x3FFu), float((w >> 10u) & 0x3FFu), float((w >> 20u) & 0x3FFu)) * (1.0 / 1023.0);
    return t * t;
}

// The direction that a pack holds.
vec3 gi_reflect_direction(uint w)
{
    const vec2 o = vec2(float(w & 0xFFFu), float((w >> 12u) & 0xFFFu)) * (2.0 / 4095.0) - 1.0;
    vec3 r = vec3(o, 1.0 - abs(o.x) - abs(o.y));
    if (r.z < 0.0) {
        r.xy = (1.0 - abs(r.yx)) * vec2(r.x >= 0.0 ? 1.0 : -1.0, r.y >= 0.0 ? 1.0 : -1.0);
    }
    return r;
}

// The cache's light at the surface point `p` (camera relative) whose geometric normal `n` faces the
// camera (ADR-0085): the four cells nearest it across the axis the surface faces most, weighted
// bilinearly by where `p` lies between their centres, each taken in the layer of cells the
// surface's plane crosses at its centre (so a wall or a floor blends its cells exactly, and a slope
// near enough), and weighted by how far it is trusted. A cell the cache does not hold (past the
// surface's edge, say) drops out, its weight shared among the others, so an edge does not fade to
// the sky. `w` is the trust the cells found have between them (0: none found); `far` the share of
// the light that is the many lights' far fields (ADR-0078). Where a fixed point would show the
// cells' edges, and a random point of the cell each frame (Binder et al.'s jittered hashing, which
// this replaced) the cells' differences as noise that temporal anti-aliasing must average, the
// blend is smooth and the same every frame. (One loop, inline: split into helpers, or its lookups
// shared across the subgroup, the resolve that calls it took more registers and ran slower.)
vec4 gi_surface_blend(GiViewRef g, vec3 p, vec3 n, out float far)
{
    const int level = gi_level(length(p));
    const vec4 origin = g.g.levels[level].origin;
    const ivec3 camera = gi_camera_cell(g, level);
    // The point in the level's cells, half a cell out along the normal, as the keys take it.
    const vec3 x = (p + 0.5 * origin.w * n + origin.xyz) / origin.w;
    // Turned so that z is the axis the surface faces most and x and y the two across it (as
    // swizzles: a vector indexed at run time goes to local memory).
    const vec3 an = abs(n);
    const uint k = an.x >= an.y ? (an.x >= an.z ? 0u : 2u) : (an.y >= an.z ? 1u : 2u);
    const vec3 xt = k == 0u ? x.yzx : (k == 1u ? x.zxy : x);
    const vec3 nt = k == 0u ? n.yzx : (k == 1u ? n.zxy : n);
    const vec2 across = xt.xy - 0.5;
    const vec2 base = floor(across);
    const vec2 t = across - base;
    const uint octant = gi_octant(n);
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    vec3 sum = vec3(0.0);
    float trusted = 0.0;
    float found = 0.0;
    float far_sum = 0.0;
    float lum_sum = 0.0;
    for (int c = 0; c < 4; ++c) {
        const vec2 o = vec2(float(c & 1), float(c >> 1));
        // Along the axis the surface faces, its plane at this cell's centre (|nt.z| >= 1 / sqrt 3).
        const vec2 centre = base + o + 0.5;
        const float along = xt.z - dot(nt.xy, centre - xt.xy) / nt.z;
        const ivec3 turned = ivec3(ivec2(base + o), int(floor(along)));
        const ivec3 cell = k == 0u ? turned.zxy : (k == 1u ? turned.yzx : turned);
        const uint slot = gi_find(g, gi_cell_key(camera + cell, level, octant));
        if (slot != GI_NONE) {
            const uvec4 cw = GiCellsRef(g.g.cells).c[slot];
            const vec3 e = uintBitsToFloat(cw.xyz);
            const float weight = (c & 1) != 0 ? t.x : 1.0 - t.x;
            const float bilinear = weight * ((c >> 1) != 0 ? t.y : 1.0 - t.y);
            const float wt = bilinear * min(float(gi_word_count(cw.w)) * (1.0 / 16.0), 1.0);
            sum += e * wt;
            trusted += wt;
            found += bilinear;
            const float l = dot(e, lum) * wt;
            far_sum += gi_word_far_share(cw.w) * l;
            lum_sum += l;
        }
    }
    far = lum_sum > 0.0 ? far_sum / lum_sum : 0.0;
    return trusted > 0.0 ? vec4(sum / trusted, trusted / found) : vec4(0.0);
}

// The cache's light at the surface point `p` (camera relative) of geometric normal `face` (either
// way round: the cells are keyed by the side the camera sees, as the update keys them), blended
// from the cells around it (gi_surface_blend): irradiance and trust, zero where the cache has
// none. The resolves look their pixels up with it (vis_resolve.glsl, vis_voxel_resolve.glsl), and
// the forward pass its blended surfaces' (forward_frag.glsl).
vec4 gi_surface_cell(GiViewRef g, vec3 p, vec3 face)
{
    float far;
    return gi_surface_blend(g, p, dot(face, p) > 0.0 ? -face : face, far);
}

// The same, and the light's share that is the many lights' far fields in `far` (0 where the cache
// has none).
vec4 gi_surface_cell_far(GiViewRef g, vec3 p, vec3 face, out float far)
{
    return gi_surface_blend(g, p, dot(face, p) > 0.0 ? -face : face, far);
}

// What the point `p` (camera relative) reflects along the direction `d`: the radiance of its
// reflection cell and how far it can be trusted, zero where there is none. `u`, uniform in [0, 1),
// takes a neighbouring direction now and then: temporal anti-aliasing averages them, where a
// fixed direction would show the bins' edges.
vec4 gi_reflect_cell(GiViewRef g, vec3 p, uvec2 d, uint rough, vec2 u)
{
    const uint coarse = rough >> 2u;
    const float width = float(32u << coarse);
    const uvec2 near_d = uvec2(clamp(vec2(d) + (u - 0.5) * width, vec2(0.0), vec2(4095.0)));
    const uint slot = gi_find_in(g.g.reflect_keys, g.g.reflect_bucket_mask, gi_reflect_key(g, p, near_d, rough));
    if (slot == GI_NONE) {
        return vec4(0.0);
    }
    const uvec4 c = GiCellsRef(g.g.reflect_cells).c[slot];
    // A narrow lobe's samples hardly differ: one is enough for the smoothest, four at the end.
    return vec4(uintBitsToFloat(c.xyz), min(float(gi_word_count(c.w)) / float(1u + coarse), 1.0));
}

#endif
