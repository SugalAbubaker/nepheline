// SPDX-License-Identifier: Apache-2.0
// Buffers, images, image views, samplers, and their bindless slots
// (docs/gpu.md "Resources").
#include "gpu_internal.h"

#include <stdio.h>

static NphAtomicU32 g_name_counter = NPH_ATOMIC_U32_INIT(0U);

static u32 next_name_counter(void)
{
    return nph_atomic_fetch_add_u32(&g_name_counter, 1U, NPH_MEMORY_ORDER_RELAXED);
}

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

static VkBufferUsageFlags buffer_usage(u32 usage)
{
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if ((usage & (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC) != 0U) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if ((usage & (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST) != 0U) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if ((usage & (u32)NPH_GPU_BUFFER_USAGE_STORAGE) != 0U) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if ((usage & (u32)NPH_GPU_BUFFER_USAGE_INDEX) != 0U) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if ((usage & (u32)NPH_GPU_BUFFER_USAGE_INDIRECT) != 0U) {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if ((usage & (u32)NPH_GPU_BUFFER_USAGE_UNIFORM) != 0U) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    return flags;
}

static void fill_buffer_info(const NphGpuDevice *dev, const NphGpuBufferDesc *desc,
                             VkBufferCreateInfo *ci)
{
    memset(ci, 0, sizeof *ci);
    ci->sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci->size = desc->size;
    ci->usage = buffer_usage(desc->usage);
    // Buffers are shared across queue families (transfer uploads, async
    // compute) without ownership transfers; images stay exclusive to keep
    // framebuffer compression (CLAUDE.md §10.3.5).
    if (dev->family_count > 1U) {
        ci->sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci->queueFamilyIndexCount = dev->family_count;
        ci->pQueueFamilyIndices = dev->families;
    } else {
        ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
}

static void finish_buffer(NphGpuDevice *dev, NphGpuBufferSlot *slot, const NphGpuBufferDesc *desc)
{
    slot->desc = *desc;
    gpu_copy_name(slot->name, desc->name, "buffer", next_name_counter());
    slot->desc.name = slot->name;
    gpu_set_name(dev, VK_OBJECT_TYPE_BUFFER, (u64)(uptr)slot->vk, slot->name);
    VkBufferDeviceAddressInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    ai.buffer = slot->vk;
    slot->address = vkGetBufferDeviceAddress(dev->device, &ai);
}

NphResult nph_gpu_buffer_create(NphGpuDevice *dev, const NphGpuBufferDesc *desc, NphGpuBuffer *out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    out->value = 0U;
    if (desc->size == 0U || desc->usage == 0U) {
        GPU_LOG_ERROR("buffer '%s': size and usage must be non-zero",
                      desc->name != NULL ? desc->name : "");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 handle = 0U;
    NphGpuBufferSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->buffers, &handle, (void **)&slot));
    VkBufferCreateInfo ci;
    fill_buffer_info(dev, desc, &ci);
    const VkResult r = nph_gpu_alloc_create_buffer(dev->vma, &ci, desc->memory, &slot->vk,
                                                   &slot->alloc, &slot->mapped);
    if (r != VK_SUCCESS) {
        gpu_pool_release(&dev->buffers, handle);
        GPU_LOG_ERROR("buffer '%s' (%llu bytes): %s", desc->name != NULL ? desc->name : "",
                      (unsigned long long)desc->size, nph_gpu_vk_result_name(r));
        return nph_gpu_vk_result(r, NULL) == NPH_RESULT_OUT_OF_MEMORY ? NPH_RESULT_OUT_OF_MEMORY
                                                                      : NPH_RESULT_UNKNOWN;
    }
    finish_buffer(dev, slot, desc);
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult gpu_buffer_memory_requirements(NphGpuDevice *dev, const NphGpuBufferDesc *desc,
                                         VkMemoryRequirements *out)
{
    VkBufferCreateInfo ci;
    fill_buffer_info(dev, desc, &ci);
    VkDeviceBufferMemoryRequirements info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS;
    info.pCreateInfo = &ci;
    VkMemoryRequirements2 req;
    memset(&req, 0, sizeof req);
    req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetDeviceBufferMemoryRequirements(dev->device, &info, &req);
    *out = req.memoryRequirements;
    return NPH_RESULT_OK;
}

NphResult gpu_buffer_create_unbound(NphGpuDevice *dev, const NphGpuBufferDesc *desc,
                                    NphGpuBuffer *out)
{
    out->value = 0U;
    u32 handle = 0U;
    NphGpuBufferSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->buffers, &handle, (void **)&slot));
    VkBufferCreateInfo ci;
    fill_buffer_info(dev, desc, &ci);
    const VkResult r = vkCreateBuffer(dev->device, &ci, NULL, &slot->vk);
    if (r != VK_SUCCESS) {
        gpu_pool_release(&dev->buffers, handle);
        return gpu_vk_fail(dev, r, "vkCreateBuffer");
    }
    slot->transient = 1;
    slot->desc = *desc;
    gpu_copy_name(slot->name, desc->name, "transient buffer", next_name_counter());
    slot->desc.name = slot->name;
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult gpu_buffer_bind_and_finish(NphGpuDevice *dev, NphGpuBuffer buffer, NphGpuAllocation block,
                                     u64 offset)
{
    NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    const VkResult r = nph_gpu_alloc_bind_buffer(dev->vma, block, offset, slot->vk);
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "nph_gpu_alloc_bind_buffer");
    }
    finish_buffer(dev, slot, &slot->desc);
    return NPH_RESULT_OK;
}

void nph_gpu_buffer_destroy(NphGpuDevice *dev, NphGpuBuffer buffer)
{
    NPH_ASSERT(dev != NULL);
    if (buffer.value == 0U) {
        return;
    }
    NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    if (slot == NULL) {
        return;
    }
    NphGpuDeferred e;
    e.type = NPH_GPU_DEFERRED_BUFFER;
    e.index = 0U;
    e.handle = slot->vk;
    e.alloc = slot->alloc;
    gpu_defer(dev, &e);
    slot->vk = VK_NULL_HANDLE;
    slot->alloc = NULL;
    gpu_pool_release(&dev->buffers, buffer.value);
}

bool nph_gpu_buffer_is_valid(const NphGpuDevice *dev, NphGpuBuffer buffer)
{
    const NphHandle h = {buffer.value};
    return nph_handle_pool_is_valid(&dev->buffers.handles, h);
}

const NphGpuBufferDesc *nph_gpu_buffer_desc(const NphGpuDevice *dev, NphGpuBuffer buffer)
{
    const NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    return slot != NULL ? &slot->desc : NULL;
}

u64 nph_gpu_buffer_size(const NphGpuDevice *dev, NphGpuBuffer buffer)
{
    const NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    return slot != NULL ? slot->desc.size : 0U;
}

u64 nph_gpu_buffer_address(const NphGpuDevice *dev, NphGpuBuffer buffer)
{
    const NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    return slot != NULL ? (u64)slot->address : 0U;
}

void *nph_gpu_buffer_map(const NphGpuDevice *dev, NphGpuBuffer buffer)
{
    const NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    return slot != NULL ? slot->mapped : NULL;
}

VkBuffer nph_gpu_vk_buffer(const NphGpuDevice *dev, NphGpuBuffer buffer)
{
    const NphGpuBufferSlot *slot = gpu_buffer(dev, buffer);
    return slot != NULL ? slot->vk : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

void gpu_image_normalize_desc(NphGpuImageDesc *desc)
{
    if (desc->depth == 0U) {
        desc->depth = 1U;
    }
    if (desc->layer_count == 0U) {
        desc->layer_count = 1U;
    }
    if (desc->samples == 0U) {
        desc->samples = 1U;
    }
    if (desc->mip_count == 0U) {
        desc->mip_count = nph_gpu_mip_count(desc->width, desc->height, desc->depth);
    }
    if (desc->mip_count > NPH_GPU_MAX_MIPS) {
        desc->mip_count = NPH_GPU_MAX_MIPS;
    }
}

static VkImageUsageFlags image_usage(u32 usage)
{
    VkImageUsageFlags flags = 0U;
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC) != 0U) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST) != 0U) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_SAMPLED) != 0U) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_STORAGE) != 0U) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT) != 0U) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT) != 0U) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return flags;
}

static VkFormatFeatureFlags required_format_features(u32 usage)
{
    VkFormatFeatureFlags f = 0U;
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC) != 0U) {
        f |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST) != 0U) {
        f |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_SAMPLED) != 0U) {
        f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_STORAGE) != 0U) {
        f |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT) != 0U) {
        f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if ((usage & (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT) != 0U) {
        f |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return f;
}

static NphResult validate_image_desc(NphGpuDevice *dev, const NphGpuImageDesc *d)
{
    const char *name = d->name != NULL ? d->name : "";
    const NphGpuFormatInfo *info = nph_gpu_format_info(d->format);
    const NphGpuLimits *lim = &dev->info.limits;
    if (d->format == NPH_GPU_FORMAT_UNDEFINED || d->width == 0U || d->height == 0U ||
        d->usage == 0U) {
        GPU_LOG_ERROR("image '%s': format, extent, and usage must be set", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (d->type == NPH_GPU_IMAGE_CUBE && (d->layer_count % 6U) != 0U) {
        GPU_LOG_ERROR("image '%s': cube images need a multiple of 6 layers", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (d->type == NPH_GPU_IMAGE_3D && d->layer_count != 1U) {
        GPU_LOG_ERROR("image '%s': 3D images have one layer", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (d->type != NPH_GPU_IMAGE_3D && d->depth != 1U) {
        GPU_LOG_ERROR("image '%s': only 3D images have depth", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 max_dim = d->type == NPH_GPU_IMAGE_3D     ? lim->max_image_dimension_3d
                        : d->type == NPH_GPU_IMAGE_CUBE ? lim->max_image_dimension_cube
                                                        : lim->max_image_dimension_2d;
    if (d->width > max_dim || d->height > max_dim || d->depth > max_dim ||
        d->layer_count > lim->max_image_layers) {
        GPU_LOG_ERROR("image '%s': %ux%ux%u x%u exceeds the device limits", name, d->width,
                      d->height, d->depth, d->layer_count);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!nph_is_pow2_u32(d->samples) || d->samples > lim->max_sample_count) {
        GPU_LOG_ERROR("image '%s': %u samples is not supported", name, d->samples);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const bool depth = (info->aspects & (u32)NPH_GPU_ASPECT_DEPTH) != 0U;
    if (depth && (d->usage & (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT) != 0U) {
        GPU_LOG_ERROR("image '%s': a depth format cannot be a colour attachment", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!depth && (d->usage & (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT) != 0U) {
        GPU_LOG_ERROR("image '%s': a colour format cannot be a depth attachment", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(dev->phys, gpu_vk_format(d->format), &props);
    const VkFormatFeatureFlags need = required_format_features(d->usage);
    if ((props.optimalTilingFeatures & need) != need) {
        GPU_LOG_ERROR("image '%s': format %s does not support usage 0x%x on this device", name,
                      nph_gpu_format_name(d->format), d->usage);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return NPH_RESULT_OK;
}

static void fill_image_info(const NphGpuDevice *dev, const NphGpuImageDesc *d, bool concurrent,
                            VkImageCreateInfo *ci)
{
    memset(ci, 0, sizeof *ci);
    if (concurrent && dev->family_count > 1U) {
        ci->sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci->queueFamilyIndexCount = dev->family_count;
        ci->pQueueFamilyIndices = dev->families;
    }
    ci->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci->imageType = d->type == NPH_GPU_IMAGE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci->format = gpu_vk_format(d->format);
    ci->extent.width = d->width;
    ci->extent.height = d->height;
    ci->extent.depth = d->depth;
    ci->mipLevels = d->mip_count;
    ci->arrayLayers = d->layer_count;
    ci->samples = (VkSampleCountFlagBits)d->samples;
    ci->tiling = VK_IMAGE_TILING_OPTIMAL;
    ci->usage = image_usage(d->usage);
    ci->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d->type == NPH_GPU_IMAGE_CUBE) {
        ci->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (d->type == NPH_GPU_IMAGE_3D &&
        (d->usage & (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT) != 0U) {
        ci->flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
}

static VkImageViewType sampled_view_type(const NphGpuImageDesc *d)
{
    switch (d->type) {
    case NPH_GPU_IMAGE_3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case NPH_GPU_IMAGE_CUBE:
        return d->layer_count > 6U ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    case NPH_GPU_IMAGE_2D:
    default:
        return d->layer_count > 1U ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static VkImageViewType layered_view_type(const NphGpuImageDesc *d, u32 layer_count)
{
    if (d->type == NPH_GPU_IMAGE_3D) {
        return VK_IMAGE_VIEW_TYPE_3D;
    }
    return layer_count > 1U ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

static NphResult create_view(NphGpuDevice *dev, NphGpuImageSlot *slot, VkImageViewType type,
                             VkImageAspectFlags aspect, u32 base_mip, u32 mip_count, u32 base_layer,
                             u32 layer_count, const char *suffix, VkImageView *out)
{
    VkImageViewCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = slot->vk;
    ci.viewType = type;
    ci.format = slot->vk_format;
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = base_mip;
    ci.subresourceRange.levelCount = mip_count;
    ci.subresourceRange.baseArrayLayer = base_layer;
    ci.subresourceRange.layerCount = layer_count;
    VK_TRY(dev, vkCreateImageView(dev->device, &ci, NULL, out));
    if (dev->has_debug_utils) {
        char name[NPH_GPU_NAME_MAX + 32U];
        snprintf(name, sizeof name, "%.*s %.31s", (int)(NPH_GPU_NAME_MAX - 1U), slot->name, suffix);
        gpu_set_name(dev, VK_OBJECT_TYPE_IMAGE_VIEW, (u64)(uptr)*out, name);
    }
    return NPH_RESULT_OK;
}

// Views and bindless slots for an image whose VkImage exists and has memory.
static NphResult finish_image(NphGpuDevice *dev, NphGpuImageSlot *slot)
{
    const NphGpuImageDesc *d = &slot->desc;
    gpu_set_name(dev, VK_OBJECT_TYPE_IMAGE, (u64)(uptr)slot->vk, slot->name);
    slot->sampled_index = NPH_GPU_INVALID_INDEX;
    for (u32 i = 0; i < NPH_GPU_MAX_MIPS; ++i) {
        slot->storage_index[i] = NPH_GPU_INVALID_INDEX;
    }
    const bool depth = (slot->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U;
    // Sampled views of depth-stencil images expose the depth aspect only.
    const VkImageAspectFlags view_aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : slot->aspect;
    if ((d->usage & (u32)NPH_GPU_IMAGE_USAGE_SAMPLED) != 0U) {
        NPH_TRY(create_view(dev, slot, sampled_view_type(d), view_aspect, 0U, d->mip_count, 0U,
                            d->layer_count, "sampled view", &slot->view));
        slot->sampled_index =
            gpu_bindless_alloc_sampled(dev, slot->view,
                                       depth ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (slot->sampled_index == NPH_GPU_INVALID_INDEX) {
            GPU_LOG_ERROR("image '%s': the bindless sampled-image array is full", slot->name);
            return NPH_RESULT_FULL;
        }
    }
    if ((d->usage & (u32)NPH_GPU_IMAGE_USAGE_STORAGE) != 0U) {
        for (u32 mip = 0; mip < d->mip_count; ++mip) {
            char suffix[32];
            snprintf(suffix, sizeof suffix, "storage view mip %u", mip);
            NPH_TRY(create_view(dev, slot, layered_view_type(d, d->layer_count), view_aspect, mip,
                                1U, 0U, d->layer_count, suffix, &slot->storage_views[mip]));
            slot->storage_index[mip] = gpu_bindless_alloc_storage(dev, slot->storage_views[mip]);
            if (slot->storage_index[mip] == NPH_GPU_INVALID_INDEX) {
                GPU_LOG_ERROR("image '%s': the bindless storage-image array is full", slot->name);
                return NPH_RESULT_FULL;
            }
        }
    }
    for (u32 mip = 0; mip < NPH_GPU_MAX_MIPS; ++mip) {
        slot->state[mip].layout = NPH_GPU_LAYOUT_UNDEFINED;
        slot->state[mip].stages = (u32)NPH_GPU_STAGE_NONE;
        slot->state[mip].access = (u32)NPH_GPU_ACCESS_NONE;
        slot->state[mip].queue = (u32)NPH_GPU_QUEUE_GRAPHICS;
    }
    return NPH_RESULT_OK;
}

static void release_image_objects(NphGpuDevice *dev, NphGpuImageSlot *slot, bool deferred)
{
    for (u32 i = 0; i < slot->attachment_view_count; ++i) {
        if (deferred) {
            gpu_defer_handle(dev, NPH_GPU_DEFERRED_IMAGE_VIEW, slot->attachment_views[i].view);
        } else {
            vkDestroyImageView(dev->device, slot->attachment_views[i].view, NULL);
        }
    }
    if (slot->attachment_views != NULL) {
        NPH_FREE_ARRAY(dev->alloc, NphGpuViewKey, slot->attachment_views,
                       slot->attachment_view_cap);
        slot->attachment_views = NULL;
        slot->attachment_view_count = 0U;
        slot->attachment_view_cap = 0U;
    }
    for (u32 mip = 0; mip < NPH_GPU_MAX_MIPS; ++mip) {
        if (slot->storage_views[mip] != VK_NULL_HANDLE) {
            if (deferred) {
                gpu_defer_handle(dev, NPH_GPU_DEFERRED_IMAGE_VIEW, slot->storage_views[mip]);
            } else {
                vkDestroyImageView(dev->device, slot->storage_views[mip], NULL);
            }
            slot->storage_views[mip] = VK_NULL_HANDLE;
        }
        if (slot->storage_index[mip] != NPH_GPU_INVALID_INDEX) {
            if (deferred) {
                NphGpuDeferred e;
                e.type = NPH_GPU_DEFERRED_BINDLESS_STORAGE;
                e.index = slot->storage_index[mip];
                e.handle = NULL;
                e.alloc = NULL;
                gpu_defer(dev, &e);
            } else {
                gpu_bindless_free(dev, NPH_GPU_DEFERRED_BINDLESS_STORAGE, slot->storage_index[mip]);
            }
            slot->storage_index[mip] = NPH_GPU_INVALID_INDEX;
        }
    }
    if (slot->view != VK_NULL_HANDLE) {
        if (deferred) {
            gpu_defer_handle(dev, NPH_GPU_DEFERRED_IMAGE_VIEW, slot->view);
        } else {
            vkDestroyImageView(dev->device, slot->view, NULL);
        }
        slot->view = VK_NULL_HANDLE;
    }
    if (slot->sampled_index != NPH_GPU_INVALID_INDEX) {
        if (deferred) {
            NphGpuDeferred e;
            e.type = NPH_GPU_DEFERRED_BINDLESS_SAMPLED;
            e.index = slot->sampled_index;
            e.handle = NULL;
            e.alloc = NULL;
            gpu_defer(dev, &e);
        } else {
            gpu_bindless_free(dev, NPH_GPU_DEFERRED_BINDLESS_SAMPLED, slot->sampled_index);
        }
        slot->sampled_index = NPH_GPU_INVALID_INDEX;
    }
    if (!slot->external && slot->vk != VK_NULL_HANDLE) {
        if (deferred) {
            NphGpuDeferred e;
            e.type = NPH_GPU_DEFERRED_IMAGE;
            e.index = 0U;
            e.handle = slot->vk;
            e.alloc = slot->alloc;
            gpu_defer(dev, &e);
        } else if (slot->alloc != NULL) {
            nph_gpu_alloc_destroy_image(dev->vma, slot->vk, slot->alloc);
        } else {
            vkDestroyImage(dev->device, slot->vk, NULL);
        }
    }
    slot->vk = VK_NULL_HANDLE;
    slot->alloc = NULL;
}

NphResult nph_gpu_image_create(NphGpuDevice *dev, const NphGpuImageDesc *in, NphGpuImage *out)
{
    NPH_ASSERT(dev != NULL && in != NULL && out != NULL);
    out->value = 0U;
    NphGpuImageDesc desc = *in;
    gpu_image_normalize_desc(&desc);
    NPH_TRY(validate_image_desc(dev, &desc));
    u32 handle = 0U;
    NphGpuImageSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->images, &handle, (void **)&slot));
    slot->desc = desc;
    gpu_copy_name(slot->name, desc.name, "image", next_name_counter());
    slot->desc.name = slot->name;
    slot->vk_format = gpu_vk_format(desc.format);
    slot->aspect = gpu_vk_aspect(desc.format);
    // Textures (uploaded, never rendered to) are shared across queue
    // families so uploads need no ownership transfer and may touch a
    // subset of layers; attachments stay exclusive for the hardware's
    // framebuffer compression (CLAUDE.md §10.3.5).
    const bool shared = (desc.usage & (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST) != 0U &&
                        (desc.usage & ((u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT |
                                       (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT)) == 0U;
    slot->exclusive = (shared && dev->family_count > 1U) ? 0 : 1;
    VkImageCreateInfo ci;
    fill_image_info(dev, &desc, shared, &ci);
    const VkResult r = nph_gpu_alloc_create_image(dev->vma, &ci, &slot->vk, &slot->alloc);
    if (r != VK_SUCCESS) {
        gpu_pool_release(&dev->images, handle);
        GPU_LOG_ERROR("image '%s' (%ux%u %s): %s", slot->name, desc.width, desc.height,
                      nph_gpu_format_name(desc.format), nph_gpu_vk_result_name(r));
        return nph_gpu_vk_result(r, NULL) == NPH_RESULT_OUT_OF_MEMORY ? NPH_RESULT_OUT_OF_MEMORY
                                                                      : NPH_RESULT_UNKNOWN;
    }
    const NphResult fr = finish_image(dev, slot);
    if (fr != NPH_RESULT_OK) {
        release_image_objects(dev, slot, false);
        gpu_pool_release(&dev->images, handle);
        return fr;
    }
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult gpu_image_memory_requirements(NphGpuDevice *dev, const NphGpuImageDesc *in,
                                        bool concurrent, VkMemoryRequirements *out)
{
    NphGpuImageDesc desc = *in;
    gpu_image_normalize_desc(&desc);
    NPH_TRY(validate_image_desc(dev, &desc));
    VkImageCreateInfo ci;
    fill_image_info(dev, &desc, concurrent, &ci);
    VkDeviceImageMemoryRequirements info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS;
    info.pCreateInfo = &ci;
    info.planeAspect = 0;
    VkMemoryRequirements2 req;
    memset(&req, 0, sizeof req);
    req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    vkGetDeviceImageMemoryRequirements(dev->device, &info, &req);
    *out = req.memoryRequirements;
    return NPH_RESULT_OK;
}

NphResult gpu_image_create_unbound(NphGpuDevice *dev, const NphGpuImageDesc *in, bool concurrent,
                                   NphGpuImage *out)
{
    out->value = 0U;
    NphGpuImageDesc desc = *in;
    gpu_image_normalize_desc(&desc);
    NPH_TRY(validate_image_desc(dev, &desc));
    u32 handle = 0U;
    NphGpuImageSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->images, &handle, (void **)&slot));
    slot->desc = desc;
    gpu_copy_name(slot->name, desc.name, "transient image", next_name_counter());
    slot->desc.name = slot->name;
    slot->vk_format = gpu_vk_format(desc.format);
    slot->aspect = gpu_vk_aspect(desc.format);
    slot->exclusive = (concurrent && dev->family_count > 1U) ? 0 : 1;
    slot->transient = 1;
    VkImageCreateInfo ci;
    fill_image_info(dev, &desc, concurrent, &ci);
    const VkResult r = vkCreateImage(dev->device, &ci, NULL, &slot->vk);
    if (r != VK_SUCCESS) {
        gpu_pool_release(&dev->images, handle);
        return gpu_vk_fail(dev, r, "vkCreateImage");
    }
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult gpu_image_bind_and_finish(NphGpuDevice *dev, NphGpuImage image, NphGpuAllocation block,
                                    u64 offset)
{
    NphGpuImageSlot *slot = gpu_image(dev, image);
    const VkResult r = nph_gpu_alloc_bind_image(dev->vma, block, offset, slot->vk);
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "nph_gpu_alloc_bind_image");
    }
    return finish_image(dev, slot);
}

NphResult gpu_image_register_external(NphGpuDevice *dev, VkImage image, const NphGpuImageDesc *in,
                                      NphGpuImage *out)
{
    out->value = 0U;
    NphGpuImageDesc desc = *in;
    gpu_image_normalize_desc(&desc);
    u32 handle = 0U;
    NphGpuImageSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->images, &handle, (void **)&slot));
    slot->desc = desc;
    gpu_copy_name(slot->name, desc.name, "swapchain image", next_name_counter());
    slot->desc.name = slot->name;
    slot->vk_format = gpu_vk_format(desc.format);
    slot->aspect = gpu_vk_aspect(desc.format);
    slot->external = 1;
    slot->exclusive = 1;
    slot->vk = image;
    const NphResult r = finish_image(dev, slot);
    if (r != NPH_RESULT_OK) {
        release_image_objects(dev, slot, false);
        gpu_pool_release(&dev->images, handle);
        return r;
    }
    out->value = handle;
    return NPH_RESULT_OK;
}

void nph_gpu_image_destroy(NphGpuDevice *dev, NphGpuImage image)
{
    NPH_ASSERT(dev != NULL);
    if (image.value == 0U) {
        return;
    }
    NphGpuImageSlot *slot = gpu_image(dev, image);
    if (slot == NULL) {
        return;
    }
    release_image_objects(dev, slot, true);
    gpu_pool_release(&dev->images, image.value);
}

bool nph_gpu_image_is_valid(const NphGpuDevice *dev, NphGpuImage image)
{
    const NphHandle h = {image.value};
    return nph_handle_pool_is_valid(&dev->images.handles, h);
}

const NphGpuImageDesc *nph_gpu_image_desc(const NphGpuDevice *dev, NphGpuImage image)
{
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    return slot != NULL ? &slot->desc : NULL;
}

u32 nph_gpu_image_sampled_index(const NphGpuDevice *dev, NphGpuImage image)
{
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    return slot != NULL ? slot->sampled_index : NPH_GPU_INVALID_INDEX;
}

u32 nph_gpu_image_storage_index(const NphGpuDevice *dev, NphGpuImage image, u32 mip)
{
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    if (slot == NULL || mip >= slot->desc.mip_count) {
        return NPH_GPU_INVALID_INDEX;
    }
    return slot->storage_index[mip];
}

NphGpuLayout nph_gpu_image_layout(const NphGpuDevice *dev, NphGpuImage image, u32 mip)
{
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    if (slot == NULL || mip >= slot->desc.mip_count) {
        return NPH_GPU_LAYOUT_UNDEFINED;
    }
    return slot->state[mip].layout;
}

void gpu_image_set_state(NphGpuImageSlot *slot, u32 base_mip, u32 mip_count, NphGpuLayout layout,
                         u32 stages, u32 access, u32 queue)
{
    const u32 n = gpu_mips_in_range(slot, base_mip, mip_count);
    for (u32 i = 0; i < n; ++i) {
        NphGpuMipState *s = &slot->state[base_mip + i];
        s->layout = layout;
        s->stages = stages;
        s->access = access;
        s->queue = queue;
    }
}

VkImageView gpu_image_attachment_view(NphGpuDevice *dev, NphGpuImageSlot *slot, u32 mip,
                                      u32 base_layer, u32 layer_count)
{
    layer_count = gpu_layers_in_range(slot, base_layer, layer_count);
    nph_spin_lock_acquire(&dev->images.lock);
    for (u32 i = 0; i < slot->attachment_view_count; ++i) {
        const NphGpuViewKey *k = &slot->attachment_views[i];
        if (k->mip == mip && k->base_layer == base_layer && k->layer_count == layer_count) {
            VkImageView v = k->view;
            nph_spin_lock_release(&dev->images.lock);
            return v;
        }
    }
    if (slot->attachment_view_count == slot->attachment_view_cap) {
        const u32 new_cap = slot->attachment_view_cap == 0U ? 4U : slot->attachment_view_cap * 2U;
        NphGpuViewKey *grown = (NphGpuViewKey *)NPH_REALLOC(
            dev->alloc, slot->attachment_views,
            (u64)slot->attachment_view_cap * sizeof(NphGpuViewKey),
            (u64)new_cap * sizeof(NphGpuViewKey), NPH_ALIGNOF(NphGpuViewKey));
        if (grown == NULL) {
            nph_spin_lock_release(&dev->images.lock);
            return VK_NULL_HANDLE;
        }
        slot->attachment_views = grown;
        slot->attachment_view_cap = new_cap;
    }
    VkImageView view = VK_NULL_HANDLE;
    char suffix[48];
    snprintf(suffix, sizeof suffix, "attachment mip %u layers %u+%u", mip, base_layer, layer_count);
    // Attachment views carry every aspect of the format (depth and stencil).
    const NphResult r = create_view(dev, slot, layered_view_type(&slot->desc, layer_count),
                                    slot->aspect, mip, 1U, base_layer, layer_count, suffix, &view);
    if (r == NPH_RESULT_OK) {
        NphGpuViewKey *k = &slot->attachment_views[slot->attachment_view_count++];
        k->mip = mip;
        k->base_layer = base_layer;
        k->layer_count = layer_count;
        k->view = view;
    }
    nph_spin_lock_release(&dev->images.lock);
    return view;
}

VkImage nph_gpu_vk_image(const NphGpuDevice *dev, NphGpuImage image)
{
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    return slot != NULL ? slot->vk : VK_NULL_HANDLE;
}

VkImageView nph_gpu_vk_image_view(const NphGpuDevice *dev, NphGpuImage image)
{
    const NphGpuImageSlot *slot = gpu_image(dev, image);
    return slot != NULL ? slot->view : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

void nph_gpu_sampler_desc_default(NphGpuSamplerDesc *desc)
{
    memset(desc, 0, sizeof *desc);
    desc->min_filter = NPH_GPU_FILTER_LINEAR;
    desc->mag_filter = NPH_GPU_FILTER_LINEAR;
    desc->mip_filter = NPH_GPU_FILTER_LINEAR;
    desc->max_lod = NPH_GPU_LOD_CLAMP_NONE;
}

NphResult nph_gpu_sampler_create(NphGpuDevice *dev, const NphGpuSamplerDesc *desc,
                                 NphGpuSampler *out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    out->value = 0U;
    if (desc->reduction != NPH_GPU_REDUCTION_WEIGHTED_AVERAGE &&
        !dev->info.features.sampler_filter_minmax) {
        GPU_LOG_ERROR("sampler '%s': min/max reduction is not supported by this device",
                      desc->name != NULL ? desc->name : "");
        return NPH_RESULT_UNSUPPORTED;
    }
    u32 handle = 0U;
    NphGpuSamplerSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->samplers, &handle, (void **)&slot));
    slot->desc = *desc;
    slot->index = NPH_GPU_INVALID_INDEX;
    gpu_copy_name(slot->name, desc->name, "sampler", next_name_counter());
    slot->desc.name = slot->name;

    VkSamplerReductionModeCreateInfo reduction;
    memset(&reduction, 0, sizeof reduction);
    reduction.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
    reduction.reductionMode = desc->reduction == NPH_GPU_REDUCTION_MIN
                                  ? VK_SAMPLER_REDUCTION_MODE_MIN
                                  : VK_SAMPLER_REDUCTION_MODE_MAX;
    VkSamplerCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    if (desc->reduction != NPH_GPU_REDUCTION_WEIGHTED_AVERAGE) {
        ci.pNext = &reduction;
    }
    ci.magFilter = gpu_vk_filter(desc->mag_filter);
    ci.minFilter = gpu_vk_filter(desc->min_filter);
    ci.mipmapMode = desc->mip_filter == NPH_GPU_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                              : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = gpu_vk_address_mode(desc->address_u);
    ci.addressModeV = gpu_vk_address_mode(desc->address_v);
    ci.addressModeW = gpu_vk_address_mode(desc->address_w);
    ci.mipLodBias = desc->lod_bias;
    const f32 aniso = desc->max_anisotropy > 1.0F ? desc->max_anisotropy : 0.0F;
    if (aniso > 0.0F && dev->info.features.sampler_anisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy =
            aniso > dev->info.limits.max_anisotropy ? dev->info.limits.max_anisotropy : aniso;
    }
    ci.compareEnable = desc->compare_enable ? VK_TRUE : VK_FALSE;
    ci.compareOp = gpu_vk_compare(desc->compare_op);
    ci.minLod = desc->min_lod;
    ci.maxLod = desc->max_lod == 0.0F ? NPH_GPU_LOD_CLAMP_NONE : desc->max_lod;
    switch (desc->border) {
    case NPH_GPU_BORDER_OPAQUE_BLACK:
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        break;
    case NPH_GPU_BORDER_OPAQUE_WHITE:
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        break;
    case NPH_GPU_BORDER_TRANSPARENT_BLACK:
    default:
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        break;
    }
    const VkResult r = vkCreateSampler(dev->device, &ci, NULL, &slot->vk);
    if (r != VK_SUCCESS) {
        gpu_pool_release(&dev->samplers, handle);
        return gpu_vk_fail(dev, r, "vkCreateSampler");
    }
    gpu_set_name(dev, VK_OBJECT_TYPE_SAMPLER, (u64)(uptr)slot->vk, slot->name);
    slot->index = gpu_bindless_alloc_sampler(dev, slot->vk);
    if (slot->index == NPH_GPU_INVALID_INDEX) {
        GPU_LOG_ERROR("sampler '%s': the bindless sampler array is full", slot->name);
        vkDestroySampler(dev->device, slot->vk, NULL);
        gpu_pool_release(&dev->samplers, handle);
        return NPH_RESULT_FULL;
    }
    out->value = handle;
    return NPH_RESULT_OK;
}

void nph_gpu_sampler_destroy(NphGpuDevice *dev, NphGpuSampler sampler)
{
    NPH_ASSERT(dev != NULL);
    if (sampler.value == 0U) {
        return;
    }
    NphGpuSamplerSlot *slot = gpu_sampler(dev, sampler);
    if (slot == NULL) {
        return;
    }
    gpu_defer_handle(dev, NPH_GPU_DEFERRED_SAMPLER, slot->vk);
    NphGpuDeferred e;
    e.type = NPH_GPU_DEFERRED_BINDLESS_SAMPLER;
    e.index = slot->index;
    e.handle = NULL;
    e.alloc = NULL;
    gpu_defer(dev, &e);
    slot->vk = VK_NULL_HANDLE;
    gpu_pool_release(&dev->samplers, sampler.value);
}

bool nph_gpu_sampler_is_valid(const NphGpuDevice *dev, NphGpuSampler sampler)
{
    const NphHandle h = {sampler.value};
    return nph_handle_pool_is_valid(&dev->samplers.handles, h);
}

u32 nph_gpu_sampler_index(const NphGpuDevice *dev, NphGpuSampler sampler)
{
    const NphGpuSamplerSlot *slot = gpu_sampler(dev, sampler);
    return slot != NULL ? slot->index : NPH_GPU_INVALID_INDEX;
}

VkSampler nph_gpu_vk_sampler(const NphGpuDevice *dev, NphGpuSampler sampler)
{
    const NphGpuSamplerSlot *slot = gpu_sampler(dev, sampler);
    return slot != NULL ? slot->vk : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Shutdown: whatever is still alive is destroyed directly (leaks were
// reported by the device).
// ---------------------------------------------------------------------------

void gpu_resources_shutdown(NphGpuDevice *dev)
{
    u32 i = 0U;
    void *slot = NULL;
    while (gpu_pool_next_live(&dev->samplers, &i, &slot)) {
        NphGpuSamplerSlot *s = (NphGpuSamplerSlot *)slot;
        if (s->vk != VK_NULL_HANDLE) {
            vkDestroySampler(dev->device, s->vk, NULL);
            s->vk = VK_NULL_HANDLE;
        }
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->images, &i, &slot)) {
        release_image_objects(dev, (NphGpuImageSlot *)slot, false);
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->buffers, &i, &slot)) {
        NphGpuBufferSlot *b = (NphGpuBufferSlot *)slot;
        if (b->vk != VK_NULL_HANDLE) {
            if (b->alloc != NULL) {
                nph_gpu_alloc_destroy_buffer(dev->vma, b->vk, b->alloc);
            } else {
                vkDestroyBuffer(dev->device, b->vk, NULL);
            }
            b->vk = VK_NULL_HANDLE;
            b->alloc = NULL;
        }
    }
}
