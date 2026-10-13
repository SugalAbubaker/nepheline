// SPDX-License-Identifier: Apache-2.0
// The global bindless descriptor set and the shared pipeline layout
// (CLAUDE.md §8.5 "Bindless"): binding 0 sampled images, binding 1 storage
// images, binding 2 samplers, all partially bound and update-after-bind,
// plus NPH_GPU_PUSH_CONSTANT_BYTES of push constants for every stage.
#include "gpu_internal.h"

static NphResult index_allocator_init(NphGpuIndexAllocator *a, NphAllocator *alloc, u32 capacity)
{
    memset(a, 0, sizeof *a);
    a->capacity = capacity;
    a->free_stack = NPH_ALLOC_ARRAY(alloc, u32, capacity);
    return a->free_stack == NULL ? NPH_RESULT_OUT_OF_MEMORY : NPH_RESULT_OK;
}

static void index_allocator_free(NphGpuIndexAllocator *a, NphAllocator *alloc)
{
    if (a->free_stack != NULL) {
        NPH_FREE_ARRAY(alloc, u32, a->free_stack, a->capacity);
        a->free_stack = NULL;
    }
}

static u32 index_alloc(NphGpuIndexAllocator *a)
{
    if (a->free_count != 0U) {
        return a->free_stack[--a->free_count];
    }
    if (a->next < a->capacity) {
        return a->next++;
    }
    return NPH_GPU_INVALID_INDEX;
}

static void index_release(NphGpuIndexAllocator *a, u32 index)
{
    NPH_ASSERT(index < a->capacity && a->free_count < a->capacity);
    a->free_stack[a->free_count++] = index;
}

NphResult gpu_bindless_init(NphGpuDevice *dev)
{
    NphGpuBindless *b = &dev->bindless;
    const NphGpuLimits *lim = &dev->info.limits;
    NPH_TRY(index_allocator_init(&b->sampled, dev->alloc, lim->bindless_sampled_images));
    NPH_TRY(index_allocator_init(&b->storage, dev->alloc, lim->bindless_storage_images));
    NPH_TRY(index_allocator_init(&b->samplers, dev->alloc, lim->bindless_samplers));

    VkDescriptorSetLayoutBinding bindings[3];
    VkDescriptorBindingFlags flags[3];
    memset(bindings, 0, sizeof bindings);
    const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_DESCRIPTOR_TYPE_SAMPLER};
    const u32 counts[3] = {lim->bindless_sampled_images, lim->bindless_storage_images,
                           lim->bindless_samplers};
    for (u32 i = 0; i < 3U; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = counts[i];
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                   VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info;
    memset(&flag_info, 0, sizeof flag_info);
    flag_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flag_info.bindingCount = 3U;
    flag_info.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo li;
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.pNext = &flag_info;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 3U;
    li.pBindings = bindings;
    VK_TRY(dev, vkCreateDescriptorSetLayout(dev->device, &li, NULL, &b->layout));
    gpu_set_name(dev, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, (u64)(uptr)b->layout,
                 "bindless layout");

    VkDescriptorPoolSize sizes[3];
    for (u32 i = 0; i < 3U; ++i) {
        sizes[i].type = types[i];
        sizes[i].descriptorCount = counts[i];
    }
    VkDescriptorPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1U;
    pi.poolSizeCount = 3U;
    pi.pPoolSizes = sizes;
    VK_TRY(dev, vkCreateDescriptorPool(dev->device, &pi, NULL, &b->pool));
    gpu_set_name(dev, VK_OBJECT_TYPE_DESCRIPTOR_POOL, (u64)(uptr)b->pool, "bindless pool");

    VkDescriptorSetAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = b->pool;
    ai.descriptorSetCount = 1U;
    ai.pSetLayouts = &b->layout;
    VK_TRY(dev, vkAllocateDescriptorSets(dev->device, &ai, &b->set));
    gpu_set_name(dev, VK_OBJECT_TYPE_DESCRIPTOR_SET, (u64)(uptr)b->set, "bindless set");

    VkPushConstantRange range;
    range.stageFlags = VK_SHADER_STAGE_ALL;
    range.offset = 0U;
    range.size = NPH_GPU_PUSH_CONSTANT_BYTES;
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof pli);
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1U;
    pli.pSetLayouts = &b->layout;
    pli.pushConstantRangeCount = 1U;
    pli.pPushConstantRanges = &range;
    VK_TRY(dev, vkCreatePipelineLayout(dev->device, &pli, NULL, &b->pipeline_layout));
    gpu_set_name(dev, VK_OBJECT_TYPE_PIPELINE_LAYOUT, (u64)(uptr)b->pipeline_layout,
                 "global pipeline layout");
    return NPH_RESULT_OK;
}

void gpu_bindless_shutdown(NphGpuDevice *dev)
{
    NphGpuBindless *b = &dev->bindless;
    if (b->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev->device, b->pipeline_layout, NULL);
        b->pipeline_layout = VK_NULL_HANDLE;
    }
    if (b->pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev->device, b->pool, NULL); // frees the set
        b->pool = VK_NULL_HANDLE;
        b->set = VK_NULL_HANDLE;
    }
    if (b->layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev->device, b->layout, NULL);
        b->layout = VK_NULL_HANDLE;
    }
    index_allocator_free(&b->sampled, dev->alloc);
    index_allocator_free(&b->storage, dev->alloc);
    index_allocator_free(&b->samplers, dev->alloc);
}

static void write_image_descriptor(NphGpuDevice *dev, u32 binding, u32 index, VkDescriptorType type,
                                   VkImageView view, VkSampler sampler, VkImageLayout layout)
{
    VkDescriptorImageInfo info;
    info.sampler = sampler;
    info.imageView = view;
    info.imageLayout = layout;
    VkWriteDescriptorSet write;
    memset(&write, 0, sizeof write);
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = dev->bindless.set;
    write.dstBinding = binding;
    write.dstArrayElement = index;
    write.descriptorCount = 1U;
    write.descriptorType = type;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(dev->device, 1U, &write, 0U, NULL);
}

u32 gpu_bindless_alloc_sampled(NphGpuDevice *dev, VkImageView view, VkImageLayout layout)
{
    NphGpuBindless *b = &dev->bindless;
    nph_spin_lock_acquire(&b->lock);
    const u32 index = index_alloc(&b->sampled);
    if (index != NPH_GPU_INVALID_INDEX) {
        write_image_descriptor(dev, 0U, index, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view,
                               VK_NULL_HANDLE, layout);
    }
    nph_spin_lock_release(&b->lock);
    return index;
}

u32 gpu_bindless_alloc_storage(NphGpuDevice *dev, VkImageView view)
{
    NphGpuBindless *b = &dev->bindless;
    nph_spin_lock_acquire(&b->lock);
    const u32 index = index_alloc(&b->storage);
    if (index != NPH_GPU_INVALID_INDEX) {
        write_image_descriptor(dev, 1U, index, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view,
                               VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
    }
    nph_spin_lock_release(&b->lock);
    return index;
}

u32 gpu_bindless_alloc_sampler(NphGpuDevice *dev, VkSampler sampler)
{
    NphGpuBindless *b = &dev->bindless;
    nph_spin_lock_acquire(&b->lock);
    const u32 index = index_alloc(&b->samplers);
    if (index != NPH_GPU_INVALID_INDEX) {
        write_image_descriptor(dev, 2U, index, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, sampler,
                               VK_IMAGE_LAYOUT_UNDEFINED);
    }
    nph_spin_lock_release(&b->lock);
    return index;
}

void gpu_bindless_free(NphGpuDevice *dev, NphGpuDeferredType type, u32 index)
{
    if (index == NPH_GPU_INVALID_INDEX) {
        return;
    }
    NphGpuBindless *b = &dev->bindless;
    nph_spin_lock_acquire(&b->lock);
    switch (type) {
    case NPH_GPU_DEFERRED_BINDLESS_SAMPLED:
        index_release(&b->sampled, index);
        break;
    case NPH_GPU_DEFERRED_BINDLESS_STORAGE:
        index_release(&b->storage, index);
        break;
    case NPH_GPU_DEFERRED_BINDLESS_SAMPLER:
        index_release(&b->samplers, index);
        break;
    default:
        NPH_UNREACHABLE();
        break;
    }
    nph_spin_lock_release(&b->lock);
}
