// SPDX-License-Identifier: Apache-2.0
#pragma once

// Helpers shared by the cooker tests: temporary directories, writers for
// the image formats the tests feed the decoder (uncompressed TGA and
// flat Radiance HDR, both trivial to produce without an encoder
// library), base64 for glTF data URIs, and a generator for small glTF
// scenes.

#include <nepheline/asset/format.h>
#include <nepheline/asset/loader.h>
#include <nepheline/core/array.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/string.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/math/vec.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>
#include <nepheline/platform/platform.h>

#include <ntest/ntest.h>

#include <cook/image.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Floating-point comparisons ntest does not provide directly.
#define NTEST_ASSERT_GE_F64(a, b) NTEST_ASSERT_TRUE((f64)(a) >= (f64)(b))
#define NTEST_ASSERT_LE_F64(a, b) NTEST_ASSERT_TRUE((f64)(a) <= (f64)(b))

// ---------------------------------------------------------------------------
// Temporary directories
// ---------------------------------------------------------------------------

typedef struct TempDir {
    char dir[1024];
} TempDir;

static inline void test_temp_dir_make(TempDir *t, const char *tag)
{
    static u32 counter = 0U;
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    snprintf(t->dir, sizeof t->dir, "%s/nph_cook_%s_%llu_%u", tmp, tag,
             (unsigned long long)nph_platform_process_id(), counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(t->dir), NPH_RESULT_OK);
}

static inline void test_temp_dir_remove(TempDir *t)
{
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(t->dir), NPH_RESULT_OK);
}

static inline const char *test_path_in(const TempDir *t, const char *name, char *buf, u64 cap)
{
    NTEST_ASSERT_EQ_INT(
        nph_path_join(buf, cap, nph_str_from_cstr(t->dir), nph_str_from_cstr(name), NULL),
        NPH_RESULT_OK);
    return buf;
}

// ---------------------------------------------------------------------------
// Deterministic data
// ---------------------------------------------------------------------------

static inline u64 test_rng_next(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Peak signal-to-noise ratio in dB of two 8-bit buffers; 99 when equal.
static inline f64 test_psnr8(const u8 *a, const u8 *b, u64 count)
{
    f64 sum = 0.0;
    for (u64 i = 0; i < count; ++i) {
        const f64 d = (f64)a[i] - (f64)b[i];
        sum += d * d;
    }
    if (sum == 0.0) {
        return 99.0;
    }
    const f64 mse = sum / (f64)count;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

// ---------------------------------------------------------------------------
// Image files
// ---------------------------------------------------------------------------

// An uncompressed 32-bit TGA with a top-left origin, from RGBA texels.
// Returns the byte count written into `out` (needs 18 + w * h * 4 bytes).
static inline u64 test_write_tga(const u8 *rgba, u32 w, u32 h, u8 *out)
{
    memset(out, 0, 18U);
    out[2] = 2U; // uncompressed true colour
    out[12] = (u8)(w & 0xFFU);
    out[13] = (u8)(w >> 8U);
    out[14] = (u8)(h & 0xFFU);
    out[15] = (u8)(h >> 8U);
    out[16] = 32U;
    out[17] = 0x28U; // 8 alpha bits, top-left origin
    u8 *p = out + 18;
    for (u64 i = 0; i < (u64)w * h; ++i) {
        p[i * 4U] = rgba[i * 4U + 2U]; // BGRA on disk
        p[i * 4U + 1U] = rgba[i * 4U + 1U];
        p[i * 4U + 2U] = rgba[i * 4U];
        p[i * 4U + 3U] = rgba[i * 4U + 3U];
    }
    return 18U + (u64)w * h * 4U;
}

// A flat (not run-length encoded) Radiance HDR file from RGB floats.
// Returns the byte count (needs about 64 + w * h * 4 bytes).
static inline u64 test_write_hdr(const f32 *rgb, u32 w, u32 h, u8 *out)
{
    const int header =
        snprintf((char *)out, 64U, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %u +X %u\n", h, w);
    u8 *p = out + header;
    for (u64 i = 0; i < (u64)w * h; ++i) {
        const f32 r = rgb[i * 3U];
        const f32 g = rgb[i * 3U + 1U];
        const f32 b = rgb[i * 3U + 2U];
        const f32 v = nph_f32_max3(r, g, b);
        if (v < 1e-32F) {
            memset(p, 0, 4U);
        } else {
            int e;
            const f32 m = frexpf(v, &e);
            const f32 scale = m * 256.0F / v;
            p[0] = (u8)(r * scale);
            p[1] = (u8)(g * scale);
            p[2] = (u8)(b * scale);
            p[3] = (u8)(e + 128);
        }
        p += 4;
    }
    return (u64)header + (u64)w * h * 4U;
}

// ---------------------------------------------------------------------------
// base64 (for glTF data URIs)
// ---------------------------------------------------------------------------

static inline u64 test_base64_size(u64 n)
{
    return ((n + 2U) / 3U) * 4U;
}

static inline void test_base64(const u8 *in, u64 n, char *out)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    u64 o = 0U;
    for (u64 i = 0; i < n; i += 3U) {
        // Copied rather than read in place: callers pass the bytes of float
        // arrays, which the static analyzer cannot follow through a u8 *.
        u8 group[3] = {0U, 0U, 0U};
        memcpy(group, in + i, (size_t)(n - i < 3U ? n - i : 3U));
        const u32 b0 = group[0];
        const u32 b1 = group[1];
        const u32 b2 = group[2];
        const u32 v = (b0 << 16U) | (b1 << 8U) | b2;
        out[o++] = table[(v >> 18U) & 63U];
        out[o++] = table[(v >> 12U) & 63U];
        out[o++] = (char)(i + 1U < n ? table[(v >> 6U) & 63U] : '=');
        out[o++] = (char)(i + 2U < n ? table[v & 63U] : '=');
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// Cooked assets from a writer: every blob unpacked, as a loader would
// ---------------------------------------------------------------------------

typedef struct TestAsset {
    NphAllocator *alloc;
    void *image;
    u64 size;
    NphAssetBlob blobs[64];
    void *owned[64];
    NphAsset asset;
} TestAsset;

static inline void test_asset_from_writer(NphAllocator *alloc, NphAssetWriter *w, TestAsset *out)
{
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    NTEST_ASSERT_EQ_INT(nph_asset_writer_finish_alloc(w, alloc, &out->image, &out->size),
                        NPH_RESULT_OK);
    NphAssetFileInfo info;
    NTEST_ASSERT_EQ_INT(nph_asset_file_validate(out->image, out->size, &info), NPH_RESULT_OK);
    NTEST_ASSERT_LE_UINT(info.blob_count, NPH_ARRAY_COUNT(out->blobs));
    for (u32 i = 0; i < info.blob_count; ++i) {
        const NphAssetBlobEntry *e = &info.blobs[i];
        NTEST_ASSERT_TRUE(nph_asset_file_verify_blob(out->image, e));
        out->blobs[i].tag = e->tag;
        out->blobs[i].index = e->index;
        out->blobs[i].codec = e->codec;
        out->blobs[i].alignment = e->alignment;
        out->blobs[i].size = e->raw_size;
        out->blobs[i].offset = e->offset;
        void *raw = NPH_ALLOC(alloc, nph_max_u64(e->raw_size, 1U), e->alignment);
        NTEST_ASSERT_NOT_NULL(raw);
        NTEST_ASSERT_EQ_INT(nph_asset_file_unpack_blob(out->image, e, raw), NPH_RESULT_OK);
        out->blobs[i].data = raw;
        out->owned[i] = raw;
    }
    out->asset.header = info.header;
    out->asset.blobs = out->blobs;
    out->asset.blob_count = info.blob_count;
    out->asset.path = NPH_STR("memory");
}

static inline void test_asset_free(TestAsset *a)
{
    for (u32 i = 0; i < a->asset.blob_count; ++i) {
        NPH_FREE(a->alloc, a->owned[i], nph_max_u64(a->blobs[i].size, 1U), a->blobs[i].alignment);
    }
    if (a->image != NULL) {
        NPH_FREE(a->alloc, a->image, a->size, NPH_ASSET_BLOB_MAX_ALIGN);
    }
    memset(a, 0, sizeof *a);
}

// ---------------------------------------------------------------------------
// A generated glTF scene
// ---------------------------------------------------------------------------

// Writes "<dir>/<name>.gltf": a cube (positions, normals, UVs, no
// tangents, 24 vertices, 36 indices) and a quad with vertex colours and
// no normals, two materials (the first with a base colour TGA and a
// normal-map TGA as data URIs, the second plain), nodes root -> {cube
// node, quad node (scaled), camera node, light node}, one perspective
// camera, and one point light. With `external`, the buffer and the base
// colour image are written as files next to the document instead.
static inline void test_write_gltf(const TempDir *dir, const char *name, bool external)
{
    NphAllocator *alloc = nph_allocator_system();
    // --- geometry -----------------------------------------------------------
    f32 cube_pos[24 * 3];
    f32 cube_nrm[24 * 3];
    f32 cube_uv[24 * 2];
    u16 cube_idx[36];
    static const f32 normals[6][3] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                      {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
    static const f32 tangents[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1},
                                       {0, 0, 1}, {1, 0, 0},  {1, 0, 0}};
    for (u32 face = 0; face < 6U; ++face) {
        const f32 *n = normals[face];
        const f32 *t = tangents[face];
        const f32 b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2],
                          n[0] * t[1] - n[1] * t[0]};
        for (u32 corner = 0; corner < 4U; ++corner) {
            const f32 u = (corner == 1U || corner == 2U) ? 1.0F : 0.0F;
            const f32 v = corner >= 2U ? 1.0F : 0.0F;
            const f32 su = u * 2.0F - 1.0F;
            const f32 sv = v * 2.0F - 1.0F;
            const u32 i = face * 4U + corner;
            for (u32 c = 0; c < 3U; ++c) {
                cube_pos[(u64)i * 3U + c] = n[c] + t[c] * su + b[c] * sv;
                cube_nrm[(u64)i * 3U + c] = n[c];
            }
            cube_uv[(u64)i * 2U] = u;
            cube_uv[(u64)i * 2U + 1U] = v;
        }
        const u16 base = (u16)(face * 4U);
        u16 *idx = &cube_idx[(u64)face * 6U];
        idx[0] = base;
        idx[1] = (u16)(base + 1U);
        idx[2] = (u16)(base + 2U);
        idx[3] = base;
        idx[4] = (u16)(base + 2U);
        idx[5] = (u16)(base + 3U);
    }
    static const f32 quad_pos[4 * 3] = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
    static const f32 quad_col[4 * 4] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1};
    static const u16 quad_idx[6] = {0, 2, 1, 0, 3, 2};
    // Buffer layout: cube pos | cube nrm | cube uv | cube idx | quad pos | quad col | quad idx
    u8 buffer[4096];
    u32 off = 0U;
    const u32 o_cpos = off;
    memcpy(buffer + off, cube_pos, sizeof cube_pos);
    off += (u32)sizeof cube_pos;
    const u32 o_cnrm = off;
    memcpy(buffer + off, cube_nrm, sizeof cube_nrm);
    off += (u32)sizeof cube_nrm;
    const u32 o_cuv = off;
    memcpy(buffer + off, cube_uv, sizeof cube_uv);
    off += (u32)sizeof cube_uv;
    const u32 o_cidx = off;
    memcpy(buffer + off, cube_idx, sizeof cube_idx);
    off += (u32)sizeof cube_idx;
    const u32 o_qpos = off;
    memcpy(buffer + off, quad_pos, sizeof quad_pos);
    off += (u32)sizeof quad_pos;
    const u32 o_qcol = off;
    memcpy(buffer + off, quad_col, sizeof quad_col);
    off += (u32)sizeof quad_col;
    const u32 o_qidx = off;
    memcpy(buffer + off, quad_idx, sizeof quad_idx);
    off += (u32)sizeof quad_idx;
    const u32 buffer_len = off;

    // --- images: an 8x8 checker base colour and a flat normal map --------
    u8 base_rgba[8 * 8 * 4];
    u8 normal_rgba[4 * 4 * 4];
    for (u32 y = 0; y < 8U; ++y) {
        for (u32 x = 0; x < 8U; ++x) {
            const bool on = ((x / 2U) + (y / 2U)) % 2U == 0U;
            u8 *p = &base_rgba[((u64)y * 8U + x) * 4U];
            p[0] = on ? 220U : 30U;
            p[1] = on ? 160U : 60U;
            p[2] = on ? 40U : 200U;
            p[3] = 255U;
        }
    }
    for (u32 i = 0; i < 16U; ++i) {
        normal_rgba[(u64)i * 4U] = 128U;
        normal_rgba[(u64)i * 4U + 1U] = 128U;
        normal_rgba[(u64)i * 4U + 2U] = 255U;
        normal_rgba[i * 4U + 3U] = 255U;
    }
    u8 base_tga[18 + 8 * 8 * 4];
    u8 normal_tga[18 + 4 * 4 * 4];
    const u64 base_tga_len = test_write_tga(base_rgba, 8U, 8U, base_tga);
    const u64 normal_tga_len = test_write_tga(normal_rgba, 4U, 4U, normal_tga);

    char buffer_uri[8192];
    char base_uri[1024];
    char normal_uri[512];
    if (external) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s.bin", dir->dir, name);
        NTEST_ASSERT_EQ_INT(nph_file_write_all(path, buffer, buffer_len), NPH_RESULT_OK);
        snprintf(buffer_uri, sizeof buffer_uri, "%s.bin", name);
        snprintf(path, sizeof path, "%s/%s_base.tga", dir->dir, name);
        NTEST_ASSERT_EQ_INT(nph_file_write_all(path, base_tga, base_tga_len), NPH_RESULT_OK);
        snprintf(base_uri, sizeof base_uri, "%s_base.tga", name);
    } else {
        strcpy(buffer_uri, "data:application/octet-stream;base64,");
        test_base64(buffer, buffer_len, buffer_uri + strlen(buffer_uri));
        strcpy(base_uri, "data:image/tga;base64,");
        test_base64(base_tga, base_tga_len, base_uri + strlen(base_uri));
    }
    strcpy(normal_uri, "data:image/tga;base64,");
    test_base64(normal_tga, normal_tga_len, normal_uri + strlen(normal_uri));

    // --- the document --------------------------------------------------------
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    NTEST_ASSERT_EQ_INT(
        nph_sb_appendf(
            &sb,
            "{\n"
            "\"asset\":{\"version\":\"2.0\",\"generator\":\"nepheline tests\"},\n"
            "\"extensionsUsed\":[\"KHR_lights_punctual\"],\n"
            "\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[{\"type\":\"point\",\"color\":[1,"
            "0.9,0.8],\"intensity\":50,\"range\":10,\"name\":\"lamp\"}]}},\n"
            "\"scene\":0,\n"
            "\"scenes\":[{\"name\":\"main\",\"nodes\":[0]}],\n"
            "\"nodes\":[\n"
            " {\"name\":\"root\",\"children\":[1,2,3,4],\"translation\":[0,1,0]},\n"
            " {\"name\":\"cube\",\"mesh\":0,\"rotation\":[0,0.7071068,0,0.7071068]},\n"
            " {\"name\":\"quad\",\"mesh\":1,\"scale\":[2,2,2],\"translation\":[0,-1,0]},\n"
            " {\"name\":\"camera\",\"camera\":0,\"matrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,2,5,1]},\n"
            " {\"name\":\"light\",\"extensions\":{\"KHR_lights_punctual\":{\"light\":0}},"
            "\"translation\":[3,3,3]}\n"
            "],\n"
            "\"cameras\":[{\"type\":\"perspective\",\"perspective\":{\"yfov\":0.8,\"znear\":0.1,"
            "\"aspectRatio\":1.5}}],\n"
            "\"meshes\":[\n"
            " {\"name\":\"Cube\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
            "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]},\n"
            " {\"name\":\"Quad\",\"primitives\":[{\"attributes\":{\"POSITION\":4,\"COLOR_0\":5},"
            "\"indices\":6,\"material\":1}]}\n"
            "],\n"
            "\"materials\":[\n"
            " {\"name\":\"Checker\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
            "\"metallicFactor\":0.2,\"roughnessFactor\":0.7},\"normalTexture\":{\"index\":1,"
            "\"scale\":0.5},\"doubleSided\":true},\n"
            " {\"name\":\"Plain\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.4,0.8,1],"
            "\"metallicFactor\":0,\"roughnessFactor\":0.3},\"alphaMode\":\"MASK\",\"alphaCutoff\":"
            "0.4}\n"
            "],\n"
            "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":1}],\n"
            "\"samplers\":[{\"magFilter\":9728,\"wrapS\":33071,\"wrapT\":33648}],\n"
            "\"images\":[{\"name\":\"checker\",\"uri\":\"%s\"},{\"name\":\"flat_normal\",\"uri\":"
            "\"%s\"}],\n",
            base_uri, normal_uri),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(
        nph_sb_appendf(
            &sb,
            "\"accessors\":[\n"
            " {\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\",\"min\":[-1,-"
            "1,-1],\"max\":[1,1,1]},\n"
            " {\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},\n"
            " {\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},\n"
            " {\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"},\n"
            " {\"bufferView\":4,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-1,0,"
            "-1],\"max\":[1,0,1]},\n"
            " {\"bufferView\":5,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},\n"
            " {\"bufferView\":6,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}\n"
            "],\n"
            "\"bufferViews\":[\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u},\n"
            " {\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}\n"
            "],\n"
            "\"buffers\":[{\"byteLength\":%u,\"uri\":\"%s\"}]\n"
            "}\n",
            o_cpos, (u32)sizeof cube_pos, o_cnrm, (u32)sizeof cube_nrm, o_cuv, (u32)sizeof cube_uv,
            o_cidx, (u32)sizeof cube_idx, o_qpos, (u32)sizeof quad_pos, o_qcol,
            (u32)sizeof quad_col, o_qidx, (u32)sizeof quad_idx, buffer_len, buffer_uri),
        NPH_RESULT_OK);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.gltf", dir->dir, name);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, sb.data, sb.len), NPH_RESULT_OK);
    nph_sb_free(&sb);
}

// ---------------------------------------------------------------------------
// A partitioned grove (the world partition's cells, cook/cells.h)
// ---------------------------------------------------------------------------

// Writes the grove, a glTF document at `path`: a tuft of 200 needles (thin
// closed prisms, 0.3 m long, in a cube of 1.5 m round its origin), placed
// twice in each of four squares of 8 m from the origin on x and z (one cell
// each at an edge of 8 m), and a ground of 40 m under them (no cell's: past
// half an edge).
static inline void test_write_grove(const char *path)
{
    NphAllocator *a = nph_allocator_system();
    NPH_ARRAY(f32) pos;
    NphArrayU32 idx;
    NPH_ARRAY_INIT(&pos, a);
    NPH_ARRAY_INIT(&idx, a);
    u64 rng = 0x9E3779B97F4A7C15ULL;
    for (u32 i = 0; i < 200U; ++i) {
        f32 c[3];
        f32 d[3];
        for (u32 k = 0; k < 3U; ++k) {
            c[k] = (f32)((f64)(test_rng_next(&rng) >> 11U) / 9007199254740992.0 * 1.5 - 0.75);
            d[k] = (f32)((f64)(test_rng_next(&rng) >> 11U) / 9007199254740992.0 * 2.0 - 1.0);
        }
        const NphVec3 axis = nph_vec3_normalize_or(nph_vec3(d[0], d[1], d[2]), nph_vec3_unit_y());
        const NphVec3 t = nph_vec3_normalize(nph_vec3_perpendicular(axis));
        const NphVec3 b = nph_vec3_cross(axis, t);
        const u32 base = pos.len / 3U;
        for (u32 end = 0; end < 2U; ++end) {
            const NphVec3 at = nph_vec3_add(nph_vec3(c[0], c[1], c[2]),
                                            nph_vec3_scale(axis, end == 0U ? -0.15F : 0.15F));
            for (u32 k = 0; k < 3U; ++k) {
                const f32 phi = 2.0F * NPH_PI * (f32)k / 3.0F;
                const NphVec3 p =
                    nph_vec3_add(at, nph_vec3_add(nph_vec3_scale(t, 0.01F * nph_f32_cos(phi)),
                                                  nph_vec3_scale(b, 0.01F * nph_f32_sin(phi))));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&pos, p.x));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&pos, p.y));
                NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&pos, p.z));
            }
        }
        const u32 q[24] = {base,      base + 2U, base + 1U, base + 3U, base + 4U, base + 5U,
                           base,      base + 1U, base + 4U, base,      base + 4U, base + 3U,
                           base + 1U, base + 2U, base + 5U, base + 1U, base + 5U, base + 4U,
                           base + 2U, base,      base + 3U, base + 2U, base + 3U, base + 5U};
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH_N(&idx, q, 24U));
    }
    // The ground: a quad from -12 to 28 on x and z.
    const f32 ground[12] = {-12.0F, 0.0F, -12.0F, 28.0F,  0.0F, -12.0F,
                            28.0F,  0.0F, 28.0F,  -12.0F, 0.0F, 28.0F};
    const u32 ground_idx[6] = {0U, 2U, 1U, 0U, 3U, 2U};
    const u64 tuft_pos = (u64)pos.len * 4U;
    const u64 tuft_idx = (u64)idx.len * 4U;
    const u64 total = tuft_pos + tuft_idx + sizeof ground + sizeof ground_idx;
    u8 *bytes = NPH_ALLOC_ARRAY(a, u8, total);
    NTEST_ASSERT_NOT_NULL(bytes);
    memcpy(bytes, pos.data, (size_t)tuft_pos);
    memcpy(bytes + tuft_pos, idx.data, (size_t)tuft_idx);
    memcpy(bytes + tuft_pos + tuft_idx, ground, sizeof ground);
    memcpy(bytes + tuft_pos + tuft_idx + sizeof ground, ground_idx, sizeof ground_idx);
    char *b64 = NPH_ALLOC_ARRAY(a, char, test_base64_size(total) + 1U);
    NTEST_ASSERT_NOT_NULL(b64);
    test_base64(bytes, total, b64);
    f32 lo[3] = {1e30F, 1e30F, 1e30F};
    f32 hi[3] = {-1e30F, -1e30F, -1e30F};
    for (u32 i = 0; i < pos.len; ++i) {
        lo[i % 3U] = nph_f32_min(lo[i % 3U], pos.data[i]);
        hi[i % 3U] = nph_f32_max(hi[i % 3U], pos.data[i]);
    }
    static const f32 at[8][2] = {{2.0F, 2.0F},  {5.0F, 5.0F},  {10.0F, 2.0F},  {13.0F, 6.0F},
                                 {2.0F, 10.0F}, {6.0F, 13.0F}, {10.0F, 10.0F}, {14.0F, 14.0F}};
    NphStrBuilder sb;
    nph_sb_init(&sb, a);
    NTEST_ASSERT_EQ_INT(nph_sb_append_cstr(&sb, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                                                "\"scenes\":[{\"nodes\":[0,1,2,3,4,5,6,7,8]}],"
                                                "\"nodes\":["),
                        NPH_RESULT_OK);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_sb_appendf(&sb,
                                           "{\"mesh\":0,\"translation\":[%g,1,%g],\"rotation\":"
                                           "[0,%g,0,%g]},",
                                           (f64)at[i][0], (f64)at[i][1], sin(0.35 * (f64)i),
                                           cos(0.35 * (f64)i)),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_INT(
        nph_sb_appendf(
            &sb,
            "{\"mesh\":1}],\"meshes\":[{\"name\":\"tuft\",\"primitives\":[{\"attributes\":{"
            "\"POSITION\":0},\"indices\":1}]},{\"name\":\"ground\",\"primitives\":[{\"attributes\":"
            "{"
            "\"POSITION\":2},\"indices\":3}]}],\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[%g,%"
            "g,"
            "%g],\"max\":[%g,%g,%g]},"
            "{\"bufferView\":1,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-12,0,"
            "-12],\"max\":[28,0,28]},"
            "{\"bufferView\":3,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":%llu},{\"buffer\":0,\"byteOffset\":%llu,"
            "\"byteLength\":%llu},{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":48},{\"buffer\":"
            "0,"
            "\"byteOffset\":%llu,\"byteLength\":24}],\"buffers\":[{\"byteLength\":%llu,\"uri\":"
            "\"data:application/octet-stream;base64,%s\"}]}",
            pos.len / 3U, (f64)lo[0], (f64)lo[1], (f64)lo[2], (f64)hi[0], (f64)hi[1], (f64)hi[2],
            idx.len, (unsigned long long)tuft_pos, (unsigned long long)tuft_pos,
            (unsigned long long)tuft_idx, (unsigned long long)(tuft_pos + tuft_idx),
            (unsigned long long)(tuft_pos + tuft_idx + sizeof ground), (unsigned long long)total,
            b64),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_file_write_all(path, sb.data, sb.len), NPH_RESULT_OK);
    nph_sb_free(&sb);
    NPH_FREE_ARRAY(a, char, b64, test_base64_size(total) + 1U);
    NPH_FREE_ARRAY(a, u8, bytes, total);
    NPH_ARRAY_FREE(&pos);
    NPH_ARRAY_FREE(&idx);
}
