// SPDX-License-Identifier: Apache-2.0
// Cooker benchmarks (docs/cook.md): the block encoders, the texture cook,
// the mesh pipeline's steps, and the glTF importer, on generated inputs so
// every run measures the same work.
//
//   bc7_encode_opaque      BC7 (the default modes) of a 128 x 128 photograph-like
//                          image: smooth shading, hard-edged discs, noise (ns per 4x4 block)
//   bc7_encode_alpha       the same image with a smooth alpha and a hard cut-out
//   bc7_decode             decode the opaque image's blocks
//   bc5_encode_normals     BC5 of a normal map's X and Y
//   bc4_encode_mask        BC4 of a one-channel mask with hard-edged squares
//   bc6h_encode_hdr        BC6H of an HDR image spanning 2^-5 .. 2^5
//   texture_cook_256       cook_texture of a 256 x 256 colour image on one thread: the
//                          linear working copy, the mip chain, BC7, LZ (ns per base texel)
//   texture_cook_256_jobs  the same with the block rows on the job system
//   mesh_weld              weld a 131072-triangle grid stored as three vertices per
//                          triangle (ns per triangle, as are the mesh rows below)
//   mesh_normals_smooth    area-weighted normals of the welded grid
//   mesh_tangents          MikkTSpace tangents of the welded grid
//   mesh_optimize          vertex cache, overdraw, and fetch order of the grid with its
//                          triangles and vertices shuffled
//   mesh_meshlets          the meshlets of the optimised grid
//   mesh_process           the whole pipeline from the shuffled grid, written with LZ HIGH
//   gltf_import_glb        cook_gltf_load_memory of an 8-mesh, 7 MiB GLB (ns per KiB)
//
// The labels carry what the work achieved: PSNR (BC6H: the mean relative
// error), vertex counts, ACMR before and after, meshlet counts.

#include <nepheline/asset/format.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/texture.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/log.h>
#include <nepheline/core/string.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>

#include <nbench/nbench.h>

#include <cook/bc.h>
#include <cook/gltf.h>
#include <cook/mesh.h>
#include <cook/texture.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The sizes are literals (checked below): tidy's implicit-widening check
// flags a product of u32 macros used where 64 bits are expected.
#define IMAGE_EXTENT   128U
#define IMAGE_BLOCKS   1024U // (IMAGE_EXTENT / 4) squared
#define TEXTURE_EXTENT 256U
#define GRID_QUADS     256U    // per side
#define GRID_VERTICES  66049U  // (GRID_QUADS + 1) squared
#define GRID_TRIANGLES 131072U // two per quad
#define GLB_MESHES     8U
#define GLB_QUADS      128U   // per side and mesh
#define GLB_VERTICES   16641U // (GLB_QUADS + 1) squared
#define GLB_TRIANGLES  32768U // two per quad
NPH_STATIC_ASSERT(IMAGE_BLOCKS == (IMAGE_EXTENT / 4U) * (IMAGE_EXTENT / 4U), "image blocks");
NPH_STATIC_ASSERT(GRID_VERTICES == (GRID_QUADS + 1U) * (GRID_QUADS + 1U), "grid vertices");
NPH_STATIC_ASSERT(GRID_TRIANGLES == 2U * GRID_QUADS * GRID_QUADS, "grid triangles");
NPH_STATIC_ASSERT(GLB_VERTICES == (GLB_QUADS + 1U) * (GLB_QUADS + 1U), "GLB vertices");
NPH_STATIC_ASSERT(GLB_TRIANGLES == 2U * GLB_QUADS * GLB_QUADS, "GLB triangles");

typedef struct Grid {
    CookMesh welded;    // shared vertices in grid order, normals and UVs
    CookMesh unwelded;  // three vertices per triangle
    CookMesh shuffled;  // welded, with its triangles and vertices in random order
    CookMesh optimized; // shuffled after cook_mesh_optimize
    f32 acmr_shuffled;
    bool ready;
} Grid;

typedef struct Glb {
    u8 *data;
    u64 size;
} Glb;

static CookImage g_photo;
static NphJobSystem *g_jobs;
static Grid g_grid;
static Glb g_glb;

static void bench_cleanup(void)
{
    nph_job_system_destroy(g_jobs);
    cook_image_free(&g_photo);
    cook_mesh_free(&g_grid.welded);
    cook_mesh_free(&g_grid.unwelded);
    cook_mesh_free(&g_grid.shuffled);
    cook_mesh_free(&g_grid.optimized);
    if (g_glb.data != NULL) {
        NPH_FREE(nph_allocator_system(), g_glb.data, g_glb.size, 16U);
    }
}

// Before nph_log_init a log line goes straight to stderr; the cooker logs
// nothing per iteration at WARN. Everything prepared is released at exit.
static void setup_once(void)
{
    static bool done;
    if (!done) {
        done = true;
        nph_log_set_global_level(NPH_LOG_LEVEL_WARN);
        atexit(bench_cleanup);
    }
}

static void check(NphResult r)
{
    if (r != NPH_RESULT_OK) {
        fprintf(stderr, "bench_cook: %s\n", nph_result_string(r));
        abort();
    }
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

static u32 texel_hash(u32 x, u32 y, u32 c)
{
    u32 h = (x * 0x8DA6B343U) ^ (y * 0xD8163841U) ^ (c * 0xCB1AB31FU);
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    return h;
}

static u8 to_byte(f32 v)
{
    return (u8)(nph_f32_clamp(v, 0.0F, 255.0F) + 0.5F);
}

// Channel c (0..2) at (x, y) of a photograph-like image `extent` texels
// across: smooth shading, hard-edged discs, and noise.
static u8 photo(u32 x, u32 y, u32 c, u32 extent)
{
    static const f32 discs[4][4] = {
        // centre u, v; radius; value
        {0.30F, 0.30F, 0.12F, 40.0F},
        {0.70F, 0.35F, 0.08F, 200.0F},
        {0.45F, 0.75F, 0.15F, 120.0F},
        {0.85F, 0.80F, 0.05F, 250.0F},
    };
    const f32 u = (f32)x / (f32)extent;
    const f32 v = (f32)y / (f32)extent;
    const f32 fc = (f32)c;
    f32 value = 110.0F + 90.0F * sinf(6.0F * u + 2.1F * fc) * cosf(4.0F * v - 1.3F * fc);
    for (u32 i = 0; i < 4U; ++i) {
        const f32 du = u - discs[i][0];
        const f32 dv = v - discs[i][1];
        if (du * du + dv * dv < discs[i][2] * discs[i][2]) {
            value = discs[i][3] - 30.0F * fc;
        }
    }
    return to_byte(value + (f32)(texel_hash(x, y, c) % 13U) - 6.0F);
}

static u8 photo_alpha(u32 x, u32 y)
{
    const f32 u = (f32)x / (f32)IMAGE_EXTENT;
    const f32 v = (f32)y / (f32)IMAGE_EXTENT;
    if (u > 0.55F && v < 0.4F) {
        return 0U; // a hard cut-out
    }
    return to_byte(128.0F + 127.0F * sinf(9.0F * u) * cosf(7.0F * v));
}

// X and Y of the tangent-space normal of the height field
// h = 0.5 sin(20u) cos(17v) + 0.2 sin(53u + 31v), scaled by 0.05.
static void normal_xy(u32 x, u32 y, u8 *out_x, u8 *out_y)
{
    const f32 u = (f32)x / (f32)IMAGE_EXTENT;
    const f32 v = (f32)y / (f32)IMAGE_EXTENT;
    const f32 dhdu =
        10.0F * cosf(20.0F * u) * cosf(17.0F * v) + 10.6F * cosf(53.0F * u + 31.0F * v);
    const f32 dhdv = -8.5F * sinf(20.0F * u) * sinf(17.0F * v) + 6.2F * cosf(53.0F * u + 31.0F * v);
    const NphVec3 n = nph_vec3_normalize(nph_vec3(-0.05F * dhdu, -0.05F * dhdv, 1.0F));
    *out_x = to_byte((n.x * 0.5F + 0.5F) * 255.0F);
    *out_y = to_byte((n.y * 0.5F + 0.5F) * 255.0F);
}

static u8 mask(u32 x, u32 y)
{
    if ((x / 16U + y / 16U) % 5U == 0U) {
        return 255U; // hard-edged squares
    }
    const f32 u = (f32)x / (f32)IMAGE_EXTENT;
    const f32 v = (f32)y / (f32)IMAGE_EXTENT;
    return to_byte(128.0F + 120.0F * sinf(13.0F * u) * sinf(11.0F * v));
}

// Linear HDR colour spanning 2^-5 .. 2^5, with 2 % noise.
static f32 hdr(u32 x, u32 y, u32 c)
{
    const f32 u = (f32)x / (f32)IMAGE_EXTENT;
    const f32 v = (f32)y / (f32)IMAGE_EXTENT;
    const f32 e = 5.0F * sinf(5.0F * u + (f32)c) * cosf(3.0F * v);
    const f32 noise = 1.0F + 0.02F * ((f32)(texel_hash(x, y, c) % 101U) - 50.0F) / 50.0F;
    return exp2f(e) * noise;
}

static CookBlockRgba8 g_opaque[IMAGE_BLOCKS];
static CookBlockRgba8 g_alpha[IMAGE_BLOCKS];
static u8 g_normal_x[IMAGE_BLOCKS][16];
static u8 g_normal_y[IMAGE_BLOCKS][16];
static u8 g_mask[IMAGE_BLOCKS][16];
static CookBlockRgbHalf g_hdr[IMAGE_BLOCKS];
static u8 g_encoded[IMAGE_BLOCKS][16];
static u8 g_opaque_bc7[IMAGE_BLOCKS][16]; // for bc7_decode
static CookBlockRgba8 g_decoded[IMAGE_BLOCKS];
static bool g_images_ready;

static void prepare_images(void)
{
    if (g_images_ready) {
        return;
    }
    setup_once();
    const u32 blocks_x = IMAGE_EXTENT / 4U;
    for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
        for (u32 t = 0; t < 16U; ++t) {
            const u32 x = (b % blocks_x) * 4U + t % 4U;
            const u32 y = (b / blocks_x) * 4U + t / 4U;
            for (u32 c = 0; c < 3U; ++c) {
                g_opaque[b].texels[t][c] = photo(x, y, c, IMAGE_EXTENT);
                g_alpha[b].texels[t][c] = g_opaque[b].texels[t][c];
                g_hdr[b].texels[t][c] = nph_f32_to_f16(hdr(x, y, c));
            }
            g_opaque[b].texels[t][3] = 255U;
            g_alpha[b].texels[t][3] = photo_alpha(x, y);
            normal_xy(x, y, &g_normal_x[b][t], &g_normal_y[b][t]);
            g_mask[b][t] = mask(x, y);
        }
        cook_bc7_encode(&g_opaque[b], NULL, g_opaque_bc7[b]);
    }
    g_images_ready = true;
}

static f64 psnr(f64 sse, u64 samples)
{
    return sse <= 0.0 ? 99.0 : 10.0 * log10(255.0 * 255.0 * (f64)samples / sse);
}

// ---------------------------------------------------------------------------
// Block encoders
// ---------------------------------------------------------------------------

static void bench_bc7(NbenchState *state, const CookBlockRgba8 *src, u32 channels,
                      const CookBc7Settings *settings, char *label, u64 label_cap)
{
    prepare_images();
    nbench_set_items_per_iteration(state, IMAGE_BLOCKS);
    while (nbench_keep_running(state)) {
        for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
            cook_bc7_encode(&src[b], settings, g_encoded[b]);
        }
        nbench_clobber_memory();
    }
    f64 sse = 0.0;
    for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
        CookBlockRgba8 back;
        (void)cook_bc7_decode(g_encoded[b], &back);
        for (u32 t = 0; t < 16U; ++t) {
            for (u32 c = 0; c < channels; ++c) {
                const f64 d = (f64)back.texels[t][c] - (f64)src[b].texels[t][c];
                sse += d * d;
            }
        }
    }
    // The hash of the blocks shows whether a change to the encoder changed
    // its output.
    snprintf(label, (size_t)label_cap, "%.2f dB %s, blocks %016llx",
             psnr(sse, (u64)IMAGE_BLOCKS * 16U * channels), channels == 4U ? "RGBA" : "RGB",
             (unsigned long long)nph_hash64(g_encoded, sizeof g_encoded, 0U));
    nbench_set_label(state, label);
}

// Labels are static per benchmark: nbench keeps the pointer until the
// report is written.
NBENCH(bc7_encode_opaque)
{
    static char label[64];
    bench_bc7(state, g_opaque, 3U, NULL, label, sizeof label);
}

NBENCH(bc7_encode_alpha)
{
    static char label[64];
    bench_bc7(state, g_alpha, 4U, NULL, label, sizeof label);
}

// The encoder's knobs (CookBc7Settings): fewer partitions fully encoded
// and fewer least-squares passes trade quality for time.
NBENCH(bc7_encode_opaque_p2_r1)
{
    static char label[64];
    CookBc7Settings s;
    memset(&s, 0, sizeof s);
    s.max_partitions = 2U;
    s.refine_passes = 1U;
    bench_bc7(state, g_opaque, 3U, &s, label, sizeof label);
}

NBENCH(bc7_encode_opaque_p1)
{
    static char label[64];
    CookBc7Settings s;
    memset(&s, 0, sizeof s);
    s.max_partitions = 1U;
    bench_bc7(state, g_opaque, 3U, &s, label, sizeof label);
}

NBENCH(bc7_encode_opaque_mode6)
{
    static char label[64];
    CookBc7Settings s;
    memset(&s, 0, sizeof s);
    s.mode_mask = 1U << 6U;
    bench_bc7(state, g_opaque, 3U, &s, label, sizeof label);
}

NBENCH(bc7_encode_opaque_mode1)
{
    static char label[64];
    CookBc7Settings s;
    memset(&s, 0, sizeof s);
    s.mode_mask = 1U << 1U;
    bench_bc7(state, g_opaque, 3U, &s, label, sizeof label);
}

NBENCH(bc7_decode)
{
    prepare_images();
    nbench_set_items_per_iteration(state, IMAGE_BLOCKS);
    while (nbench_keep_running(state)) {
        for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
            (void)cook_bc7_decode(g_opaque_bc7[b], &g_decoded[b]);
        }
        nbench_clobber_memory();
    }
}

NBENCH(bc5_encode_normals)
{
    static char label[64];
    prepare_images();
    nbench_set_items_per_iteration(state, IMAGE_BLOCKS);
    while (nbench_keep_running(state)) {
        for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
            cook_bc5_encode(g_normal_x[b], g_normal_y[b], g_encoded[b]);
        }
        nbench_clobber_memory();
    }
    f64 sse = 0.0;
    for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
        u8 x[16];
        u8 y[16];
        cook_bc5_decode(g_encoded[b], x, y);
        for (u32 t = 0; t < 16U; ++t) {
            const f64 dx = (f64)x[t] - (f64)g_normal_x[b][t];
            const f64 dy = (f64)y[t] - (f64)g_normal_y[b][t];
            sse += dx * dx + dy * dy;
        }
    }
    snprintf(label, sizeof label, "%.2f dB XY", psnr(sse, (u64)IMAGE_BLOCKS * 32U));
    nbench_set_label(state, label);
}

NBENCH(bc4_encode_mask)
{
    static char label[64];
    prepare_images();
    nbench_set_items_per_iteration(state, IMAGE_BLOCKS);
    while (nbench_keep_running(state)) {
        for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
            cook_bc4_encode(g_mask[b], g_encoded[b]);
        }
        nbench_clobber_memory();
    }
    f64 sse = 0.0;
    for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
        u8 back[16];
        cook_bc4_decode(g_encoded[b], back);
        for (u32 t = 0; t < 16U; ++t) {
            const f64 d = (f64)back[t] - (f64)g_mask[b][t];
            sse += d * d;
        }
    }
    snprintf(label, sizeof label, "%.2f dB", psnr(sse, (u64)IMAGE_BLOCKS * 16U));
    nbench_set_label(state, label);
}

NBENCH(bc6h_encode_hdr)
{
    static char label[64];
    prepare_images();
    nbench_set_items_per_iteration(state, IMAGE_BLOCKS);
    while (nbench_keep_running(state)) {
        for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
            cook_bc6h_encode(&g_hdr[b], g_encoded[b]);
        }
        nbench_clobber_memory();
    }
    f64 error = 0.0;
    for (u32 b = 0; b < IMAGE_BLOCKS; ++b) {
        CookBlockRgbHalf back;
        (void)cook_bc6h_decode(g_encoded[b], &back);
        for (u32 t = 0; t < 16U; ++t) {
            for (u32 c = 0; c < 3U; ++c) {
                const f64 want = (f64)nph_f16_to_f32(g_hdr[b].texels[t][c]);
                const f64 got = (f64)nph_f16_to_f32(back.texels[t][c]);
                error += fabs(got - want) / fmax(want, 1e-3);
            }
        }
    }
    snprintf(label, sizeof label, "%.2f%% mean relative error",
             100.0 * error / ((f64)IMAGE_BLOCKS * 48.0));
    nbench_set_label(state, label);
}

// ---------------------------------------------------------------------------
// The texture cook
// ---------------------------------------------------------------------------

static void prepare_photo(void)
{
    if (g_photo.pixels != NULL) {
        return;
    }
    setup_once();
    check(cook_image_create(nph_allocator_system(), TEXTURE_EXTENT, TEXTURE_EXTENT,
                            COOK_IMAGE_RGBA8, &g_photo));
    u8 *p = (u8 *)g_photo.pixels;
    for (u32 y = 0; y < TEXTURE_EXTENT; ++y) {
        for (u32 x = 0; x < TEXTURE_EXTENT; ++x) {
            u8 *t = p + ((u64)y * TEXTURE_EXTENT + x) * 4U;
            for (u32 c = 0; c < 3U; ++c) {
                t[c] = photo(x, y, c, TEXTURE_EXTENT);
            }
            t[3] = 255U;
        }
    }
}

static NphJobSystem *job_system(void)
{
    if (g_jobs == NULL) {
        setup_once();
        NphJobSystemDesc jd;
        memset(&jd, 0, sizeof jd);
        jd.alloc = nph_allocator_system();
        check(nph_job_system_create(&jd, &g_jobs));
    }
    return g_jobs;
}

static void bench_texture_cook(NbenchState *state, NphJobSystem *jobs)
{
    prepare_photo();
    NphAllocator *alloc = nph_allocator_system();
    CookTextureSettings s;
    cook_texture_settings_default(&s);
    nbench_set_items_per_iteration(state, (u64)TEXTURE_EXTENT * TEXTURE_EXTENT);
    while (nbench_keep_running(state)) {
        NphAssetWriter w;
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                              nph_guid_from_cstr("bench"), 0U);
        check(cook_texture(alloc, &g_photo, &s, jobs, &w));
        nph_asset_writer_free(&w);
    }
}

NBENCH(texture_cook_256)
{
    bench_texture_cook(state, NULL);
}

NBENCH(texture_cook_256_jobs)
{
    static char label[64];
    NphJobSystem *jobs = job_system();
    bench_texture_cook(state, jobs);
    snprintf(label, sizeof label, "%u workers", nph_job_system_worker_count(jobs));
    nbench_set_label(state, label);
}

// ---------------------------------------------------------------------------
// The mesh pipeline, on a height-field grid
// ---------------------------------------------------------------------------

// Vertex (i, j) of a grid of `quads` quads per side over the unit square,
// displaced along +Z, with its analytic normal and tiling UVs.
static CookVertex grid_vertex(u32 i, u32 j, u32 quads)
{
    const f32 x = (f32)i / (f32)quads;
    const f32 y = (f32)j / (f32)quads;
    CookVertex v;
    memset(&v, 0, sizeof v);
    v.position = nph_vec3(x, y, 0.05F * sinf(12.0F * x) * cosf(9.0F * y));
    const f32 dhdx = 0.6F * cosf(12.0F * x) * cosf(9.0F * y);
    const f32 dhdy = -0.45F * sinf(12.0F * x) * sinf(9.0F * y);
    v.normal = nph_vec3_normalize(nph_vec3(-dhdx, -dhdy, 1.0F));
    v.tangent = nph_vec4(1.0F, 0.0F, 0.0F, 1.0F);
    v.uv0 = nph_vec2(4.0F * x, 4.0F * y);
    v.color = nph_vec4_one();
    return v;
}

// The two counter-clockwise triangles of quad (i, j), as vertex indices of
// a grid `quads` quads across.
static void grid_quad(u32 i, u32 j, u32 quads, u32 out[6])
{
    const u32 v00 = j * (quads + 1U) + i;
    const u32 v10 = v00 + 1U;
    const u32 v01 = v00 + quads + 1U;
    const u32 v11 = v01 + 1U;
    const u32 tris[6] = {v00, v10, v11, v00, v11, v01};
    memcpy(out, tris, sizeof tris);
}

static u64 next_random(u64 *s)
{
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s >> 33U;
}

static void mesh_from(CookMesh *m, const CookVertex *vertices, u32 vertex_count, const u32 *indices,
                      u32 index_count)
{
    cook_mesh_init(m, nph_allocator_system());
    check(cook_mesh_add_submesh(m, vertices, vertex_count, indices, index_count, 0U));
    m->flags = (u32)COOK_MESH_HAS_NORMALS | (u32)COOK_MESH_HAS_UV0;
}

static void mesh_copy(const CookMesh *src, CookMesh *dst)
{
    mesh_from(dst, src->vertices.data, src->vertices.len, src->indices.data, src->indices.len);
    dst->flags = src->flags;
}

static void prepare_grid(void)
{
    if (g_grid.ready) {
        return;
    }
    setup_once();
    NphAllocator *alloc = nph_allocator_system();
    const u32 index_count = GRID_TRIANGLES * 3U;
    CookVertex *vertices = NPH_ALLOC_ARRAY(alloc, CookVertex, GRID_VERTICES);
    u32 *indices = NPH_ALLOC_ARRAY(alloc, u32, index_count);
    u32 *order = NPH_ALLOC_ARRAY(alloc, u32, GRID_VERTICES);
    u32 *remap = NPH_ALLOC_ARRAY(alloc, u32, GRID_VERTICES);
    if (vertices == NULL || indices == NULL || order == NULL || remap == NULL) {
        abort();
    }
    // Welded, in grid order.
    for (u32 j = 0; j <= GRID_QUADS; ++j) {
        for (u32 i = 0; i <= GRID_QUADS; ++i) {
            vertices[j * (GRID_QUADS + 1U) + i] = grid_vertex(i, j, GRID_QUADS);
        }
    }
    for (u32 j = 0; j < GRID_QUADS; ++j) {
        for (u32 i = 0; i < GRID_QUADS; ++i) {
            grid_quad(i, j, GRID_QUADS, &indices[((u64)j * GRID_QUADS + i) * 6U]);
        }
    }
    mesh_from(&g_grid.welded, vertices, GRID_VERTICES, indices, index_count);

    // Unwelded: every corner its own vertex, as a glTF without indices.
    CookVertex *flat = NPH_ALLOC_ARRAY(alloc, CookVertex, index_count);
    u32 *flat_indices = NPH_ALLOC_ARRAY(alloc, u32, index_count);
    if (flat == NULL || flat_indices == NULL) {
        abort();
    }
    for (u32 k = 0; k < index_count; ++k) {
        flat[k] = vertices[indices[k]];
        flat_indices[k] = k;
    }
    mesh_from(&g_grid.unwelded, flat, index_count, flat_indices, index_count);

    // Shuffled: a random vertex order (Fisher-Yates) and triangle order.
    for (u32 v = 0; v < GRID_VERTICES; ++v) {
        order[v] = v;
    }
    u64 rng = 0x5EEDU;
    for (u32 v = GRID_VERTICES - 1U; v > 0U; --v) {
        const u32 k = (u32)(next_random(&rng) % (v + 1U));
        const u32 t = order[v];
        order[v] = order[k];
        order[k] = t;
    }
    for (u32 v = 0; v < GRID_VERTICES; ++v) {
        flat[v] = vertices[order[v]];
        remap[order[v]] = v;
    }
    for (u32 t = GRID_TRIANGLES - 1U; t > 0U; --t) {
        const u32 k = (u32)(next_random(&rng) % (t + 1U));
        for (u32 c = 0; c < 3U; ++c) {
            const u32 a = indices[(u64)t * 3U + c];
            indices[(u64)t * 3U + c] = indices[(u64)k * 3U + c];
            indices[(u64)k * 3U + c] = a;
        }
    }
    for (u32 k = 0; k < index_count; ++k) {
        flat_indices[k] = remap[indices[k]];
    }
    mesh_from(&g_grid.shuffled, flat, GRID_VERTICES, flat_indices, index_count);
    g_grid.acmr_shuffled = cook_mesh_acmr(flat_indices, index_count, 32U);
    mesh_copy(&g_grid.shuffled, &g_grid.optimized);
    check(cook_mesh_optimize(&g_grid.optimized));

    NPH_FREE_ARRAY(alloc, u32, flat_indices, index_count);
    NPH_FREE_ARRAY(alloc, CookVertex, flat, index_count);
    NPH_FREE_ARRAY(alloc, u32, remap, GRID_VERTICES);
    NPH_FREE_ARRAY(alloc, u32, order, GRID_VERTICES);
    NPH_FREE_ARRAY(alloc, u32, indices, index_count);
    NPH_FREE_ARRAY(alloc, CookVertex, vertices, GRID_VERTICES);
    g_grid.ready = true;
}

// Runs `step` on a fresh copy of `src` per iteration (the copy untimed).
typedef NphResult (*MeshStep)(CookMesh *mesh);

static u32 bench_mesh_step(NbenchState *state, const CookMesh *src, MeshStep step, f32 *out_acmr)
{
    prepare_grid();
    nbench_set_items_per_iteration(state, GRID_TRIANGLES);
    u32 vertices = 0U;
    f32 acmr = 0.0F;
    while (nbench_keep_running(state)) {
        CookMesh m;
        nbench_pause_timing(state);
        mesh_copy(src, &m);
        nbench_resume_timing(state);
        check(step(&m));
        nbench_pause_timing(state);
        vertices = m.vertices.len;
        if (out_acmr != NULL) {
            acmr = cook_mesh_acmr(m.indices.data, m.indices.len, 32U);
        }
        cook_mesh_free(&m);
        nbench_resume_timing(state);
    }
    if (out_acmr != NULL) {
        *out_acmr = acmr;
    }
    return vertices;
}

static NphResult smooth_normals(CookMesh *mesh)
{
    return cook_mesh_generate_normals(mesh, COOK_NORMALS_SMOOTH);
}

NBENCH(mesh_weld)
{
    static char label[64];
    const u32 vertices = bench_mesh_step(state, &g_grid.unwelded, cook_mesh_weld, NULL);
    snprintf(label, sizeof label, "%u -> %u vertices", GRID_TRIANGLES * 3U, vertices);
    nbench_set_label(state, label);
}

NBENCH(mesh_normals_smooth)
{
    (void)bench_mesh_step(state, &g_grid.welded, smooth_normals, NULL);
}

NBENCH(mesh_tangents)
{
    static char label[64];
    const u32 vertices = bench_mesh_step(state, &g_grid.welded, cook_mesh_generate_tangents, NULL);
    snprintf(label, sizeof label, "%u -> %u vertices", GRID_VERTICES, vertices);
    nbench_set_label(state, label);
}

NBENCH(mesh_optimize)
{
    static char label[64];
    f32 acmr = 0.0F;
    (void)bench_mesh_step(state, &g_grid.shuffled, cook_mesh_optimize, &acmr);
    snprintf(label, sizeof label, "ACMR %.2f -> %.2f", (f64)g_grid.acmr_shuffled, (f64)acmr);
    nbench_set_label(state, label);
}

NBENCH(mesh_meshlets)
{
    static char label[64];
    prepare_grid();
    nbench_set_items_per_iteration(state, GRID_TRIANGLES);
    u32 count = 0U;
    while (nbench_keep_running(state)) {
        CookMeshlets ml;
        cook_meshlets_init(&ml, nph_allocator_system());
        check(cook_mesh_build_meshlets(&g_grid.optimized, 0U, 0U, 0.25F, &ml));
        count = ml.meshlets.len;
        cook_meshlets_free(&ml);
    }
    snprintf(label, sizeof label, "%u meshlets, %.1f triangles each", count,
             (f64)GRID_TRIANGLES / (f64)(count != 0U ? count : 1U));
    nbench_set_label(state, label);
}

NBENCH(mesh_process)
{
    static char label[64];
    prepare_grid();
    NphAllocator *alloc = nph_allocator_system();
    CookMeshSettings settings;
    cook_mesh_settings_default(&settings);
    nbench_set_items_per_iteration(state, GRID_TRIANGLES);
    CookMeshStats stats;
    memset(&stats, 0, sizeof stats);
    while (nbench_keep_running(state)) {
        CookMesh m;
        NphAssetWriter w;
        nbench_pause_timing(state);
        mesh_copy(&g_grid.shuffled, &m);
        nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION,
                              nph_guid_from_cstr("bench"), 0U);
        nbench_resume_timing(state);
        check(cook_mesh_process(&m, &settings, &w, &stats));
        nbench_pause_timing(state);
        nph_asset_writer_free(&w);
        cook_mesh_free(&m);
        nbench_resume_timing(state);
    }
    snprintf(label, sizeof label, "%u meshlets, ACMR %.2f -> %.2f", stats.meshlets,
             (f64)stats.acmr_before, (f64)stats.acmr_after);
    nbench_set_label(state, label);
}

// ---------------------------------------------------------------------------
// glTF import
// ---------------------------------------------------------------------------

static void put_u32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8U) & 0xFFU);
    p[2] = (u8)((v >> 16U) & 0xFFU);
    p[3] = (u8)(v >> 24U);
}

// GLB_MESHES grids of GLB_QUADS quads, each with its own POSITION, NORMAL,
// TEXCOORD_0, and 32-bit indices in one binary chunk (glTF 2.0, "GLB File
// Format Specification").
static void prepare_glb(void)
{
    if (g_glb.data != NULL) {
        return;
    }
    setup_once();
    NphAllocator *alloc = nph_allocator_system();
    const u64 positions = (u64)GLB_VERTICES * 12U;
    const u64 normals = (u64)GLB_VERTICES * 12U;
    const u64 uvs = (u64)GLB_VERTICES * 8U;
    const u64 index_bytes = (u64)GLB_TRIANGLES * 12U;
    const u64 per_mesh = positions + normals + uvs + index_bytes;
    const u64 bin_size = per_mesh * GLB_MESHES;
    u8 *bin = (u8 *)NPH_ALLOC(alloc, bin_size, 16U);
    if (bin == NULL) {
        abort();
    }
    f32 zmin = 0.0F;
    f32 zmax = 0.0F;
    for (u32 m = 0; m < GLB_MESHES; ++m) {
        u8 *base = bin + per_mesh * m;
        for (u32 j = 0; j <= GLB_QUADS; ++j) {
            for (u32 i = 0; i <= GLB_QUADS; ++i) {
                const u64 v = (u64)j * (GLB_QUADS + 1U) + i;
                const CookVertex cv = grid_vertex(i, j, GLB_QUADS);
                memcpy(base + v * 12U, &cv.position, 12U);
                memcpy(base + positions + v * 12U, &cv.normal, 12U);
                memcpy(base + positions + normals + v * 8U, &cv.uv0, 8U);
                zmin = nph_f32_min(zmin, cv.position.z);
                zmax = nph_f32_max(zmax, cv.position.z);
            }
        }
        u8 *idx = base + positions + normals + uvs;
        for (u32 j = 0; j < GLB_QUADS; ++j) {
            for (u32 i = 0; i < GLB_QUADS; ++i) {
                u32 quad[6];
                grid_quad(i, j, GLB_QUADS, quad);
                memcpy(idx + ((u64)j * GLB_QUADS + i) * 24U, quad, sizeof quad);
            }
        }
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, alloc);
    check(nph_sb_append_cstr(&sb, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                                  "\"scenes\":[{\"nodes\":["));
    for (u32 m = 0; m < GLB_MESHES; ++m) {
        check(nph_sb_appendf(&sb, "%s%u", m == 0U ? "" : ",", m));
    }
    check(nph_sb_append_cstr(&sb, "]}],\"nodes\":["));
    for (u32 m = 0; m < GLB_MESHES; ++m) {
        check(nph_sb_appendf(&sb, "%s{\"mesh\":%u,\"translation\":[%u,0,0]}", m == 0U ? "" : ",", m,
                             m));
    }
    check(nph_sb_append_cstr(&sb, "],\"meshes\":["));
    for (u32 m = 0; m < GLB_MESHES; ++m) {
        const u32 a = m * 4U;
        check(nph_sb_appendf(&sb,
                             "%s{\"primitives\":[{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u,"
                             "\"TEXCOORD_0\":%u},\"indices\":%u}]}",
                             m == 0U ? "" : ",", a, a + 1U, a + 2U, a + 3U));
    }
    check(nph_sb_append_cstr(&sb, "],\"accessors\":["));
    for (u32 m = 0; m < GLB_MESHES; ++m) {
        const u32 bv = m * 4U;
        check(nph_sb_appendf(
            &sb,
            "%s{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\","
            "\"min\":[0,0,%.6f],\"max\":[1,1,%.6f]},"
            "{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
            "{\"bufferView\":%u,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"},"
            "{\"bufferView\":%u,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}",
            m == 0U ? "" : ",", bv, GLB_VERTICES, (f64)zmin, (f64)zmax, bv + 1U, GLB_VERTICES,
            bv + 2U, GLB_VERTICES, bv + 3U, GLB_TRIANGLES * 3U));
    }
    check(nph_sb_append_cstr(&sb, "],\"bufferViews\":["));
    for (u32 m = 0; m < GLB_MESHES; ++m) {
        const u64 o = per_mesh * m;
        const u64 at_normals = o + positions;
        const u64 at_uvs = at_normals + normals;
        const u64 at_indices = at_uvs + uvs;
        check(nph_sb_appendf(&sb,
                             "%s{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu},"
                             "{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu},"
                             "{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu},"
                             "{\"buffer\":0,\"byteOffset\":%llu,\"byteLength\":%llu}",
                             m == 0U ? "" : ",", (unsigned long long)o,
                             (unsigned long long)positions, (unsigned long long)at_normals,
                             (unsigned long long)normals, (unsigned long long)at_uvs,
                             (unsigned long long)uvs, (unsigned long long)at_indices,
                             (unsigned long long)index_bytes));
    }
    check(nph_sb_appendf(&sb, "],\"buffers\":[{\"byteLength\":%llu}]}",
                         (unsigned long long)bin_size));

    const u64 json_size = (sb.len + 3U) & ~(u64)3U;
    g_glb.size = 12U + 8U + json_size + 8U + bin_size;
    g_glb.data = (u8 *)NPH_ALLOC(alloc, g_glb.size, 16U);
    if (g_glb.data == NULL) {
        abort();
    }
    u8 *p = g_glb.data;
    put_u32(p, 0x46546C67U); // "glTF"
    put_u32(p + 4, 2U);
    put_u32(p + 8, (u32)g_glb.size);
    put_u32(p + 12, (u32)json_size);
    put_u32(p + 16, 0x4E4F534AU); // "JSON"
    memcpy(p + 20, sb.data, (size_t)sb.len);
    memset(p + 20 + sb.len, ' ', (size_t)(json_size - sb.len)); // padded with spaces
    u8 *chunk = p + 20 + json_size;
    put_u32(chunk, (u32)bin_size);
    put_u32(chunk + 4, 0x004E4942U); // "BIN\0"
    memcpy(chunk + 8, bin, (size_t)bin_size);
    nph_sb_free(&sb);
    NPH_FREE(alloc, bin, bin_size, 16U);
}

NBENCH(gltf_import_glb)
{
    static char label[64];
    prepare_glb();
    NphAllocator *alloc = nph_allocator_system();
    nbench_set_items_per_iteration(state, g_glb.size / 1024U);
    u32 meshes = 0U;
    while (nbench_keep_running(state)) {
        CookGltfScene scene;
        check(cook_gltf_load_memory(alloc, g_glb.data, g_glb.size, NULL, &scene));
        meshes = scene.mesh_count;
        cook_gltf_free(&scene);
    }
    snprintf(label, sizeof label, "%.1f MiB, %u meshes", (f64)g_glb.size / (1024.0 * 1024.0),
             meshes);
    nbench_set_label(state, label);
}
