// SPDX-License-Identifier: Apache-2.0
// Acceleration structures (gpu/accel.h; docs/gpu.md "Ray queries"): each
// lives in a buffer of its own, created through the buffer pool so that it
// is counted, named, and released like any other; the structure object is
// destroyed with the frame that may still trace against it, before its
// buffer.
#include <nepheline/gpu/accel.h>

#include "gpu_internal.h"

#include <string.h>

// Geometries per bottom-level build, on the stack.
#define ACCEL_MAX_GEOMETRIES 64U

static NphGpuAccelSlot *accel_slot(const NphGpuDevice *dev, NphGpuAccel accel)
{
    return (NphGpuAccelSlot *)gpu_pool_get(&dev->accels, accel.value, "acceleration structure");
}

static VkBuildAccelerationStructureFlagsKHR build_flags(u32 flags)
{
    VkBuildAccelerationStructureFlagsKHR out = 0U;
    if ((flags & (u32)NPH_GPU_ACCEL_PREFER_TRACE) != 0U) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    }
    if ((flags & (u32)NPH_GPU_ACCEL_PREFER_BUILD) != 0U) {
        out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    }
    return out;
}

static u64 address_at(const NphGpuDevice *dev, NphGpuBuffer buffer, u64 offset)
{
    return nph_gpu_buffer_is_null(buffer) ? 0U : nph_gpu_buffer_address(dev, buffer) + offset;
}

// The Vulkan description of triangle geometries; false for one the device
// cannot build.
static bool triangle_geometries(const NphGpuDevice *dev, const NphGpuAccelTriangles *in,
                                u32 count, VkAccelerationStructureGeometryKHR *out,
                                VkAccelerationStructureBuildRangeInfoKHR *ranges,
                                u32 *primitive_counts)
{
    for (u32 i = 0; i < count; ++i) {
        const NphGpuAccelTriangles *t = &in[i];
        if (!nph_gpu_format_supports(dev, t->vertex_format,
                                     (u32)NPH_GPU_FORMAT_FEATURE_ACCEL_VERTEX) ||
            t->vertex_count == 0U || t->vertex_stride == 0U || (t->index_offset & 3U) != 0U) {
            GPU_LOG_ERROR("acceleration structure: geometry %u has a vertex format the device "
                          "cannot build from, no vertices, or misaligned indices",
                          i);
            return false;
        }
        VkAccelerationStructureGeometryKHR *g = &out[i];
        memset(g, 0, sizeof *g);
        g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g->flags = t->opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0U;
        VkAccelerationStructureGeometryTrianglesDataKHR *tri = &g->geometry.triangles;
        tri->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri->vertexFormat = gpu_vk_format(t->vertex_format);
        tri->vertexData.deviceAddress = address_at(dev, t->vertices, t->vertex_offset);
        tri->vertexStride = t->vertex_stride;
        tri->maxVertex = t->vertex_count - 1U;
        tri->indexType = VK_INDEX_TYPE_UINT32;
        tri->indexData.deviceAddress = address_at(dev, t->indices, t->index_offset);
        if (ranges != NULL) {
            memset(&ranges[i], 0, sizeof ranges[i]);
            ranges[i].primitiveCount = t->triangle_count;
        }
        primitive_counts[i] = t->triangle_count;
    }
    return true;
}

static void instance_geometry(const NphGpuDevice *dev, NphGpuBuffer instances, u64 offset,
                              VkAccelerationStructureGeometryKHR *out)
{
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    out->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    VkAccelerationStructureGeometryInstancesDataKHR *inst = &out->geometry.instances;
    inst->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    inst->arrayOfPointers = VK_FALSE;
    inst->data.deviceAddress = address_at(dev, instances, offset);
}

static void build_info(VkAccelerationStructureTypeKHR type, u32 flags,
                       const VkAccelerationStructureGeometryKHR *geometries, u32 count,
                       VkAccelerationStructureBuildGeometryInfoKHR *out)
{
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    out->type = type;
    out->flags = build_flags(flags);
    out->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    out->geometryCount = count;
    out->pGeometries = geometries;
}

static void sizes_of(NphGpuDevice *dev, const VkAccelerationStructureBuildGeometryInfoKHR *info,
                     const u32 *primitive_counts, NphGpuAccelSizes *out)
{
    VkAccelerationStructureBuildSizesInfoKHR sizes;
    memset(&sizes, 0, sizeof sizes);
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        dev->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, info, primitive_counts,
        &sizes);
    out->accel = sizes.accelerationStructureSize;
    out->scratch = sizes.buildScratchSize;
}

NphResult nph_gpu_accel_blas_sizes(NphGpuDevice *dev, const NphGpuAccelTriangles *geometries,
                                   u32 count, u32 flags, NphGpuAccelSizes *out)
{
    NPH_ASSERT(dev != NULL && out != NULL && dev->info.features.ray_query);
    memset(out, 0, sizeof *out);
    if (geometries == NULL || count == 0U || count > ACCEL_MAX_GEOMETRIES) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkAccelerationStructureGeometryKHR g[ACCEL_MAX_GEOMETRIES];
    u32 counts[ACCEL_MAX_GEOMETRIES];
    if (!triangle_geometries(dev, geometries, count, g, NULL, counts)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkAccelerationStructureBuildGeometryInfoKHR info;
    build_info(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, g, count, &info);
    sizes_of(dev, &info, counts, out);
    return NPH_RESULT_OK;
}

NphResult nph_gpu_accel_tlas_sizes(NphGpuDevice *dev, u32 max_instances, u32 flags,
                                   NphGpuAccelSizes *out)
{
    NPH_ASSERT(dev != NULL && out != NULL && dev->info.features.ray_query);
    memset(out, 0, sizeof *out);
    if (max_instances == 0U || max_instances > dev->info.limits.accel_max_instances) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkAccelerationStructureGeometryKHR g;
    instance_geometry(dev, NPH_GPU_BUFFER_NULL, 0U, &g);
    VkAccelerationStructureBuildGeometryInfoKHR info;
    build_info(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, flags, &g, 1U, &info);
    sizes_of(dev, &info, &max_instances, out);
    return NPH_RESULT_OK;
}

NphResult nph_gpu_accel_create(NphGpuDevice *dev, const NphGpuAccelDesc *desc, NphGpuAccel *out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL && dev->info.features.ray_query);
    out->value = 0U;
    if (desc->size == 0U || (u32)desc->type > (u32)NPH_GPU_ACCEL_TOP) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u32 handle = 0U;
    NphGpuAccelSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->accels, &handle, (void **)&slot));
    memset(slot, 0, sizeof *slot);
    gpu_copy_name(slot->name, desc->name, "acceleration structure", handle);
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = desc->size;
    bd.usage = GPU_BUFFER_USAGE_ACCEL_STORAGE;
    bd.memory = NPH_GPU_MEMORY_DEVICE;
    bd.name = slot->name;
    NphResult r = nph_gpu_buffer_create(dev, &bd, &slot->buffer);
    if (r != NPH_RESULT_OK) {
        gpu_pool_release(&dev->accels, handle);
        return r;
    }
    VkAccelerationStructureCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    ci.buffer = gpu_buffer(dev, slot->buffer)->vk;
    ci.size = desc->size;
    ci.type = desc->type == NPH_GPU_ACCEL_TOP ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                                              : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    const VkResult vr = vkCreateAccelerationStructureKHR(dev->device, &ci, NULL, &slot->vk);
    if (vr != VK_SUCCESS) {
        nph_gpu_buffer_destroy(dev, slot->buffer);
        gpu_pool_release(&dev->accels, handle);
        return gpu_vk_fail(dev, vr, "vkCreateAccelerationStructureKHR");
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    ai.accelerationStructure = slot->vk;
    slot->address = vkGetAccelerationStructureDeviceAddressKHR(dev->device, &ai);
    slot->size = desc->size;
    slot->type = (u32)desc->type;
    gpu_set_name(dev, VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (u64)(uptr)slot->vk, slot->name);
    out->value = handle;
    return NPH_RESULT_OK;
}

void nph_gpu_accel_destroy(NphGpuDevice *dev, NphGpuAccel accel)
{
    NPH_ASSERT(dev != NULL);
    if (accel.value == 0U) {
        return;
    }
    NphGpuAccelSlot *slot = accel_slot(dev, accel);
    if (slot == NULL) {
        return;
    }
    NphGpuDeferred e;
    memset(&e, 0, sizeof e);
    e.type = NPH_GPU_DEFERRED_ACCEL;
    e.handle = slot->vk;
    gpu_defer(dev, &e);
    // The buffer's own destruction is deferred too, and runs after the
    // structure's (the frame's list is processed in order).
    nph_gpu_buffer_destroy(dev, slot->buffer);
    slot->vk = VK_NULL_HANDLE;
    gpu_pool_release(&dev->accels, accel.value);
}

bool nph_gpu_accel_is_valid(const NphGpuDevice *dev, NphGpuAccel accel)
{
    const NphHandle h = {accel.value};
    return nph_handle_pool_is_valid(&dev->accels.handles, h);
}

u64 nph_gpu_accel_address(const NphGpuDevice *dev, NphGpuAccel accel)
{
    const NphGpuAccelSlot *slot = accel_slot(dev, accel);
    return slot != NULL ? slot->address : 0U;
}

NphGpuBuffer nph_gpu_accel_buffer(const NphGpuDevice *dev, NphGpuAccel accel)
{
    const NphGpuAccelSlot *slot = accel_slot(dev, accel);
    return slot != NULL ? slot->buffer : NPH_GPU_BUFFER_NULL;
}

void nph_gpu_cmd_build_blas(NphGpuCmd *cmd, NphGpuAccel dst, const NphGpuAccelTriangles *geometries,
                            u32 count, u32 flags, NphGpuBuffer scratch, u64 scratch_offset)
{
    NPH_ASSERT(cmd != NULL && cmd->recording && geometries != NULL);
    NPH_ASSERT(count != 0U && count <= ACCEL_MAX_GEOMETRIES);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuAccelSlot *slot = accel_slot(dev, dst);
    NPH_ASSERT(slot != NULL && slot->type == (u32)NPH_GPU_ACCEL_BOTTOM);
    NPH_ASSERT(address_at(dev, scratch, scratch_offset) %
                   nph_max_u32(dev->info.limits.accel_scratch_alignment, 1U) ==
               0U);
    VkAccelerationStructureGeometryKHR g[ACCEL_MAX_GEOMETRIES];
    VkAccelerationStructureBuildRangeInfoKHR ranges[ACCEL_MAX_GEOMETRIES];
    u32 counts[ACCEL_MAX_GEOMETRIES];
    if (slot == NULL || !triangle_geometries(dev, geometries, count, g, ranges, counts)) {
        return;
    }
    VkAccelerationStructureBuildGeometryInfoKHR info;
    build_info(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, flags, g, count, &info);
    info.dstAccelerationStructure = slot->vk;
    info.scratchData.deviceAddress = address_at(dev, scratch, scratch_offset);
    const VkAccelerationStructureBuildRangeInfoKHR *range_list = ranges;
    vkCmdBuildAccelerationStructuresKHR(cmd->vk, 1U, &info, &range_list);
}

void nph_gpu_cmd_build_tlas(NphGpuCmd *cmd, NphGpuAccel dst, NphGpuBuffer instances, u64 offset,
                            u32 count, u32 flags, NphGpuBuffer scratch, u64 scratch_offset)
{
    NPH_ASSERT(cmd != NULL && cmd->recording && (offset & 15U) == 0U);
    NphGpuDevice *dev = cmd->dev;
    const NphGpuAccelSlot *slot = accel_slot(dev, dst);
    NPH_ASSERT(slot != NULL && slot->type == (u32)NPH_GPU_ACCEL_TOP);
    NPH_ASSERT(address_at(dev, scratch, scratch_offset) %
                   nph_max_u32(dev->info.limits.accel_scratch_alignment, 1U) ==
               0U);
    if (slot == NULL) {
        return;
    }
    VkAccelerationStructureGeometryKHR g;
    instance_geometry(dev, instances, offset, &g);
    VkAccelerationStructureBuildGeometryInfoKHR info;
    build_info(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, flags, &g, 1U, &info);
    info.dstAccelerationStructure = slot->vk;
    info.scratchData.deviceAddress = address_at(dev, scratch, scratch_offset);
    VkAccelerationStructureBuildRangeInfoKHR range;
    memset(&range, 0, sizeof range);
    range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR *range_list = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd->vk, 1U, &info, &range_list);
}
