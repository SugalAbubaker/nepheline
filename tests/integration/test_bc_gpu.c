// SPDX-License-Identifier: Apache-2.0
// The cooker's block codecs against a GPU's decoders (CLAUDE.md §8.8 "our
// own BC7/BC5/BC4/BC6H encoders"). Blocks are uploaded as BC4, BC5, BC6H,
// and BC7 textures, read back texel by texel with texelFetch
// (shaders/test/test_texel_fetch.comp), and compared with the cooker's
// CPU decoders (cook_bc*_decode).
//
// The CPU decoders are what the encoders measure their error against, so
// a decoder that misread the format would hide an encoder that writes it
// wrong: both would agree with each other and disagree with every GPU.
// This test is the independent check. Random bytes exercise every BC7
// mode, partition, rotation, and index selector; the encoders' own output
// for each format closes the loop. It runs on the RTX 3060 Ti locally and
// on lavapipe (Mesa's decoders) in CI.
#include <nepheline/math/pack.h>

#include "test_cook_support.h"
#include "test_gpu_support.h"

#include <cook/bc.h>
#include <nph_shared.h>

// A 64 x 64 texture: 16 x 16 blocks of each format per run.
#define BC_GRID   16U
#define BC_BLOCKS (BC_GRID * BC_GRID)
#define BC_SIZE   64U // BC_GRID blocks of 4 texels
NPH_STATIC_ASSERT(BC_SIZE == BC_GRID * 4U, "the test image is BC_GRID blocks across");

typedef struct BcGpuFixture {
    NphGpuDevice *dev;
    NphGpuShader shader;
    NphGpuPipeline pipeline;
    f32 *texels; // BC_SIZE * BC_SIZE RGBA floats, the GPU's decode
} BcGpuFixture;

NTEST_SETUP(BcGpuFixture)
{
    memset(f, 0, sizeof *f);
    f->dev = test_gpu_create(NULL);
    if (f->dev == NULL || !nph_gpu_device_info(f->dev)->features.texture_compression_bc) {
        return;
    }
    f->shader = test_load_shader(f->dev, "test_texel_fetch.comp");
    NphGpuComputePipelineDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.shader = f->shader;
    desc.name = "texel fetch";
    NTEST_ASSERT_EQ_INT(nph_gpu_pipeline_create_compute(f->dev, &desc, &f->pipeline),
                        NPH_RESULT_OK);
    f->texels = NPH_ALLOC_ARRAY(nph_allocator_system(), f32, (u64)BC_SIZE * BC_SIZE * 4U);
    NTEST_ASSERT_NOT_NULL(f->texels);
}

NTEST_TEARDOWN(BcGpuFixture)
{
    if (f->texels != NULL) {
        NPH_FREE_ARRAY(nph_allocator_system(), f32, f->texels, (u64)BC_SIZE * BC_SIZE * 4U);
    }
    if (f->dev != NULL) {
        if (!nph_gpu_pipeline_is_null(f->pipeline)) {
            nph_gpu_pipeline_destroy(f->dev, f->pipeline);
        }
        if (!nph_gpu_shader_is_null(f->shader)) {
            nph_gpu_shader_destroy(f->dev, f->shader);
        }
    }
    test_gpu_destroy(f->dev);
}

#define REQUIRE_BC(f)                                                                              \
    do {                                                                                           \
        if ((f)->dev == NULL) {                                                                    \
            NTEST_SKIP("no Vulkan device available");                                              \
        }                                                                                          \
        if (nph_gpu_pipeline_is_null((f)->pipeline)) {                                             \
            NTEST_SKIP("the device has no BC texture compression");                                \
        }                                                                                          \
    } while (0)

// Uploads BC_BLOCKS blocks (row-major, BC_GRID per row) as a texture of
// `format` and has the GPU decode every texel into f->texels.
static void gpu_decode(BcGpuFixture *f, NphGpuFormat format, const u8 *blocks, u32 block_bytes)
{
    NphGpuImage img = test_make_image_2d(
        f->dev, BC_SIZE, BC_SIZE, format, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST, "bc blocks");
    NTEST_ASSERT_EQ_INT(
        nph_gpu_upload_image(f->dev, img, 0U, 0U, 1U, blocks, (u64)BC_BLOCKS * block_bytes),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    const u64 bytes = (u64)BC_SIZE * BC_SIZE * 4U * sizeof(f32);
    NphGpuBuffer out = test_make_buffer(
        f->dev, bytes, (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
        NPH_GPU_MEMORY_DEVICE, "decoded texels");
    NphTestPush push;
    memset(&push, 0, sizeof push);
    push.buffer_address = nph_gpu_buffer_address(f->dev, out);
    push.count = BC_SIZE;
    push.value = BC_SIZE;
    push.texture_index = nph_gpu_image_sampled_index(f->dev, img);
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(f->dev, NPH_GPU_QUEUE_GRAPHICS, "bc decode", &cmd),
                        NPH_RESULT_OK);
    nph_gpu_cmd_bind_pipeline(cmd, f->pipeline);
    nph_gpu_cmd_push_constants(cmd, &push, sizeof push);
    nph_gpu_cmd_dispatch(cmd, BC_SIZE / 8U, BC_SIZE / 8U, 1U);
    test_submit_wait(f->dev, cmd);
    test_read_buffer(f->dev, out, 0U, bytes, f->texels);
    nph_gpu_buffer_destroy(f->dev, out);
    nph_gpu_image_destroy(f->dev, img);
}

// The GPU's decode of texel `t` (0..15, row-major) of block `b`.
static const f32 *gpu_texel(const BcGpuFixture *f, u32 b, u32 t)
{
    const u32 x = (b % BC_GRID) * 4U + t % 4U;
    const u32 y = (b / BC_GRID) * 4U + t / 4U;
    return &f->texels[((u64)y * BC_SIZE + x) * 4U];
}

static void random_bytes(u64 *rng, u8 *out, u64 n)
{
    for (u64 i = 0; i < n; ++i) {
        out[i] = (u8)test_rng_next(rng);
    }
}

// ---------------------------------------------------------------------------
// BC7
// ---------------------------------------------------------------------------

// UNORM BC7 decodes to 8-bit values by the format's integer arithmetic, so
// the GPU's floats are exactly value / 255: the comparison is exact.
static void check_bc7(BcGpuFixture *f, const u8 *blocks, const char *what)
{
    gpu_decode(f, NPH_GPU_FORMAT_BC7_UNORM, blocks, 16U);
    u32 mismatches = 0U;
    for (u32 b = 0; b < BC_BLOCKS; ++b) {
        CookBlockRgba8 out;
        NPH_IGNORE_RESULT(cook_bc7_decode(blocks + (u64)b * 16U, &out)); // reserved: all zero
        u8(*cpu)[4] = out.texels;
        for (u32 t = 0; t < 16U; ++t) {
            const f32 *g = gpu_texel(f, b, t);
            for (u32 c = 0; c < 4U; ++c) {
                const u32 gv = (u32)(g[c] * 255.0F + 0.5F);
                if (gv != cpu[t][c]) {
                    if (mismatches < 8U) {
                        NTEST_LOG("%s: block %u (mode bits 0x%02x) texel %u channel %u: GPU %u, "
                                  "CPU %u",
                                  what, b, blocks[(u64)b * 16U], t, c, gv, cpu[t][c]);
                    }
                    ++mismatches;
                }
            }
        }
    }
    NTEST_ASSERT_EQ_UINT(mismatches, 0U);
}

NTEST_F(BcGpuFixture, bc7_random_blocks_decode_as_the_gpu_decodes_them)
{
    REQUIRE_BC(f);
    // Any 16 bytes are a BC7 block: random ones cover every mode with its
    // partitions, rotations, index selectors, p-bits, and the reserved
    // mode (no mode bit, which decodes to zero).
    u8 blocks[BC_BLOCKS * 16U];
    u64 rng = 2026U;
    for (u32 run = 0; run < 4U; ++run) {
        random_bytes(&rng, blocks, sizeof blocks);
        // Force a spread of modes: block b gets mode b % 9 (8: reserved).
        for (u32 b = 0; b < BC_BLOCKS; ++b) {
            const u32 mode = b % 9U;
            u8 *first = &blocks[(u64)b * 16U];
            *first = (u8)(mode == 8U ? 0U : ((*first & ~((2U << mode) - 1U)) | (1U << mode)));
        }
        check_bc7(f, blocks, "random");
    }
}

NTEST_F(BcGpuFixture, bc7_encoder_output_decodes_as_the_gpu_decodes_it)
{
    REQUIRE_BC(f);
    // Blocks the encoder writes for gradients, edges, noise, and alpha,
    // with every mode forced in turn and the default mode set.
    u8 blocks[BC_BLOCKS * 16U];
    u64 rng = 9U;
    for (u32 b = 0; b < BC_BLOCKS; ++b) {
        CookBlockRgba8 in;
        u8(*rgba)[4] = in.texels;
        const u32 kind = b % 4U;
        for (u32 i = 0; i < 16U; ++i) {
            const u32 x = i % 4U;
            const u32 y = i / 4U;
            switch (kind) {
            case 0: // a gradient
                rgba[i][0] = (u8)(20U + x * 60U);
                rgba[i][1] = (u8)(240U - y * 50U);
                rgba[i][2] = (u8)(b & 0xFFU);
                rgba[i][3] = 255U;
                break;
            case 1: // an edge
                rgba[i][0] = x + y < 3U ? 250U : 10U;
                rgba[i][1] = x + y < 3U ? 30U : 200U;
                rgba[i][2] = (u8)(100U + y * 10U);
                rgba[i][3] = 255U;
                break;
            case 2: // noise
                rgba[i][0] = (u8)test_rng_next(&rng);
                rgba[i][1] = (u8)test_rng_next(&rng);
                rgba[i][2] = (u8)test_rng_next(&rng);
                rgba[i][3] = 255U;
                break;
            default: // colour and alpha ramps
                rgba[i][0] = (u8)(i * 16U);
                rgba[i][1] = (u8)(255U - i * 9U);
                rgba[i][2] = 128U;
                rgba[i][3] = (u8)(x * 80U + y * 5U);
                break;
            }
        }
        CookBc7Settings s;
        memset(&s, 0, sizeof s);
        const u32 choice = (b / 4U) % 9U;
        s.mode_mask = choice == 8U ? 0U : 1U << choice; // 0: the default set
        cook_bc7_encode(&in, &s, &blocks[(u64)b * 16U]);
    }
    check_bc7(f, blocks, "encoded");
}

// ---------------------------------------------------------------------------
// BC4 and BC5
// ---------------------------------------------------------------------------

// The format interpolates between two 8-bit endpoints in sevenths (e0 >
// e1) or fifths (otherwise, with 0 and 1 as the last two entries), and the
// specification gives the exact fractions. GPUs do not all honour them:
// the RTX 3060 Ti decodes the six-value palette of (0, 255) as 47.6, 95.3,
// 159.7, 207.4 where the fractions give 51, 102, 153, 204, and the eight-
// value one with similarly uneven steps (measured with this test's
// fixture; the deviation is the same for every block, at most 13 % of a
// palette step). The endpoints and the fixed 0 and 255 entries come out
// exact. A bitstream error (an index read from the wrong bits, endpoints
// swapped) moves a texel by whole palette steps, so the comparison allows
// 1.5 levels plus a fifth of the block's palette step: tight enough to
// catch any layout error on the random blocks, loose enough for every
// decoder's interpolation.
static f32 bc4_tolerance(const u8 *block, u32 index)
{
    const u32 e0 = block[0];
    const u32 e1 = block[1];
    if (e0 <= e1 && index >= 6U) {
        return 0.0F; // the fixed 0 and 255 entries
    }
    if (index <= 1U) {
        return 0.0F; // the endpoints themselves
    }
    const f32 range = (f32)(e0 > e1 ? e0 - e1 : e1 - e0);
    const f32 step = range / (e0 > e1 ? 7.0F : 5.0F);
    return 1.5F + 0.2F * step;
}

// The 3-bit index of `texel`: 48 bits, LSB first, in bytes 2..7.
static u32 bc4_index(const u8 *block, u32 texel)
{
    u64 bits = 0U;
    for (u32 i = 0; i < 6U; ++i) {
        bits |= (u64)block[2U + i] << (8U * i);
    }
    return (u32)(bits >> (3U * texel)) & 7U;
}

// Compares one channel of block `b` with the CPU's decode: raises *worst to
// the largest difference seen (in 8-bit levels) and counts the texels
// beyond the tolerance in *large.
static void check_bc4_channel(const BcGpuFixture *f, u32 b, u32 channel, const u8 cpu[16],
                              const u8 *block, f32 *worst, u32 *large)
{
    for (u32 t = 0; t < 16U; ++t) {
        const f32 g = gpu_texel(f, b, t)[channel] * 255.0F;
        const f32 d = nph_f32_abs(g - (f32)cpu[t]);
        *worst = nph_f32_max(*worst, d);
        const u32 index = bc4_index(block, t);
        if (d > bc4_tolerance(block, index) + 1e-3F) {
            if (*large < 6U) {
                NTEST_LOG("block %u texel %u: e0 %u e1 %u index %u: GPU %.3f CPU %u", b, t,
                          block[0], block[1], index, (f64)g, cpu[t]);
            }
            *large += 1U;
        }
    }
}

NTEST_F(BcGpuFixture, bc4_and_bc5_decode_as_the_gpu_decodes_them)
{
    REQUIRE_BC(f);
    u8 blocks[BC_BLOCKS * 16U];
    u64 rng = 44U;
    // BC4: random blocks (both endpoint orders occur) and encoded ones.
    random_bytes(&rng, blocks, (u64)BC_BLOCKS * 8U);
    for (u32 b = BC_BLOCKS / 2U; b < BC_BLOCKS; ++b) {
        u8 values[16];
        for (u32 i = 0; i < 16U; ++i) {
            values[i] = (u8)(b + i * 13U);
        }
        cook_bc4_encode(values, &blocks[(u64)b * 8U]);
    }
    gpu_decode(f, NPH_GPU_FORMAT_BC4_UNORM, blocks, 8U);
    f32 worst = 0.0F;
    u32 large = 0U;
    for (u32 b = 0; b < BC_BLOCKS; ++b) {
        u8 cpu[16];
        cook_bc4_decode(&blocks[(u64)b * 8U], cpu);
        check_bc4_channel(f, b, 0U, cpu, &blocks[(u64)b * 8U], &worst, &large);
    }
    NTEST_LOG("BC4: largest difference %.3f levels", (f64)worst);
    NTEST_ASSERT_EQ_UINT(large, 0U);
    // BC5: two BC4 halves, red then green.
    random_bytes(&rng, blocks, sizeof blocks);
    gpu_decode(f, NPH_GPU_FORMAT_BC5_UNORM, blocks, 16U);
    worst = 0.0F;
    for (u32 b = 0; b < BC_BLOCKS; ++b) {
        u8 r[16];
        u8 g[16];
        cook_bc5_decode(&blocks[(u64)b * 16U], r, g);
        check_bc4_channel(f, b, 0U, r, &blocks[(u64)b * 16U], &worst, &large);
        check_bc4_channel(f, b, 1U, g, &blocks[(u64)b * 16U + 8U], &worst, &large);
    }
    NTEST_LOG("BC5: largest difference %.3f levels", (f64)worst);
    NTEST_ASSERT_EQ_UINT(large, 0U);
}

// ---------------------------------------------------------------------------
// BC6H
// ---------------------------------------------------------------------------

NTEST_F(BcGpuFixture, bc6h_mode11_decodes_as_the_gpu_decodes_it)
{
    REQUIRE_BC(f);
    // Mode 11 is what the encoder writes: random endpoints and indices
    // under its mode bits, and encoded HDR gradients. BC6H's unquantising
    // and interpolation are integer arithmetic ending in half-float bits,
    // so the GPU's floats are exactly the CPU's halves.
    u8 blocks[BC_BLOCKS * 16U];
    u64 rng = 6U;
    random_bytes(&rng, blocks, sizeof blocks);
    for (u32 b = 0; b < BC_BLOCKS; ++b) {
        u8 *first = &blocks[(u64)b * 16U];
        *first = (u8)((*first & 0xE0U) | 0x03U); // mode 11 in the low five bits
    }
    for (u32 b = BC_BLOCKS / 2U; b < BC_BLOCKS; ++b) {
        CookBlockRgbHalf in;
        u16(*rgb)[3] = in.texels;
        for (u32 i = 0; i < 16U; ++i) {
            rgb[i][0] = nph_f32_to_f16(0.01F * (f32)(b + 1U) * (f32)(i + 1U));
            rgb[i][1] = nph_f32_to_f16(2.0F + (f32)(i % 4U));
            rgb[i][2] = nph_f32_to_f16((f32)(b % 7U) * 30.0F);
        }
        cook_bc6h_encode(&in, &blocks[(u64)b * 16U]);
    }
    gpu_decode(f, NPH_GPU_FORMAT_BC6H_UFLOAT, blocks, 16U);
    u32 mismatches = 0U;
    for (u32 b = 0; b < BC_BLOCKS; ++b) {
        CookBlockRgbHalf out;
        NTEST_ASSERT_TRUE(cook_bc6h_decode(&blocks[(u64)b * 16U], &out));
        u16(*cpu)[3] = out.texels;
        for (u32 t = 0; t < 16U; ++t) {
            const f32 *g = gpu_texel(f, b, t);
            for (u32 c = 0; c < 3U; ++c) {
                const f32 expect = nph_f16_to_f32(cpu[t][c]);
                if (g[c] != expect) {
                    if (mismatches < 8U) {
                        NTEST_LOG("BC6H block %u texel %u channel %u: GPU %g, CPU %g", b, t, c,
                                  (f64)g[c], (f64)expect);
                    }
                    ++mismatches;
                }
            }
        }
    }
    NTEST_ASSERT_EQ_UINT(mismatches, 0U);
}
