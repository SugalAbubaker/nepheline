// SPDX-License-Identifier: Apache-2.0
// Shader modules (docs/gpu.md "Shaders and pipelines").
#include <nepheline/platform/file.h>

#include "gpu_internal.h"

static NphResult create_module(NphGpuDevice *dev, const void *spirv, u64 size, VkShaderModule *out)
{
    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)size;
    ci.pCode = (const u32 *)spirv;
    VK_TRY(dev, vkCreateShaderModule(dev->device, &ci, NULL, out));
    return NPH_RESULT_OK;
}

NphResult nph_gpu_shader_create(NphGpuDevice *dev, const void *spirv, u64 size, const char *name,
                                NphGpuShader *out)
{
    NPH_ASSERT(dev != NULL && out != NULL);
    out->value = 0U;
    NphGpuShaderInfo info;
    NPH_TRY(nph_gpu_spirv_reflect(spirv, size, dev->alloc, &info));
    u32 handle = 0U;
    NphGpuShaderSlot *slot = NULL;
    NPH_TRY(gpu_pool_alloc(&dev->shaders, &handle, (void **)&slot));
    const NphResult r = create_module(dev, spirv, size, &slot->vk);
    if (r != NPH_RESULT_OK) {
        gpu_pool_release(&dev->shaders, handle);
        return r;
    }
    slot->info = info;
    slot->generation = 1U;
    gpu_copy_name(slot->name, name, "shader", handle);
    gpu_set_name(dev, VK_OBJECT_TYPE_SHADER_MODULE, (u64)(uptr)slot->vk, slot->name);
    out->value = handle;
    return NPH_RESULT_OK;
}

NphResult nph_gpu_shader_load(NphGpuDevice *dev, const char *path, const char *name,
                              NphGpuShader *out)
{
    NPH_ASSERT(dev != NULL && path != NULL && out != NULL);
    out->value = 0U;
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, dev->alloc, &data, &size));
    const NphResult r = nph_gpu_shader_create(dev, data, size, name != NULL ? name : path, out);
    nph_file_free_all(dev->alloc, data, size);
    if (r != NPH_RESULT_OK) {
        GPU_LOG_ERROR("shader '%s': %s", path, nph_result_string(r));
    }
    return r;
}

NphResult nph_gpu_shader_reload(NphGpuDevice *dev, NphGpuShader shader, const void *spirv, u64 size)
{
    NPH_ASSERT(dev != NULL);
    NphGpuShaderSlot *slot = gpu_shader(dev, shader);
    if (slot == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuShaderInfo info;
    NPH_TRY(nph_gpu_spirv_reflect(spirv, size, dev->alloc, &info));
    if (info.stage != slot->info.stage) {
        GPU_LOG_ERROR("shader '%s': reload changes the stage from %s to %s", slot->name,
                      nph_gpu_shader_stage_name(slot->info.stage),
                      nph_gpu_shader_stage_name(info.stage));
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkShaderModule module = VK_NULL_HANDLE;
    NPH_TRY(create_module(dev, spirv, size, &module));
    gpu_defer_handle(dev, NPH_GPU_DEFERRED_SHADER_MODULE, slot->vk);
    slot->vk = module;
    slot->info = info;
    ++slot->generation;
    gpu_set_name(dev, VK_OBJECT_TYPE_SHADER_MODULE, (u64)(uptr)slot->vk, slot->name);
    return NPH_RESULT_OK;
}

void nph_gpu_shader_destroy(NphGpuDevice *dev, NphGpuShader shader)
{
    NPH_ASSERT(dev != NULL);
    if (shader.value == 0U) {
        return;
    }
    NphGpuShaderSlot *slot = gpu_shader(dev, shader);
    if (slot == NULL) {
        return;
    }
    gpu_defer_handle(dev, NPH_GPU_DEFERRED_SHADER_MODULE, slot->vk);
    slot->vk = VK_NULL_HANDLE;
    gpu_pool_release(&dev->shaders, shader.value);
}

bool nph_gpu_shader_is_valid(const NphGpuDevice *dev, NphGpuShader shader)
{
    const NphHandle h = {shader.value};
    return nph_handle_pool_is_valid(&dev->shaders.handles, h);
}

const NphGpuShaderInfo *nph_gpu_shader_info(const NphGpuDevice *dev, NphGpuShader shader)
{
    const NphGpuShaderSlot *slot = gpu_shader(dev, shader);
    return slot != NULL ? &slot->info : NULL;
}

u32 nph_gpu_shader_generation(const NphGpuDevice *dev, NphGpuShader shader)
{
    const NphGpuShaderSlot *slot = gpu_shader(dev, shader);
    return slot != NULL ? slot->generation : 0U;
}

VkShaderModule nph_gpu_vk_shader_module(const NphGpuDevice *dev, NphGpuShader shader)
{
    const NphGpuShaderSlot *slot = gpu_shader(dev, shader);
    return slot != NULL ? slot->vk : VK_NULL_HANDLE;
}
