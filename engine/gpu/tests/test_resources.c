// SPDX-License-Identifier: Apache-2.0
// Buffers, images, samplers, bindless slots, handle staleness.
#include "test_gpu_support.h"

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

NTEST_F(GpuFixture, buffers_of_every_memory_class)
{
    REQUIRE_DEVICE(f);
    NphGpuBuffer dev_buf = test_make_buffer(
        f->dev, 4096U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
        NPH_GPU_MEMORY_DEVICE, "device");
    NphGpuBuffer up = test_make_buffer(f->dev, 4096U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC,
                                       NPH_GPU_MEMORY_UPLOAD, "upload");
    NphGpuBuffer rb = test_make_buffer(f->dev, 4096U, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                       NPH_GPU_MEMORY_READBACK, "readback");
    NTEST_ASSERT_TRUE(nph_gpu_buffer_is_valid(f->dev, dev_buf));
    NTEST_ASSERT_EQ_UINT(nph_gpu_buffer_size(f->dev, dev_buf), 4096U);
    NTEST_ASSERT_NE_UINT(nph_gpu_buffer_address(f->dev, dev_buf), 0U);
    NTEST_ASSERT_NE_UINT(nph_gpu_buffer_address(f->dev, up), nph_gpu_buffer_address(f->dev, rb));
    NTEST_ASSERT_NULL(nph_gpu_buffer_map(f->dev, dev_buf));
    NTEST_ASSERT_NOT_NULL(nph_gpu_buffer_map(f->dev, up));
    NTEST_ASSERT_NOT_NULL(nph_gpu_buffer_map(f->dev, rb));
    const NphGpuBufferDesc *desc = nph_gpu_buffer_desc(f->dev, up);
    NTEST_ASSERT_EQ_STR(desc->name, "upload");
    NTEST_ASSERT_EQ_INT(desc->memory, NPH_GPU_MEMORY_UPLOAD);
    NTEST_ASSERT_NE_PTR(nph_gpu_vk_buffer(f->dev, up), VK_NULL_HANDLE);
    memset(nph_gpu_buffer_map(f->dev, up), 0xAB, 4096U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).buffers, 3U);
    NTEST_ASSERT_GE_UINT(nph_gpu_device_memory_stats(f->dev).allocation_count, 3U);
    nph_gpu_buffer_destroy(f->dev, dev_buf);
    nph_gpu_buffer_destroy(f->dev, up);
    nph_gpu_buffer_destroy(f->dev, rb);
    nph_gpu_buffer_destroy(f->dev, NPH_GPU_BUFFER_NULL);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).buffers, 0U);
}

NTEST_F(GpuFixture, buffer_descriptions_are_validated)
{
    REQUIRE_DEVICE(f);
    NphGpuBufferDesc desc;
    memset(&desc, 0, sizeof desc);
    NphGpuBuffer b = NPH_GPU_BUFFER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_buffer_create(f->dev, &desc, &b), NPH_RESULT_INVALID_ARGUMENT);
    desc.size = 16U;
    NTEST_ASSERT_EQ_INT(nph_gpu_buffer_create(f->dev, &desc, &b), NPH_RESULT_INVALID_ARGUMENT);
    desc.usage = (u32)NPH_GPU_BUFFER_USAGE_INDEX | (u32)NPH_GPU_BUFFER_USAGE_INDIRECT |
                 (u32)NPH_GPU_BUFFER_USAGE_UNIFORM | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    NTEST_ASSERT_EQ_INT(nph_gpu_buffer_create(f->dev, &desc, &b), NPH_RESULT_OK);
    // Unnamed resources get a generated name.
    NTEST_ASSERT_EQ_INT(strncmp(nph_gpu_buffer_desc(f->dev, b)->name, "buffer#", 7U), 0);
    nph_gpu_buffer_destroy(f->dev, b);
}

NTEST_F(GpuFixture, stale_buffer_handles_are_detected)
{
    REQUIRE_DEVICE(f);
    NphGpuBuffer b = test_make_buffer(f->dev, 64U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                      NPH_GPU_MEMORY_DEVICE, "stale");
    nph_gpu_buffer_destroy(f->dev, b);
    NTEST_ASSERT_FALSE(nph_gpu_buffer_is_valid(f->dev, b));
    u64 size = 1U;
    NPH_TEST_EXPECT_ASSERT(size = nph_gpu_buffer_size(f->dev, b));
    NTEST_ASSERT_EQ_UINT(size, 0U);
    NPH_TEST_EXPECT_ASSERT(nph_gpu_buffer_destroy(f->dev, b)); // double destroy: ignored
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).buffers, 0U);
    // A new buffer gets a different handle even if it reuses the slot.
    NphGpuBuffer c = test_make_buffer(f->dev, 64U, (u32)NPH_GPU_BUFFER_USAGE_STORAGE,
                                      NPH_GPU_MEMORY_DEVICE, "fresh");
    NTEST_ASSERT_NE_UINT(c.value, b.value);
    nph_gpu_buffer_destroy(f->dev, c);
}

NTEST(gpu_resources, buffer_pool_is_bounded)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.max_buffers = 4U; // the staging ring takes one
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphGpuBuffer bufs[4];
    u32 made = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        NphGpuBufferDesc bd;
        memset(&bd, 0, sizeof bd);
        bd.size = 64U;
        bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE;
        const NphResult r = nph_gpu_buffer_create(dev, &bd, &bufs[i]);
        if (r != NPH_RESULT_OK) {
            NTEST_ASSERT_EQ_INT(r, NPH_RESULT_FULL);
            break;
        }
        ++made;
    }
    NTEST_ASSERT_EQ_UINT(made, 3U);
    for (u32 i = 0; i < made; ++i) {
        nph_gpu_buffer_destroy(dev, bufs[i]);
    }
    test_gpu_destroy(dev);
}

NTEST_F(GpuFixture, images_get_views_and_bindless_slots)
{
    REQUIRE_DEVICE(f);
    NphGpuImage img = test_make_image_2d(
        f->dev, 64U, 32U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 0U,
        (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_STORAGE |
            (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT,
        "tex");
    NTEST_ASSERT_TRUE(nph_gpu_image_is_valid(f->dev, img));
    const NphGpuImageDesc *d = nph_gpu_image_desc(f->dev, img);
    NTEST_ASSERT_EQ_UINT(d->mip_count, 7U);
    NTEST_ASSERT_EQ_UINT(d->layer_count, 1U);
    NTEST_ASSERT_EQ_UINT(d->depth, 1U);
    NTEST_ASSERT_EQ_UINT(d->samples, 1U);
    NTEST_ASSERT_EQ_STR(d->name, "tex");
    NTEST_ASSERT_NE_UINT(nph_gpu_image_sampled_index(f->dev, img), NPH_GPU_INVALID_INDEX);
    for (u32 m = 0; m < 7U; ++m) {
        NTEST_ASSERT_NE_UINT(nph_gpu_image_storage_index(f->dev, img, m), NPH_GPU_INVALID_INDEX);
        NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, m), NPH_GPU_LAYOUT_UNDEFINED);
    }
    NTEST_ASSERT_EQ_UINT(nph_gpu_image_storage_index(f->dev, img, 7U), NPH_GPU_INVALID_INDEX);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_layout(f->dev, img, 7U), NPH_GPU_LAYOUT_UNDEFINED);
    NTEST_ASSERT_NE_PTR(nph_gpu_vk_image(f->dev, img), VK_NULL_HANDLE);
    NTEST_ASSERT_NE_PTR(nph_gpu_vk_image_view(f->dev, img), VK_NULL_HANDLE);

    // Storage indices are distinct across mips; sampled index is distinct
    // across images.
    NphGpuImage img2 = test_make_image_2d(f->dev, 8U, 8U, NPH_GPU_FORMAT_R16G16B16A16_SFLOAT, 1U,
                                          (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "tex2");
    NTEST_ASSERT_NE_UINT(nph_gpu_image_sampled_index(f->dev, img2),
                         nph_gpu_image_sampled_index(f->dev, img));
    NTEST_ASSERT_EQ_UINT(nph_gpu_image_storage_index(f->dev, img2, 0U), NPH_GPU_INVALID_INDEX);
    NTEST_ASSERT_NE_UINT(nph_gpu_image_storage_index(f->dev, img, 0U),
                         nph_gpu_image_storage_index(f->dev, img, 1U));

    // A transfer-only image has no bindless slot at all.
    NphGpuImage plain = test_make_image_2d(
        f->dev, 8U, 8U, NPH_GPU_FORMAT_R8_UNORM, 1U,
        (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC, "plain");
    NTEST_ASSERT_EQ_UINT(nph_gpu_image_sampled_index(f->dev, plain), NPH_GPU_INVALID_INDEX);
    NTEST_ASSERT_EQ_PTR(nph_gpu_vk_image_view(f->dev, plain), VK_NULL_HANDLE);

    nph_gpu_image_destroy(f->dev, img);
    nph_gpu_image_destroy(f->dev, img2);
    nph_gpu_image_destroy(f->dev, plain);
    nph_gpu_image_destroy(f->dev, NPH_GPU_IMAGE_NULL);
    NTEST_ASSERT_FALSE(nph_gpu_image_is_valid(f->dev, img));
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).images, 0U);
    NTEST_ASSERT_GT_UINT(nph_gpu_device_live_counts(f->dev).pending_destroys, 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).pending_destroys, 0U);
}

NTEST_F(GpuFixture, image_kinds)
{
    REQUIRE_DEVICE(f);
    NphGpuImageDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.type = NPH_GPU_IMAGE_CUBE;
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    desc.width = 16U;
    desc.height = 16U;
    desc.layer_count = 6U;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    desc.name = "cube";
    NphGpuImage cube = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &cube), NPH_RESULT_OK);

    desc.type = NPH_GPU_IMAGE_2D;
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM; // sRGB formats have no storage support
    desc.layer_count = 4U;
    desc.mip_count = 2U;
    desc.usage |= (u32)NPH_GPU_IMAGE_USAGE_STORAGE;
    desc.name = "array";
    NphGpuImage arr = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &arr), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_gpu_image_desc(f->dev, arr)->mip_count, 2U);

    desc.type = NPH_GPU_IMAGE_3D;
    desc.layer_count = 1U;
    desc.depth = 8U;
    desc.mip_count = 1U;
    desc.name = "volume";
    NphGpuImage vol = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &vol), NPH_RESULT_OK);

    memset(&desc, 0, sizeof desc);
    desc.format = NPH_GPU_FORMAT_D32_SFLOAT;
    desc.width = 32U;
    desc.height = 32U;
    desc.mip_count = 1U;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    desc.name = "depth";
    NphGpuImage depth = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &depth), NPH_RESULT_OK);
    NTEST_ASSERT_NE_UINT(nph_gpu_image_sampled_index(f->dev, depth), NPH_GPU_INVALID_INDEX);

    desc.format = NPH_GPU_FORMAT_D24_UNORM_S8_UINT;
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
    NphGpuImage ds = NPH_GPU_IMAGE_NULL;
    const NphResult dsr = nph_gpu_image_create(f->dev, &desc, &ds);
    if (dsr == NPH_RESULT_OK) {
        nph_gpu_image_destroy(f->dev, ds);
    } else {
        NTEST_ASSERT_EQ_INT(dsr, NPH_RESULT_INVALID_ARGUMENT); // format not supported here
    }

    nph_gpu_image_destroy(f->dev, cube);
    nph_gpu_image_destroy(f->dev, arr);
    nph_gpu_image_destroy(f->dev, vol);
    nph_gpu_image_destroy(f->dev, depth);
}

NTEST_F(GpuFixture, image_descriptions_are_validated)
{
    REQUIRE_DEVICE(f);
    NphGpuImageDesc desc;
    NphGpuImage img = NPH_GPU_IMAGE_NULL;
    memset(&desc, 0, sizeof desc);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.width = 4U;
    desc.height = 4U;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT; // colour format as depth
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT;
    desc.format = NPH_GPU_FORMAT_D32_SFLOAT; // depth format as colour
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    desc.type = NPH_GPU_IMAGE_CUBE;
    desc.layer_count = 5U; // not a multiple of 6
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.type = NPH_GPU_IMAGE_2D;
    desc.layer_count = 1U;
    desc.depth = 3U; // depth on a 2D image
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.depth = 1U;
    desc.samples = 3U; // not a power of two
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.samples = 1U;
    desc.width = 1U << 20U; // too large
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    desc.width = 4U;
    desc.type = NPH_GPU_IMAGE_3D;
    desc.layer_count = 2U; // layers on a 3D image
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(f->dev, &desc, &img), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_TRUE(nph_gpu_image_is_null(img));
    // Stale handle use asserts and returns the defaults.
    u32 index = 0U;
    NPH_TEST_EXPECT_ASSERT(index = nph_gpu_image_sampled_index(f->dev, (NphGpuImage){12345U}));
    NTEST_ASSERT_EQ_UINT(index, NPH_GPU_INVALID_INDEX);
}

NTEST_F(GpuFixture, samplers)
{
    REQUIRE_DEVICE(f);
    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    NTEST_ASSERT_EQ_INT(sd.min_filter, NPH_GPU_FILTER_LINEAR);
    NTEST_ASSERT_NEAR_F32(sd.max_lod, NPH_GPU_LOD_CLAMP_NONE, 0.0F);
    sd.name = "trilinear";
    NphGpuSampler a = NPH_GPU_SAMPLER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(f->dev, &sd, &a), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_gpu_sampler_is_valid(f->dev, a));
    NTEST_ASSERT_NE_UINT(nph_gpu_sampler_index(f->dev, a), NPH_GPU_INVALID_INDEX);
    NTEST_ASSERT_NE_PTR(nph_gpu_vk_sampler(f->dev, a), VK_NULL_HANDLE);

    memset(&sd, 0, sizeof sd); // nearest, repeat, every field at its zero
    sd.address_u = NPH_GPU_ADDRESS_CLAMP_TO_BORDER;
    sd.address_v = NPH_GPU_ADDRESS_MIRRORED_REPEAT;
    sd.address_w = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
    sd.border = NPH_GPU_BORDER_OPAQUE_WHITE;
    sd.max_anisotropy = 16.0F;
    sd.compare_enable = 1;
    sd.compare_op = NPH_GPU_COMPARE_GREATER_OR_EQUAL;
    NphGpuSampler b = NPH_GPU_SAMPLER_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(f->dev, &sd, &b), NPH_RESULT_OK);
    NTEST_ASSERT_NE_UINT(nph_gpu_sampler_index(f->dev, a), nph_gpu_sampler_index(f->dev, b));

    sd.compare_enable = 0;
    sd.border = NPH_GPU_BORDER_OPAQUE_BLACK;
    sd.reduction = NPH_GPU_REDUCTION_MAX;
    NphGpuSampler c = NPH_GPU_SAMPLER_NULL;
    const NphResult rr = nph_gpu_sampler_create(f->dev, &sd, &c);
    if (nph_gpu_device_info(f->dev)->features.sampler_filter_minmax) {
        NTEST_ASSERT_EQ_INT(rr, NPH_RESULT_OK);
        nph_gpu_sampler_destroy(f->dev, c);
    } else {
        NTEST_ASSERT_EQ_INT(rr, NPH_RESULT_UNSUPPORTED);
    }
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).samplers, 2U);
    nph_gpu_sampler_destroy(f->dev, a);
    nph_gpu_sampler_destroy(f->dev, b);
    nph_gpu_sampler_destroy(f->dev, NPH_GPU_SAMPLER_NULL);
    NTEST_ASSERT_FALSE(nph_gpu_sampler_is_valid(f->dev, a));
    NTEST_ASSERT_EQ_UINT(nph_gpu_device_live_counts(f->dev).samplers, 0U);
}

NTEST(gpu_resources, bindless_arrays_are_bounded)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.headless = 1;
    desc.bindless_samplers = 2U;
    desc.bindless_sampled_images = 2U;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    NphGpuSampler s[3];
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(dev, &sd, &s[0]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(dev, &sd, &s[1]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(dev, &sd, &s[2]), NPH_RESULT_FULL);
    NphGpuImage imgs[3];
    imgs[0] = test_make_image_2d(dev, 4U, 4U, NPH_GPU_FORMAT_R8_UNORM, 1U,
                                 (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "a");
    imgs[1] = test_make_image_2d(dev, 4U, 4U, NPH_GPU_FORMAT_R8_UNORM, 1U,
                                 (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "b");
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = NPH_GPU_FORMAT_R8_UNORM;
    id.width = 4U;
    id.height = 4U;
    id.mip_count = 1U;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &id, &imgs[2]), NPH_RESULT_FULL);
    // Slots return to the pool once the frame that could use them is over.
    nph_gpu_sampler_destroy(dev, s[0]);
    nph_gpu_image_destroy(dev, imgs[0]);
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(dev, &sd, &s[2]), NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_sampler_create(dev, &sd, &s[2]), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &id, &imgs[2]), NPH_RESULT_OK);
    nph_gpu_sampler_destroy(dev, s[1]);
    nph_gpu_sampler_destroy(dev, s[2]);
    nph_gpu_image_destroy(dev, imgs[1]);
    nph_gpu_image_destroy(dev, imgs[2]);
    test_gpu_destroy(dev);
}
