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
NPH_BUFFER_REF(GiCellKeysRef) { uvec4 k[]; };
NPH_BUFFER_REF(GiHeldRef) { vec4 h[]; };

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
    return min(count, (1u << NPH_RENDER_GI_COUNT_BITS) - 1u) | (age << NPH_RENDER_GI_COUNT_BITS) |
           (far << (NPH_RENDER_GI_COUNT_BITS + 8));
}

// An irradiance cell's moments' z (NphRenderGiView.moments): the frames its variance has seen
// (5 bits), the frames what it is held to be still follows its mean after its light changed (3
// bits: NPH_RENDER_GI_FOLLOW_FRAMES at most), and its sample count to its history, which may
// be more than its word counts (ADR-0091).
uint gi_moments_frames(uint z)
{
    return z & 0x1Fu;
}

uint gi_moments_follow(uint z)
{
    return (z >> 5u) & 7u;
}

uint gi_moments_history(uint z)
{
    return z >> 8u;
}

#define GI_HISTORY_MOST 0xFFFFFFu

uint gi_moments_z(uint frames, uint follow, uint history)
{
    return frames | (follow << 5u) | (min(history, GI_HISTORY_MOST) << 8u);
}

// The level of a point `dist` metres from the camera: floor(log2(dist)),
// the float's exponent, biased and clamped, to `lowest` at least.
int gi_level_from(float dist, int lowest)
{
    const int e = (floatBitsToInt(max(dist, 1e-30)) >> 23) - 127;
    return clamp(e + NPH_RENDER_GI_LEVEL_BIAS, lowest, NPH_RENDER_GI_LEVELS - 1);
}

// An irradiance cell's level for a point `dist` metres from the camera.
int gi_level(float dist)
{
    return gi_level_from(dist, NPH_RENDER_GI_LEVEL_MIN);
}

// The size of a cell at level `level` (metres).
float gi_cell_size(GiViewRef g, int level)
{
    return g.g.levels[level].origin.w;
}

// The octant a normal faces, its components' signs (one a thousandth below
// zero still counts as zero, so a face whose normal lies in a plane of the
// axes does not flicker between octants). (The axis it faces most was tried
// as a part of the key too, ADR-0090, to keep a floor's light out of the cells
// of the walls that share its octant: a wall that stands at 45 degrees to two
// axes, as a street's may, then broke into its bricks, each triangle's cells
// its own.)
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
// whose geometric normal is `n`, at the level its distance gives (gi_key) or
// at `level` (gi_key_at). The point is taken half a cell out along
// the normal: a surface on a cell's face (a floor at y = 0, which the
// cells' powers of two align with) would otherwise fall either side of it
// as its points' rounding goes, and lookups (blended, or another frame's
// pixels) would miss the cells the update filled.
uint64_t gi_key_at(GiViewRef g, vec3 p, vec3 n, int level)
{
    const vec4 origin = g.g.levels[level].origin;
    const ivec3 c =
        gi_camera_cell(g, level) + ivec3(floor((p + 0.5 * origin.w * n + origin.xyz) / origin.w));
    return gi_cell_key(c, level, gi_octant(n));
}

uint64_t gi_key(GiViewRef g, vec3 p, vec3 n)
{
    return gi_key_at(g, p, n, gi_level(length(p)));
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
// `created` tells whether this invocation took a free slot for the key: of invocations claiming
// the same new key in a frame, one does. A new key takes a free slot among the bucket's first
// `within` only.
uint gi_claim_within_in(uint64_t keys_at, uint bucket_mask, uint64_t key, uint within, out bool created)
{
    created = false;
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
    for (uint i = 0u; i < within; ++i) {
        const uint prev = atomicCompSwap(keys.k[base + i], 0u, fp);
        if (prev == 0u || prev == fp) {
            created = prev == 0u;
            return base + i;
        }
    }
    return GI_NONE;
}

uint gi_claim_new_in(uint64_t keys_at, uint bucket_mask, uint64_t key, out bool created)
{
    return gi_claim_within_in(keys_at, bucket_mask, key, uint(NPH_RENDER_GI_BUCKET), created);
}

uint gi_claim_in(uint64_t keys_at, uint bucket_mask, uint64_t key)
{
    bool created;
    return gi_claim_new_in(keys_at, bucket_mask, key, created);
}

uint gi_claim(GiViewRef g, uint64_t key)
{
    return gi_claim_in(g.g.keys, g.g.bucket_mask, key);
}

uint gi_claim_new(GiViewRef g, uint64_t key, out bool created)
{
    return gi_claim_new_in(g.g.keys, g.g.bucket_mask, key, created);
}

// The slot of an irradiance cell's `key` for a sample to be added to (GI_NONE: its bucket is
// full), its whole key left with the slot by whoever takes it, for the filter (ADR-0090), and
// nothing yet of what the filter keeps beside it. A new cell takes a free slot among its
// bucket's first `within`.
//
// Where none of those is free, the one no sample has reached for longest gives its slot up
// (ADR-0091), from NPH_RENDER_GI_EVICT_AGE frames on: a camera at a run leaves cells behind
// faster than their age frees them, and a table full of what the frames have left had no slot
// for what they see. Whoever swaps the old cell's word for none has the slot. The others,
// whose swaps find the word changed, look again: several of a frame's rays land in a new cell,
// and all but the first find it made (or take the next oldest: where two make the same cell
// at once, the frame's samples are split between two slots, and the one the lookups do not
// find ages away). A cell with a sample this frame keeps its slot. An invocation that found
// the old cell before the swap may still add its sample to the new one: one sample of another
// place, in a cell whose first samples count for little (gi_filter.comp).
uint gi_claim_cell_within(GiViewRef g, uint64_t key, uint within)
{
    bool created;
    uint slot = gi_claim_within_in(g.g.keys, g.g.bucket_mask, key, within, created);
    // (Only for a cell that may take any slot of its bucket, the frame's own: the cells the
    // rays make where they land, which keep to a part of it, take no cell's slot.)
    if (slot == GI_NONE && within == uint(NPH_RENDER_GI_BUCKET)) {
        const uint64_t h = gi_mix(key);
        const uint base = gi_bucket(g, h);
        GiCellsRef raw = GiCellsRef(g.g.raw_cells);
        uint kept = 0u; // the slots whose cells have a sample this frame, a bit each
        for (uint attempt = 0u; attempt < 4u && slot == GI_NONE; ++attempt) {
            if (attempt != 0u) {
                slot = gi_find(g, key);
                if (slot != GI_NONE) {
                    break; // made meanwhile
                }
            }
            uint oldest = GI_NONE;
            uint oldest_word = 0u;
            uint oldest_age = uint(NPH_RENDER_GI_EVICT_AGE) - 1u;
            for (uint i = 0u; i < within; ++i) {
                const uint word = raw.c[base + i].w;
                const uint age = gi_word_age(word);
                if (age > oldest_age && (kept >> i & 1u) == 0u) {
                    oldest = base + i;
                    oldest_word = word;
                    oldest_age = age;
                }
            }
            if (oldest == GI_NONE) {
                return GI_NONE;
            }
            if (GiSumsRef(g.g.sums).s[oldest].w != 0u) {
                kept |= 1u << (oldest - base);
                continue;
            }
            if (atomicCompSwap(raw.c[oldest].w, oldest_word, 0u) != oldest_word) {
                continue;
            }
            GiKeysRef(g.g.keys).k[oldest] = gi_fingerprint(h);
            raw.c[oldest].xyz = uvec3(0u);
            GiMomentsRef(g.g.moments).m[oldest] = uvec4(0u);
            GiHeldRef(g.g.held_before).h[oldest] = vec4(0.0);
            GiCellsRef(g.g.cells).c[oldest] = uvec4(0u);
            slot = oldest;
            created = true;
        }
        if (slot == GI_NONE) {
            return GI_NONE;
        }
    }
    if (created) {
        GiCellKeysRef records = GiCellKeysRef(g.g.cell_keys);
        records.k[2u * slot] = uvec4(uint(key), uint(key >> 32ul), 0u, 0u);
        records.k[2u * slot + 1u] = uvec4(0u);
    }
    return slot;
}

uint gi_claim_cell(GiViewRef g, uint64_t key)
{
    return gi_claim_cell_within(g, key, uint(NPH_RENDER_GI_BUCKET));
}

// A cell's irradiance (lux, rgb) and whether it can be trusted (w: 1 from its first sample on, 0
// with none). A young cell's few samples are noisy, but no worse a guess than the environment's
// irradiance, which a trust ramp up to 16 samples once mixed in: many times what arrives in shade,
// it lit every new cell's surface while the camera moved (ADR-0087).
vec4 gi_cell(GiViewRef g, uint slot)
{
    const uvec4 c = GiCellsRef(g.g.cells).c[slot];
    return vec4(uintBitsToFloat(c.xyz), min(float(gi_word_count(c.w)), 1.0));
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

// The share of what a smooth pixel reflects that its ray gives, where each takes a ray a frame
// (ADR-0092), by its roughness as gi_reflect_rough keeps it: all of it up to
// NPH_RENDER_REFLECT_RAY_ROUGHNESS, none at NPH_RENDER_REFLECT_MAX_ROUGHNESS, where the surfaces
// that take no ray begin. The rest is what its irradiance cell has gathered, as theirs is.
float gi_reflect_ray_share(uint rough)
{
    const float roughness = float(rough) * (NPH_RENDER_REFLECT_MAX_ROUGHNESS / 15.0);
    return clamp((NPH_RENDER_REFLECT_MAX_ROUGHNESS - roughness) /
                     (NPH_RENDER_REFLECT_MAX_ROUGHNESS - NPH_RENDER_REFLECT_RAY_ROUGHNESS),
                 0.0, 1.0);
}

// The key of the reflection cell of the camera-relative point `p` on a surface facing `n` (towards
// the camera), whose mirror direction is `d` (gi_reflect_oct), of roughness `rough`
// (gi_reflect_rough), ADR-0077: cells of the same grid, finer, for each direction of bins as wide
// as the surface's lobe, 128 a side for the smoothest. The point is taken half a cell out along the
// normal, as the irradiance's keys take theirs (gi_key; ADR-0086): a surface on a cell's face (a
// floor at y = 0) would otherwise fall either side of it as its points' rounding goes, and each
// frame's jitter rounds them anew.
uint64_t gi_reflect_key(GiViewRef g, vec3 p, vec3 n, uvec2 d, uint rough)
{
    const int level = max(gi_level_from(length(p), 0) - NPH_RENDER_REFLECT_LEVEL_SHIFT, 0);
    const vec4 origin = g.g.levels[level].origin;
    const ivec3 camera = ivec3(uvec3(g.g.levels[level].cell[0], g.g.levels[level].cell[1],
                                     g.g.levels[level].cell[2]));
    const ivec3 c = camera + ivec3(floor((p + 0.5 * origin.w * n + origin.xyz) / origin.w));
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

// What a smooth pixel's ray leaves for reflect_gather.comp (ADR-0089): the light it met, pre-exposed,
// as three 9-bit mantissas under one 5-bit exponent (the layout of
// VK_FORMAT_E5B9G9R9_UFLOAT_PACK32), and as two half floats how far the ray went and how far the
// pixel is from the camera, the second above zero: a word of zero is a pixel with no ray.
uvec2 gi_reflect_light_pack(vec3 light, float ray, float distance)
{
    const vec3 c = clamp(light, vec3(0.0), vec3(65408.0));
    const float top = max(max(c.r, c.g), max(c.b, 1e-12));
    int e = max(-16, int(floor(log2(top)))) + 16;
    float unit = exp2(float(e - 24));
    if (uint(top / unit + 0.5) == 512u) {
        unit *= 2.0;
        e += 1;
    }
    const uvec3 m = uvec3(c / unit + 0.5);
    return uvec2((uint(e) << 27u) | (m.b << 18u) | (m.g << 9u) | m.r,
                 packHalf2x16(vec2(min(ray, 60000.0), clamp(distance, 1e-3, 60000.0))));
}

// The light that a pack holds.
vec3 gi_reflect_light(uint w)
{
    return vec3(float(w & 0x1FFu), float((w >> 9u) & 0x1FFu), float((w >> 18u) & 0x1FFu)) *
           exp2(float(int(w >> 27u) - 24));
}

// The cache's light at the surface point `p` (camera relative) whose geometric normal `n` faces the
// camera (ADR-0085): the four cells nearest it across the axis the surface faces most, weighted
// bilinearly by where `p` lies between their centres, each taken in the layer of cells the
// surface's plane crosses at its centre (so a wall or a floor blends its cells exactly, and a slope
// near enough), and weighted by how far it is trusted. A cell the cache does not hold (past the
// surface's edge, say) drops out, its weight shared among the others, so an edge does not fade to
// the sky. `w` is 1 where a cell found holds a sample, 0 where none does (gi_cell); `far` the share of
// the light that is the many lights' far fields (ADR-0078). Where a fixed point would show the
// cells' edges, and a random point of the cell each frame (Binder et al.'s jittered hashing, which
// this replaced) the cells' differences as noise that temporal anti-aliasing must average, the
// blend is smooth and the same every frame. (One loop, inline: split into helpers, or its lookups
// shared across the subgroup, the resolve that calls it took more registers and ran slower.)
vec4 gi_surface_blend(GiViewRef g, vec3 p, vec3 n, out float far)
{
    const int own_level = gi_level(length(p));
    // Turned so that z is the axis the surface faces most and x and y the two across it (as
    // swizzles: a vector indexed at run time goes to local memory).
    const vec3 an = abs(n);
    const uint k = an.x >= an.y ? (an.x >= an.z ? 0u : 2u) : (an.y >= an.z ? 1u : 2u);
    const vec3 nt = k == 0u ? n.yzx : (k == 1u ? n.zxy : n);
    const uint octant = gi_octant(n);
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    vec3 sum = vec3(0.0);
    float trusted = 0.0;
    float far_sum = 0.0;
    float lum_sum = 0.0;
    // The point's own level's cells; and where none of the four is held to be anything yet (a
    // surface the frames have just come to, or one seen at so low an angle that few of its cells
    // have had a ray), the next level's, those cells' parents, and theirs, which the same
    // samples and the rays that met the surface before have filled (ADR-0090, ADR-0091). Where
    // those hold nothing either the caller takes the environment's light, which indoors is many
    // times the room's: white patches wherever a view turned to something new. (The parents'
    // four, not the point's one parent: that one is as often not there yet.)
    for (int up = 0; up <= NPH_RENDER_GI_ABOVE && trusted <= 0.0; ++up) {
        const int level = min(own_level + up, NPH_RENDER_GI_LEVELS - 1);
        const vec4 origin = g.g.levels[level].origin;
        const ivec3 camera = gi_camera_cell(g, level);
        // The point in the level's cells, half a cell out along the normal, as the keys take it.
        const vec3 x = (p + 0.5 * origin.w * n + origin.xyz) / origin.w;
        const vec3 xt = k == 0u ? x.yzx : (k == 1u ? x.zxy : x);
        const vec2 across = xt.xy - 0.5;
        const vec2 base = floor(across);
        const vec2 t = across - base;
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
                const float wt = bilinear * min(float(gi_word_count(cw.w)) *
                                                    (1.0 / float(NPH_RENDER_GI_TRUSTED)),
                                                1.0);
                sum += e * wt;
                trusted += wt;
                const float l = dot(e, lum) * wt;
                far_sum += gi_word_far_share(cw.w) * l;
                lum_sum += l;
            }
        }
    }
    far = lum_sum > 0.0 ? far_sum / lum_sum : 0.0;
    // Trusted wholly where any cell has a sample (ADR-0087): its weight among the four is its own
    // trust, so a young cell counts for little beside settled neighbours, but alone it gives the
    // light, not the environment's (gi_cell).
    return trusted > 0.0 ? vec4(sum / trusted, 1.0) : vec4(0.0);
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

#endif
