// SPDX-License-Identifier: Apache-2.0
// GPU microbenchmarks (CLAUDE.md §12 Phase 5: "GPU microbenchmarks measure
// achievable bandwidth and throughput on the RTX 3060 Ti, and the §10.4
// budget table is updated via ADR"). Each benchmark records one command
// buffer between two GPU timestamps and runs it for several frames; the
// median GPU time is the headline, with a derived metric (GB/s, Gpix/s,
// TFLOPS) from the bytes, pixels, or FLOPs the command moves.
//
// The GPU is driven to its sustained clock state before anything is
// measured: one second of FMA load before the suite, then at least 150 ms
// of each benchmark's own workload before its measured frames. The whole
// suite is only a few hundred milliseconds of GPU time, and short bursts
// leave the clock governor at a low boost step (1665 MHz was observed
// against ~1900 MHz sustained on the RTX 3060 Ti), which moved the raster
// numbers by 15 % between otherwise identical runs.
//
//     gpu_bench [--filter=GLOB] [--repeats=N] [--json=FILE] [--budgets=FILE] [--list]
//
// Output follows the nbench JSON schema (docs/benchmarks.md) with two extra
// fields per entry, `metric` and `unit`. Exit codes match nbench: 0 ok,
// 2 usage error, 3 nothing matched, 4 a budget was exceeded; 77 when no
// Vulkan device is available (CTest "skipped"). Validation is off: these
// numbers are the hardware's, not the layer's.

#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/gpu/vk.h>
#include <nepheline/math/pack.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include <nph_shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXIT_SKIPPED 77
#define WIDTH_4K     3840U
#define HEIGHT_4K    2160U
#define MAX_REPEATS  64U
#define BIG_BYTES    NPH_MIB(256)
#define WARMUP_NS    (150ULL * 1000000ULL)  // per benchmark, before the measured frames
#define PREWARM_NS   (1000ULL * 1000000ULL) // once, before the suite

// The temporal resolve's forms (bench_taa.frag k_mode: nine fetches, the
// quad's shared block, the cross) and history formats (B10G11R11, RGBA16F).
#define TAA_MODES        3U
#define TAA_MODE_QUAD    1U
#define TAA_FORMATS      2U
#define TAA_COMPUTE      TAA_MODES // the row of taa_supported for the compute form
#define TAA_TILE         16U       // bench_taa.comp's workgroup
#define TAA_PARAM(m, f)  ((m) | ((f) << 4U))
#define TAA_MODE_OF(p)   ((p) & 0xFU)
#define TAA_FORMAT_OF(p) ((p) >> 4U)

typedef struct Ctx {
    NphGpuDevice *dev;
    NphGpuShader fill_cs;
    NphGpuShader read_cs;
    NphGpuShader fma_cs;
    NphGpuShader random_cs;
    NphGpuShader fullscreen_vs;
    NphGpuShader color_fs;
    NphGpuShader sample_fs;
    NphGpuShader visbuffer_fs;
    NphGpuShader taa_fs;
    NphGpuShader taa_cs;
    NphGpuShader tonemap_fs;
    NphGpuPipeline fill;
    NphGpuPipeline read;
    NphGpuPipeline fma;
    NphGpuPipeline random;
    NphGpuPipeline raster_rgba8;
    NphGpuPipeline raster_r11g11b10;
    NphGpuPipeline raster_rgba16f;
    NphGpuPipeline sample_rgba8;
    NphGpuPipeline visbuffer;
    // The temporal resolve (bench_taa.*): [neighbourhood mode][history
    // format], the compute form per history format, and the tonemap.
    NphGpuPipeline taa_fragment[TAA_MODES][TAA_FORMATS];
    NphGpuPipeline taa_compute[TAA_FORMATS];
    NphGpuPipeline tonemap;
    b32 taa_supported[TAA_MODES + 1U][TAA_FORMATS]; // the last row: the compute form
    NphGpuBuffer big_a;
    NphGpuBuffer big_b;
    NphGpuBuffer small;
    NphGpuImage target_rgba8;
    NphGpuImage target_r11g11b10;
    NphGpuImage target_rgba16f;
    NphGpuImage target_r32g32;
    NphGpuImage clear_target;
    NphGpuImage texture;
    NphGpuSampler sampler;
    // The temporal resolve's inputs, filled with noise (the worst case for
    // the hardware's colour compression), and its new history.
    NphGpuImage hdr;                     // B10G11R11, the current frame
    NphGpuImage vis;                     // R32G32_UINT, the visibility buffer
    NphGpuImage history_in[TAA_FORMATS]; // the previous frame's history
    NphGpuImage history_out[TAA_FORMATS];
    NphGpuSampler linear_clamp;
} Ctx;

typedef struct Bench Bench;
typedef void (*BenchRecordFn)(Ctx *ctx, const Bench *b, NphGpuCmd *cmd);

struct Bench {
    const char *name;
    BenchRecordFn record;
    f64 items;        // bytes, pixels, or FLOPs per iteration
    const char *unit; // "GB/s", "Gpix/s", "TFLOPS", or "ns"
    u32 param;        // benchmark-specific (working-set mask, instance count)
};

// ---------------------------------------------------------------------------
// Recording helpers
// ---------------------------------------------------------------------------

static NphBenchPush base_push(const Ctx *ctx)
{
    NphBenchPush p;
    memset(&p, 0, sizeof p);
    p.address = nph_gpu_buffer_address(ctx->dev, ctx->big_a);
    p.address2 = nph_gpu_buffer_address(ctx->dev, ctx->small);
    p.texture_index = nph_gpu_image_sampled_index(ctx->dev, ctx->texture);
    p.sampler_index = nph_gpu_sampler_index(ctx->dev, ctx->sampler);
    p.color = nph_vec4(0.2F, 0.4F, 0.6F, 1.0F);
    return p;
}

static void record_copy(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(b);
    NphGpuBufferCopy region = {0U, 0U, BIG_BYTES};
    nph_gpu_cmd_copy_buffer(cmd, ctx->big_a, ctx->big_b, &region, 1U);
}

static void record_fill(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(b);
    NphBenchPush p = base_push(ctx);
    p.count = (u32)(BIG_BYTES / 16U);
    nph_gpu_cmd_bind_pipeline(cmd, ctx->fill);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, p.count / 256U, 1U, 1U);
}

#define READ_GROUPS 8192U

static void record_read(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(b);
    NphBenchPush p = base_push(ctx);
    p.count = (u32)(BIG_BYTES / 16U);
    p.iterations = p.count / (READ_GROUPS * 256U);
    nph_gpu_cmd_bind_pipeline(cmd, ctx->read);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, READ_GROUPS, 1U, 1U);
}

#define FMA_THREADS    (1U << 20U)
#define FMA_ITERATIONS 1024U

static void record_fma(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(b);
    NphBenchPush p = base_push(ctx);
    p.count = FMA_THREADS;
    p.iterations = FMA_ITERATIONS;
    nph_gpu_cmd_bind_pipeline(cmd, ctx->fma);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, FMA_THREADS / 256U, 1U, 1U);
}

#define RANDOM_GROUPS     4096U
#define RANDOM_ITERATIONS 64U

static void record_random(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NphBenchPush p = base_push(ctx);
    p.mask = b->param;
    p.iterations = RANDOM_ITERATIONS;
    nph_gpu_cmd_bind_pipeline(cmd, ctx->random);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, RANDOM_GROUPS, 1U, 1U);
}

static void record_clear(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(b);
    const NphGpuClearValue v = nph_gpu_clear_color(0.1F, 0.2F, 0.3F, 1.0F);
    nph_gpu_cmd_image_barrier(cmd, ctx->clear_target, (u32)NPH_GPU_STAGE_TRANSFER,
                              (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, NPH_GPU_LAYOUT_UNDEFINED,
                              (u32)NPH_GPU_STAGE_TRANSFER, (u32)NPH_GPU_ACCESS_TRANSFER_WRITE,
                              NPH_GPU_LAYOUT_TRANSFER_DST);
    nph_gpu_cmd_clear_color_image(cmd, ctx->clear_target, NPH_GPU_LAYOUT_TRANSFER_DST, &v, 0U, 1U,
                                  0U, 1U);
}

static void record_raster(Ctx *ctx, NphGpuCmd *cmd, NphGpuImage target, NphGpuPipeline pipeline,
                          u32 instances)
{
    nph_gpu_cmd_image_barrier(cmd, target, (u32)NPH_GPU_STAGE_COLOR_OUTPUT,
                              (u32)NPH_GPU_ACCESS_COLOR_WRITE, NPH_GPU_LAYOUT_UNDEFINED,
                              (u32)NPH_GPU_STAGE_COLOR_OUTPUT, (u32)NPH_GPU_ACCESS_COLOR_WRITE,
                              NPH_GPU_LAYOUT_COLOR_ATTACHMENT);
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.colors[0].image = target;
    rd.colors[0].load = NPH_GPU_LOAD_OP_DONT_CARE;
    rd.colors[0].store = NPH_GPU_STORE_OP_STORE;
    rd.color_count = 1U;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    NphBenchPush p = base_push(ctx);
    p.iterations = instances;
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_draw(cmd, 3U, instances, 0U, 0U);
    nph_gpu_cmd_end_rendering(cmd);
}

static void record_raster_rgba8(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    record_raster(ctx, cmd, ctx->target_rgba8, ctx->raster_rgba8, b->param);
}
static void record_raster_r11g11b10(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    record_raster(ctx, cmd, ctx->target_r11g11b10, ctx->raster_r11g11b10, b->param);
}
static void record_raster_rgba16f(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    record_raster(ctx, cmd, ctx->target_rgba16f, ctx->raster_rgba16f, b->param);
}
static void record_sample(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    record_raster(ctx, cmd, ctx->target_rgba8, ctx->sample_rgba8, b->param);
}
static void record_visbuffer(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    record_raster(ctx, cmd, ctx->target_r32g32, ctx->visbuffer, b->param);
}
// A full-screen triangle into `count` colour targets, their contents
// discarded first.
static void record_fullscreen(NphGpuCmd *cmd, const NphGpuImage *targets, u32 count,
                              NphGpuPipeline pipeline, const NphBenchPush *p)
{
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    for (u32 i = 0; i < count; ++i) {
        nph_gpu_cmd_image_barrier(cmd, targets[i], (u32)NPH_GPU_STAGE_ALL,
                                  (u32)NPH_GPU_ACCESS_MEMORY_WRITE, NPH_GPU_LAYOUT_UNDEFINED,
                                  (u32)NPH_GPU_STAGE_COLOR_OUTPUT, (u32)NPH_GPU_ACCESS_COLOR_WRITE,
                                  NPH_GPU_LAYOUT_COLOR_ATTACHMENT);
        rd.colors[i].image = targets[i];
        rd.colors[i].load = NPH_GPU_LOAD_OP_DONT_CARE;
        rd.colors[i].store = NPH_GPU_STORE_OP_STORE;
    }
    rd.color_count = count;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    nph_gpu_cmd_bind_pipeline(cmd, pipeline);
    nph_gpu_cmd_push_constants(cmd, p, sizeof *p);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
    nph_gpu_cmd_end_rendering(cmd);
}

static NphBenchPush taa_push(const Ctx *ctx, u32 format)
{
    NphBenchPush p = base_push(ctx);
    p.texture_index = nph_gpu_image_sampled_index(ctx->dev, ctx->hdr);
    p.texture2_index = nph_gpu_image_sampled_index(ctx->dev, ctx->vis);
    p.texture3_index = nph_gpu_image_sampled_index(ctx->dev, ctx->history_in[format]);
    p.sampler_index = nph_gpu_sampler_index(ctx->dev, ctx->linear_clamp);
    p.image_index = nph_gpu_image_storage_index(ctx->dev, ctx->history_out[format], 0U);
    p.color = nph_vec4(0.37F, -0.21F, 0.0F, 0.0F); // the motion, in pixels
    return p;
}

static void record_tonemap(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(b);
    const NphBenchPush p = taa_push(ctx, 0U);
    record_fullscreen(cmd, &ctx->target_rgba8, 1U, ctx->tonemap, &p);
}

// The fused form: the display colour and the new history in one pass.
static void record_taa_fragment(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    const u32 format = TAA_FORMAT_OF(b->param);
    const NphBenchPush p = taa_push(ctx, format);
    const NphGpuImage targets[2] = {ctx->target_rgba8, ctx->history_out[format]};
    record_fullscreen(cmd, targets, 2U, ctx->taa_fragment[TAA_MODE_OF(b->param)][format], &p);
}

// The compute form: the history from a compute pass, then the tonemap.
static void record_taa_compute(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    const u32 format = TAA_FORMAT_OF(b->param);
    const NphGpuImage history = ctx->history_out[format];
    nph_gpu_cmd_image_barrier(cmd, history, (u32)NPH_GPU_STAGE_ALL,
                              (u32)NPH_GPU_ACCESS_MEMORY_WRITE, NPH_GPU_LAYOUT_UNDEFINED,
                              (u32)NPH_GPU_STAGE_COMPUTE, (u32)NPH_GPU_ACCESS_SHADER_WRITE,
                              NPH_GPU_LAYOUT_GENERAL);
    NphBenchPush p = taa_push(ctx, format);
    nph_gpu_cmd_bind_pipeline(cmd, ctx->taa_compute[format]);
    nph_gpu_cmd_push_constants(cmd, &p, sizeof p);
    nph_gpu_cmd_dispatch(cmd, WIDTH_4K / TAA_TILE, HEIGHT_4K / TAA_TILE, 1U);
    nph_gpu_cmd_image_barrier(cmd, history, (u32)NPH_GPU_STAGE_COMPUTE,
                              (u32)NPH_GPU_ACCESS_SHADER_WRITE, NPH_GPU_LAYOUT_GENERAL,
                              (u32)NPH_GPU_STAGE_FRAGMENT_SHADER, (u32)NPH_GPU_ACCESS_SHADER_READ,
                              NPH_GPU_LAYOUT_SHADER_READ_ONLY);
    p.texture_index = nph_gpu_image_sampled_index(ctx->dev, history);
    record_fullscreen(cmd, &ctx->target_rgba8, 1U, ctx->tonemap, &p);
}

static void record_nothing(Ctx *ctx, const Bench *b, NphGpuCmd *cmd)
{
    NPH_UNUSED(ctx);
    NPH_UNUSED(b);
    NPH_UNUSED(cmd);
}

#define PIXELS_4K ((f64)WIDTH_4K * (f64)HEIGHT_4K)

static const Bench g_benches[] = {
    {"timestamp_resolution", record_nothing, 1.0, "ns", 0U},
    {"copy_256mib", record_copy, 2.0 * (f64)BIG_BYTES, "GB/s", 0U},
    {"write_256mib", record_fill, (f64)BIG_BYTES, "GB/s", 0U},
    {"read_256mib", record_read, (f64)BIG_BYTES, "GB/s", 0U},
    {"fma_fp32", record_fma, (f64)FMA_THREADS *FMA_ITERATIONS * 16.0, "TFLOPS", 0U},
    {"random_read_1mib", record_random, (f64)RANDOM_GROUPS * 256.0 * RANDOM_ITERATIONS * 16.0,
     "GB/s", (1U << 16U) - 1U},
    {"random_read_2mib", record_random, (f64)RANDOM_GROUPS * 256.0 * RANDOM_ITERATIONS * 16.0,
     "GB/s", (1U << 17U) - 1U},
    {"random_read_4mib", record_random, (f64)RANDOM_GROUPS * 256.0 * RANDOM_ITERATIONS * 16.0,
     "GB/s", (1U << 18U) - 1U},
    {"random_read_8mib", record_random, (f64)RANDOM_GROUPS * 256.0 * RANDOM_ITERATIONS * 16.0,
     "GB/s", (1U << 19U) - 1U},
    {"random_read_16mib", record_random, (f64)RANDOM_GROUPS * 256.0 * RANDOM_ITERATIONS * 16.0,
     "GB/s", (1U << 20U) - 1U},
    {"random_read_64mib", record_random, (f64)RANDOM_GROUPS * 256.0 * RANDOM_ITERATIONS * 16.0,
     "GB/s", (1U << 22U) - 1U},
    {"clear_4k_rgba8", record_clear, PIXELS_4K * 4.0, "GB/s", 0U},
    {"raster_fill_4k_rgba8_x8", record_raster_rgba8, PIXELS_4K * 8.0, "Gpix/s", 8U},
    {"raster_fill_4k_r11g11b10_x8", record_raster_r11g11b10, PIXELS_4K * 8.0, "Gpix/s", 8U},
    {"raster_fill_4k_rgba16f_x8", record_raster_rgba16f, PIXELS_4K * 8.0, "Gpix/s", 8U},
    {"visbuffer_write_4k_8bpp", record_visbuffer, PIXELS_4K, "Gpix/s", 1U},
    {"texture_sample_4k_rgba8", record_sample, PIXELS_4K, "Gpix/s", 1U},
    // The temporal resolve and the tonemap (docs/adr/0034-temporal-anti-aliasing.md).
    {"tonemap_4k", record_tonemap, PIXELS_4K, "Gpix/s", 0U},
    {"taa_fragment_4k", record_taa_fragment, PIXELS_4K, "Gpix/s", TAA_PARAM(0U, 0U)},
    {"taa_fragment_quad_4k", record_taa_fragment, PIXELS_4K, "Gpix/s", TAA_PARAM(1U, 0U)},
    {"taa_fragment_cross_4k", record_taa_fragment, PIXELS_4K, "Gpix/s", TAA_PARAM(2U, 0U)},
    {"taa_compute_4k", record_taa_compute, PIXELS_4K, "Gpix/s", TAA_PARAM(TAA_COMPUTE, 0U)},
    {"taa_fragment_4k_rgba16f", record_taa_fragment, PIXELS_4K, "Gpix/s", TAA_PARAM(0U, 1U)},
    {"taa_fragment_quad_4k_rgba16f", record_taa_fragment, PIXELS_4K, "Gpix/s", TAA_PARAM(1U, 1U)},
    {"taa_fragment_cross_4k_rgba16f", record_taa_fragment, PIXELS_4K, "Gpix/s", TAA_PARAM(2U, 1U)},
    {"taa_compute_4k_rgba16f", record_taa_compute, PIXELS_4K, "Gpix/s", TAA_PARAM(TAA_COMPUTE, 1U)},
};

// Whether the device can run `b`: the temporal forms need their history
// format's uses, and the quad form subgroup quad operations in fragment
// shaders.
static bool bench_supported(const Ctx *ctx, const Bench *b)
{
    if (b->record == record_taa_fragment || b->record == record_taa_compute) {
        return ctx->taa_supported[TAA_MODE_OF(b->param)][TAA_FORMAT_OF(b->param)] != 0;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static NphResult load_shader(Ctx *ctx, const char *file, NphGpuShader *out)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.spv", NPH_SHADER_DIR, file);
    return nph_gpu_shader_load(ctx->dev, path, file, out);
}

static NphResult make_compute(Ctx *ctx, NphGpuShader shader, const char *name, NphGpuPipeline *out)
{
    NphGpuComputePipelineDesc d;
    memset(&d, 0, sizeof d);
    d.shader = shader;
    d.name = name;
    return nph_gpu_pipeline_create_compute(ctx->dev, &d, out);
}

static NphResult make_graphics(Ctx *ctx, NphGpuShader fs, NphGpuFormat format, const char *name,
                               NphGpuPipeline *out)
{
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = ctx->fullscreen_vs;
    d.fragment = fs;
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.name = name;
    return nph_gpu_pipeline_create_graphics(ctx->dev, &d, out);
}

static NphResult make_target(Ctx *ctx, NphGpuFormat format, u32 usage, const char *name,
                             NphGpuImage *out)
{
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = format;
    d.width = WIDTH_4K;
    d.height = HEIGHT_4K;
    d.mip_count = 1U;
    d.usage = usage;
    d.name = name;
    return nph_gpu_image_create(ctx->dev, &d, out);
}

// A small xorshift generator: the noise needs no quality, only to defeat
// the hardware's colour compression.
static u32 noise_next(u32 *state)
{
    u32 x = *state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return x;
}

// A B10G11R11 texel of noise: each channel's exponent between 2^-6 and
// 2^4, so every value is finite and in the range shading produces.
static u32 noise_b10g11r11(u32 *state)
{
    const u32 n = noise_next(state);
    const u32 r = ((8U + (n & 7U) + ((n >> 3U) & 3U)) << 6U) | ((n >> 5U) & 0x3FU);
    const u32 g = ((8U + ((n >> 11U) & 7U) + ((n >> 14U) & 3U)) << 6U) | ((n >> 16U) & 0x3FU);
    const u32 b = ((8U + ((n >> 22U) & 7U) + ((n >> 25U) & 3U)) << 5U) | ((n >> 27U) & 0x1FU);
    return r | (g << 11U) | (b << 22U);
}

// Creates a 4K image of `format` and, when `noise` is set, fills it with
// noise through the upload ring (4 bytes per texel, or 8: RGBA16F and the
// visibility buffer's R32G32_UINT).
static NphResult make_noise_image(Ctx *ctx, NphGpuFormat format, u32 usage, const char *name,
                                  u32 seed, NphGpuImage *out)
{
    NPH_TRY(make_target(ctx, format, usage, name, out));
    const u64 texels = (u64)WIDTH_4K * HEIGHT_4K;
    const bool wide = format != NPH_GPU_FORMAT_B10G11R11_UFLOAT;
    const u64 bytes = texels * (wide ? 8U : 4U);
    u32 *words = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, bytes / 4U);
    if (words == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 state = seed;
    for (u64 i = 0; i < texels; ++i) {
        if (format == NPH_GPU_FORMAT_B10G11R11_UFLOAT) {
            words[i] = noise_b10g11r11(&state);
        } else if (format == NPH_GPU_FORMAT_R32G32_UINT) {
            // A cluster and triangle, then a reverse-Z depth in (0, 1].
            words[2U * i] = noise_next(&state);
            const f32 depth = (f32)((noise_next(&state) >> 8U) + 1U) / 16777216.0F;
            memcpy(&words[2U * i + 1U], &depth, sizeof depth);
        } else {
            const u32 n = noise_next(&state);
            const NphVec4 c = nph_vec4((f32)(n & 0xFFU) / 64.0F, (f32)((n >> 8U) & 0xFFU) / 64.0F,
                                       (f32)((n >> 16U) & 0xFFU) / 64.0F, 1.0F);
            const u64 packed = nph_pack_f16x4(c);
            memcpy(&words[2U * i], &packed, sizeof packed);
        }
    }
    const NphResult r = nph_gpu_upload_image(ctx->dev, *out, 0U, 0U, 1U, words, bytes);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, words, bytes / 4U);
    NPH_TRY(r);
    return nph_gpu_upload_flush(ctx->dev, NULL);
}

// The temporal resolve's images and pipelines, for the history formats the
// device can sample, render to, and store to.
static NphResult setup_taa(Ctx *ctx)
{
    NphGpuDevice *dev = ctx->dev;
    const u32 input = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    NPH_TRY(make_noise_image(ctx, NPH_GPU_FORMAT_B10G11R11_UFLOAT, input, "taa hdr", 0x9E3779B9U,
                             &ctx->hdr));
    NPH_TRY(make_noise_image(ctx, NPH_GPU_FORMAT_R32G32_UINT, input, "taa visibility", 0x85EBCA6BU,
                             &ctx->vis));
    const NphGpuFormat formats[TAA_FORMATS] = {NPH_GPU_FORMAT_B10G11R11_UFLOAT,
                                               NPH_GPU_FORMAT_R16G16B16A16_SFLOAT};
    const NphGpuFeatures *feat = &nph_gpu_device_info(dev)->features;
    for (u32 f = 0; f < TAA_FORMATS; ++f) {
        const u32 render = (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED_LINEAR |
                           (u32)NPH_GPU_FORMAT_FEATURE_COLOR_ATTACHMENT;
        const bool can_render = nph_gpu_format_supports(dev, formats[f], render);
        const bool can_store =
            can_render && nph_gpu_format_supports(dev, formats[f], NPH_GPU_FORMAT_FEATURE_STORAGE);
        if (!can_render) {
            continue;
        }
        NPH_TRY(make_noise_image(ctx, formats[f], input, "taa history in", 0xC2B2AE35U + f,
                                 &ctx->history_in[f]));
        u32 usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT;
        if (can_store) {
            usage |= (u32)NPH_GPU_IMAGE_USAGE_STORAGE;
        }
        NPH_TRY(make_target(ctx, formats[f], usage, "taa history out", &ctx->history_out[f]));
        for (u32 m = 0; m < TAA_MODES; ++m) {
            if (m == TAA_MODE_QUAD && !(feat->subgroup_quad && feat->subgroup_fragment)) {
                continue;
            }
            NphGpuGraphicsPipelineDesc d;
            memset(&d, 0, sizeof d);
            d.vertex = ctx->fullscreen_vs;
            d.fragment = ctx->taa_fs;
            d.color_formats[0] = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
            d.color_formats[1] = formats[f];
            d.color_count = 2U;
            d.specialization[0] = m;
            d.specialization[1] = formats[f] == NPH_GPU_FORMAT_B10G11R11_UFLOAT ? 1U : 0U;
            d.specialization_count = 2U;
            d.name = "taa (fragment)";
            NPH_TRY(nph_gpu_pipeline_create_graphics(dev, &d, &ctx->taa_fragment[m][f]));
            ctx->taa_supported[m][f] = 1;
        }
        if (can_store) {
            NphGpuComputePipelineDesc d;
            memset(&d, 0, sizeof d);
            d.shader = ctx->taa_cs;
            d.specialization[1] = formats[f] == NPH_GPU_FORMAT_B10G11R11_UFLOAT ? 1U : 0U;
            d.specialization_count = 2U;
            d.name = "taa (compute)";
            NPH_TRY(nph_gpu_pipeline_create_compute(dev, &d, &ctx->taa_compute[f]));
            ctx->taa_supported[TAA_COMPUTE][f] = 1;
        }
    }
    NphGpuSamplerDesc smp;
    nph_gpu_sampler_desc_default(&smp);
    smp.mip_filter = NPH_GPU_FILTER_NEAREST;
    smp.address_u = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    smp.address_v = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    smp.address_w = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    smp.name = "taa history";
    return nph_gpu_sampler_create(dev, &smp, &ctx->linear_clamp);
}

static NphResult setup(Ctx *ctx)
{
    NphGpuDevice *dev = ctx->dev;
    NPH_TRY(load_shader(ctx, "bench_fill.comp", &ctx->fill_cs));
    NPH_TRY(load_shader(ctx, "bench_read.comp", &ctx->read_cs));
    NPH_TRY(load_shader(ctx, "bench_fma.comp", &ctx->fma_cs));
    NPH_TRY(load_shader(ctx, "bench_random.comp", &ctx->random_cs));
    NPH_TRY(load_shader(ctx, "bench_fullscreen.vert", &ctx->fullscreen_vs));
    NPH_TRY(load_shader(ctx, "bench_color.frag", &ctx->color_fs));
    NPH_TRY(load_shader(ctx, "bench_sample.frag", &ctx->sample_fs));
    NPH_TRY(load_shader(ctx, "bench_visbuffer.frag", &ctx->visbuffer_fs));
    NPH_TRY(load_shader(ctx, "bench_taa.frag", &ctx->taa_fs));
    NPH_TRY(load_shader(ctx, "bench_taa.comp", &ctx->taa_cs));
    NPH_TRY(load_shader(ctx, "bench_tonemap.frag", &ctx->tonemap_fs));
    NPH_TRY(make_compute(ctx, ctx->fill_cs, "fill", &ctx->fill));
    NPH_TRY(make_compute(ctx, ctx->read_cs, "read", &ctx->read));
    NPH_TRY(make_compute(ctx, ctx->fma_cs, "fma", &ctx->fma));
    NPH_TRY(make_compute(ctx, ctx->random_cs, "random", &ctx->random));
    NPH_TRY(make_graphics(ctx, ctx->color_fs, NPH_GPU_FORMAT_R8G8B8A8_UNORM, "raster rgba8",
                          &ctx->raster_rgba8));
    NPH_TRY(make_graphics(ctx, ctx->color_fs, NPH_GPU_FORMAT_B10G11R11_UFLOAT, "raster r11g11b10",
                          &ctx->raster_r11g11b10));
    NPH_TRY(make_graphics(ctx, ctx->color_fs, NPH_GPU_FORMAT_R16G16B16A16_SFLOAT, "raster rgba16f",
                          &ctx->raster_rgba16f));
    NPH_TRY(make_graphics(ctx, ctx->sample_fs, NPH_GPU_FORMAT_R8G8B8A8_UNORM, "sample",
                          &ctx->sample_rgba8));
    NPH_TRY(make_graphics(ctx, ctx->visbuffer_fs, NPH_GPU_FORMAT_R32G32_UINT, "visbuffer",
                          &ctx->visbuffer));
    NPH_TRY(make_graphics(ctx, ctx->tonemap_fs, NPH_GPU_FORMAT_R8G8B8A8_UNORM, "tonemap",
                          &ctx->tonemap));
    NPH_TRY(setup_taa(ctx));

    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = BIG_BYTES;
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC |
               (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.name = "big a";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &ctx->big_a));
    bd.name = "big b";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &ctx->big_b));
    bd.size = NPH_MIB(1);
    bd.name = "small";
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, &ctx->small));

    const u32 color = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT;
    NPH_TRY(
        make_target(ctx, NPH_GPU_FORMAT_R8G8B8A8_UNORM, color, "target rgba8", &ctx->target_rgba8));
    NPH_TRY(make_target(ctx, NPH_GPU_FORMAT_B10G11R11_UFLOAT, color, "target r11g11b10",
                        &ctx->target_r11g11b10));
    NPH_TRY(make_target(ctx, NPH_GPU_FORMAT_R16G16B16A16_SFLOAT, color, "target rgba16f",
                        &ctx->target_rgba16f));
    NPH_TRY(
        make_target(ctx, NPH_GPU_FORMAT_R32G32_UINT, color, "target r32g32", &ctx->target_r32g32));
    NPH_TRY(make_target(ctx, NPH_GPU_FORMAT_R8G8B8A8_UNORM, (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST,
                        "clear target", &ctx->clear_target));

    // A 4K texture with a mip chain, so sampling sees one texel per pixel.
    NphGpuImageDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    td.width = 4096U;
    td.height = 4096U;
    td.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
               (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    td.name = "texture 4k";
    NPH_TRY(nph_gpu_image_create(dev, &td, &ctx->texture));
    const u64 texel_bytes = 4096ULL * 4096ULL * 4ULL;
    u8 *texels = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, texel_bytes);
    if (texels == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u64 i = 0; i < texel_bytes; ++i) {
        texels[i] = (u8)(i * 2654435761ULL >> 13U);
    }
    NphResult r = nph_gpu_upload_image(dev, ctx->texture, 0U, 0U, 1U, texels, texel_bytes);
    NPH_FREE_ARRAY(nph_allocator_system(), u8, texels, texel_bytes);
    NPH_TRY(r);
    NPH_TRY(nph_gpu_upload_flush(dev, NULL));
    NphGpuCmd *cmd = NULL;
    NPH_TRY(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "mips", &cmd));
    NphGpuImageBarrier to_src;
    memset(&to_src, 0, sizeof to_src);
    to_src.image = ctx->texture;
    to_src.src_stages = (u32)NPH_GPU_STAGE_ALL;
    to_src.src_access = (u32)NPH_GPU_ACCESS_MEMORY_READ;
    to_src.old_layout = NPH_GPU_LAYOUT_SHADER_READ_ONLY;
    to_src.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
    to_src.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
    to_src.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
    to_src.mip_count = 1U;
    to_src.layer_count = NPH_GPU_ALL_LAYERS;
    NphGpuBarrierDesc barrier;
    memset(&barrier, 0, sizeof barrier);
    barrier.images = &to_src;
    barrier.image_count = 1U;
    nph_gpu_cmd_barrier(cmd, &barrier);
    nph_gpu_cmd_generate_mips(cmd, ctx->texture, NPH_GPU_LAYOUT_SHADER_READ_ONLY,
                              (u32)NPH_GPU_STAGE_FRAGMENT_SHADER, (u32)NPH_GPU_ACCESS_SHADER_READ);
    nph_gpu_cmd_end(cmd);
    NphGpuSubmitDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.cmds = &cmd;
    sd.cmd_count = 1U;
    NPH_TRY(nph_gpu_submit(dev, &sd, NULL));
    NPH_TRY(nph_gpu_device_wait_idle(dev));

    NphGpuSamplerDesc smp;
    nph_gpu_sampler_desc_default(&smp);
    NPH_TRY(nph_gpu_sampler_create(dev, &smp, &ctx->sampler));
    return NPH_RESULT_OK;
}

static void teardown(Ctx *ctx)
{
    NphGpuDevice *dev = ctx->dev;
    NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(dev));
    const NphGpuPipeline pipelines[] = {
        ctx->fill,           ctx->read,         ctx->fma,
        ctx->random,         ctx->raster_rgba8, ctx->raster_r11g11b10,
        ctx->raster_rgba16f, ctx->sample_rgba8, ctx->visbuffer,
        ctx->tonemap};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(pipelines); ++i) {
        nph_gpu_pipeline_destroy(dev, pipelines[i]);
    }
    for (u32 f = 0; f < TAA_FORMATS; ++f) {
        for (u32 m = 0; m < TAA_MODES; ++m) {
            nph_gpu_pipeline_destroy(dev, ctx->taa_fragment[m][f]);
        }
        nph_gpu_pipeline_destroy(dev, ctx->taa_compute[f]);
        nph_gpu_image_destroy(dev, ctx->history_in[f]);
        nph_gpu_image_destroy(dev, ctx->history_out[f]);
    }
    const NphGpuShader shaders[] = {ctx->fill_cs,   ctx->read_cs,       ctx->fma_cs,
                                    ctx->random_cs, ctx->fullscreen_vs, ctx->color_fs,
                                    ctx->sample_fs, ctx->visbuffer_fs,  ctx->taa_fs,
                                    ctx->taa_cs,    ctx->tonemap_fs};
    for (u32 i = 0; i < NPH_ARRAY_COUNT(shaders); ++i) {
        nph_gpu_shader_destroy(dev, shaders[i]);
    }
    nph_gpu_image_destroy(dev, ctx->vis);
    nph_gpu_image_destroy(dev, ctx->hdr);
    nph_gpu_sampler_destroy(dev, ctx->linear_clamp);
    nph_gpu_sampler_destroy(dev, ctx->sampler);
    nph_gpu_image_destroy(dev, ctx->texture);
    nph_gpu_image_destroy(dev, ctx->clear_target);
    nph_gpu_image_destroy(dev, ctx->target_r32g32);
    nph_gpu_image_destroy(dev, ctx->target_rgba16f);
    nph_gpu_image_destroy(dev, ctx->target_r11g11b10);
    nph_gpu_image_destroy(dev, ctx->target_rgba8);
    nph_gpu_buffer_destroy(dev, ctx->small);
    nph_gpu_buffer_destroy(dev, ctx->big_b);
    nph_gpu_buffer_destroy(dev, ctx->big_a);
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

typedef struct Result {
    const Bench *bench;
    u32 repeats;
    f64 median_ns;
    f64 min_ns;
    f64 max_ns;
    f64 metric;
} Result;

static int compare_f64(const void *a, const void *b)
{
    const f64 x = *(const f64 *)a;
    const f64 y = *(const f64 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Runs `b` back to back, two frames in flight, for at least `min_ns` of wall
// time, so the GPU reaches its sustained clock state before the measured
// frames. Nothing is read back.
static NphResult warm_frames(Ctx *ctx, const Bench *b, u64 min_ns)
{
    NphGpuDevice *dev = ctx->dev;
    const u64 start = nph_time_now_ns();
    u32 frames = 0U;
    while (frames < 2U || nph_time_now_ns() - start < min_ns) {
        NPH_TRY(nph_gpu_frame_begin(dev));
        NphGpuCmd *cmd = NULL;
        NPH_TRY(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "warm-up", &cmd));
        b->record(ctx, b, cmd);
        nph_gpu_cmd_end(cmd);
        NphGpuSubmitDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.cmds = &cmd;
        sd.cmd_count = 1U;
        NPH_TRY(nph_gpu_submit(dev, &sd, NULL));
        NPH_TRY(nph_gpu_frame_end(dev));
        ++frames;
    }
    return NPH_RESULT_OK;
}

// One benchmark: `repeats` frames, each with the recorded commands between
// two timestamps, read back two frames later through the device's frame
// timestamp mechanism.
static NphResult run_bench(Ctx *ctx, const Bench *b, u32 repeats, Result *out)
{
    NphGpuDevice *dev = ctx->dev;
    const f64 period = (f64)nph_gpu_device_info(dev)->limits.timestamp_period_ns;
    u32 begin_index[MAX_REPEATS];
    u32 end_index[MAX_REPEATS];
    f64 samples[MAX_REPEATS];
    u32 sample_count = 0U;
    // Warm-up frames bring the clocks up (see the file comment); the first
    // iteration of the measured loop is discarded as well.
    NPH_TRY(warm_frames(ctx, b, WARMUP_NS));
    const u32 total = repeats + 1U;
    for (u32 frame = 0; frame < total + NPH_GPU_FRAMES_IN_FLIGHT; ++frame) {
        NPH_TRY(nph_gpu_frame_begin(dev));
        if (frame >= NPH_GPU_FRAMES_IN_FLIGHT) {
            const u32 done = frame - NPH_GPU_FRAMES_IN_FLIGHT;
            u64 index = 0U;
            const u64 *ticks = NULL;
            u32 count = 0U;
            if (nph_gpu_frame_timestamps(dev, &index, &ticks, &count) && done >= 1U &&
                begin_index[done] < count && end_index[done] < count) {
                samples[sample_count++] =
                    (f64)(ticks[end_index[done]] - ticks[begin_index[done]]) * period;
            }
        }
        if (frame < total) {
            NphGpuCmd *cmd = NULL;
            NPH_TRY(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, b->name, &cmd));
            begin_index[frame] = nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
            b->record(ctx, b, cmd);
            end_index[frame] = nph_gpu_cmd_write_timestamp(cmd, (u32)NPH_GPU_STAGE_ALL);
            nph_gpu_cmd_end(cmd);
            NphGpuSubmitDesc sd;
            memset(&sd, 0, sizeof sd);
            sd.cmds = &cmd;
            sd.cmd_count = 1U;
            NPH_TRY(nph_gpu_submit(dev, &sd, NULL));
        }
        NPH_TRY(nph_gpu_frame_end(dev));
    }
    NPH_TRY(nph_gpu_device_wait_idle(dev));
    if (sample_count == 0U) {
        return NPH_RESULT_UNSUPPORTED;
    }
    qsort(samples, sample_count, sizeof samples[0], compare_f64);
    out->bench = b;
    out->repeats = sample_count;
    out->median_ns = samples[sample_count / 2U];
    out->min_ns = samples[0];
    out->max_ns = samples[sample_count - 1U];
    if (strcmp(b->unit, "ns") == 0) {
        out->metric = out->median_ns;
    } else if (strcmp(b->unit, "TFLOPS") == 0) {
        out->metric = b->items / out->median_ns / 1000.0; // FLOP/ns = GFLOPS
    } else {
        out->metric = b->items / out->median_ns; // bytes/ns = GB/s, pixels/ns = Gpix/s
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static bool glob_match(const char *pattern, const char *s)
{
    if (*pattern == '\0') {
        return *s == '\0';
    }
    if (*pattern == '*') {
        for (const char *t = s;; ++t) {
            if (glob_match(pattern + 1, t)) {
                return true;
            }
            if (*t == '\0') {
                return false;
            }
        }
    }
    return *s != '\0' && *pattern == *s && glob_match(pattern + 1, s + 1);
}

static int write_json(const char *path, const Result *results, u32 count,
                      const NphGpuDeviceInfo *info)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "gpu_bench: cannot write %s\n", path);
        return 2;
    }
    // Portable C, as in nbench: the tool is single-threaded, so gmtime's
    // static result is fine.
    char stamp[32] = "unknown";
    const time_t now = (time_t)(nph_time_wall_ns() / NPH_TIME_NS_PER_S);
    const struct tm *utc = gmtime(&now);
    if (utc != NULL) {
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", utc);
    }
    fprintf(f, "{\n  \"schema\": 1,\n  \"executable\": \"gpu_bench\",\n  \"timestamp\": \"%s\",\n",
            stamp);
#if NPH_CONFIG_DEBUG
    const char *config = "Debug";
#elif NPH_CONFIG_DEVELOPMENT
    const char *config = "Development";
#elif NPH_CONFIG_PROFILE
    const char *config = "Profile";
#else
    const char *config = "Shipping";
#endif
    fprintf(f, "  \"config\": \"%s\",\n", config);
    fprintf(f,
            "  \"host\": {\"os\": \"%s\", \"arch\": \"x86_64\", \"gpu\": \"%s\", \"driver\": \"%s "
            "%s\", "
            "\"api\": \"%u.%u.%u\"},\n",
            nph_platform_os_name(), info->name, info->driver_name, info->driver_info,
            info->api_version_major, info->api_version_minor, info->api_version_patch);
    fprintf(f, "  \"benchmarks\": [\n");
    for (u32 i = 0; i < count; ++i) {
        const Result *r = &results[i];
        fprintf(f,
                "    {\"name\": \"%s\", \"label\": \"\", \"iterations\": 1, "
                "\"items_per_iteration\": 1, "
                "\"repeats\": %u, \"ns_per_op\": %.3f, \"min_ns_per_op\": %.3f, \"max_ns_per_op\": "
                "%.3f, "
                "\"metric\": %.3f, \"unit\": \"%s\"}%s\n",
                r->bench->name, r->repeats, r->median_ns, r->min_ns, r->max_ns, r->metric,
                r->bench->unit, i + 1U < count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}

static int check_budgets(const char *path, const Result *results, u32 count)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "gpu_bench: cannot read budgets %s\n", path);
        return 2;
    }
    int status = 0;
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char name[128];
        if (sscanf(line, "%127s", name) <= 0) {
            continue; // blank or comment-only line
        }
        // The number follows the name; strtod reports where it stopped (as
        // in nbench; sscanf cannot report a conversion failure).
        const char *rest = strstr(line, name) + strlen(name);
        char *end = NULL;
        const double max_ns = strtod(rest, &end);
        if (end == rest || !(max_ns > 0.0)) {
            fprintf(stderr, "gpu_bench: %s: expected `name max_ns`, got: %s", path, line);
            fclose(f);
            return 2;
        }
        for (u32 i = 0; i < count; ++i) {
            if (strcmp(results[i].bench->name, name) == 0 && results[i].median_ns > max_ns) {
                fprintf(stderr, "BUDGET EXCEEDED: %s %.3f ms > %.3f ms\n", name,
                        results[i].median_ns / 1.0e6, max_ns / 1.0e6);
                status = 4;
            }
        }
    }
    fclose(f);
    return status;
}

int main(int argc, char **argv)
{
    const char *filter = "*";
    const char *json = NULL;
    const char *budgets = NULL;
    u32 repeats = 10U;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--filter=", 9U) == 0) {
            filter = argv[i] + 9;
        } else if (strncmp(argv[i], "--repeats=", 10U) == 0) {
            repeats = (u32)strtoul(argv[i] + 10, NULL, 10);
        } else if (strncmp(argv[i], "--json=", 7U) == 0) {
            json = argv[i] + 7;
        } else if (strncmp(argv[i], "--budgets=", 10U) == 0) {
            budgets = argv[i] + 10;
        } else if (strncmp(argv[i], "--min-time=", 11U) == 0) {
            // Accepted for symmetry with nbench; GPU runs are counted, not timed.
        } else if (strcmp(argv[i], "--list") == 0) {
            list = true;
        } else {
            fprintf(stderr, "usage: gpu_bench [--filter=GLOB] [--repeats=N] [--json=FILE] "
                            "[--budgets=FILE] [--list]\n");
            return 2;
        }
    }
    repeats = nph_clamp_u32(repeats, 1U, MAX_REPEATS - 1U);
    if (list) {
        for (u32 i = 0; i < NPH_ARRAY_COUNT(g_benches); ++i) {
            printf("%s\n", g_benches[i].name);
        }
        return 0;
    }

    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = nph_allocator_system();
    dd.app_name = "gpu_bench";
    dd.headless = 1;
    dd.validation = NPH_GPU_TOGGLE_OFF;
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    const NphResult dr = nph_gpu_device_create(&dd, &ctx.dev);
    if (dr == NPH_RESULT_UNSUPPORTED) {
        fprintf(stderr, "gpu_bench: no Vulkan device; skipping\n");
        return EXIT_SKIPPED;
    }
    if (dr != NPH_RESULT_OK) {
        fprintf(stderr, "gpu_bench: device creation failed (%s)\n", nph_result_string(dr));
        return 1;
    }
    const NphGpuDeviceInfo *info = nph_gpu_device_info(ctx.dev);
    printf("gpu_bench: %s (%s %s), %u repeats per benchmark, timestamp period %.3f ns\n",
           info->name, info->driver_name, info->driver_info, repeats,
           (f64)info->limits.timestamp_period_ns);
    int status = 0;
    const NphResult sr = setup(&ctx);
    if (sr != NPH_RESULT_OK) {
        fprintf(stderr, "gpu_bench: setup failed (%s)\n", nph_result_string(sr));
        status = 1;
    }
    Result results[NPH_ARRAY_COUNT(g_benches)];
    u32 result_count = 0U;
    if (status == 0) {
        // One second of FMA load before the suite (see the file comment).
        for (u32 i = 0; i < NPH_ARRAY_COUNT(g_benches) && status == 0; ++i) {
            if (strcmp(g_benches[i].name, "fma_fp32") == 0) {
                const NphResult wr = warm_frames(&ctx, &g_benches[i], PREWARM_NS);
                if (wr != NPH_RESULT_OK) {
                    fprintf(stderr, "gpu_bench: warm-up failed (%s)\n", nph_result_string(wr));
                    status = 1;
                }
            }
        }
    }
    if (status == 0) {
        printf("%-32s %12s %12s %12s %14s\n", "benchmark", "median ms", "min ms", "max ms",
               "metric");
        for (u32 i = 0; i < NPH_ARRAY_COUNT(g_benches); ++i) {
            const Bench *b = &g_benches[i];
            if (!glob_match(filter, b->name)) {
                continue;
            }
            if (!bench_supported(&ctx, b)) {
                printf("%-32s skipped: the device lacks what it needs\n", b->name);
                continue;
            }
            Result r;
            memset(&r, 0, sizeof r);
            const NphResult rr = run_bench(&ctx, b, repeats, &r);
            if (rr != NPH_RESULT_OK) {
                fprintf(stderr, "%-32s failed: %s\n", b->name, nph_result_string(rr));
                status = 1;
                break;
            }
            results[result_count++] = r;
            printf("%-32s %12.3f %12.3f %12.3f %9.1f %s\n", b->name, r.median_ns / 1.0e6,
                   r.min_ns / 1.0e6, r.max_ns / 1.0e6, r.metric, b->unit);
        }
        if (result_count == 0U && status == 0) {
            fprintf(stderr, "gpu_bench: no benchmark matched '%s'\n", filter);
            status = 3;
        }
    }
    if (status == 0 && json != NULL) {
        status = write_json(json, results, result_count, info);
    }
    if (status == 0 && budgets != NULL) {
        status = check_budgets(budgets, results, result_count);
    }
    teardown(&ctx);
    nph_gpu_device_destroy(ctx.dev);
    return status;
}
