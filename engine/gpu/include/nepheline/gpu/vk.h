// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Vulkan escape hatch. This is the only public header that exposes
// Vulkan types, and it exists for three users: the editor's UI backend
// (cimgui's Vulkan backend needs the raw handles), the gpu module's own
// tests, and the GPU benchmarks. Engine modules never include it
// (CLAUDE.md §1.8; enforced by scripts/check_layering.py).
//
// Function pointers come from volk; the device loads them at creation, so
// after nph_gpu_device_create any vk* call made through this header is
// valid for that device.

#include <nepheline/gpu/pipeline.h>

#if defined(VULKAN_H_) && !defined(VK_NO_PROTOTYPES)
#error "include <nepheline/gpu/vk.h> before any Vulkan header"
#endif
#define VK_NO_PROTOTYPES
#include <volk.h>

#ifdef __cplusplus
extern "C" {
#endif

VkInstance nph_gpu_vk_instance(const NphGpuDevice *dev);
VkPhysicalDevice nph_gpu_vk_physical_device(const NphGpuDevice *dev);
VkDevice nph_gpu_vk_device(const NphGpuDevice *dev);
VkQueue nph_gpu_vk_queue(const NphGpuDevice *dev, NphGpuQueueType queue);
u32 nph_gpu_vk_queue_family(const NphGpuDevice *dev, NphGpuQueueType queue);

VkBuffer nph_gpu_vk_buffer(const NphGpuDevice *dev, NphGpuBuffer buffer);
VkImage nph_gpu_vk_image(const NphGpuDevice *dev, NphGpuImage image);
// The whole-image view used by the sampled slot (VK_NULL_HANDLE without one).
VkImageView nph_gpu_vk_image_view(const NphGpuDevice *dev, NphGpuImage image);
VkSampler nph_gpu_vk_sampler(const NphGpuDevice *dev, NphGpuSampler sampler);
VkShaderModule nph_gpu_vk_shader_module(const NphGpuDevice *dev, NphGpuShader shader);
VkPipeline nph_gpu_vk_pipeline(const NphGpuDevice *dev, NphGpuPipeline pipeline);
VkSemaphore nph_gpu_vk_timeline(const NphGpuDevice *dev, const NphGpuTimeline *timeline);
VkCommandBuffer nph_gpu_vk_command_buffer(const NphGpuCmd *cmd);

VkDescriptorSetLayout nph_gpu_vk_bindless_layout(const NphGpuDevice *dev);
VkDescriptorSet nph_gpu_vk_bindless_set(const NphGpuDevice *dev);
VkPipelineLayout nph_gpu_vk_pipeline_layout(const NphGpuDevice *dev);
VkPipelineCache nph_gpu_vk_pipeline_cache(const NphGpuDevice *dev);

VkFormat nph_gpu_vk_format(NphGpuFormat format);
VkImageLayout nph_gpu_vk_layout(NphGpuLayout layout);
VkPipelineStageFlags2 nph_gpu_vk_stages(u32 stages);
VkAccessFlags2 nph_gpu_vk_access(u32 access);

// Maps a VkResult to the engine's result codes (logging the VkResult name
// at error level when `what` is not NULL).
NphResult nph_gpu_vk_result(VkResult result, const char *what);
const char *nph_gpu_vk_result_name(VkResult result);

#ifdef __cplusplus
}
#endif
