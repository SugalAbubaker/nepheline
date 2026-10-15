// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>
#include <nepheline/math/pack.h>

#include <cook/texture.h>
#include <string.h>

void cook_texture_settings_default(CookTextureSettings *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->usage = COOK_TEXTURE_COLOR;
    out->lz = NPH_LZ_LEVEL_HIGH;
}

NphTextureFormat cook_texture_format(CookTextureUsage usage, bool uncompressed)
{
    switch (usage) {
    case COOK_TEXTURE_COLOR:
        return uncompressed ? NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB : NPH_TEXTURE_FORMAT_BC7_SRGB;
    case COOK_TEXTURE_LINEAR:
        return uncompressed ? NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM : NPH_TEXTURE_FORMAT_BC7_UNORM;
    case COOK_TEXTURE_NORMAL:
        return uncompressed ? NPH_TEXTURE_FORMAT_R8G8_UNORM : NPH_TEXTURE_FORMAT_BC5_UNORM;
    case COOK_TEXTURE_MASK:
        return uncompressed ? NPH_TEXTURE_FORMAT_R8_UNORM : NPH_TEXTURE_FORMAT_BC4_UNORM;
    case COOK_TEXTURE_HDR:
        return uncompressed ? NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT
                            : NPH_TEXTURE_FORMAT_BC6H_UFLOAT;
    default:
        return NPH_TEXTURE_FORMAT_UNDEFINED;
    }
}

// ---------------------------------------------------------------------------
// Level encoding, one block row per job
// ---------------------------------------------------------------------------

typedef struct EncodeJob {
    const CookImage *level;
    const CookTextureSettings *settings;
    NphTextureFormat format;
    u8 *out;
    u32 blocks_x;
    u32 blocks_y;
    u32 block_bytes;
} EncodeJob;

static void encode_block_row(void *user, u32 begin, u32 end)
{
    const EncodeJob *job = (const EncodeJob *)user;
    for (u32 by = begin; by < end; ++by) {
        for (u32 bx = 0; bx < job->blocks_x; ++bx) {
            u8 *dst = job->out + ((u64)by * job->blocks_x + bx) * job->block_bytes;
            if (job->format == NPH_TEXTURE_FORMAT_BC6H_UFLOAT) {
                CookBlockRgbHalf half;
                cook_image_block_half(job->level, bx * 4U, by * 4U, &half);
                cook_bc6h_encode(&half, dst);
                continue;
            }
            CookBlockRgba8 block;
            cook_image_block_rgba8(job->level, bx * 4U, by * 4U, &block);
            switch (job->format) {
            case NPH_TEXTURE_FORMAT_BC7_UNORM:
            case NPH_TEXTURE_FORMAT_BC7_SRGB:
                cook_bc7_encode(&block, &job->settings->bc7, dst);
                break;
            case NPH_TEXTURE_FORMAT_BC5_UNORM: {
                u8 r[16];
                u8 g[16];
                for (u32 i = 0; i < 16U; ++i) {
                    r[i] = block.texels[i][0];
                    g[i] = block.texels[i][1];
                }
                cook_bc5_encode(r, g, dst);
                break;
            }
            case NPH_TEXTURE_FORMAT_BC4_UNORM: {
                u8 r[16];
                for (u32 i = 0; i < 16U; ++i) {
                    r[i] = block.texels[i][0];
                }
                cook_bc4_encode(r, dst);
                break;
            }
            default:
                NPH_UNREACHABLE();
            }
        }
    }
}

// Uncompressed formats are a channel selection per texel.
static void copy_uncompressed(const CookImage *level, NphTextureFormat format, u8 *out)
{
    const u64 texels = (u64)level->width * level->height;
    if (format == NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT) {
        const f32 *src = (const f32 *)level->pixels;
        u16 *dst = (u16 *)out;
        for (u64 i = 0; i < texels * 4U; ++i) {
            dst[i] = nph_f32_to_f16(src[i]);
        }
        return;
    }
    const u8 *src = (const u8 *)level->pixels;
    switch (format) {
    case NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    case NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB:
        memcpy(out, src, (size_t)(texels * 4U));
        break;
    case NPH_TEXTURE_FORMAT_R8G8_UNORM:
        for (u64 i = 0; i < texels; ++i) {
            out[i * 2U] = src[i * 4U];
            out[i * 2U + 1U] = src[i * 4U + 1U];
        }
        break;
    case NPH_TEXTURE_FORMAT_R8_UNORM:
        for (u64 i = 0; i < texels; ++i) {
            out[i] = src[i * 4U];
        }
        break;
    default:
        NPH_UNREACHABLE();
    }
}

NphResult cook_texture_encode_level(const CookImage *level, const CookTextureSettings *settings,
                                    NphJobSystem *jobs, void *out, u64 out_size)
{
    NPH_ASSERT(level != NULL && settings != NULL && out != NULL);
    const NphTextureFormat format =
        cook_texture_format(settings->usage, settings->uncompressed != 0);
    const NphTextureFormatInfo *info = nph_texture_format_info((u32)format);
    if (info->bytes_per_block == 0U ||
        out_size != nph_texture_level_bytes((u32)format, level->width, level->height)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const bool hdr = settings->usage == COOK_TEXTURE_HDR;
    if ((hdr && level->format != COOK_IMAGE_RGBA32F) ||
        (!hdr && level->format != COOK_IMAGE_RGBA8)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!info->compressed) {
        copy_uncompressed(level, format, (u8 *)out);
        return NPH_RESULT_OK;
    }
    EncodeJob job;
    job.level = level;
    job.settings = settings;
    job.format = format;
    job.out = (u8 *)out;
    job.blocks_x = (level->width + 3U) / 4U;
    job.blocks_y = (level->height + 3U) / 4U;
    job.block_bytes = info->bytes_per_block;
    if (jobs != NULL && job.blocks_y > 1U) {
        NphJobCounter counter = NPH_JOB_COUNTER_INIT;
        const NphResult r = nph_job_parallel_for(jobs, job.blocks_y, 1U, encode_block_row, &job,
                                                 NPH_JOB_PRIORITY_NORMAL, &counter);
        if (r == NPH_RESULT_OK) {
            nph_job_wait(jobs, &counter);
            return NPH_RESULT_OK;
        }
        // A full job pool is not a failure: encode here.
    }
    encode_block_row(&job, 0U, job.blocks_y);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The whole texture
// ---------------------------------------------------------------------------

// The storage image of one level from its linear float working copy.
static NphResult level_storage(NphAllocator *alloc, const CookImage *linear,
                               const CookTextureSettings *settings, CookImage *out)
{
    if (settings->usage == COOK_TEXTURE_HDR) {
        return cook_image_to_f32(alloc, linear, false, out);
    }
    return cook_image_to_rgba8(alloc, linear, settings->usage == COOK_TEXTURE_COLOR, out);
}

NphResult cook_texture(NphAllocator *alloc, const CookImage *image,
                       const CookTextureSettings *settings, NphJobSystem *jobs,
                       NphAssetWriter *writer)
{
    NPH_ASSERT(alloc != NULL && image != NULL && settings != NULL && writer != NULL);
    const bool hdr = settings->usage == COOK_TEXTURE_HDR;
    if ((hdr && image->format != COOK_IMAGE_RGBA32F) ||
        (!hdr && image->format != COOK_IMAGE_RGBA8)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphTextureFormat format =
        cook_texture_format(settings->usage, settings->uncompressed != 0);

    // The working copy: linear float, limited in size, normals unit length.
    CookImage linear;
    NPH_TRY(cook_image_to_f32(alloc, image, settings->usage == COOK_TEXTURE_COLOR, &linear));
    NphResult r = NPH_RESULT_OK;
    if (settings->max_extent != 0U) {
        r = cook_image_limit_extent(alloc, &linear, settings->max_extent);
    }
    if (r == NPH_RESULT_OK && settings->usage == COOK_TEXTURE_NORMAL) {
        cook_image_renormalize_normals(&linear);
    }
    u32 mip_count = settings->no_mips ? 1U : nph_texture_mip_count(linear.width, linear.height);
    mip_count = nph_min_u32(mip_count, NPH_TEXTURE_MAX_MIPS);

    NphTextureHeader header;
    memset(&header, 0, sizeof header);
    header.width = linear.width;
    header.height = linear.height;
    header.mip_count = mip_count;
    header.layer_count = 1U;
    header.format = (u32)format;
    header.kind = (u32)NPH_TEXTURE_2D;
    header.flags = settings->usage == COOK_TEXTURE_NORMAL ? (u32)NPH_TEXTURE_FLAG_NORMAL_MAP : 0U;
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(writer, NPH_TEXTURE_TAG_HEADER, 0U, &header, sizeof header,
                                      NPH_ASSET_CODEC_NONE, 0U);
    }
    nph_asset_writer_set_lz_level(writer, settings->lz);

    for (u32 mip = 0; mip < mip_count && r == NPH_RESULT_OK; ++mip) {
        if (mip != 0U) {
            CookImage smaller;
            r = cook_image_downsample(alloc, &linear, &smaller);
            if (r != NPH_RESULT_OK) {
                break;
            }
            cook_image_free(&linear);
            linear = smaller;
            if (settings->usage == COOK_TEXTURE_NORMAL) {
                cook_image_renormalize_normals(&linear);
            }
        }
        CookImage storage;
        r = level_storage(alloc, &linear, settings, &storage);
        if (r != NPH_RESULT_OK) {
            break;
        }
        const u64 bytes = nph_texture_level_bytes((u32)format, storage.width, storage.height);
        void *encoded = NPH_ALLOC(alloc, bytes, 16U);
        if (encoded == NULL) {
            cook_image_free(&storage);
            r = NPH_RESULT_OUT_OF_MEMORY;
            break;
        }
        r = cook_texture_encode_level(&storage, settings, jobs, encoded, bytes);
        if (r == NPH_RESULT_OK) {
            r = nph_asset_writer_add_blob(writer, NPH_TEXTURE_TAG_MIP, mip, encoded, bytes,
                                          NPH_ASSET_CODEC_LZ, 0U);
        }
        NPH_FREE(alloc, encoded, bytes, 16U);
        cook_image_free(&storage);
    }
    cook_image_free(&linear);
    return r;
}
