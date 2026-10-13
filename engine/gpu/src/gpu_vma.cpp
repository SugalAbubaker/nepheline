// SPDX-License-Identifier: Apache-2.0
// The Vulkan Memory Allocator implementation: the one C++ translation unit
// in the runtime (CLAUDE.md §3). It exposes the C interface of gpu_alloc.h
// and nothing else; no other file includes vk_mem_alloc.h.

#include <nepheline/core/assert.h>

#include <cstring>

// VMA's assertions follow the engine's configuration (ADR-0003): on in Debug
// and Development, where they go through the engine's handler (the default
// one writes to stderr and aborts without the CRT's modal dialog, and tests
// can intercept it); compiled out in Profile and Shipping like NPH_ASSERT.
#if NPH_ASSERTS_ENABLED
#define VMA_ASSERT(expr)                                                                           \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            nph_assert_fail_nomsg(__FILE__, __func__, (u32)__LINE__, #expr);                       \
        }                                                                                          \
    } while (0)
#else
#define VMA_ASSERT(expr) ((void)0)
#endif

#define VK_NO_PROTOTYPES
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_VULKAN_VERSION           1003000
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "gpu_alloc.h"

namespace {

VmaAllocator as_vma(NphGpuAllocatorHandle h)
{
    return reinterpret_cast<VmaAllocator>(h);
}

VmaAllocation as_vma(NphGpuAllocation a)
{
    return reinterpret_cast<VmaAllocation>(a);
}

NphGpuAllocation from_vma(VmaAllocation a)
{
    return reinterpret_cast<NphGpuAllocation>(a);
}

} // namespace

extern "C" VkResult nph_gpu_alloc_create(const NphGpuAllocCreateInfo *info,
                                         NphGpuAllocatorHandle *out)
{
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = info->get_instance_proc_addr;
    fns.vkGetDeviceProcAddr = info->get_device_proc_addr;

    VmaAllocatorCreateInfo ci{};
    ci.instance = info->instance;
    ci.physicalDevice = info->physical_device;
    ci.device = info->device;
    ci.vulkanApiVersion = info->api_version;
    ci.pVulkanFunctions = &fns;
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (info->memory_budget) {
        ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    VmaAllocator allocator = VK_NULL_HANDLE;
    const VkResult r = vmaCreateAllocator(&ci, &allocator);
    *out = reinterpret_cast<NphGpuAllocatorHandle>(allocator);
    return r;
}

extern "C" void nph_gpu_alloc_destroy(NphGpuAllocatorHandle allocator)
{
    if (allocator != nullptr) {
        vmaDestroyAllocator(as_vma(allocator));
    }
}

extern "C" VkResult nph_gpu_alloc_create_buffer(NphGpuAllocatorHandle allocator,
                                                const VkBufferCreateInfo *info, NphGpuMemory memory,
                                                VkBuffer *out_buffer, NphGpuAllocation *out_alloc,
                                                void **out_mapped)
{
    VmaAllocationCreateInfo aci{};
    switch (memory) {
    case NPH_GPU_MEMORY_UPLOAD:
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case NPH_GPU_MEMORY_READBACK:
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    case NPH_GPU_MEMORY_DEVICE:
    default:
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }
    VmaAllocation alloc = VK_NULL_HANDLE;
    VmaAllocationInfo ai{};
    const VkResult r = vmaCreateBuffer(as_vma(allocator), info, &aci, out_buffer, &alloc, &ai);
    *out_alloc = from_vma(alloc);
    if (out_mapped != nullptr) {
        *out_mapped = r == VK_SUCCESS ? ai.pMappedData : nullptr;
    }
    return r;
}

extern "C" void nph_gpu_alloc_destroy_buffer(NphGpuAllocatorHandle allocator, VkBuffer buffer,
                                             NphGpuAllocation alloc)
{
    vmaDestroyBuffer(as_vma(allocator), buffer, as_vma(alloc));
}

extern "C" VkResult nph_gpu_alloc_create_image(NphGpuAllocatorHandle allocator,
                                               const VkImageCreateInfo *info, VkImage *out_image,
                                               NphGpuAllocation *out_alloc)
{
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Attachments and large textures get their own allocation, which the
    // driver can place optimally (VMA's documented recommendation).
    const bool big = (info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0 ||
                     (static_cast<uint64_t>(info->extent.width) * info->extent.height >=
                      2048ULL * 2048ULL);
    if (big) {
        aci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    VmaAllocation alloc = VK_NULL_HANDLE;
    const VkResult r = vmaCreateImage(as_vma(allocator), info, &aci, out_image, &alloc, nullptr);
    *out_alloc = from_vma(alloc);
    return r;
}

extern "C" void nph_gpu_alloc_destroy_image(NphGpuAllocatorHandle allocator, VkImage image,
                                            NphGpuAllocation alloc)
{
    vmaDestroyImage(as_vma(allocator), image, as_vma(alloc));
}

extern "C" VkResult nph_gpu_alloc_allocate_block(NphGpuAllocatorHandle allocator, u64 size,
                                                 u64 alignment, u32 memory_type_bits,
                                                 NphGpuAllocation *out_alloc)
{
    VkMemoryRequirements req{};
    req.size = size;
    req.alignment = alignment;
    req.memoryTypeBits = memory_type_bits;
    VmaAllocationCreateInfo aci{};
    // AUTO usages need a resource to look at; a raw block states its needs
    // directly: device-local, in its own allocation so nothing else shares
    // the granularity with the transients bound into it.
    aci.usage = VMA_MEMORY_USAGE_UNKNOWN;
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    aci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VmaAllocation alloc = VK_NULL_HANDLE;
    const VkResult r = vmaAllocateMemory(as_vma(allocator), &req, &aci, &alloc, nullptr);
    *out_alloc = from_vma(alloc);
    return r;
}

extern "C" VkResult nph_gpu_alloc_bind_image(NphGpuAllocatorHandle allocator,
                                             NphGpuAllocation block, u64 offset, VkImage image)
{
    return vmaBindImageMemory2(as_vma(allocator), as_vma(block), offset, image, nullptr);
}

extern "C" VkResult nph_gpu_alloc_bind_buffer(NphGpuAllocatorHandle allocator,
                                              NphGpuAllocation block, u64 offset, VkBuffer buffer)
{
    return vmaBindBufferMemory2(as_vma(allocator), as_vma(block), offset, buffer, nullptr);
}

extern "C" void nph_gpu_alloc_free(NphGpuAllocatorHandle allocator, NphGpuAllocation alloc)
{
    vmaFreeMemory(as_vma(allocator), as_vma(alloc));
}

extern "C" void nph_gpu_alloc_set_frame(NphGpuAllocatorHandle allocator, u32 frame_index)
{
    vmaSetCurrentFrameIndex(as_vma(allocator), frame_index);
}

extern "C" void nph_gpu_alloc_stats(NphGpuAllocatorHandle allocator, NphGpuMemoryStats *out)
{
    std::memset(out, 0, sizeof *out);
    const VkPhysicalDeviceMemoryProperties *props = nullptr;
    vmaGetMemoryProperties(as_vma(allocator), &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(as_vma(allocator), budgets);
    for (uint32_t i = 0; i < props->memoryHeapCount; ++i) {
        const bool device_local = (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        if (device_local) {
            out->device_local_used += budgets[i].usage;
            out->device_local_budget += budgets[i].budget;
        } else {
            out->host_visible_used += budgets[i].usage;
            out->host_visible_budget += budgets[i].budget;
        }
        out->block_bytes += budgets[i].statistics.blockBytes;
        out->allocation_count += budgets[i].statistics.allocationCount;
    }
}
