// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for the cooker's image decoding and texture pipeline
// (tools/cook, CLAUDE.md §8.8): PNG, JPEG, TGA, BMP, and Radiance HDR
// through stb_image and the engine's conversions, then (for small images)
// the texture cook into a block-compressed mip chain, whose written asset
// must pass the runtime's validation and texture view.
#include <nepheline/asset/format.h>
#include <nepheline/asset/texture.h>
#include <nepheline/core/log.h>

#include <cook/image.h>
#include <cook/texture.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Decoding is allowed up to this many texels (a hostile header can claim
// 16384 x 16384, a gigabyte of pixels); the texture cook runs on images up
// to 64 x 64, where the BC7 encoder stays fast.
#define FUZZ_MAX_DECODE_TEXELS (1U << 20U)
#define FUZZ_MAX_COOK_EXTENT   64U

static void cook_and_check(NphAllocator *alloc, const CookImage *img, CookTextureUsage usage)
{
    CookTextureSettings s;
    cook_texture_settings_default(&s);
    s.usage = usage;
    s.lz = NPH_LZ_LEVEL_FAST;
    NphAssetWriter w;
    nph_asset_writer_init(&w, alloc, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION,
                          nph_guid_from_cstr("fuzz"), 0U);
    if (cook_texture(alloc, img, &s, NULL, &w) == NPH_RESULT_OK) {
        void *image = NULL;
        u64 size = 0U;
        if (nph_asset_writer_finish_alloc(&w, alloc, &image, &size) == NPH_RESULT_OK) {
            NphAssetFileInfo info;
            if (nph_asset_file_validate(image, size, &info) != NPH_RESULT_OK) {
                abort(); // the cooker wrote a file its own reader refuses
            }
            NphAssetBlob blobs[NPH_TEXTURE_MAX_MIPS + 1U];
            void *owned[NPH_TEXTURE_MAX_MIPS + 1U];
            u32 count = 0U;
            for (u32 i = 0; i < info.blob_count && i < NPH_ARRAY_COUNT(blobs); ++i) {
                const NphAssetBlobEntry *e = &info.blobs[i];
                void *raw = NPH_ALLOC(alloc, e->raw_size != 0U ? e->raw_size : 1U, e->alignment);
                if (raw == NULL || nph_asset_file_unpack_blob(image, e, raw) != NPH_RESULT_OK) {
                    abort();
                }
                blobs[i].tag = e->tag;
                blobs[i].index = e->index;
                blobs[i].codec = e->codec;
                blobs[i].alignment = e->alignment;
                blobs[i].size = e->raw_size;
                blobs[i].data = raw;
                owned[i] = raw;
                ++count;
            }
            NphAsset asset;
            memset(&asset, 0, sizeof asset);
            asset.header = info.header;
            asset.blobs = blobs;
            asset.blob_count = count;
            asset.path = NPH_STR("fuzz");
            NphTextureView view;
            if (nph_texture_view(&asset, &view) != NPH_RESULT_OK) {
                abort(); // the cooker wrote a texture the runtime cannot view
            }
            for (u32 i = 0; i < count; ++i) {
                NPH_FREE(alloc, owned[i], blobs[i].size != 0U ? blobs[i].size : 1U,
                         blobs[i].alignment);
            }
            NPH_FREE(alloc, image, size, NPH_ASSET_BLOB_MAX_ALIGN);
        }
    }
    nph_asset_writer_free(&w);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0U || size > (1U << 20U)) {
        return 0;
    }
    // Rejecting input is the common case; logging every rejection would
    // dominate the run.
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    u32 w;
    u32 h;
    u32 channels;
    if (cook_image_info(data, size, &w, &h, &channels) != NPH_RESULT_OK ||
        (u64)w * h > FUZZ_MAX_DECODE_TEXELS) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    CookImage img;
    if (cook_image_decode(alloc, data, size, &img) != NPH_RESULT_OK) {
        return 0;
    }
    if (img.width != w || img.height != h) {
        abort(); // the header and the decode disagree
    }
    if (img.width <= FUZZ_MAX_COOK_EXTENT && img.height <= FUZZ_MAX_COOK_EXTENT) {
        if (img.format == COOK_IMAGE_RGBA32F) {
            cook_and_check(alloc, &img, COOK_TEXTURE_HDR);
        } else {
            // A different usage per input keeps every encoder in play
            // without paying for all of them each time.
            static const CookTextureUsage usages[] = {COOK_TEXTURE_COLOR, COOK_TEXTURE_LINEAR,
                                                      COOK_TEXTURE_NORMAL, COOK_TEXTURE_MASK};
            cook_and_check(alloc, &img, usages[(data[size - 1U] ^ size) % 4U]);
        }
    }
    cook_image_free(&img);
    return 0;
}
