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

#define GI_NONE 0xFFFFFFFFu

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

// The key of the cell holding the camera-relative point `p` on a surface
// whose geometric normal is `n`. The point is taken half a cell out along
// the normal: a surface on a cell's face (a floor at y = 0, which the
// cells' powers of two align with) would otherwise fall either side of it
// as its points' rounding goes, and lookups (jittered, or another frame's
// pixels) would miss the cells the update filled.
uint64_t gi_key(GiViewRef g, vec3 p, vec3 n)
{
    const int level = gi_level(length(p));
    // Read field by field: a level holds an array (docs/build.md "Shaders").
    const vec4 origin = g.g.levels[level].origin;
    const ivec3 camera = ivec3(uvec3(g.g.levels[level].cell[0], g.g.levels[level].cell[1],
                                     g.g.levels[level].cell[2]));
    const ivec3 c = camera + ivec3(floor((p + 0.5 * origin.w * n + origin.xyz) / origin.w));
    const uint m = 0x1FFFFu;
    return uint64_t(uint(c.x) & m) | (uint64_t(uint(c.y) & m) << 17ul) |
           (uint64_t(uint(c.z) & m) << 34ul) | (uint64_t(level) << 51ul) |
           (uint64_t(gi_octant(n)) << 56ul);
}

// MurmurHash3's 64-bit finaliser (Appleby): every bit of the key moves
// every bit of the hash.
uint64_t gi_mix(uint64_t k)
{
    k ^= k >> 33ul;
    k *= 0xff51afd7ed558ccdul;
    k ^= k >> 33ul;
    k *= 0xc4ceb9fe1a85ec53ul;
    k ^= k >> 33ul;
    return k;
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

// The slot holding `key`, GI_NONE if none does. Slots freed by the resolve
// leave gaps in a bucket, so the whole bucket is read (64 bytes).
uint gi_find(GiViewRef g, uint64_t key)
{
    const uint64_t h = gi_mix(key);
    const uint base = gi_bucket(g, h);
    const uint fp = gi_fingerprint(h);
    GiKeys4Ref keys = GiKeys4Ref(g.g.keys);
    for (uint i = 0u; i < uint(NPH_RENDER_GI_BUCKET) / 4u; ++i) {
        const bvec4 hit = equal(keys.k[base / 4u + i], uvec4(fp));
        if (any(hit)) {
            return base + i * 4u + (hit.x ? 0u : (hit.y ? 1u : (hit.z ? 2u : 3u)));
        }
    }
    return GI_NONE;
}

// The slot of `key`, claimed if no slot holds it yet: its bucket's first
// free one, by compare-and-swap in order, so that two invocations claiming
// the same new key meet in the same slot. GI_NONE when the bucket is full
// (the sample is dropped). Only the update claims slots, and only the
// resolve frees them, so during the update a bucket only fills.
uint gi_claim(GiViewRef g, uint64_t key)
{
    const uint64_t h = gi_mix(key);
    const uint base = gi_bucket(g, h);
    const uint fp = gi_fingerprint(h);
    GiKeys4Ref keys4 = GiKeys4Ref(g.g.keys);
    for (uint i = 0u; i < uint(NPH_RENDER_GI_BUCKET) / 4u; ++i) {
        const bvec4 hit = equal(keys4.k[base / 4u + i], uvec4(fp));
        if (any(hit)) {
            return base + i * 4u + (hit.x ? 0u : (hit.y ? 1u : (hit.z ? 2u : 3u)));
        }
    }
    GiKeysRef keys = GiKeysRef(g.g.keys);
    for (uint i = 0u; i < uint(NPH_RENDER_GI_BUCKET); ++i) {
        const uint prev = atomicCompSwap(keys.k[base + i], 0u, fp);
        if (prev == 0u || prev == fp) {
            return base + i;
        }
    }
    return GI_NONE;
}

// A cell's irradiance (lux, rgb) and how far it can be trusted (w: 0 for
// none to 1 from 16 samples on).
vec4 gi_cell(GiViewRef g, uint slot)
{
    const uvec4 c = GiCellsRef(g.g.cells).c[slot];
    return vec4(uintBitsToFloat(c.xyz), min(float(c.w & 0xFFFFu) * (1.0 / 16.0), 1.0));
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

// A random point of the cell around `p` in the plane of normal `n` (u: two
// uniform numbers): looked up there instead of at p, a cell's irradiance
// blends into its neighbours' over frames (Binder et al.'s jittered
// hashing), where a fixed point would show the cells' edges.
vec3 gi_jitter(GiViewRef g, vec3 p, vec3 n, vec2 u)
{
    const float size = gi_cell_size(g, gi_level(length(p)));
    // A frame around n (Duff et al., "Building an Orthonormal Basis,
    // Revisited", JCGT 2017).
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float b = n.x * n.y * a;
    const vec3 t = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    const vec3 bt = vec3(b, s + n.y * n.y * a, -n.y);
    return p + size * ((u.x - 0.5) * t + (u.y - 0.5) * bt);
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

#endif
