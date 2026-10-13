// SPDX-License-Identifier: Apache-2.0
// Conversions between the engine's enums and Vulkan's, the format table,
// result mapping, and the handle pools.
#include "gpu_internal.h"

#include <stdio.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_GPU, "gpu", NPH_LOG_LEVEL_DEBUG);

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

typedef struct FormatEntry {
    NphGpuFormatInfo info;
    VkFormat vk;
} FormatEntry;

#define F(name, bpb, bw, bh, aspects, srgb, compressed, vk)                                        \
    {{name, bpb, bw, bh, aspects, srgb, compressed}, vk}

static const FormatEntry g_formats[NPH_GPU_FORMAT_COUNT] = {
    F("UNDEFINED", 0U, 1U, 1U, 0U, 0, 0, VK_FORMAT_UNDEFINED),
    F("R8_UNORM", 1U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R8_UNORM),
    F("R8_UINT", 1U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R8_UINT),
    F("R8G8_UNORM", 2U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R8G8_UNORM),
    F("R8G8B8A8_UNORM", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R8G8B8A8_UNORM),
    F("R8G8B8A8_SRGB", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 1, 0, VK_FORMAT_R8G8B8A8_SRGB),
    F("B8G8R8A8_UNORM", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_B8G8R8A8_UNORM),
    F("B8G8R8A8_SRGB", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 1, 0, VK_FORMAT_B8G8R8A8_SRGB),
    F("R16_UNORM", 2U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16_UNORM),
    F("R16_UINT", 2U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16_UINT),
    F("R16_SFLOAT", 2U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16_SFLOAT),
    F("R16G16_SFLOAT", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16G16_SFLOAT),
    F("R16G16_UNORM", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16G16_UNORM),
    F("R16G16B16A16_UNORM", 8U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16G16B16A16_UNORM),
    F("R16G16B16A16_SFLOAT", 8U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R16G16B16A16_SFLOAT),
    F("R32_UINT", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R32_UINT),
    F("R32_SFLOAT", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R32_SFLOAT),
    F("R32G32_UINT", 8U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R32G32_UINT),
    F("R32G32_SFLOAT", 8U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R32G32_SFLOAT),
    F("R32G32B32A32_UINT", 16U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R32G32B32A32_UINT),
    F("R32G32B32A32_SFLOAT", 16U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0,
      VK_FORMAT_R32G32B32A32_SFLOAT),
    F("R64_UINT", 8U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_R64_UINT),
    F("B10G11R11_UFLOAT", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0,
      VK_FORMAT_B10G11R11_UFLOAT_PACK32),
    F("E5B9G9R9_UFLOAT", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    F("A2B10G10R10_UNORM", 4U, 1U, 1U, NPH_GPU_ASPECT_COLOR, 0, 0,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32),
    F("D16_UNORM", 2U, 1U, 1U, NPH_GPU_ASPECT_DEPTH, 0, 0, VK_FORMAT_D16_UNORM),
    F("D32_SFLOAT", 4U, 1U, 1U, NPH_GPU_ASPECT_DEPTH, 0, 0, VK_FORMAT_D32_SFLOAT),
    F("D24_UNORM_S8_UINT", 4U, 1U, 1U, NPH_GPU_ASPECT_DEPTH | NPH_GPU_ASPECT_STENCIL, 0, 0,
      VK_FORMAT_D24_UNORM_S8_UINT),
    F("D32_SFLOAT_S8_UINT", 8U, 1U, 1U, NPH_GPU_ASPECT_DEPTH | NPH_GPU_ASPECT_STENCIL, 0, 0,
      VK_FORMAT_D32_SFLOAT_S8_UINT),
    F("BC1_RGBA_UNORM", 8U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 0, 1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
    F("BC1_RGBA_SRGB", 8U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 1, 1, VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
    F("BC3_UNORM", 16U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 0, 1, VK_FORMAT_BC3_UNORM_BLOCK),
    F("BC3_SRGB", 16U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 1, 1, VK_FORMAT_BC3_SRGB_BLOCK),
    F("BC4_UNORM", 8U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 0, 1, VK_FORMAT_BC4_UNORM_BLOCK),
    F("BC5_UNORM", 16U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 0, 1, VK_FORMAT_BC5_UNORM_BLOCK),
    F("BC6H_UFLOAT", 16U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 0, 1, VK_FORMAT_BC6H_UFLOAT_BLOCK),
    F("BC7_UNORM", 16U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 0, 1, VK_FORMAT_BC7_UNORM_BLOCK),
    F("BC7_SRGB", 16U, 4U, 4U, NPH_GPU_ASPECT_COLOR, 1, 1, VK_FORMAT_BC7_SRGB_BLOCK),
};
#undef F

const NphGpuFormatInfo *nph_gpu_format_info(NphGpuFormat format)
{
    if ((u32)format >= (u32)NPH_GPU_FORMAT_COUNT) {
        return &g_formats[0].info;
    }
    return &g_formats[format].info;
}

VkFormat gpu_vk_format(NphGpuFormat format)
{
    if ((u32)format >= (u32)NPH_GPU_FORMAT_COUNT) {
        return VK_FORMAT_UNDEFINED;
    }
    return g_formats[format].vk;
}

VkFormat nph_gpu_vk_format(NphGpuFormat format)
{
    return gpu_vk_format(format);
}

u64 nph_gpu_format_level_bytes(NphGpuFormat format, u32 width, u32 height, u32 depth)
{
    const NphGpuFormatInfo *info = nph_gpu_format_info(format);
    if (info->bytes_per_block == 0U) {
        return 0U;
    }
    const u64 bw = (width + info->block_width - 1U) / info->block_width;
    const u64 bh = (height + info->block_height - 1U) / info->block_height;
    const u64 d = depth == 0U ? 1U : depth;
    return bw * bh * d * info->bytes_per_block;
}

u32 nph_gpu_mip_count(u32 width, u32 height, u32 depth)
{
    u32 largest = nph_max_u32(width, nph_max_u32(height, depth));
    if (largest == 0U) {
        return 1U;
    }
    return nph_log2_floor_u32(largest) + 1U;
}

VkImageAspectFlags gpu_vk_aspect(NphGpuFormat format)
{
    const u32 aspects = nph_gpu_format_info(format)->aspects;
    VkImageAspectFlags flags = 0U;
    if ((aspects & (u32)NPH_GPU_ASPECT_COLOR) != 0U) {
        flags |= VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if ((aspects & (u32)NPH_GPU_ASPECT_DEPTH) != 0U) {
        flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if ((aspects & (u32)NPH_GPU_ASPECT_STENCIL) != 0U) {
        flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return flags;
}

// ---------------------------------------------------------------------------
// Layouts, stages, access
// ---------------------------------------------------------------------------

static const char *const g_layout_names[NPH_GPU_LAYOUT_COUNT] = {
    "UNDEFINED",        "GENERAL",      "COLOR_ATTACHMENT", "DEPTH_ATTACHMENT", "DEPTH_READ_ONLY",
    "SHADER_READ_ONLY", "TRANSFER_SRC", "TRANSFER_DST",     "PRESENT",
};

const char *nph_gpu_layout_name(NphGpuLayout layout)
{
    if ((u32)layout >= (u32)NPH_GPU_LAYOUT_COUNT) {
        return "?";
    }
    return g_layout_names[layout];
}

VkImageLayout gpu_vk_layout(NphGpuLayout layout)
{
    switch (layout) {
    case NPH_GPU_LAYOUT_UNDEFINED:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    case NPH_GPU_LAYOUT_GENERAL:
        return VK_IMAGE_LAYOUT_GENERAL;
    case NPH_GPU_LAYOUT_COLOR_ATTACHMENT:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case NPH_GPU_LAYOUT_DEPTH_ATTACHMENT:
        return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case NPH_GPU_LAYOUT_DEPTH_READ_ONLY:
        return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    case NPH_GPU_LAYOUT_SHADER_READ_ONLY:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case NPH_GPU_LAYOUT_TRANSFER_SRC:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case NPH_GPU_LAYOUT_TRANSFER_DST:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case NPH_GPU_LAYOUT_PRESENT:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case NPH_GPU_LAYOUT_COUNT:
        break;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkImageLayout nph_gpu_vk_layout(NphGpuLayout layout)
{
    return gpu_vk_layout(layout);
}

VkPipelineStageFlags2 gpu_vk_stages(u32 stages)
{
    VkPipelineStageFlags2 out = 0U;
    if ((stages & (u32)NPH_GPU_STAGE_INDIRECT) != 0U) {
        out |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_INDEX_INPUT) != 0U) {
        out |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_VERTEX_SHADER) != 0U) {
        out |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_FRAGMENT_SHADER) != 0U) {
        out |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_DEPTH_STENCIL) != 0U) {
        out |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_COLOR_OUTPUT) != 0U) {
        out |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_COMPUTE) != 0U) {
        out |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_TRANSFER) != 0U) {
        out |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_HOST) != 0U) {
        out |= VK_PIPELINE_STAGE_2_HOST_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_TASK_SHADER) != 0U) {
        out |= VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_MESH_SHADER) != 0U) {
        out |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_ALL_GRAPHICS) != 0U) {
        out |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    }
    if ((stages & (u32)NPH_GPU_STAGE_ALL) != 0U) {
        out |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return out;
}

VkPipelineStageFlags2 nph_gpu_vk_stages(u32 stages)
{
    return gpu_vk_stages(stages);
}

VkAccessFlags2 gpu_vk_access(u32 access)
{
    VkAccessFlags2 out = 0U;
    if ((access & (u32)NPH_GPU_ACCESS_INDIRECT_READ) != 0U) {
        out |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_INDEX_READ) != 0U) {
        out |= VK_ACCESS_2_INDEX_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_SHADER_READ) != 0U) {
        out |= VK_ACCESS_2_SHADER_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_SHADER_WRITE) != 0U) {
        out |= VK_ACCESS_2_SHADER_WRITE_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_COLOR_READ) != 0U) {
        out |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_COLOR_WRITE) != 0U) {
        out |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_DEPTH_READ) != 0U) {
        out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_DEPTH_WRITE) != 0U) {
        out |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_TRANSFER_READ) != 0U) {
        out |= VK_ACCESS_2_TRANSFER_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_TRANSFER_WRITE) != 0U) {
        out |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_HOST_READ) != 0U) {
        out |= VK_ACCESS_2_HOST_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_HOST_WRITE) != 0U) {
        out |= VK_ACCESS_2_HOST_WRITE_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_MEMORY_READ) != 0U) {
        out |= VK_ACCESS_2_MEMORY_READ_BIT;
    }
    if ((access & (u32)NPH_GPU_ACCESS_MEMORY_WRITE) != 0U) {
        out |= VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
    return out;
}

VkAccessFlags2 nph_gpu_vk_access(u32 access)
{
    return gpu_vk_access(access);
}

VkPipelineStageFlags2 gpu_vk_queue_all_stages(NphGpuQueueType queue)
{
    switch (queue) {
    case NPH_GPU_QUEUE_COMPUTE:
        return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
               VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    case NPH_GPU_QUEUE_TRANSFER:
        return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    case NPH_GPU_QUEUE_GRAPHICS:
    case NPH_GPU_QUEUE_COUNT:
        break;
    }
    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

VkFilter gpu_vk_filter(NphGpuFilter filter)
{
    return filter == NPH_GPU_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkCompareOp gpu_vk_compare(NphGpuCompareOp op)
{
    switch (op) {
    case NPH_GPU_COMPARE_NEVER:
        return VK_COMPARE_OP_NEVER;
    case NPH_GPU_COMPARE_LESS:
        return VK_COMPARE_OP_LESS;
    case NPH_GPU_COMPARE_EQUAL:
        return VK_COMPARE_OP_EQUAL;
    case NPH_GPU_COMPARE_LESS_OR_EQUAL:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NPH_GPU_COMPARE_GREATER:
        return VK_COMPARE_OP_GREATER;
    case NPH_GPU_COMPARE_NOT_EQUAL:
        return VK_COMPARE_OP_NOT_EQUAL;
    case NPH_GPU_COMPARE_GREATER_OR_EQUAL:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case NPH_GPU_COMPARE_ALWAYS:
        return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_NEVER;
}

VkSamplerAddressMode gpu_vk_address_mode(NphGpuAddressMode mode)
{
    switch (mode) {
    case NPH_GPU_ADDRESS_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NPH_GPU_ADDRESS_MIRRORED_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NPH_GPU_ADDRESS_CLAMP_TO_EDGE:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NPH_GPU_ADDRESS_CLAMP_TO_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkAttachmentLoadOp gpu_vk_load_op(NphGpuLoadOp op)
{
    switch (op) {
    case NPH_GPU_LOAD_OP_LOAD:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case NPH_GPU_LOAD_OP_CLEAR:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case NPH_GPU_LOAD_OP_DONT_CARE:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}

VkAttachmentStoreOp gpu_vk_store_op(NphGpuStoreOp op)
{
    return op == NPH_GPU_STORE_OP_DONT_CARE ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                            : VK_ATTACHMENT_STORE_OP_STORE;
}

VkShaderStageFlagBits gpu_vk_shader_stage(NphGpuShaderStage stage)
{
    switch (stage) {
    case NPH_GPU_SHADER_VERTEX:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case NPH_GPU_SHADER_FRAGMENT:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case NPH_GPU_SHADER_COMPUTE:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    case NPH_GPU_SHADER_TASK:
        return VK_SHADER_STAGE_TASK_BIT_EXT;
    case NPH_GPU_SHADER_MESH:
        return VK_SHADER_STAGE_MESH_BIT_EXT;
    case NPH_GPU_SHADER_STAGE_COUNT:
        break;
    }
    return VK_SHADER_STAGE_VERTEX_BIT;
}

const char *nph_gpu_shader_stage_name(NphGpuShaderStage stage)
{
    switch (stage) {
    case NPH_GPU_SHADER_VERTEX:
        return "vertex";
    case NPH_GPU_SHADER_FRAGMENT:
        return "fragment";
    case NPH_GPU_SHADER_COMPUTE:
        return "compute";
    case NPH_GPU_SHADER_TASK:
        return "task";
    case NPH_GPU_SHADER_MESH:
        return "mesh";
    case NPH_GPU_SHADER_STAGE_COUNT:
        break;
    }
    return "?";
}

const char *nph_gpu_device_type_name(NphGpuDeviceType type)
{
    switch (type) {
    case NPH_GPU_DEVICE_TYPE_OTHER:
        return "other";
    case NPH_GPU_DEVICE_TYPE_INTEGRATED:
        return "integrated";
    case NPH_GPU_DEVICE_TYPE_DISCRETE:
        return "discrete";
    case NPH_GPU_DEVICE_TYPE_VIRTUAL:
        return "virtual";
    case NPH_GPU_DEVICE_TYPE_CPU:
        return "cpu";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

const char *nph_gpu_vk_result_name(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_FRAGMENTATION:
        return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
        return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
        return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT:
        return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_INVALID_SHADER_NV:
        return "VK_ERROR_INVALID_SHADER_NV";
    case VK_PIPELINE_COMPILE_REQUIRED:
        return "VK_PIPELINE_COMPILE_REQUIRED";
    default:
        break;
    }
    return "VK_RESULT_?";
}

NphResult nph_gpu_vk_result(VkResult result, const char *what)
{
    NphResult r;
    switch (result) {
    case VK_SUCCESS:
    case VK_SUBOPTIMAL_KHR:
    case VK_PIPELINE_COMPILE_REQUIRED:
        return NPH_RESULT_OK;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS:
    case VK_ERROR_FRAGMENTED_POOL:
    case VK_ERROR_FRAGMENTATION:
        r = NPH_RESULT_OUT_OF_MEMORY;
        break;
    case VK_ERROR_DEVICE_LOST:
        r = NPH_RESULT_DEVICE_LOST;
        break;
    case VK_ERROR_LAYER_NOT_PRESENT:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        r = NPH_RESULT_UNSUPPORTED;
        break;
    case VK_TIMEOUT:
    case VK_NOT_READY:
        r = NPH_RESULT_BUSY;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
    case VK_ERROR_SURFACE_LOST_KHR:
        r = NPH_RESULT_UNSUPPORTED;
        break;
    default:
        r = NPH_RESULT_UNKNOWN;
        break;
    }
    if (what != NULL) {
        GPU_LOG_ERROR("%s failed: %s (%d)", what, nph_gpu_vk_result_name(result), (int)result);
    }
    return r;
}

NphResult gpu_vk_fail(NphGpuDevice *dev, VkResult r, const char *what)
{
    if (r == VK_ERROR_DEVICE_LOST && dev != NULL) {
        dev->lost = 1;
    }
    return nph_gpu_vk_result(r, what);
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

void gpu_copy_name(char *dst, const char *src, const char *fallback, u32 counter)
{
    if (src != NULL && src[0] != '\0') {
        size_t n = strlen(src);
        if (n >= NPH_GPU_NAME_MAX) {
            n = NPH_GPU_NAME_MAX - 1U;
        }
        memcpy(dst, src, n);
        dst[n] = '\0';
    } else {
        snprintf(dst, NPH_GPU_NAME_MAX, "%.*s#%u", (int)(NPH_GPU_NAME_MAX - 12U), fallback,
                 counter);
    }
}

void gpu_set_name(NphGpuDevice *dev, VkObjectType type, u64 handle, const char *name)
{
    if (!dev->has_debug_utils || handle == 0U || name == NULL ||
        vkSetDebugUtilsObjectNameEXT == NULL) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    (void)vkSetDebugUtilsObjectNameEXT(dev->device, &info);
}

// ---------------------------------------------------------------------------
// Handle pools
// ---------------------------------------------------------------------------

NphResult gpu_pool_init(NphGpuPool *pool, NphAllocator *alloc, u32 capacity, u32 slot_size)
{
    memset(pool, 0, sizeof *pool);
    u32 index_bits = nph_log2_ceil_u32(capacity);
    if (index_bits < NPH_HANDLE_POOL_MIN_INDEX_BITS) {
        index_bits = NPH_HANDLE_POOL_MIN_INDEX_BITS;
    }
    if (index_bits > 24U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 reuse_delay = nph_min_u32(capacity / 4U, 256U);
    NPH_TRY(nph_handle_pool_init(&pool->handles, alloc, capacity, index_bits, reuse_delay));
    pool->slots = (u8 *)NPH_ALLOC_ZEROED(alloc, (u64)slot_size * capacity, 16U);
    if (pool->slots == NULL) {
        nph_handle_pool_free(&pool->handles);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    pool->live = (u8 *)NPH_ALLOC_ZEROED(alloc, capacity, 16U);
    if (pool->live == NULL) {
        NPH_FREE(alloc, pool->slots, (u64)slot_size * capacity, 16U);
        pool->slots = NULL;
        nph_handle_pool_free(&pool->handles);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    pool->slot_size = slot_size;
    return NPH_RESULT_OK;
}

void gpu_pool_free(NphGpuPool *pool, NphAllocator *alloc)
{
    if (pool->slots != NULL) {
        NPH_FREE(alloc, pool->slots, (u64)pool->slot_size * pool->handles.capacity, 16U);
        pool->slots = NULL;
    }
    if (pool->live != NULL) {
        NPH_FREE(alloc, pool->live, pool->handles.capacity, 16U);
        pool->live = NULL;
    }
    nph_handle_pool_free(&pool->handles);
}

NphResult gpu_pool_alloc(NphGpuPool *pool, u32 *out_handle, void **out_slot)
{
    NphHandle h;
    nph_spin_lock_acquire(&pool->lock);
    const NphResult r = nph_handle_pool_alloc(&pool->handles, &h);
    nph_spin_lock_release(&pool->lock);
    if (r != NPH_RESULT_OK) {
        *out_handle = 0U;
        *out_slot = NULL;
        return r;
    }
    const u32 index = nph_handle_pool_index(&pool->handles, h);
    u8 *slot = pool->slots + (u64)index * pool->slot_size;
    memset(slot, 0, pool->slot_size);
    pool->live[index] = 1U;
    *out_handle = h.value;
    *out_slot = slot;
    return NPH_RESULT_OK;
}

void *gpu_pool_get(const NphGpuPool *pool, u32 handle, const char *what)
{
    const NphHandle h = {handle};
    if (!nph_handle_pool_is_valid(&pool->handles, h)) {
        NPH_ASSERT_MSG(handle == 0U, "stale %s handle 0x%08x", what, handle);
        NPH_UNUSED(what);
        return NULL;
    }
    const u32 index = nph_handle_pool_index(&pool->handles, h);
    return pool->slots + (u64)index * pool->slot_size;
}

bool gpu_pool_release(NphGpuPool *pool, u32 handle)
{
    const NphHandle h = {handle};
    nph_spin_lock_acquire(&pool->lock);
    const bool ok = nph_handle_pool_release(&pool->handles, h);
    if (ok) {
        pool->live[nph_handle_pool_index(&pool->handles, h)] = 0U;
    }
    nph_spin_lock_release(&pool->lock);
    return ok;
}

u32 gpu_pool_live(const NphGpuPool *pool)
{
    return nph_handle_pool_live(&pool->handles);
}

// Iterates live slots: `*index` starts at 0; returns false at the end.
bool gpu_pool_next_live(const NphGpuPool *pool, u32 *index, void **out_slot)
{
    while (*index < pool->handles.bump) {
        const u32 i = (*index)++;
        if (pool->live[i] != 0U) {
            *out_slot = pool->slots + (u64)i * pool->slot_size;
            return true;
        }
    }
    return false;
}
