// SPDX-License-Identifier: Apache-2.0
// Graphics and compute pipelines, and the pipeline cache persisted to disk
// (docs/gpu.md "Shaders and pipelines").
#include <nepheline/core/hash.h>
#include <nepheline/platform/file.h>

#include "gpu_internal.h"

#include <stdio.h>

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

static VkPrimitiveTopology vk_topology(NphGpuTopology t)
{
    switch (t) {
    case NPH_GPU_TOPOLOGY_TRIANGLE_LIST:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NPH_GPU_TOPOLOGY_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NPH_GPU_TOPOLOGY_LINE_LIST:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NPH_GPU_TOPOLOGY_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NPH_GPU_TOPOLOGY_POINT_LIST:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static VkBlendFactor vk_blend_factor(NphGpuBlendFactor f)
{
    switch (f) {
    case NPH_GPU_BLEND_ZERO:
        return VK_BLEND_FACTOR_ZERO;
    case NPH_GPU_BLEND_ONE:
        return VK_BLEND_FACTOR_ONE;
    case NPH_GPU_BLEND_SRC_COLOR:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case NPH_GPU_BLEND_ONE_MINUS_SRC_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NPH_GPU_BLEND_DST_COLOR:
        return VK_BLEND_FACTOR_DST_COLOR;
    case NPH_GPU_BLEND_ONE_MINUS_DST_COLOR:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NPH_GPU_BLEND_SRC_ALPHA:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NPH_GPU_BLEND_DST_ALPHA:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case NPH_GPU_BLEND_ONE_MINUS_DST_ALPHA:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

static VkBlendOp vk_blend_op(NphGpuBlendOp op)
{
    switch (op) {
    case NPH_GPU_BLEND_OP_ADD:
        return VK_BLEND_OP_ADD;
    case NPH_GPU_BLEND_OP_SUBTRACT:
        return VK_BLEND_OP_SUBTRACT;
    case NPH_GPU_BLEND_OP_REVERSE_SUBTRACT:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NPH_GPU_BLEND_OP_MIN:
        return VK_BLEND_OP_MIN;
    case NPH_GPU_BLEND_OP_MAX:
        return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

static NphResult stage_info(NphGpuDevice *dev, NphGpuShader shader, NphGpuShaderStage expected,
                            VkPipelineShaderStageCreateInfo *out, u32 *push_bytes)
{
    const NphGpuShaderSlot *slot = gpu_shader(dev, shader);
    if (slot == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (slot->info.stage != expected) {
        GPU_LOG_ERROR("shader '%s' is a %s shader, expected %s", slot->name,
                      nph_gpu_shader_stage_name(slot->info.stage),
                      nph_gpu_shader_stage_name(expected));
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    out->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    out->stage = gpu_vk_shader_stage(expected);
    out->module = slot->vk;
    out->pName = slot->info.entry_point;
    if (slot->info.push_constant_bytes > *push_bytes) {
        *push_bytes = slot->info.push_constant_bytes;
    }
    return NPH_RESULT_OK;
}

static NphResult build_graphics(NphGpuDevice *dev, const NphGpuGraphicsPipelineDesc *d,
                                const char *name, VkPipeline *out, u32 *out_push_bytes)
{
    const bool mesh = !nph_gpu_shader_is_null(d->mesh);
    const bool vertex = !nph_gpu_shader_is_null(d->vertex);
    if (mesh == vertex) {
        GPU_LOG_ERROR("pipeline '%s': exactly one of vertex or mesh must be set", name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (mesh && !dev->info.features.mesh_shader) {
        GPU_LOG_ERROR("pipeline '%s': mesh shaders are not supported by this device", name);
        return NPH_RESULT_UNSUPPORTED;
    }
    if (d->polygon_mode == NPH_GPU_POLYGON_LINE && !dev->info.features.fill_mode_non_solid) {
        GPU_LOG_ERROR("pipeline '%s': line polygons are not supported by this device", name);
        return NPH_RESULT_UNSUPPORTED;
    }
    if (d->color_count > NPH_GPU_MAX_COLOR_ATTACHMENTS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkPipelineShaderStageCreateInfo stages[4];
    u32 stage_count = 0U;
    u32 push_bytes = 0U;
    if (mesh) {
        if (!nph_gpu_shader_is_null(d->task)) {
            NPH_TRY(
                stage_info(dev, d->task, NPH_GPU_SHADER_TASK, &stages[stage_count++], &push_bytes));
        }
        NPH_TRY(stage_info(dev, d->mesh, NPH_GPU_SHADER_MESH, &stages[stage_count++], &push_bytes));
    } else {
        NPH_TRY(
            stage_info(dev, d->vertex, NPH_GPU_SHADER_VERTEX, &stages[stage_count++], &push_bytes));
    }
    if (!nph_gpu_shader_is_null(d->fragment)) {
        NPH_TRY(stage_info(dev, d->fragment, NPH_GPU_SHADER_FRAGMENT, &stages[stage_count++],
                           &push_bytes));
    }

    VkFormat color_formats[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    VkPipelineColorBlendAttachmentState blends[NPH_GPU_MAX_COLOR_ATTACHMENTS];
    for (u32 i = 0; i < d->color_count; ++i) {
        color_formats[i] = gpu_vk_format(d->color_formats[i]);
        const NphGpuBlendState *b = &d->blend[i];
        memset(&blends[i], 0, sizeof blends[i]);
        blends[i].blendEnable = b->enable ? VK_TRUE : VK_FALSE;
        blends[i].srcColorBlendFactor = vk_blend_factor(b->src_color);
        blends[i].dstColorBlendFactor = vk_blend_factor(b->dst_color);
        blends[i].colorBlendOp = vk_blend_op(b->color_op);
        blends[i].srcAlphaBlendFactor = vk_blend_factor(b->src_alpha);
        blends[i].dstAlphaBlendFactor = vk_blend_factor(b->dst_alpha);
        blends[i].alphaBlendOp = vk_blend_op(b->alpha_op);
        blends[i].colorWriteMask = b->write_mask == 0U ? 0xFU : (b->write_mask & 0xFU);
    }
    const NphGpuFormatInfo *depth_info = nph_gpu_format_info(d->depth_format);

    VkPipelineRenderingCreateInfo rendering;
    memset(&rendering, 0, sizeof rendering);
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = d->color_count;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = gpu_vk_format(d->depth_format);
    rendering.stencilAttachmentFormat = (depth_info->aspects & (u32)NPH_GPU_ASPECT_STENCIL) != 0U
                                            ? gpu_vk_format(d->depth_format)
                                            : VK_FORMAT_UNDEFINED;

    VkPipelineVertexInputStateCreateInfo vertex_input;
    memset(&vertex_input, 0, sizeof vertex_input);
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly;
    memset(&assembly, 0, sizeof assembly);
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = vk_topology(d->topology);

    VkPipelineViewportStateCreateInfo viewport;
    memset(&viewport, 0, sizeof viewport);
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1U;
    viewport.scissorCount = 1U;

    VkPipelineRasterizationStateCreateInfo raster;
    memset(&raster, 0, sizeof raster);
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable =
        (d->depth.clamp && dev->info.features.depth_clamp) ? VK_TRUE : VK_FALSE;
    raster.polygonMode =
        d->polygon_mode == NPH_GPU_POLYGON_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = d->cull_mode == NPH_GPU_CULL_BACK    ? VK_CULL_MODE_BACK_BIT
                      : d->cull_mode == NPH_GPU_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT
                                                           : VK_CULL_MODE_NONE;
    raster.frontFace = d->front_face == NPH_GPU_FRONT_FACE_CW ? VK_FRONT_FACE_CLOCKWISE
                                                              : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = d->depth.bias_enable ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = d->depth.bias_constant;
    raster.depthBiasClamp = d->depth.bias_clamp;
    raster.depthBiasSlopeFactor = d->depth.bias_slope;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample;
    memset(&multisample, 0, sizeof multisample);
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = (VkSampleCountFlagBits)(d->samples == 0U ? 1U : d->samples);

    VkPipelineDepthStencilStateCreateInfo depth;
    memset(&depth, 0, sizeof depth);
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = d->depth.test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = d->depth.write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = gpu_vk_compare(d->depth.compare);

    VkPipelineColorBlendStateCreateInfo blend;
    memset(&blend, 0, sizeof blend);
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = d->color_count;
    blend.pAttachments = blends;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic;
    memset(&dynamic, 0, sizeof dynamic);
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2U;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering;
    ci.stageCount = stage_count;
    ci.pStages = stages;
    ci.pVertexInputState = mesh ? NULL : &vertex_input;
    ci.pInputAssemblyState = mesh ? NULL : &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = d->depth_format != NPH_GPU_FORMAT_UNDEFINED ? &depth : NULL;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = dev->bindless.pipeline_layout;
    const VkResult r =
        vkCreateGraphicsPipelines(dev->device, dev->pipeline_cache, 1U, &ci, NULL, out);
    if (r != VK_SUCCESS) {
        GPU_LOG_ERROR("pipeline '%s': vkCreateGraphicsPipelines failed: %s", name,
                      nph_gpu_vk_result_name(r));
        return nph_gpu_vk_result(r, NULL) == NPH_RESULT_OUT_OF_MEMORY ? NPH_RESULT_OUT_OF_MEMORY
                                                                      : NPH_RESULT_UNKNOWN;
    }
    *out_push_bytes = push_bytes;
    return NPH_RESULT_OK;
}

static NphResult build_compute(NphGpuDevice *dev, const NphGpuComputePipelineDesc *d,
                               const char *name, VkPipeline *out, u32 *out_push_bytes)
{
    u32 push_bytes = 0U;
    VkComputePipelineCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    NPH_TRY(stage_info(dev, d->shader, NPH_GPU_SHADER_COMPUTE, &ci.stage, &push_bytes));
    ci.layout = dev->bindless.pipeline_layout;
    const VkResult r =
        vkCreateComputePipelines(dev->device, dev->pipeline_cache, 1U, &ci, NULL, out);
    if (r != VK_SUCCESS) {
        GPU_LOG_ERROR("pipeline '%s': vkCreateComputePipelines failed: %s", name,
                      nph_gpu_vk_result_name(r));
        return nph_gpu_vk_result(r, NULL) == NPH_RESULT_OUT_OF_MEMORY ? NPH_RESULT_OUT_OF_MEMORY
                                                                      : NPH_RESULT_UNKNOWN;
    }
    *out_push_bytes = push_bytes;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_pipeline_create_graphics(NphGpuDevice *dev,
                                           const NphGpuGraphicsPipelineDesc *desc,
                                           NphGpuPipeline *out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    out->value = 0U;
    const char *name = desc->name != NULL ? desc->name : "graphics pipeline";
    VkPipeline vk = VK_NULL_HANDLE;
    u32 push_bytes = 0U;
    NPH_TRY(build_graphics(dev, desc, name, &vk, &push_bytes));
    u32 handle = 0U;
    NphGpuPipelineSlot *slot = NULL;
    const NphResult r = gpu_pool_alloc(&dev->pipelines, &handle, (void **)&slot);
    if (r != NPH_RESULT_OK) {
        vkDestroyPipeline(dev->device, vk, NULL);
        return r;
    }
    slot->vk = vk;
    slot->compute = 0;
    slot->gdesc = *desc;
    slot->push_bytes = push_bytes;
    slot->generation = 1U;
    gpu_copy_name(slot->name, desc->name, "graphics pipeline", handle);
    slot->gdesc.name = slot->name;
    gpu_set_name(dev, VK_OBJECT_TYPE_PIPELINE, (u64)(uptr)vk, slot->name);
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_pipeline_create_compute(NphGpuDevice *dev, const NphGpuComputePipelineDesc *desc,
                                          NphGpuPipeline *out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    out->value = 0U;
    const char *name = desc->name != NULL ? desc->name : "compute pipeline";
    VkPipeline vk = VK_NULL_HANDLE;
    u32 push_bytes = 0U;
    NPH_TRY(build_compute(dev, desc, name, &vk, &push_bytes));
    u32 handle = 0U;
    NphGpuPipelineSlot *slot = NULL;
    const NphResult r = gpu_pool_alloc(&dev->pipelines, &handle, (void **)&slot);
    if (r != NPH_RESULT_OK) {
        vkDestroyPipeline(dev->device, vk, NULL);
        return r;
    }
    slot->vk = vk;
    slot->compute = 1;
    slot->cdesc = *desc;
    slot->push_bytes = push_bytes;
    slot->generation = 1U;
    gpu_copy_name(slot->name, desc->name, "compute pipeline", handle);
    slot->cdesc.name = slot->name;
    gpu_set_name(dev, VK_OBJECT_TYPE_PIPELINE, (u64)(uptr)vk, slot->name);
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_pipeline_reload(NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    NPH_ASSERT(dev != NULL);
    NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    if (slot == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkPipeline vk = VK_NULL_HANDLE;
    u32 push_bytes = 0U;
    if (slot->compute) {
        NPH_TRY(build_compute(dev, &slot->cdesc, slot->name, &vk, &push_bytes));
    } else {
        NPH_TRY(build_graphics(dev, &slot->gdesc, slot->name, &vk, &push_bytes));
    }
    gpu_defer_handle(dev, NPH_GPU_DEFERRED_PIPELINE, (u64)(uptr)slot->vk);
    slot->vk = vk;
    slot->push_bytes = push_bytes;
    ++slot->generation;
    gpu_set_name(dev, VK_OBJECT_TYPE_PIPELINE, (u64)(uptr)vk, slot->name);
    return NPH_RESULT_OK;
}

void nph_gpu_pipeline_destroy(NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    NPH_ASSERT(dev != NULL);
    if (pipeline.value == 0U) {
        return;
    }
    NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    if (slot == NULL) {
        return;
    }
    gpu_defer_handle(dev, NPH_GPU_DEFERRED_PIPELINE, (u64)(uptr)slot->vk);
    slot->vk = VK_NULL_HANDLE;
    gpu_pool_release(&dev->pipelines, pipeline.value);
}

bool nph_gpu_pipeline_is_valid(const NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    const NphHandle h = {pipeline.value};
    return nph_handle_pool_is_valid(&dev->pipelines.handles, h);
}

bool nph_gpu_pipeline_is_compute(const NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    const NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    return slot != NULL && slot->compute;
}

u32 nph_gpu_pipeline_push_constant_bytes(const NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    const NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    return slot != NULL ? slot->push_bytes : 0U;
}

u32 nph_gpu_pipeline_generation(const NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    const NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    return slot != NULL ? slot->generation : 0U;
}

VkPipeline nph_gpu_vk_pipeline(const NphGpuDevice *dev, NphGpuPipeline pipeline)
{
    const NphGpuPipelineSlot *slot = gpu_pipeline(dev, pipeline);
    return slot != NULL ? slot->vk : VK_NULL_HANDLE;
}

void gpu_pipelines_shutdown(NphGpuDevice *dev)
{
    u32 i = 0U;
    void *slot = NULL;
    while (gpu_pool_next_live(&dev->pipelines, &i, &slot)) {
        NphGpuPipelineSlot *p = (NphGpuPipelineSlot *)slot;
        if (p->vk != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev->device, p->vk, NULL);
            p->vk = VK_NULL_HANDLE;
        }
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->shaders, &i, &slot)) {
        NphGpuShaderSlot *s = (NphGpuShaderSlot *)slot;
        if (s->vk != VK_NULL_HANDLE) {
            vkDestroyShaderModule(dev->device, s->vk, NULL);
            s->vk = VK_NULL_HANDLE;
        }
    }
}

// ---------------------------------------------------------------------------
// Pipeline cache file: a small header of ours (magic, version, size,
// CRC32C) in front of the driver's blob, whose own header
// (VkPipelineCacheHeaderVersionOne) is checked against the device before
// the driver sees it.
// ---------------------------------------------------------------------------

#define CACHE_MAGIC   0x4348504EU // "NPHC"
#define CACHE_VERSION 1U

typedef struct CacheFileHeader {
    u32 magic;
    u32 version;
    u64 data_size;
    u32 crc32c;
    u32 reserved;
} CacheFileHeader;

NPH_STATIC_ASSERT(sizeof(CacheFileHeader) == 24U, "cache header layout");

static bool cache_blob_matches_device(const NphGpuDevice *dev, const void *data, u64 size)
{
    if (size < sizeof(VkPipelineCacheHeaderVersionOne)) {
        return false;
    }
    VkPipelineCacheHeaderVersionOne h;
    memcpy(&h, data, sizeof h);
    if (h.headerSize < sizeof(VkPipelineCacheHeaderVersionOne) ||
        h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        h.vendorID != dev->props.vendorID || h.deviceID != dev->props.deviceID ||
        memcmp(h.pipelineCacheUUID, dev->props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        return false;
    }
    return true;
}

NphResult gpu_pipeline_cache_init(NphGpuDevice *dev)
{
    void *initial = NULL;
    u64 initial_size = 0U;
    void *file = NULL;
    u64 file_size = 0U;
    if (dev->desc.pipeline_cache_path != NULL) {
        const u64 len = strlen(dev->desc.pipeline_cache_path);
        dev->pipeline_cache_path = (char *)NPH_ALLOC(dev->alloc, len + 1U, 1U);
        if (dev->pipeline_cache_path == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        memcpy(dev->pipeline_cache_path, dev->desc.pipeline_cache_path, len + 1U);
        dev->pipeline_cache_path_len = len;
        if (nph_file_read_all(dev->pipeline_cache_path, dev->alloc, &file, &file_size) ==
            NPH_RESULT_OK) {
            CacheFileHeader h;
            if (file_size >= sizeof h) {
                memcpy(&h, file, sizeof h);
                const u8 *blob = (const u8 *)file + sizeof h;
                if (h.magic == CACHE_MAGIC && h.version == CACHE_VERSION &&
                    h.data_size == file_size - sizeof h &&
                    nph_crc32c(blob, h.data_size, 0U) == h.crc32c &&
                    cache_blob_matches_device(dev, blob, h.data_size)) {
                    initial = (void *)blob; // NOLINT(clang-diagnostic-cast-qual)
                    initial_size = h.data_size;
                } else {
                    GPU_LOG_WARN("pipeline cache '%s' is stale or corrupt; starting empty",
                                 dev->pipeline_cache_path);
                }
            }
        }
    }
    VkPipelineCacheCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = (size_t)initial_size;
    ci.pInitialData = initial;
    VkResult r = vkCreatePipelineCache(dev->device, &ci, NULL, &dev->pipeline_cache);
    if (r != VK_SUCCESS && initial != NULL) {
        GPU_LOG_WARN("the driver rejected the pipeline cache data; starting empty");
        ci.initialDataSize = 0U;
        ci.pInitialData = NULL;
        initial = NULL;
        r = vkCreatePipelineCache(dev->device, &ci, NULL, &dev->pipeline_cache);
    }
    if (file != NULL) {
        nph_file_free_all(dev->alloc, file, file_size);
    }
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "vkCreatePipelineCache");
    }
    dev->pipeline_cache_loaded = initial != NULL;
    gpu_set_name(dev, VK_OBJECT_TYPE_PIPELINE_CACHE, (u64)(uptr)dev->pipeline_cache,
                 "pipeline cache");
    if (dev->pipeline_cache_loaded) {
        GPU_LOG_INFO("pipeline cache primed from '%s' (%llu bytes)", dev->pipeline_cache_path,
                     (unsigned long long)initial_size);
    }
    return NPH_RESULT_OK;
}

NphResult nph_gpu_pipeline_cache_save(NphGpuDevice *dev)
{
    NPH_ASSERT(dev != NULL);
    if (dev->pipeline_cache_path == NULL || dev->pipeline_cache == VK_NULL_HANDLE) {
        return NPH_RESULT_UNSUPPORTED;
    }
    size_t size = 0U;
    VK_TRY(dev, vkGetPipelineCacheData(dev->device, dev->pipeline_cache, &size, NULL));
    if (size == 0U) {
        return NPH_RESULT_OK;
    }
    const u64 total = sizeof(CacheFileHeader) + size;
    u8 *buffer = (u8 *)NPH_ALLOC(dev->alloc, total, 16U);
    if (buffer == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u8 *blob = buffer + sizeof(CacheFileHeader);
    const VkResult r = vkGetPipelineCacheData(dev->device, dev->pipeline_cache, &size, blob);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        NPH_FREE(dev->alloc, buffer, total, 16U);
        return gpu_vk_fail(dev, r, "vkGetPipelineCacheData");
    }
    CacheFileHeader h;
    memset(&h, 0, sizeof h);
    h.magic = CACHE_MAGIC;
    h.version = CACHE_VERSION;
    h.data_size = size;
    h.crc32c = nph_crc32c(blob, size, 0U);
    memcpy(buffer, &h, sizeof h);

    // Write beside the target, then rename, so a crash never leaves a torn file.
    const u64 tmp_len = dev->pipeline_cache_path_len + 5U;
    char *tmp = (char *)NPH_ALLOC(dev->alloc, tmp_len, 1U);
    NphResult result = NPH_RESULT_OUT_OF_MEMORY;
    if (tmp != NULL) {
        snprintf(tmp, tmp_len, "%s.tmp", dev->pipeline_cache_path);
        result = nph_file_write_all(tmp, buffer, sizeof h + size);
        if (result == NPH_RESULT_OK) {
            result = nph_fs_rename(tmp, dev->pipeline_cache_path);
        }
        if (result != NPH_RESULT_OK) {
            GPU_LOG_WARN("pipeline cache '%s' could not be written (%s)", dev->pipeline_cache_path,
                         nph_result_string(result));
            (void)nph_fs_remove_file(tmp);
        } else {
            GPU_LOG_DEBUG("pipeline cache saved to '%s' (%llu bytes)", dev->pipeline_cache_path,
                          (unsigned long long)size);
        }
        NPH_FREE(dev->alloc, tmp, tmp_len, 1U);
    }
    NPH_FREE(dev->alloc, buffer, total, 16U);
    return result;
}

void gpu_pipeline_cache_shutdown(NphGpuDevice *dev)
{
    if (dev->pipeline_cache != VK_NULL_HANDLE) {
        if (dev->pipeline_cache_path != NULL) {
            (void)nph_gpu_pipeline_cache_save(dev);
        }
        vkDestroyPipelineCache(dev->device, dev->pipeline_cache, NULL);
        dev->pipeline_cache = VK_NULL_HANDLE;
    }
    if (dev->pipeline_cache_path != NULL) {
        NPH_FREE(dev->alloc, dev->pipeline_cache_path, dev->pipeline_cache_path_len + 1U, 1U);
        dev->pipeline_cache_path = NULL;
    }
}

bool nph_gpu_pipeline_cache_loaded(const NphGpuDevice *dev)
{
    return dev->pipeline_cache_loaded != 0;
}

u64 nph_gpu_pipeline_cache_size(const NphGpuDevice *dev)
{
    size_t size = 0U;
    if (dev->pipeline_cache == VK_NULL_HANDLE ||
        vkGetPipelineCacheData(dev->device, dev->pipeline_cache, &size, NULL) != VK_SUCCESS) {
        return 0U;
    }
    return (u64)size;
}
