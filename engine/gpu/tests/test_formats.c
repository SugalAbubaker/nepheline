// SPDX-License-Identifier: Apache-2.0
// The format table and the small helpers around it (no device needed).
#include "test_gpu_support.h"

NTEST(gpu_formats, every_format_is_described)
{
    for (u32 i = 1; i < (u32)NPH_GPU_FORMAT_COUNT; ++i) {
        const NphGpuFormat fmt = (NphGpuFormat)i;
        const NphGpuFormatInfo *info = nph_gpu_format_info(fmt);
        NTEST_ASSERT_NOT_NULL(info->name);
        NTEST_ASSERT_GT_UINT(info->bytes_per_block, 0U);
        NTEST_ASSERT_GE_UINT(info->block_width, 1U);
        NTEST_ASSERT_GE_UINT(info->block_height, 1U);
        NTEST_ASSERT_NE_UINT(info->aspects, 0U);
        NTEST_ASSERT_NE_INT(nph_gpu_vk_format(fmt), VK_FORMAT_UNDEFINED);
        if (info->compressed) {
            NTEST_ASSERT_EQ_UINT(info->block_width, 4U);
        }
        // Names are unique.
        for (u32 j = 1; j < i; ++j) {
            NTEST_ASSERT_NE_STR(info->name, nph_gpu_format_name((NphGpuFormat)j));
        }
    }
    NTEST_ASSERT_EQ_STR(nph_gpu_format_name(NPH_GPU_FORMAT_UNDEFINED), "UNDEFINED");
    volatile int bad = 9999; // not a constant: the analyzer would reject the cast
    NTEST_ASSERT_EQ_STR(nph_gpu_format_name((NphGpuFormat)bad), "UNDEFINED");
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_format((NphGpuFormat)bad), VK_FORMAT_UNDEFINED);
}

NTEST(gpu_formats, level_bytes_round_up_to_blocks)
{
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_R8G8B8A8_UNORM, 16U, 8U, 1U),
                         512U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_R8G8B8A8_UNORM, 16U, 8U, 0U),
                         512U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_BC7_UNORM, 4U, 4U, 1U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_BC7_UNORM, 5U, 5U, 1U), 64U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_BC1_RGBA_UNORM, 1U, 1U, 1U), 8U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_R32_SFLOAT, 3U, 3U, 3U), 108U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_level_bytes(NPH_GPU_FORMAT_UNDEFINED, 3U, 3U, 3U), 0U);
}

NTEST(gpu_formats, mip_helpers)
{
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_count(1U, 1U, 1U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_count(256U, 128U, 1U), 9U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_count(1920U, 1080U, 1U), 11U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_count(0U, 0U, 0U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_extent(256U, 0U), 256U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_extent(256U, 8U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_extent(256U, 12U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_gpu_mip_extent(100U, 1U), 50U);
}

NTEST(gpu_formats, depth_and_aspect)
{
    NTEST_ASSERT_TRUE(nph_gpu_format_is_depth(NPH_GPU_FORMAT_D32_SFLOAT));
    NTEST_ASSERT_TRUE(nph_gpu_format_is_depth(NPH_GPU_FORMAT_D24_UNORM_S8_UINT));
    NTEST_ASSERT_FALSE(nph_gpu_format_is_depth(NPH_GPU_FORMAT_R8G8B8A8_UNORM));
    NTEST_ASSERT_EQ_UINT(nph_gpu_format_info(NPH_GPU_FORMAT_D32_SFLOAT_S8_UINT)->aspects,
                         (u32)NPH_GPU_ASPECT_DEPTH | (u32)NPH_GPU_ASPECT_STENCIL);
    NTEST_ASSERT_TRUE(nph_gpu_format_info(NPH_GPU_FORMAT_BC7_SRGB)->srgb);
    NTEST_ASSERT_FALSE(nph_gpu_format_info(NPH_GPU_FORMAT_BC7_UNORM)->srgb);
}

NTEST(gpu_formats, layouts_stages_access_map_to_vulkan)
{
    for (u32 i = 0; i < (u32)NPH_GPU_LAYOUT_COUNT; ++i) {
        NTEST_ASSERT_NE_STR(nph_gpu_layout_name((NphGpuLayout)i), "?");
    }
    volatile int bad_layout = 77;
    NTEST_ASSERT_EQ_STR(nph_gpu_layout_name((NphGpuLayout)bad_layout), "?");
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_layout(NPH_GPU_LAYOUT_PRESENT), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_layout(NPH_GPU_LAYOUT_COLOR_ATTACHMENT),
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_layout((NphGpuLayout)bad_layout), VK_IMAGE_LAYOUT_UNDEFINED);
    NTEST_ASSERT_EQ_UINT((u64)nph_gpu_vk_stages((u32)NPH_GPU_STAGE_COMPUTE),
                         (u64)VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    NTEST_ASSERT_EQ_UINT((u64)nph_gpu_vk_stages((u32)NPH_GPU_STAGE_DEPTH_STENCIL),
                         (u64)(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT));
    const u32 all_stages = (1U << 13U) - 1U;
    NTEST_ASSERT_NE_UINT((u64)nph_gpu_vk_stages(all_stages), 0U);
    NTEST_ASSERT_EQ_UINT((u64)nph_gpu_vk_access((u32)NPH_GPU_ACCESS_SHADER_WRITE),
                         (u64)VK_ACCESS_2_SHADER_WRITE_BIT);
    const u32 all_access = (1U << 14U) - 1U;
    NTEST_ASSERT_NE_UINT((u64)nph_gpu_vk_access(all_access), 0U);
    NTEST_ASSERT_EQ_UINT((u64)nph_gpu_vk_access(0U), 0U);
}

NTEST(gpu_formats, results_map_and_name)
{
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_result(VK_SUCCESS, NULL), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_result(VK_ERROR_OUT_OF_DEVICE_MEMORY, NULL),
                        NPH_RESULT_OUT_OF_MEMORY);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_result(VK_ERROR_DEVICE_LOST, NULL), NPH_RESULT_DEVICE_LOST);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_result(VK_ERROR_EXTENSION_NOT_PRESENT, NULL),
                        NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_result(VK_TIMEOUT, NULL), NPH_RESULT_BUSY);
    NTEST_ASSERT_EQ_INT(nph_gpu_vk_result(VK_ERROR_UNKNOWN, "logged"), NPH_RESULT_UNKNOWN);
    NTEST_ASSERT_EQ_STR(nph_gpu_vk_result_name(VK_ERROR_DEVICE_LOST), "VK_ERROR_DEVICE_LOST");
    volatile int bad_result = -999999;
    NTEST_ASSERT_EQ_STR(nph_gpu_vk_result_name((VkResult)bad_result), "VK_RESULT_?");
    NTEST_ASSERT_EQ_STR(nph_result_string(NPH_RESULT_DEVICE_LOST), "DEVICE_LOST");
}

NTEST(gpu_formats, clear_values_and_shader_stage_names)
{
    const NphGpuClearValue c = nph_gpu_clear_color(0.25F, 0.5F, 0.75F, 1.0F);
    NTEST_ASSERT_NEAR_F32(c.color_f32[2], 0.75F, 0.0F);
    const NphGpuClearValue d = nph_gpu_clear_depth(0.0F, 3U);
    NTEST_ASSERT_NEAR_F32(d.ds.depth, 0.0F, 0.0F);
    NTEST_ASSERT_EQ_UINT(d.ds.stencil, 3U);
    NTEST_ASSERT_EQ_STR(nph_gpu_shader_stage_name(NPH_GPU_SHADER_MESH), "mesh");
    NTEST_ASSERT_EQ_STR(nph_gpu_shader_stage_name(NPH_GPU_SHADER_STAGE_COUNT), "?");
    NTEST_ASSERT_TRUE(nph_gpu_buffer_is_null(NPH_GPU_BUFFER_NULL));
    NTEST_ASSERT_TRUE(nph_gpu_image_is_null(NPH_GPU_IMAGE_NULL));
    NTEST_ASSERT_TRUE(nph_gpu_sampler_is_null(NPH_GPU_SAMPLER_NULL));
    NTEST_ASSERT_TRUE(nph_gpu_shader_is_null(NPH_GPU_SHADER_NULL));
    NTEST_ASSERT_TRUE(nph_gpu_pipeline_is_null(NPH_GPU_PIPELINE_NULL));
    NTEST_ASSERT_TRUE(nph_gpu_rg_image_is_null(NPH_GPU_RG_IMAGE_NULL));
    NTEST_ASSERT_TRUE(nph_gpu_rg_buffer_is_null(NPH_GPU_RG_BUFFER_NULL));
}
