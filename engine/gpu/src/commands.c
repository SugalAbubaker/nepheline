// SPDX-License-Identifier: Apache-2.0
// Command recording: barriers, dynamic rendering, draws, dispatches,
// transfers, and debug labels (docs/gpu.md "Commands").
#include "gpu_internal.h"

#define CHECK_RECORDING(cmd) NPH_ASSERT((cmd) != NULL && (cmd)->recording)

// ---------------------------------------------------------------------------
// Barriers
// ---------------------------------------------------------------------------

void gpu_fill_image_barrier(const NphGpuDevice *dev, const NphGpuImageBarrier *b,
                            const NphGpuImageSlot *slot, NphGpuQueueType cmd_queue,
                            VkImageMemoryBarrier2 *out)
{
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    out->srcStageMask = gpu_vk_stages(b->src_stages);
    out->srcAccessMask = gpu_vk_access(b->src_access);
    out->dstStageMask = gpu_vk_stages(b->dst_stages);
    out->dstAccessMask = gpu_vk_access(b->dst_access);
    out->oldLayout = gpu_vk_layout(b->old_layout);
    out->newLayout = gpu_vk_layout(b->new_layout);
    out->image = slot->vk;
    out->subresourceRange.aspectMask = slot->aspect;
    out->subresourceRange.baseMipLevel = b->base_mip;
    out->subresourceRange.levelCount = gpu_mips_in_range(slot, b->base_mip, b->mip_count);
    out->subresourceRange.baseArrayLayer = b->base_layer;
    out->subresourceRange.layerCount = gpu_layers_in_range(slot, b->base_layer, b->layer_count);
    const u32 src_family = dev->queues[b->src_queue]->family;
    const u32 dst_family = dev->queues[b->dst_queue]->family;
    if (slot->exclusive && src_family != dst_family) {
        out->srcQueueFamilyIndex = src_family;
        out->dstQueueFamilyIndex = dst_family;
        // A release ignores the destination scope and an acquire the
        // source scope, but both sides must state the same transition;
        // the caller passes identical descriptions to both queues.
        if (dev->queues[cmd_queue]->family == src_family) {
            out->dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            out->dstAccessMask = VK_ACCESS_2_NONE;
        } else {
            out->srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            out->srcAccessMask = VK_ACCESS_2_NONE;
        }
    } else {
        out->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    if (out->srcStageMask == 0U) {
        out->srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    }
    if (out->dstStageMask == 0U) {
        out->dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    }
}

#define BARRIER_BATCH 32U

void nph_gpu_cmd_barrier(NphGpuCmd *cmd, const NphGpuBarrierDesc *desc)
{
    CHECK_RECORDING(cmd);
    NphGpuDevice *dev = cmd->dev;
    VkMemoryBarrier2 mem[BARRIER_BATCH];
    VkBufferMemoryBarrier2 buf[BARRIER_BATCH];
    VkImageMemoryBarrier2 img[BARRIER_BATCH];
    u32 mi = 0U;
    u32 bi = 0U;
    u32 ii = 0U;
    // Batches of up to 32 of each kind; larger descriptions take several
    // vkCmdPipelineBarrier2 calls.
    while (mi < desc->memory_count || bi < desc->buffer_count || ii < desc->image_count) {
        u32 m = 0U;
        u32 b = 0U;
        u32 i = 0U;
        for (; mi < desc->memory_count && m < BARRIER_BATCH; ++mi, ++m) {
            const NphGpuMemoryBarrier *src = &desc->memory[mi];
            memset(&mem[m], 0, sizeof mem[m]);
            mem[m].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            mem[m].srcStageMask = gpu_vk_stages(src->src_stages);
            mem[m].srcAccessMask = gpu_vk_access(src->src_access);
            mem[m].dstStageMask = gpu_vk_stages(src->dst_stages);
            mem[m].dstAccessMask = gpu_vk_access(src->dst_access);
        }
        for (; bi < desc->buffer_count && b < BARRIER_BATCH; ++bi, ++b) {
            const NphGpuBufferBarrier *src = &desc->buffers[bi];
            NphGpuBufferSlot *slot = gpu_buffer(dev, src->buffer);
            slot->state.stages = src->dst_stages;
            slot->state.access = src->dst_access;
            slot->state.queue = (u32)cmd->queue;
            memset(&buf[b], 0, sizeof buf[b]);
            buf[b].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            buf[b].srcStageMask = gpu_vk_stages(src->src_stages);
            buf[b].srcAccessMask = gpu_vk_access(src->src_access);
            buf[b].dstStageMask = gpu_vk_stages(src->dst_stages);
            buf[b].dstAccessMask = gpu_vk_access(src->dst_access);
            buf[b].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf[b].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf[b].buffer = slot->vk;
            buf[b].offset = src->offset;
            buf[b].size =
                (src->size == 0U || src->size == NPH_GPU_WHOLE_SIZE) ? VK_WHOLE_SIZE : src->size;
        }
        for (; ii < desc->image_count && i < BARRIER_BATCH; ++ii, ++i) {
            const NphGpuImageBarrier *src = &desc->images[ii];
            NphGpuImageSlot *slot = gpu_image(dev, src->image);
            gpu_fill_image_barrier(dev, src, slot, cmd->queue, &img[i]);
            gpu_image_set_state(slot, src->base_mip, src->mip_count, src->new_layout,
                                src->dst_stages, src->dst_access, (u32)src->dst_queue);
        }
        VkDependencyInfo dep;
        memset(&dep, 0, sizeof dep);
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = m;
        dep.pMemoryBarriers = mem;
        dep.bufferMemoryBarrierCount = b;
        dep.pBufferMemoryBarriers = buf;
        dep.imageMemoryBarrierCount = i;
        dep.pImageMemoryBarriers = img;
        vkCmdPipelineBarrier2(cmd->vk, &dep);
    }
}

void nph_gpu_cmd_image_barrier(NphGpuCmd *cmd, NphGpuImage image, u32 src_stages, u32 src_access,
                               NphGpuLayout old_layout, u32 dst_stages, u32 dst_access,
                               NphGpuLayout new_layout)
{
    NphGpuImageBarrier b;
    memset(&b, 0, sizeof b);
    b.image = image;
    b.src_stages = src_stages;
    b.src_access = src_access;
    b.old_layout = old_layout;
    b.dst_stages = dst_stages;
    b.dst_access = dst_access;
    b.new_layout = new_layout;
    b.mip_count = NPH_GPU_ALL_MIPS;
    b.layer_count = NPH_GPU_ALL_LAYERS;
    b.src_queue = cmd->queue;
    b.dst_queue = cmd->queue;
    NphGpuBarrierDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.images = &b;
    desc.image_count = 1U;
    nph_gpu_cmd_barrier(cmd, &desc);
}

void nph_gpu_cmd_buffer_barrier(NphGpuCmd *cmd, NphGpuBuffer buffer, u32 src_stages, u32 src_access,
                                u32 dst_stages, u32 dst_access)
{
    NphGpuBufferBarrier b;
    memset(&b, 0, sizeof b);
    b.buffer = buffer;
    b.src_stages = src_stages;
    b.src_access = src_access;
    b.dst_stages = dst_stages;
    b.dst_access = dst_access;
    b.size = NPH_GPU_WHOLE_SIZE;
    NphGpuBarrierDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.buffers = &b;
    desc.buffer_count = 1U;
    nph_gpu_cmd_barrier(cmd, &desc);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void fill_attachment(NphGpuDevice *dev, const NphGpuAttachment *a, VkImageLayout layout,
                            VkRenderingAttachmentInfo *out, VkImageAspectFlags aspect_for_clear)
{
    NphGpuImageSlot *slot = gpu_image(dev, a->image);
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    out->imageView = gpu_image_attachment_view(dev, slot, a->mip, a->base_layer, a->layer_count);
    out->imageLayout = layout;
    out->loadOp = gpu_vk_load_op(a->load);
    out->storeOp = gpu_vk_store_op(a->store);
    if ((aspect_for_clear & VK_IMAGE_ASPECT_COLOR_BIT) != 0U) {
        memcpy(out->clearValue.color.float32, a->clear.color_f32, sizeof a->clear.color_f32);
    } else {
        out->clearValue.depthStencil.depth = a->clear.ds.depth;
        out->clearValue.depthStencil.stencil = a->clear.ds.stencil;
    }
}

void nph_gpu_cmd_begin_rendering(NphGpuCmd *cmd, const NphGpuRenderingDesc *desc)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT_MSG(!cmd->rendering, "nested begin_rendering");
    NPH_ASSERT(desc->color_count <= NPH_GPU_MAX_COLOR_ATTACHMENTS);
    NphGpuDevice *dev = cmd->dev;
    VkRenderingAttachmentInfo colors[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    VkRenderingAttachmentInfo depth;
    VkRenderingInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    u32 width = desc->width;
    u32 height = desc->height;
    u32 layers = 1U;
    for (u32 i = 0; i < desc->color_count; ++i) {
        const NphGpuAttachment *a = &desc->colors[i];
        fill_attachment(dev, a, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &colors[i],
                        VK_IMAGE_ASPECT_COLOR_BIT);
        const NphGpuImageSlot *slot = gpu_image(dev, a->image);
        if (width == 0U || height == 0U) {
            width = nph_gpu_mip_extent(slot->desc.width, a->mip);
            height = nph_gpu_mip_extent(slot->desc.height, a->mip);
        }
        layers = nph_max_u32(layers, gpu_layers_in_range(slot, a->base_layer, a->layer_count));
    }
    info.colorAttachmentCount = desc->color_count;
    info.pColorAttachments = colors;
    if (!nph_gpu_image_is_null(desc->depth.image)) {
        const NphGpuImageSlot *slot = gpu_image(dev, desc->depth.image);
        fill_attachment(dev, &desc->depth,
                        desc->depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                              : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        &depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        info.pDepthAttachment = &depth;
        if ((slot->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U) {
            // Depth-stencil formats: the stencil aspect follows the depth
            // attachment's ops (the layout covers both aspects).
            info.pStencilAttachment = &depth;
            if (!desc->depth_read_only) {
                depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            } else {
                depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            }
        }
        if (width == 0U || height == 0U) {
            width = nph_gpu_mip_extent(slot->desc.width, desc->depth.mip);
            height = nph_gpu_mip_extent(slot->desc.height, desc->depth.mip);
        }
        layers = nph_max_u32(
            layers, gpu_layers_in_range(slot, desc->depth.base_layer, desc->depth.layer_count));
    }
    info.renderArea.offset.x = (i32)desc->x;
    info.renderArea.offset.y = (i32)desc->y;
    info.renderArea.extent.width = width;
    info.renderArea.extent.height = height;
    info.layerCount = layers;
    vkCmdBeginRendering(cmd->vk, &info);
    cmd->rendering = 1;
    nph_gpu_cmd_set_viewport(cmd, (f32)desc->x, (f32)desc->y, (f32)width, (f32)height, 0.0F, 1.0F);
    nph_gpu_cmd_set_scissor(cmd, (i32)desc->x, (i32)desc->y, width, height);
}

void nph_gpu_cmd_end_rendering(NphGpuCmd *cmd)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT(cmd->rendering);
    vkCmdEndRendering(cmd->vk);
    cmd->rendering = 0;
}

void nph_gpu_cmd_set_viewport(NphGpuCmd *cmd, f32 x, f32 y, f32 width, f32 height, f32 min_depth,
                              f32 max_depth)
{
    CHECK_RECORDING(cmd);
    VkViewport vp;
    vp.x = x;
    vp.y = y;
    vp.width = width;
    vp.height = height;
    vp.minDepth = min_depth;
    vp.maxDepth = max_depth;
    vkCmdSetViewport(cmd->vk, 0U, 1U, &vp);
}

void nph_gpu_cmd_set_scissor(NphGpuCmd *cmd, i32 x, i32 y, u32 width, u32 height)
{
    CHECK_RECORDING(cmd);
    VkRect2D rect;
    rect.offset.x = x;
    rect.offset.y = y;
    rect.extent.width = width;
    rect.extent.height = height;
    vkCmdSetScissor(cmd->vk, 0U, 1U, &rect);
}

void nph_gpu_cmd_bind_pipeline(NphGpuCmd *cmd, NphGpuPipeline pipeline)
{
    CHECK_RECORDING(cmd);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    const VkPipelineBindPoint point =
        slot->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(cmd->vk, point, slot->vk);
    vkCmdBindDescriptorSets(cmd->vk, point, dev->bindless.pipeline_layout, 0U, 1U,
                            &dev->bindless.set, 0U, NULL);
    cmd->pipeline = pipeline;
}

void nph_gpu_cmd_push_constants(NphGpuCmd *cmd, const void *data, u32 size)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT(size <= NPH_GPU_PUSH_CONSTANT_BYTES && (size % 4U) == 0U && size != 0U);
    vkCmdPushConstants(cmd->vk, cmd->dev->bindless.pipeline_layout, VK_SHADER_STAGE_ALL, 0U, size,
                       data);
}

void nph_gpu_cmd_bind_index_buffer(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset,
                                   NphGpuIndexType type)
{
    CHECK_RECORDING(cmd);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    vkCmdBindIndexBuffer(cmd->vk, slot->vk, offset,
                         type == NPH_GPU_INDEX_U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void nph_gpu_cmd_draw(NphGpuCmd *cmd, u32 vertex_count, u32 instance_count, u32 first_vertex,
                      u32 first_instance)
{
    CHECK_RECORDING(cmd);
    vkCmdDraw(cmd->vk, vertex_count, instance_count, first_vertex, first_instance);
}

void nph_gpu_cmd_draw_indexed(NphGpuCmd *cmd, u32 index_count, u32 instance_count, u32 first_index,
                              i32 vertex_offset, u32 first_instance)
{
    CHECK_RECORDING(cmd);
    vkCmdDrawIndexed(cmd->vk, index_count, instance_count, first_index, vertex_offset,
                     first_instance);
}

void nph_gpu_cmd_draw_indirect(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset, u32 draw_count,
                               u32 stride)
{
    CHECK_RECORDING(cmd);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    vkCmdDrawIndirect(cmd->vk, slot->vk, offset, draw_count, stride);
}

void nph_gpu_cmd_draw_indexed_indirect(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset,
                                       u32 draw_count, u32 stride)
{
    CHECK_RECORDING(cmd);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    vkCmdDrawIndexedIndirect(cmd->vk, slot->vk, offset, draw_count, stride);
}

void nph_gpu_cmd_draw_indexed_indirect_count(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset,
                                             NphGpuBuffer count_buffer, u64 count_offset,
                                             u32 max_draw_count, u32 stride)
{
    CHECK_RECORDING(cmd);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    const NphGpuBufferSlot *count = gpu_buffer(cmd->dev, count_buffer);
    vkCmdDrawIndexedIndirectCount(cmd->vk, slot->vk, offset, count->vk, count_offset,
                                  max_draw_count, stride);
}

void nph_gpu_cmd_draw_mesh_tasks(NphGpuCmd *cmd, u32 x, u32 y, u32 z)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT_MSG(cmd->dev->info.features.mesh_shader, "mesh shaders are not supported");
    vkCmdDrawMeshTasksEXT(cmd->vk, x, y, z);
}

void nph_gpu_cmd_dispatch(NphGpuCmd *cmd, u32 x, u32 y, u32 z)
{
    CHECK_RECORDING(cmd);
    vkCmdDispatch(cmd->vk, x, y, z);
}

void nph_gpu_cmd_dispatch_indirect(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset)
{
    CHECK_RECORDING(cmd);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    vkCmdDispatchIndirect(cmd->vk, slot->vk, offset);
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

void nph_gpu_cmd_copy_buffer(NphGpuCmd *cmd, NphGpuBuffer src, NphGpuBuffer dst,
                             const NphGpuBufferCopy *regions, u32 region_count)
{
    CHECK_RECORDING(cmd);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuBufferSlot *s = gpu_buffer(dev, src);
    const NphGpuBufferSlot *d = gpu_buffer(dev, dst);
    VkBufferCopy2 copies[16];
    u32 done = 0U;
    while (done < region_count) {
        const u32 n = nph_min_u32(region_count - done, 16U);
        for (u32 i = 0; i < n; ++i) {
            memset(&copies[i], 0, sizeof copies[i]);
            copies[i].sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
            copies[i].srcOffset = regions[done + i].src_offset;
            copies[i].dstOffset = regions[done + i].dst_offset;
            copies[i].size = regions[done + i].size;
        }
        VkCopyBufferInfo2 info;
        memset(&info, 0, sizeof info);
        info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
        info.srcBuffer = s->vk;
        info.dstBuffer = d->vk;
        info.regionCount = n;
        info.pRegions = copies;
        vkCmdCopyBuffer2(cmd->vk, &info);
        done += n;
    }
}

static void fill_buffer_image_copy(const NphGpuImageSlot *slot, const NphGpuBufferImageCopy *r,
                                   VkBufferImageCopy2 *out)
{
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    out->bufferOffset = r->buffer_offset;
    out->imageSubresource.aspectMask =
        (slot->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U ? VK_IMAGE_ASPECT_DEPTH_BIT : slot->aspect;
    out->imageSubresource.mipLevel = r->mip;
    out->imageSubresource.baseArrayLayer = r->base_layer;
    out->imageSubresource.layerCount = r->layer_count == 0U ? 1U : r->layer_count;
    out->imageOffset.x = r->x;
    out->imageOffset.y = r->y;
    out->imageOffset.z = r->z;
    if (r->width == 0U || r->height == 0U) {
        out->imageExtent.width = nph_gpu_mip_extent(slot->desc.width, r->mip) - (u32)r->x;
        out->imageExtent.height = nph_gpu_mip_extent(slot->desc.height, r->mip) - (u32)r->y;
        out->imageExtent.depth = nph_gpu_mip_extent(slot->desc.depth, r->mip) - (u32)r->z;
    } else {
        out->imageExtent.width = r->width;
        out->imageExtent.height = r->height;
        out->imageExtent.depth = r->depth == 0U ? 1U : r->depth;
    }
}

void nph_gpu_cmd_copy_buffer_to_image(NphGpuCmd *cmd, NphGpuBuffer src, NphGpuImage dst,
                                      NphGpuLayout dst_layout, const NphGpuBufferImageCopy *regions,
                                      u32 region_count)
{
    CHECK_RECORDING(cmd);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuBufferSlot *s = gpu_buffer(dev, src);
    const NphGpuImageSlot *d = gpu_image(dev, dst);
    VkBufferImageCopy2 copies[16];
    u32 done = 0U;
    while (done < region_count) {
        const u32 n = nph_min_u32(region_count - done, 16U);
        for (u32 i = 0; i < n; ++i) {
            fill_buffer_image_copy(d, &regions[done + i], &copies[i]);
        }
        VkCopyBufferToImageInfo2 info;
        memset(&info, 0, sizeof info);
        info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
        info.srcBuffer = s->vk;
        info.dstImage = d->vk;
        info.dstImageLayout = gpu_vk_layout(dst_layout);
        info.regionCount = n;
        info.pRegions = copies;
        vkCmdCopyBufferToImage2(cmd->vk, &info);
        done += n;
    }
}

void nph_gpu_cmd_copy_image_to_buffer(NphGpuCmd *cmd, NphGpuImage src, NphGpuLayout src_layout,
                                      NphGpuBuffer dst, const NphGpuBufferImageCopy *regions,
                                      u32 region_count)
{
    CHECK_RECORDING(cmd);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuImageSlot *s = gpu_image(dev, src);
    const NphGpuBufferSlot *d = gpu_buffer(dev, dst);
    VkBufferImageCopy2 copies[16];
    u32 done = 0U;
    while (done < region_count) {
        const u32 n = nph_min_u32(region_count - done, 16U);
        for (u32 i = 0; i < n; ++i) {
            fill_buffer_image_copy(s, &regions[done + i], &copies[i]);
        }
        VkCopyImageToBufferInfo2 info;
        memset(&info, 0, sizeof info);
        info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
        info.srcImage = s->vk;
        info.srcImageLayout = gpu_vk_layout(src_layout);
        info.dstBuffer = d->vk;
        info.regionCount = n;
        info.pRegions = copies;
        vkCmdCopyImageToBuffer2(cmd->vk, &info);
        done += n;
    }
}

void nph_gpu_cmd_copy_image(NphGpuCmd *cmd, NphGpuImage src, u32 src_mip, NphGpuLayout src_layout,
                            NphGpuImage dst, u32 dst_mip, NphGpuLayout dst_layout)
{
    CHECK_RECORDING(cmd);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuImageSlot *s = gpu_image(dev, src);
    const NphGpuImageSlot *d = gpu_image(dev, dst);
    VkImageCopy2 region;
    memset(&region, 0, sizeof region);
    region.sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2;
    region.srcSubresource.aspectMask = s->aspect;
    region.srcSubresource.mipLevel = src_mip;
    region.srcSubresource.layerCount = s->desc.layer_count;
    region.dstSubresource.aspectMask = d->aspect;
    region.dstSubresource.mipLevel = dst_mip;
    region.dstSubresource.layerCount = d->desc.layer_count;
    region.extent.width = nph_gpu_mip_extent(s->desc.width, src_mip);
    region.extent.height = nph_gpu_mip_extent(s->desc.height, src_mip);
    region.extent.depth = nph_gpu_mip_extent(s->desc.depth, src_mip);
    VkCopyImageInfo2 info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
    info.srcImage = s->vk;
    info.srcImageLayout = gpu_vk_layout(src_layout);
    info.dstImage = d->vk;
    info.dstImageLayout = gpu_vk_layout(dst_layout);
    info.regionCount = 1U;
    info.pRegions = &region;
    vkCmdCopyImage2(cmd->vk, &info);
}

static void blit_mip(NphGpuCmd *cmd, const NphGpuImageSlot *s, u32 src_mip,
                     VkImageLayout src_layout, const NphGpuImageSlot *d, u32 dst_mip,
                     VkImageLayout dst_layout, VkFilter filter)
{
    VkImageBlit2 region;
    memset(&region, 0, sizeof region);
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource.aspectMask = s->aspect;
    region.srcSubresource.mipLevel = src_mip;
    region.srcSubresource.layerCount = s->desc.layer_count;
    region.srcOffsets[1].x = (i32)nph_gpu_mip_extent(s->desc.width, src_mip);
    region.srcOffsets[1].y = (i32)nph_gpu_mip_extent(s->desc.height, src_mip);
    region.srcOffsets[1].z = (i32)nph_gpu_mip_extent(s->desc.depth, src_mip);
    region.dstSubresource.aspectMask = d->aspect;
    region.dstSubresource.mipLevel = dst_mip;
    region.dstSubresource.layerCount = d->desc.layer_count;
    region.dstOffsets[1].x = (i32)nph_gpu_mip_extent(d->desc.width, dst_mip);
    region.dstOffsets[1].y = (i32)nph_gpu_mip_extent(d->desc.height, dst_mip);
    region.dstOffsets[1].z = (i32)nph_gpu_mip_extent(d->desc.depth, dst_mip);
    VkBlitImageInfo2 info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
    info.srcImage = s->vk;
    info.srcImageLayout = src_layout;
    info.dstImage = d->vk;
    info.dstImageLayout = dst_layout;
    info.regionCount = 1U;
    info.pRegions = &region;
    info.filter = filter;
    vkCmdBlitImage2(cmd->vk, &info);
}

void nph_gpu_cmd_blit_image(NphGpuCmd *cmd, NphGpuImage src, u32 src_mip, NphGpuLayout src_layout,
                            NphGpuImage dst, u32 dst_mip, NphGpuLayout dst_layout,
                            NphGpuFilter filter)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT_MSG(cmd->queue == NPH_GPU_QUEUE_GRAPHICS, "blits need the graphics queue");
    blit_mip(cmd, gpu_image(cmd->dev, src), src_mip, gpu_vk_layout(src_layout),
             gpu_image(cmd->dev, dst), dst_mip, gpu_vk_layout(dst_layout), gpu_vk_filter(filter));
}

void nph_gpu_cmd_generate_mips(NphGpuCmd *cmd, NphGpuImage image, NphGpuLayout final_layout,
                               u32 final_stages, u32 final_access)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT_MSG(cmd->queue == NPH_GPU_QUEUE_GRAPHICS, "mip generation needs the graphics queue");
    NphGpuImageSlot *slot = gpu_image(cmd->dev, image);
    const u32 mips = slot->desc.mip_count;
    for (u32 mip = 1; mip < mips; ++mip) {
        NphGpuImageBarrier to_dst;
        memset(&to_dst, 0, sizeof to_dst);
        to_dst.image = image;
        to_dst.src_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        to_dst.src_access = (u32)NPH_GPU_ACCESS_NONE;
        to_dst.old_layout = NPH_GPU_LAYOUT_UNDEFINED;
        to_dst.dst_stages = (u32)NPH_GPU_STAGE_TRANSFER;
        to_dst.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        to_dst.new_layout = NPH_GPU_LAYOUT_TRANSFER_DST;
        to_dst.base_mip = mip;
        to_dst.mip_count = 1U;
        to_dst.layer_count = NPH_GPU_ALL_LAYERS;
        to_dst.src_queue = cmd->queue;
        to_dst.dst_queue = cmd->queue;
        NphGpuBarrierDesc desc;
        memset(&desc, 0, sizeof desc);
        desc.images = &to_dst;
        desc.image_count = 1U;
        nph_gpu_cmd_barrier(cmd, &desc);
        blit_mip(cmd, slot, mip - 1U, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot, mip,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_FILTER_LINEAR);
        // This mip becomes the source of the next one.
        NphGpuImageBarrier to_src = to_dst;
        to_src.src_access = (u32)NPH_GPU_ACCESS_TRANSFER_WRITE;
        to_src.old_layout = NPH_GPU_LAYOUT_TRANSFER_DST;
        to_src.dst_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
        to_src.new_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
        desc.images = &to_src;
        nph_gpu_cmd_barrier(cmd, &desc);
    }
    NphGpuImageBarrier final;
    memset(&final, 0, sizeof final);
    final.image = image;
    final.src_stages = (u32)NPH_GPU_STAGE_TRANSFER;
    final.src_access = (u32)NPH_GPU_ACCESS_TRANSFER_READ;
    final.old_layout = NPH_GPU_LAYOUT_TRANSFER_SRC;
    final.dst_stages = final_stages;
    final.dst_access = final_access;
    final.new_layout = final_layout;
    final.mip_count = NPH_GPU_ALL_MIPS;
    final.layer_count = NPH_GPU_ALL_LAYERS;
    final.src_queue = cmd->queue;
    final.dst_queue = cmd->queue;
    NphGpuBarrierDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.images = &final;
    desc.image_count = 1U;
    nph_gpu_cmd_barrier(cmd, &desc);
}

void nph_gpu_cmd_fill_buffer(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset, u64 size, u32 data)
{
    CHECK_RECORDING(cmd);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    vkCmdFillBuffer(cmd->vk, slot->vk, offset,
                    (size == 0U || size == NPH_GPU_WHOLE_SIZE) ? VK_WHOLE_SIZE : size, data);
}

void nph_gpu_cmd_update_buffer(NphGpuCmd *cmd, NphGpuBuffer buffer, u64 offset, const void *data,
                               u64 size)
{
    CHECK_RECORDING(cmd);
    NPH_ASSERT(size != 0U && size <= 65536U && (size % 4U) == 0U && (offset % 4U) == 0U);
    const NphGpuBufferSlot *slot = gpu_buffer(cmd->dev, buffer);
    vkCmdUpdateBuffer(cmd->vk, slot->vk, offset, size, data);
}

static void fill_range(const NphGpuImageSlot *slot, VkImageAspectFlags aspect, u32 base_mip,
                       u32 mip_count, u32 base_layer, u32 layer_count, VkImageSubresourceRange *out)
{
    out->aspectMask = aspect;
    out->baseMipLevel = base_mip;
    out->levelCount = gpu_mips_in_range(slot, base_mip, mip_count);
    out->baseArrayLayer = base_layer;
    out->layerCount = gpu_layers_in_range(slot, base_layer, layer_count);
}

void nph_gpu_cmd_clear_color_image(NphGpuCmd *cmd, NphGpuImage image, NphGpuLayout layout,
                                   const NphGpuClearValue *value, u32 base_mip, u32 mip_count,
                                   u32 base_layer, u32 layer_count)
{
    CHECK_RECORDING(cmd);
    const NphGpuImageSlot *slot = gpu_image(cmd->dev, image);
    VkClearColorValue color;
    memcpy(color.float32, value->color_f32, sizeof color.float32);
    VkImageSubresourceRange range;
    fill_range(slot, VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, base_layer, layer_count,
               &range);
    vkCmdClearColorImage(cmd->vk, slot->vk, gpu_vk_layout(layout), &color, 1U, &range);
}

void nph_gpu_cmd_clear_depth_image(NphGpuCmd *cmd, NphGpuImage image, NphGpuLayout layout,
                                   const NphGpuClearValue *value, u32 base_mip, u32 mip_count,
                                   u32 base_layer, u32 layer_count)
{
    CHECK_RECORDING(cmd);
    const NphGpuImageSlot *slot = gpu_image(cmd->dev, image);
    VkClearDepthStencilValue ds;
    ds.depth = value->ds.depth;
    ds.stencil = value->ds.stencil;
    VkImageSubresourceRange range;
    fill_range(slot, slot->aspect, base_mip, mip_count, base_layer, layer_count, &range);
    vkCmdClearDepthStencilImage(cmd->vk, slot->vk, gpu_vk_layout(layout), &ds, 1U, &range);
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

void nph_gpu_cmd_begin_label(NphGpuCmd *cmd, const char *name, f32 r, f32 g, f32 b)
{
    CHECK_RECORDING(cmd);
    if (!cmd->dev->has_debug_utils || vkCmdBeginDebugUtilsLabelEXT == NULL) {
        return;
    }
    VkDebugUtilsLabelEXT label;
    memset(&label, 0, sizeof label);
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0] = r;
    label.color[1] = g;
    label.color[2] = b;
    label.color[3] = 1.0F;
    vkCmdBeginDebugUtilsLabelEXT(cmd->vk, &label);
}

void nph_gpu_cmd_end_label(NphGpuCmd *cmd)
{
    CHECK_RECORDING(cmd);
    if (!cmd->dev->has_debug_utils || vkCmdEndDebugUtilsLabelEXT == NULL) {
        return;
    }
    vkCmdEndDebugUtilsLabelEXT(cmd->vk);
}
