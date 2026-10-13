// SPDX-License-Identifier: Apache-2.0
#pragma once

// nph_gpu_alloc_*: the engine's interface to the Vulkan Memory Allocator
// (CLAUDE.md §3: "VMA behind nph_gpu_alloc_*; C++ implementation compiled
// as one isolated TU"). gpu_vma.cpp is the only file that includes
// vk_mem_alloc.h; everything else sees these C functions and opaque
// handles. Candidate for replacement by our own allocator later (§3).

#include <nepheline/gpu/types.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphGpuAllocatorT *NphGpuAllocatorHandle;
typedef struct NphGpuAllocationT *NphGpuAllocation;

typedef struct NphGpuAllocCreateInfo {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    u32 api_version; // VK_API_VERSION_1_3
    b32 memory_budget;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
} NphGpuAllocCreateInfo;

VkResult nph_gpu_alloc_create(const NphGpuAllocCreateInfo *info, NphGpuAllocatorHandle *out);
void nph_gpu_alloc_destroy(NphGpuAllocatorHandle allocator);

// Creates a buffer with memory of the given class; UPLOAD and READBACK
// buffers come back persistently mapped (`out_mapped`).
VkResult nph_gpu_alloc_create_buffer(NphGpuAllocatorHandle allocator,
                                     const VkBufferCreateInfo *info, NphGpuMemory memory,
                                     VkBuffer *out_buffer, NphGpuAllocation *out_alloc,
                                     void **out_mapped);
void nph_gpu_alloc_destroy_buffer(NphGpuAllocatorHandle allocator, VkBuffer buffer,
                                  NphGpuAllocation alloc);

VkResult nph_gpu_alloc_create_image(NphGpuAllocatorHandle allocator, const VkImageCreateInfo *info,
                                    VkImage *out_image, NphGpuAllocation *out_alloc);
void nph_gpu_alloc_destroy_image(NphGpuAllocatorHandle allocator, VkImage image,
                                 NphGpuAllocation alloc);

// Raw device-local memory for render-graph aliasing: a block that several
// transient resources are bound into at offsets.
VkResult nph_gpu_alloc_allocate_block(NphGpuAllocatorHandle allocator, u64 size, u64 alignment,
                                      u32 memory_type_bits, NphGpuAllocation *out_alloc);
VkResult nph_gpu_alloc_bind_image(NphGpuAllocatorHandle allocator, NphGpuAllocation block,
                                  u64 offset, VkImage image);
VkResult nph_gpu_alloc_bind_buffer(NphGpuAllocatorHandle allocator, NphGpuAllocation block,
                                   u64 offset, VkBuffer buffer);
void nph_gpu_alloc_free(NphGpuAllocatorHandle allocator, NphGpuAllocation alloc);

void nph_gpu_alloc_set_frame(NphGpuAllocatorHandle allocator, u32 frame_index);
void nph_gpu_alloc_stats(NphGpuAllocatorHandle allocator, NphGpuMemoryStats *out);

#ifdef __cplusplus
}
#endif
