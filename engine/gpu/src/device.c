// SPDX-License-Identifier: Apache-2.0
// Instance, physical device selection, logical device, queues, timelines,
// submission, and device lifetime (docs/gpu.md, docs/adr/0014-gpu-module.md).
#include <nepheline/core/version.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/window.h>

#include "gpu_internal.h"

#include <stdio.h>

#define GPU_API_VERSION VK_API_VERSION_1_3

static const char *const g_validation_layer = "VK_LAYER_KHRONOS_validation";

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

static void fill_defaults(NphGpuDeviceDesc *d)
{
    if (d->app_name == NULL) {
        d->app_name = "Nepheline";
    }
    if (d->max_buffers == 0U) {
        d->max_buffers = 65536U;
    }
    if (d->max_images == 0U) {
        d->max_images = 16384U;
    }
    if (d->max_samplers == 0U) {
        d->max_samplers = 1024U;
    }
    if (d->max_shaders == 0U) {
        d->max_shaders = 4096U;
    }
    if (d->max_pipelines == 0U) {
        d->max_pipelines = 4096U;
    }
    if (d->max_accels == 0U) {
        d->max_accels = 4096U;
    }
    if (d->max_timelines == 0U) {
        d->max_timelines = 64U;
    }
    if (d->max_command_threads == 0U) {
        d->max_command_threads = 64U;
    }
    if (d->max_commands_per_thread == 0U) {
        d->max_commands_per_thread = 32U;
    }
    if (d->timestamp_queries_per_frame == 0U) {
        d->timestamp_queries_per_frame = 1024U;
    }
    if (d->accel_queries_per_frame == 0U) {
        d->accel_queries_per_frame = 4096U;
    }
    if (d->bindless_sampled_images == 0U) {
        d->bindless_sampled_images = 65536U;
    }
    if (d->bindless_storage_images == 0U) {
        d->bindless_storage_images = 16384U;
    }
    if (d->bindless_samplers == 0U) {
        d->bindless_samplers = 1024U;
    }
    if (d->staging_ring_bytes == 0U) {
        d->staging_ring_bytes = NPH_MIB(64);
    }
    if (d->deferred_destroy_capacity == 0U) {
        d->deferred_destroy_capacity = 4096U;
    }
    // The queues' timelines and the sparse bindings' are the device's own;
    // one more leaves room for a caller's.
    if (d->max_timelines < NPH_GPU_MAX_QUEUES + 2U) {
        d->max_timelines = NPH_GPU_MAX_QUEUES + 2U;
    }
}

// ---------------------------------------------------------------------------
// Validation messenger
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    NphGpuDevice *dev = (NphGpuDevice *)user;
    const char *id = data->pMessageIdName != NULL ? data->pMessageIdName : "?";
    const char *msg = data->pMessage != NULL ? data->pMessage : "";
    // GENERAL messages below error level are the loader's and the layer's
    // status chatter (layer search paths, "validation layer active"), not
    // findings about the application: trace them, count nothing.
    const bool finding = (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0U &&
                         strstr(id, "-status-message") == NULL;
    if (!finding && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) == 0U) {
        NPH_LOG_TRACE(&NPH_LOG_GPU, "vulkan: [%s] %s", id, msg);
        return VK_FALSE;
    }
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
        nph_atomic_fetch_add_u64(&dev->val_errors, 1U, NPH_MEMORY_ORDER_RELAXED);
        // Validation messages run to a kilobyte; the log record keeps the
        // head and stderr gets the whole text, which is what a developer
        // reading a failing test needs.
        GPU_LOG_ERROR("validation: [%s] %s", id, msg);
        fprintf(stderr, "vulkan validation error [%s]:\n%s\n", id, msg);
        if (dev->desc.break_on_validation_error && nph_platform_debugger_present()) {
            NPH_DEBUG_BREAK();
        }
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
        if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0U) {
            nph_atomic_fetch_add_u64(&dev->val_perf, 1U, NPH_MEMORY_ORDER_RELAXED);
        } else {
            nph_atomic_fetch_add_u64(&dev->val_warnings, 1U, NPH_MEMORY_ORDER_RELAXED);
        }
        GPU_LOG_WARN("validation: [%s] %s", id, msg);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0U) {
        nph_atomic_fetch_add_u64(&dev->val_infos, 1U, NPH_MEMORY_ORDER_RELAXED);
        GPU_LOG_DEBUG("validation: [%s] %s", id, msg);
    } else {
        NPH_LOG_TRACE(&NPH_LOG_GPU, "validation: [%s] %s", id, msg);
    }
    return VK_FALSE;
}

static void fill_messenger_info(NphGpuDevice *dev, VkDebugUtilsMessengerCreateInfoEXT *info)
{
    memset(info, 0, sizeof *info);
    info->sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    info->messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info->pfnUserCallback = debug_callback;
    info->pUserData = dev;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

static bool has_extension(const VkExtensionProperties *exts, u32 count, const char *name)
{
    for (u32 i = 0; i < count; ++i) {
        if (strcmp(exts[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

typedef struct InstanceSetup {
    const char *extensions[16];
    u32 extension_count;
    const char *layers[1];
    u32 layer_count;
    b32 validation;
    b32 sync_validation;
    b32 debug_utils;
} InstanceSetup;

static NphResult create_instance(NphGpuDevice *dev, bool want_validation, bool want_sync)
{
    NphAllocator *alloc = dev->alloc;
    u32 version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != NULL) {
        (void)vkEnumerateInstanceVersion(&version);
    }
    if (version < GPU_API_VERSION) {
        GPU_LOG_ERROR("the Vulkan loader supports %u.%u, Nepheline needs 1.3",
                      VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
        return NPH_RESULT_UNSUPPORTED;
    }

    InstanceSetup setup;
    memset(&setup, 0, sizeof setup);

    // Layers
    u32 layer_count = 0U;
    (void)vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    VkLayerProperties *layers = NULL;
    if (layer_count != 0U) {
        layers = NPH_ALLOC_ARRAY(alloc, VkLayerProperties, layer_count);
        if (layers == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        (void)vkEnumerateInstanceLayerProperties(&layer_count, layers);
    }
    bool has_validation = false;
    for (u32 i = 0; i < layer_count; ++i) {
        if (strcmp(layers[i].layerName, g_validation_layer) == 0) {
            has_validation = true;
        }
    }
    if (layers != NULL) {
        NPH_FREE_ARRAY(alloc, VkLayerProperties, layers, layer_count);
    }
    if (want_validation && !has_validation) {
        GPU_LOG_WARN("validation requested but %s is not installed; continuing without it",
                     g_validation_layer);
    }
    setup.validation = want_validation && has_validation;
    if (setup.validation) {
        setup.layers[setup.layer_count++] = g_validation_layer;
    }

    // Extensions: the loader's plus the validation layer's.
    u32 ext_count = 0U;
    (void)vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL);
    u32 layer_ext_count = 0U;
    if (setup.validation) {
        (void)vkEnumerateInstanceExtensionProperties(g_validation_layer, &layer_ext_count, NULL);
    }
    const u32 total = ext_count + layer_ext_count;
    VkExtensionProperties *exts = NULL;
    if (total != 0U) {
        exts = NPH_ALLOC_ARRAY(alloc, VkExtensionProperties, total);
        if (exts == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        (void)vkEnumerateInstanceExtensionProperties(NULL, &ext_count, exts);
        if (layer_ext_count != 0U) {
            (void)vkEnumerateInstanceExtensionProperties(g_validation_layer, &layer_ext_count,
                                                         exts + ext_count);
        }
    }

    NphResult result = NPH_RESULT_OK;
    if (!dev->desc.headless) {
        // Function-pointer to function-pointer through the universal
        // void (*)(void) type, as the window layer's own typedef is.
        nph_window_vulkan_set_loader(
            (NphVulkanGetInstanceProcAddrFn)(NphVulkanProcFn)vkGetInstanceProcAddr);
        const char *const *names = NULL;
        u32 n = 0U;
        if (nph_window_vulkan_extensions(&names, &n) != NPH_RESULT_OK) {
            GPU_LOG_ERROR("the window system is not initialised or has no Vulkan support; "
                          "create the device headless or initialise the window system first");
            result = NPH_RESULT_UNSUPPORTED;
        } else {
            for (u32 i = 0; i < n && result == NPH_RESULT_OK; ++i) {
                if (!has_extension(exts, total, names[i])) {
                    GPU_LOG_ERROR("instance extension %s is required for windows but missing",
                                  names[i]);
                    result = NPH_RESULT_UNSUPPORTED;
                } else if (setup.extension_count < NPH_ARRAY_COUNT(setup.extensions)) {
                    setup.extensions[setup.extension_count++] = names[i];
                }
            }
        }
    }
    if (result == NPH_RESULT_OK) {
        if (has_extension(exts, total, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            setup.extensions[setup.extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            setup.debug_utils = 1;
        }
        if (setup.validation && want_sync &&
            has_extension(exts, total, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            setup.extensions[setup.extension_count++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
            setup.sync_validation = 1;
        } else if (want_sync && setup.validation) {
            GPU_LOG_WARN("the validation layer lacks VK_EXT_validation_features; "
                         "synchronisation validation is off");
        }
    }
    if (exts != NULL) {
        NPH_FREE_ARRAY(alloc, VkExtensionProperties, exts, total);
    }
    if (result != NPH_RESULT_OK) {
        return result;
    }

    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = dev->desc.app_name;
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "Nepheline";
    app.engineVersion =
        VK_MAKE_API_VERSION(0, NPH_VERSION_MAJOR, NPH_VERSION_MINOR, NPH_VERSION_PATCH);
    app.apiVersion = GPU_API_VERSION;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = setup.layer_count;
    ci.ppEnabledLayerNames = setup.layers;
    ci.enabledExtensionCount = setup.extension_count;
    ci.ppEnabledExtensionNames = setup.extensions;

    const void **next = &ci.pNext;
    VkDebugUtilsMessengerCreateInfoEXT messenger_info;
    if (setup.debug_utils) {
        fill_messenger_info(dev, &messenger_info);
        *next = &messenger_info;
        next = &messenger_info.pNext;
    }
    VkValidationFeaturesEXT features;
    const VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    if (setup.sync_validation) {
        memset(&features, 0, sizeof features);
        features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        features.enabledValidationFeatureCount = NPH_ARRAY_COUNT(enables);
        features.pEnabledValidationFeatures = enables;
        *next = &features;
        next = &features.pNext;
    }
    NPH_UNUSED(next);

    const VkResult r = vkCreateInstance(&ci, NULL, &dev->instance);
    if (r != VK_SUCCESS) {
        GPU_LOG_ERROR("vkCreateInstance failed: %s", nph_gpu_vk_result_name(r));
        dev->instance = VK_NULL_HANDLE;
        return nph_gpu_vk_result(r, NULL) == NPH_RESULT_OUT_OF_MEMORY ? NPH_RESULT_OUT_OF_MEMORY
                                                                      : NPH_RESULT_UNSUPPORTED;
    }
    volkLoadInstanceOnly(dev->instance);

    dev->has_debug_utils = setup.debug_utils;
    dev->info.validation = setup.validation;
    dev->info.sync_validation = setup.sync_validation;
    dev->info.debug_utils = setup.debug_utils;
    if (setup.debug_utils) {
        fill_messenger_info(dev, &messenger_info);
        const VkResult mr =
            vkCreateDebugUtilsMessengerEXT(dev->instance, &messenger_info, NULL, &dev->messenger);
        if (mr != VK_SUCCESS) {
            GPU_LOG_WARN("vkCreateDebugUtilsMessengerEXT failed: %s", nph_gpu_vk_result_name(mr));
            dev->messenger = VK_NULL_HANDLE;
        }
    }
    GPU_LOG_INFO("instance created: loader %u.%u.%u, validation %s, sync validation %s, "
                 "debug utils %s",
                 VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                 VK_API_VERSION_PATCH(version), setup.validation ? "on" : "off",
                 setup.sync_validation ? "on" : "off", setup.debug_utils ? "on" : "off");
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Physical device selection
// ---------------------------------------------------------------------------

typedef struct Candidate {
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties2 props;
    VkPhysicalDeviceVulkan11Properties p11;
    VkPhysicalDeviceVulkan12Properties p12;
    VkPhysicalDeviceVulkan13Properties p13;
    VkPhysicalDeviceDriverProperties driver;
    VkPhysicalDeviceFeatures2 features;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel;
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT atomic64;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executables;
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_props;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accel_props;
    VkPhysicalDeviceMemoryProperties memory;
    b32 ext_swapchain;
    b32 ext_mesh;
    b32 ext_ray_query;
    b32 ext_accel;
    b32 ext_deferred_ops;
    b32 ext_atomic64;
    b32 ext_memory_budget;
    b32 ext_calibrated;
    b32 ext_executables;
    u32 graphics_family;
    u32 graphics_queue_count;
    u32 compute_family; // NPH_U32_MAX: none dedicated
    u32 transfer_family;
    u32 family_timestamp_bits[64];
    b32 family_sparse[64]; // the family's queues bind sparse memory
    u32 family_count;
    i64 score;
    const char *reject;
} Candidate;

static void query_candidate(Candidate *c, NphAllocator *alloc)
{
    c->f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    c->f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    c->f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    c->mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    c->ray_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    c->accel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    c->atomic64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
    c->executables.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR;
    c->features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    // Extensions first: feature structs of absent extensions must not be chained.
    u32 n = 0U;
    (void)vkEnumerateDeviceExtensionProperties(c->phys, NULL, &n, NULL);
    VkExtensionProperties *exts = n != 0U ? NPH_ALLOC_ARRAY(alloc, VkExtensionProperties, n) : NULL;
    if (exts != NULL) {
        (void)vkEnumerateDeviceExtensionProperties(c->phys, NULL, &n, exts);
        c->ext_swapchain = has_extension(exts, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        c->ext_mesh = has_extension(exts, n, VK_EXT_MESH_SHADER_EXTENSION_NAME);
        c->ext_ray_query = has_extension(exts, n, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        c->ext_accel = has_extension(exts, n, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        c->ext_deferred_ops =
            has_extension(exts, n, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        c->ext_atomic64 = has_extension(exts, n, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
        c->ext_memory_budget = has_extension(exts, n, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        c->ext_calibrated = has_extension(exts, n, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        c->ext_executables =
            has_extension(exts, n, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
        NPH_FREE_ARRAY(alloc, VkExtensionProperties, exts, n);
    }

    void **next = &c->features.pNext;
    *next = &c->f11;
    next = &c->f11.pNext;
    *next = &c->f12;
    next = &c->f12.pNext;
    *next = &c->f13;
    next = &c->f13.pNext;
    if (c->ext_mesh) {
        *next = &c->mesh;
        next = &c->mesh.pNext;
    }
    if (c->ext_ray_query && c->ext_accel && c->ext_deferred_ops) {
        *next = &c->ray_query;
        next = &c->ray_query.pNext;
        *next = &c->accel;
        next = &c->accel.pNext;
    }
    if (c->ext_atomic64) {
        *next = &c->atomic64;
        next = &c->atomic64.pNext;
    }
    if (c->ext_executables) {
        *next = &c->executables;
        next = &c->executables.pNext;
    }
    *next = NULL;
    vkGetPhysicalDeviceFeatures2(c->phys, &c->features);

    c->props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    c->p11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    c->p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    c->p13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    c->driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    c->props.pNext = &c->p11;
    c->p11.pNext = &c->p12;
    c->p12.pNext = &c->p13;
    c->p13.pNext = &c->driver;
    c->driver.pNext = NULL;
    void **prop_next = &c->driver.pNext;
    if (c->ext_mesh) {
        c->mesh_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
        *prop_next = &c->mesh_props;
        prop_next = &c->mesh_props.pNext;
    }
    if (c->ext_accel) {
        c->accel_props.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        *prop_next = &c->accel_props;
        prop_next = &c->accel_props.pNext;
    }
    *prop_next = NULL;
    vkGetPhysicalDeviceProperties2(c->phys, &c->props);
    vkGetPhysicalDeviceMemoryProperties(c->phys, &c->memory);

    // Queue families
    u32 fam_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &fam_count, NULL);
    if (fam_count > 64U) {
        fam_count = 64U;
    }
    VkQueueFamilyProperties fams[64];
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &fam_count, fams);
    c->family_count = fam_count;
    c->graphics_family = NPH_U32_MAX;
    c->compute_family = NPH_U32_MAX;
    c->transfer_family = NPH_U32_MAX;
    for (u32 i = 0; i < fam_count; ++i) {
        const VkQueueFlags f = fams[i].queueFlags;
        c->family_timestamp_bits[i] = fams[i].timestampValidBits;
        c->family_sparse[i] = (f & VK_QUEUE_SPARSE_BINDING_BIT) != 0U;
        const bool graphics = (f & VK_QUEUE_GRAPHICS_BIT) != 0U;
        const bool compute = (f & VK_QUEUE_COMPUTE_BIT) != 0U;
        const bool transfer = (f & VK_QUEUE_TRANSFER_BIT) != 0U;
        if (graphics && compute && c->graphics_family == NPH_U32_MAX) {
            c->graphics_family = i;
            c->graphics_queue_count = fams[i].queueCount;
        }
        if (compute && !graphics && c->compute_family == NPH_U32_MAX) {
            c->compute_family = i;
        }
        if (transfer && !graphics && !compute && c->transfer_family == NPH_U32_MAX) {
            c->transfer_family = i;
        }
    }
}

static void check_baseline(Candidate *c, bool headless)
{
    const VkPhysicalDeviceProperties *p = &c->props.properties;
    const VkPhysicalDeviceFeatures *f = &c->features.features;
    if (p->apiVersion < GPU_API_VERSION) {
        c->reject = "Vulkan 1.3 not supported";
        return;
    }
    if (c->graphics_family == NPH_U32_MAX) {
        c->reject = "no graphics+compute queue family";
        return;
    }
    if (!headless && !c->ext_swapchain) {
        c->reject = "VK_KHR_swapchain missing";
        return;
    }
    if (!c->f13.dynamicRendering || !c->f13.synchronization2) {
        c->reject = "dynamic rendering or synchronization2 missing";
        return;
    }
    if (!c->f12.timelineSemaphore || !c->f12.bufferDeviceAddress || !c->f12.hostQueryReset ||
        !c->f12.drawIndirectCount || !c->f12.scalarBlockLayout) {
        c->reject = "timeline semaphores, buffer device address, host query reset, draw "
                    "indirect count, or scalar block layout missing";
        return;
    }
    if (!c->f12.descriptorIndexing || !c->f12.runtimeDescriptorArray ||
        !c->f12.shaderSampledImageArrayNonUniformIndexing ||
        !c->f12.shaderStorageImageArrayNonUniformIndexing ||
        !c->f12.descriptorBindingSampledImageUpdateAfterBind ||
        !c->f12.descriptorBindingStorageImageUpdateAfterBind ||
        !c->f12.descriptorBindingPartiallyBound ||
        !c->f12.descriptorBindingUpdateUnusedWhilePending) {
        c->reject = "descriptor indexing (bindless) features missing";
        return;
    }
    if (!c->f11.shaderDrawParameters) {
        c->reject = "shaderDrawParameters missing";
        return;
    }
    if (!f->multiDrawIndirect || !f->shaderStorageImageWriteWithoutFormat ||
        !f->shaderStorageImageReadWithoutFormat || !f->fragmentStoresAndAtomics ||
        !f->independentBlend) {
        c->reject = "multiDrawIndirect, format-less storage images, fragment stores, or "
                    "independent blend missing";
        return;
    }
    if (p->limits.maxPushConstantsSize < NPH_GPU_PUSH_CONSTANT_BYTES) {
        c->reject = "fewer than 128 bytes of push constants";
        return;
    }
    if (p->limits.timestampComputeAndGraphics == VK_FALSE &&
        c->family_timestamp_bits[c->graphics_family] == 0U) {
        c->reject = "no timestamps on the graphics queue";
        return;
    }
}

static b32 vk_bool(VkBool32 v)
{
    return v != VK_FALSE ? 1 : 0;
}

static NphGpuDeviceType device_type(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return NPH_GPU_DEVICE_TYPE_INTEGRATED;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return NPH_GPU_DEVICE_TYPE_DISCRETE;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return NPH_GPU_DEVICE_TYPE_VIRTUAL;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return NPH_GPU_DEVICE_TYPE_CPU;
    default:
        return NPH_GPU_DEVICE_TYPE_OTHER;
    }
}

static u64 device_local_bytes(const VkPhysicalDeviceMemoryProperties *m)
{
    u64 total = 0U;
    for (u32 i = 0; i < m->memoryHeapCount; ++i) {
        if ((m->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0U) {
            total += m->memoryHeaps[i].size;
        }
    }
    return total;
}

static u64 host_visible_bytes(const VkPhysicalDeviceMemoryProperties *m)
{
    u64 total = 0U;
    for (u32 i = 0; i < m->memoryHeapCount; ++i) {
        if ((m->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0U) {
            total += m->memoryHeaps[i].size;
        }
    }
    return total;
}

static void score_candidate(Candidate *c)
{
    i64 score = 0;
    switch (device_type(c->props.properties.deviceType)) {
    case NPH_GPU_DEVICE_TYPE_DISCRETE:
        score += 1000;
        break;
    case NPH_GPU_DEVICE_TYPE_INTEGRATED:
        score += 500;
        break;
    case NPH_GPU_DEVICE_TYPE_VIRTUAL:
        score += 200;
        break;
    case NPH_GPU_DEVICE_TYPE_CPU:
        score += 100;
        break;
    case NPH_GPU_DEVICE_TYPE_OTHER:
        score += 50;
        break;
    }
    score += (i64)(device_local_bytes(&c->memory) >> 30U);
    if (c->ext_mesh && c->mesh.meshShader) {
        score += 10;
    }
    if (c->ext_ray_query && c->ray_query.rayQuery) {
        score += 10;
    }
    if (c->compute_family != NPH_U32_MAX) {
        score += 5;
    }
    if (c->transfer_family != NPH_U32_MAX) {
        score += 5;
    }
    c->score = score;
}

static NphResult pick_physical_device(NphGpuDevice *dev, Candidate *chosen)
{
    NphAllocator *alloc = dev->alloc;
    u32 count = 0U;
    VK_TRY(dev, vkEnumeratePhysicalDevices(dev->instance, &count, NULL));
    if (count == 0U) {
        GPU_LOG_ERROR("no Vulkan physical devices");
        return NPH_RESULT_UNSUPPORTED;
    }
    VkPhysicalDevice *devices = NPH_ALLOC_ARRAY(alloc, VkPhysicalDevice, count);
    Candidate *cands = NPH_ALLOC_ARRAY_ZEROED(alloc, Candidate, count);
    if (devices == NULL || cands == NULL) {
        if (devices != NULL) {
            NPH_FREE_ARRAY(alloc, VkPhysicalDevice, devices, count);
        }
        if (cands != NULL) {
            NPH_FREE_ARRAY(alloc, Candidate, cands, count);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    (void)vkEnumeratePhysicalDevices(dev->instance, &count, devices);

    u32 best = NPH_U32_MAX;
    for (u32 i = 0; i < count; ++i) {
        Candidate *c = &cands[i];
        c->phys = devices[i];
        query_candidate(c, alloc);
        check_baseline(c, dev->desc.headless != 0);
        score_candidate(c);
        const VkPhysicalDeviceProperties *p = &c->props.properties;
        GPU_LOG_INFO("device %u: %s (%s, Vulkan %u.%u.%u, %llu MiB local) score %lld%s%s", i,
                     p->deviceName, nph_gpu_device_type_name(device_type(p->deviceType)),
                     VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion),
                     VK_API_VERSION_PATCH(p->apiVersion),
                     (unsigned long long)(device_local_bytes(&c->memory) >> 20U),
                     (long long)c->score, c->reject != NULL ? ", rejected: " : "",
                     c->reject != NULL ? c->reject : "");
        if (c->reject != NULL) {
            continue;
        }
        if (dev->desc.device_index_plus_one != 0U) {
            if (dev->desc.device_index_plus_one - 1U == i) {
                best = i;
            }
        } else if (best == NPH_U32_MAX || c->score > cands[best].score) {
            best = i;
        }
    }
    NphResult result = NPH_RESULT_OK;
    if (best == NPH_U32_MAX) {
        if (dev->desc.device_index_plus_one != 0U) {
            GPU_LOG_ERROR("requested device %u is absent or does not meet the baseline",
                          dev->desc.device_index_plus_one - 1U);
        } else {
            GPU_LOG_ERROR("no physical device meets the Vulkan 1.3 baseline");
        }
        result = NPH_RESULT_UNSUPPORTED;
    } else {
        *chosen = cands[best];
        dev->info.physical_device_count = count;
        dev->info.physical_device_index = best;
    }
    NPH_FREE_ARRAY(alloc, VkPhysicalDevice, devices, count);
    NPH_FREE_ARRAY(alloc, Candidate, cands, count);
    return result;
}

// ---------------------------------------------------------------------------
// Logical device
// ---------------------------------------------------------------------------

static NphResult create_logical_device(NphGpuDevice *dev, Candidate *c)
{
    dev->phys = c->phys;
    dev->props = c->props.properties;
    dev->mem_props = c->memory;

    // Queue plan. desc.single_queue routes everything to the graphics
    // queue, as hardware with one family and one queue would.
    const bool single = dev->desc.single_queue != 0;
    u32 graphics_queue_index = 0U;
    u32 compute_family = single ? NPH_U32_MAX : c->compute_family;
    u32 compute_queue_index = 0U;
    bool compute_distinct = true;
    if (compute_family == NPH_U32_MAX) {
        compute_family = c->graphics_family;
        if (!single && c->graphics_queue_count >= 2U) {
            compute_queue_index = 1U;
        } else {
            compute_distinct = false;
        }
    }
    u32 transfer_family = single ? NPH_U32_MAX : c->transfer_family;
    const bool transfer_distinct = transfer_family != NPH_U32_MAX;
    if (!transfer_distinct) {
        transfer_family = c->graphics_family;
    }

    VkDeviceQueueCreateInfo qcis[3];
    const f32 priorities[2] = {1.0F, 1.0F};
    u32 qci_count = 0U;
    memset(qcis, 0, sizeof qcis);
    qcis[qci_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qcis[qci_count].queueFamilyIndex = c->graphics_family;
    qcis[qci_count].queueCount =
        (compute_family == c->graphics_family && compute_distinct) ? 2U : 1U;
    qcis[qci_count].pQueuePriorities = priorities;
    ++qci_count;
    if (compute_family != c->graphics_family) {
        qcis[qci_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[qci_count].queueFamilyIndex = compute_family;
        qcis[qci_count].queueCount = 1U;
        qcis[qci_count].pQueuePriorities = priorities;
        ++qci_count;
    }
    if (transfer_distinct) {
        qcis[qci_count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[qci_count].queueFamilyIndex = transfer_family;
        qcis[qci_count].queueCount = 1U;
        qcis[qci_count].pQueuePriorities = priorities;
        ++qci_count;
    }

    // Features: the baseline plus every optional one the device has.
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel;
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT atomic64;
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executables;
    memset(&executables, 0, sizeof executables);
    executables.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR;
    memset(&f2, 0, sizeof f2);
    memset(&f11, 0, sizeof f11);
    memset(&f12, 0, sizeof f12);
    memset(&f13, 0, sizeof f13);
    memset(&mesh, 0, sizeof mesh);
    memset(&ray_query, 0, sizeof ray_query);
    memset(&accel, 0, sizeof accel);
    memset(&atomic64, 0, sizeof atomic64);
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    ray_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    accel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    atomic64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;

    const VkPhysicalDeviceFeatures *hf = &c->features.features;
    const u32 off = dev->desc.disable_features;
    const bool want_mesh = (off & (u32)NPH_GPU_OPTIONAL_MESH_SHADER) == 0U;
    const bool want_ray_query = (off & (u32)NPH_GPU_OPTIONAL_RAY_QUERY) == 0U;
    const bool want_image_atomic64 = (off & (u32)NPH_GPU_OPTIONAL_IMAGE_ATOMIC_INT64) == 0U;
    const bool want_buffer_atomic64 = (off & (u32)NPH_GPU_OPTIONAL_BUFFER_ATOMIC_INT64) == 0U;
    const bool want_float16 = (off & (u32)NPH_GPU_OPTIONAL_SHADER_FLOAT16) == 0U;
    const bool want_minmax = (off & (u32)NPH_GPU_OPTIONAL_SAMPLER_FILTER_MINMAX) == 0U;
    // Sparse residency (docs/adr/0065): images bound page by page from a
    // queue that binds, sampled with a minimum level of detail and with
    // residency codes, their unbound pages read as zeros.
    const VkPhysicalDeviceSparseProperties *sp = &c->props.properties.sparseProperties;
    const bool sparse = (off & (u32)NPH_GPU_OPTIONAL_SPARSE_RESIDENCY) == 0U && hf->sparseBinding &&
                        hf->sparseResidencyImage2D && hf->shaderResourceMinLod &&
                        hf->shaderResourceResidency && sp->residencyStandard2DBlockShape &&
                        sp->residencyNonResidentStrict &&
                        (c->family_sparse[transfer_family] || c->family_sparse[c->graphics_family]);
    f2.features.sparseBinding = sparse ? VK_TRUE : VK_FALSE;
    f2.features.sparseResidencyImage2D = sparse ? VK_TRUE : VK_FALSE;
    f2.features.shaderResourceMinLod = sparse ? VK_TRUE : VK_FALSE;
    f2.features.shaderResourceResidency = sparse ? VK_TRUE : VK_FALSE;
    f2.features.multiDrawIndirect = VK_TRUE;
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    f2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    f2.features.fragmentStoresAndAtomics = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    f2.features.samplerAnisotropy = hf->samplerAnisotropy;
    f2.features.depthClamp = hf->depthClamp;
    f2.features.shaderClipDistance = hf->shaderClipDistance;
    // Fragment shaders that read gl_PrimitiveID declare the Geometry
    // capability (the visibility buffer's raster does).
    f2.features.geometryShader = hf->geometryShader;
    f2.features.fillModeNonSolid = hf->fillModeNonSolid;
    f2.features.textureCompressionBC = hf->textureCompressionBC;
    f2.features.shaderInt64 = hf->shaderInt64;
    f2.features.shaderInt16 = hf->shaderInt16;
    f2.features.pipelineStatisticsQuery = hf->pipelineStatisticsQuery;
    f2.features.vertexPipelineStoresAndAtomics = hf->vertexPipelineStoresAndAtomics;
    f2.features.imageCubeArray = hf->imageCubeArray;
    f2.features.drawIndirectFirstInstance = hf->drawIndirectFirstInstance;
    f2.features.sampleRateShading = hf->sampleRateShading;
    f2.features.shaderImageGatherExtended = hf->shaderImageGatherExtended;

    f11.shaderDrawParameters = VK_TRUE;
    f11.storageBuffer16BitAccess = c->f11.storageBuffer16BitAccess;

    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f12.drawIndirectCount = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderFloat16 = want_float16 ? c->f12.shaderFloat16 : VK_FALSE;
    f12.shaderInt8 = c->f12.shaderInt8;
    f12.storageBuffer8BitAccess = c->f12.storageBuffer8BitAccess;
    f12.samplerFilterMinmax = want_minmax ? c->f12.samplerFilterMinmax : VK_FALSE;
    f12.shaderBufferInt64Atomics =
        want_buffer_atomic64 ? c->f12.shaderBufferInt64Atomics : VK_FALSE;
    f12.shaderSharedInt64Atomics = c->f12.shaderSharedInt64Atomics;
    f12.vulkanMemoryModel = c->f12.vulkanMemoryModel;
    f12.vulkanMemoryModelDeviceScope = c->f12.vulkanMemoryModelDeviceScope;
    f12.shaderOutputLayer = c->f12.shaderOutputLayer;
    f12.shaderOutputViewportIndex = c->f12.shaderOutputViewportIndex;
    f12.subgroupBroadcastDynamicId = c->f12.subgroupBroadcastDynamicId;

    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = c->f13.maintenance4;
    f13.subgroupSizeControl = c->f13.subgroupSizeControl;
    f13.computeFullSubgroups = c->f13.computeFullSubgroups;
    f13.shaderDemoteToHelperInvocation = c->f13.shaderDemoteToHelperInvocation;

    const char *exts[16];
    u32 ext_count = 0U;
    if (!dev->desc.headless) {
        exts[ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        dev->has_swapchain_ext = 1;
    }
    void **next = &f2.pNext;
    *next = &f11;
    next = &f11.pNext;
    *next = &f12;
    next = &f12.pNext;
    *next = &f13;
    next = &f13.pNext;

    NphGpuFeatures *feat = &dev->info.features;
    memset(feat, 0, sizeof *feat);
    if (want_mesh && c->ext_mesh && c->mesh.meshShader && c->mesh.taskShader) {
        mesh.meshShader = VK_TRUE;
        mesh.taskShader = VK_TRUE;
        exts[ext_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
        *next = &mesh;
        next = &mesh.pNext;
        feat->mesh_shader = 1;
    }
    if (want_ray_query && c->ext_ray_query && c->ext_accel && c->ext_deferred_ops &&
        c->ray_query.rayQuery && c->accel.accelerationStructure) {
        ray_query.rayQuery = VK_TRUE;
        accel.accelerationStructure = VK_TRUE;
        exts[ext_count++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        exts[ext_count++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        exts[ext_count++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        *next = &ray_query;
        next = &ray_query.pNext;
        *next = &accel;
        next = &accel.pNext;
        feat->ray_query = 1;
    }
    if (want_image_atomic64 && c->ext_atomic64 && c->atomic64.shaderImageInt64Atomics) {
        atomic64.shaderImageInt64Atomics = VK_TRUE;
        exts[ext_count++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;
        *next = &atomic64;
        next = &atomic64.pNext;
        feat->image_atomic_int64 = 1;
    }
    if (c->ext_memory_budget) {
        exts[ext_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
        feat->memory_budget = 1;
    }
    if (c->ext_calibrated) {
        exts[ext_count++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
        feat->calibrated_timestamps = 1;
    }
    if (dev->desc.shader_statistics && c->ext_executables &&
        c->executables.pipelineExecutableInfo) {
        executables.pipelineExecutableInfo = VK_TRUE;
        exts[ext_count++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
        *next = &executables;
        next = &executables.pNext;
        feat->shader_statistics = 1;
    }
    *next = NULL;
    feat->sampler_filter_minmax = vk_bool(f12.samplerFilterMinmax);
    feat->shader_float16 = vk_bool(f12.shaderFloat16);
    feat->buffer_atomic_int64 = vk_bool(f12.shaderBufferInt64Atomics);
    feat->draw_indirect_first_instance = vk_bool(hf->drawIndirectFirstInstance);
    // Subgroup operations in compute shaders.
    if ((c->p11.subgroupSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0U) {
        const VkSubgroupFeatureFlags ops = c->p11.subgroupSupportedOperations;
        feat->subgroup_vote = (ops & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0U;
        feat->subgroup_ballot = (ops & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0U;
        feat->subgroup_arithmetic = (ops & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0U;
        feat->subgroup_shuffle = (ops & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0U;
        feat->subgroup_quad = (ops & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0U;
        feat->subgroup_fragment =
            (c->p11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0U;
    }
    feat->shader_int8 = vk_bool(c->f12.shaderInt8);
    feat->shader_int64 = vk_bool(hf->shaderInt64);
    feat->storage_8bit = vk_bool(c->f12.storageBuffer8BitAccess);
    feat->storage_16bit = vk_bool(c->f11.storageBuffer16BitAccess);
    feat->depth_clamp = vk_bool(hf->depthClamp);
    feat->clip_distance = vk_bool(hf->shaderClipDistance);
    feat->geometry_shader = vk_bool(hf->geometryShader);
    feat->fill_mode_non_solid = vk_bool(hf->fillModeNonSolid);
    feat->texture_compression_bc = vk_bool(hf->textureCompressionBC);
    feat->sampler_anisotropy = vk_bool(hf->samplerAnisotropy);
    feat->sparse_residency = sparse;
    feat->pipeline_statistics = vk_bool(hf->pipelineStatisticsQuery);
    feat->async_compute = compute_distinct;
    feat->dedicated_transfer = transfer_distinct;
    feat->timestamps_on_compute = c->family_timestamp_bits[compute_family] != 0U;
    feat->timestamps_on_transfer = c->family_timestamp_bits[transfer_family] != 0U;

    VkDeviceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &f2;
    ci.queueCreateInfoCount = qci_count;
    ci.pQueueCreateInfos = qcis;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    const VkResult r = vkCreateDevice(dev->phys, &ci, NULL, &dev->device);
    if (r != VK_SUCCESS) {
        GPU_LOG_ERROR("vkCreateDevice failed: %s", nph_gpu_vk_result_name(r));
        dev->device = VK_NULL_HANDLE;
        return nph_gpu_vk_result(r, NULL);
    }
    volkLoadDevice(dev->device);

    // Queues: distinct families first, then the queue table that may alias.
    dev->family_count = 0U;
    u32 family_slot[3];
    const u32 wanted_families[3] = {c->graphics_family, compute_family, transfer_family};
    for (u32 q = 0; q < 3U; ++q) {
        u32 slot = NPH_U32_MAX;
        for (u32 i = 0; i < dev->family_count; ++i) {
            if (dev->families[i] == wanted_families[q]) {
                slot = i;
            }
        }
        if (slot == NPH_U32_MAX) {
            slot = dev->family_count;
            dev->families[dev->family_count++] = wanted_families[q];
        }
        family_slot[q] = slot;
    }

    NphGpuQueue *g = &dev->queue_storage[0];
    g->family = c->graphics_family;
    g->index = graphics_queue_index;
    g->family_slot = family_slot[0];
    g->timestamps = c->family_timestamp_bits[g->family] != 0U;
    vkGetDeviceQueue(dev->device, g->family, g->index, &g->vk);
    nph_mutex_init(&g->lock);
    dev->queues[NPH_GPU_QUEUE_GRAPHICS] = g;
    dev->queue_count = 1U;

    if (compute_distinct) {
        NphGpuQueue *cq = &dev->queue_storage[dev->queue_count++];
        cq->family = compute_family;
        cq->index = compute_queue_index;
        cq->family_slot = family_slot[1];
        cq->timestamps = c->family_timestamp_bits[cq->family] != 0U;
        vkGetDeviceQueue(dev->device, cq->family, cq->index, &cq->vk);
        nph_mutex_init(&cq->lock);
        dev->queues[NPH_GPU_QUEUE_COMPUTE] = cq;
    } else {
        dev->queues[NPH_GPU_QUEUE_COMPUTE] = g;
    }
    if (transfer_distinct) {
        NphGpuQueue *tq = &dev->queue_storage[dev->queue_count++];
        tq->family = transfer_family;
        tq->index = 0U;
        tq->family_slot = family_slot[2];
        tq->timestamps = c->family_timestamp_bits[tq->family] != 0U;
        vkGetDeviceQueue(dev->device, tq->family, tq->index, &tq->vk);
        nph_mutex_init(&tq->lock);
        dev->queues[NPH_GPU_QUEUE_TRANSFER] = tq;
    } else {
        dev->queues[NPH_GPU_QUEUE_TRANSFER] = g;
    }
    if (sparse) {
        dev->sparse_queue = c->family_sparse[dev->queues[NPH_GPU_QUEUE_TRANSFER]->family]
                                ? dev->queues[NPH_GPU_QUEUE_TRANSFER]
                                : g;
    }

    gpu_set_name(dev, VK_OBJECT_TYPE_DEVICE, (u64)(uptr)dev->device, "nph device");
    gpu_set_name(dev, VK_OBJECT_TYPE_QUEUE, (u64)(uptr)g->vk, "graphics queue");
    if (compute_distinct) {
        gpu_set_name(dev, VK_OBJECT_TYPE_QUEUE, (u64)(uptr)dev->queues[NPH_GPU_QUEUE_COMPUTE]->vk,
                     "compute queue");
    }
    if (transfer_distinct) {
        gpu_set_name(dev, VK_OBJECT_TYPE_QUEUE, (u64)(uptr)dev->queues[NPH_GPU_QUEUE_TRANSFER]->vk,
                     "transfer queue");
    }

    // What each format can do (optimal tiling).
    for (u32 f = 1U; f < (u32)NPH_GPU_FORMAT_COUNT; ++f) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dev->phys, gpu_vk_format((NphGpuFormat)f), &fp);
        const VkFormatFeatureFlags o = fp.optimalTilingFeatures;
        u32 bits = 0U;
        bits |= (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED
                    : 0U;
        bits |= (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_SAMPLED_LINEAR
                    : 0U;
        bits |= (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_STORAGE
                    : 0U;
        bits |= (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_STORAGE_ATOMIC
                    : 0U;
        bits |= (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_COLOR_ATTACHMENT
                    : 0U;
        bits |= (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_BLEND
                    : 0U;
        bits |= (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_DEPTH_ATTACHMENT
                    : 0U;
        // Vertex formats of acceleration-structure builds are buffer
        // features, meaningful only with the extension enabled.
        bits |= dev->info.features.ray_query &&
                        (fp.bufferFeatures &
                         VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR) != 0U
                    ? (u32)NPH_GPU_FORMAT_FEATURE_ACCEL_VERTEX
                    : 0U;
        dev->format_features[f] = bits;
    }

    // Info
    NphGpuDeviceInfo *info = &dev->info;
    const VkPhysicalDeviceProperties *p = &c->props.properties;
    snprintf(info->name, sizeof info->name, "%s", p->deviceName);
    snprintf(info->driver_name, sizeof info->driver_name, "%s", c->driver.driverName);
    snprintf(info->driver_info, sizeof info->driver_info, "%s", c->driver.driverInfo);
    info->vendor_id = p->vendorID;
    info->device_id = p->deviceID;
    info->type = device_type(p->deviceType);
    info->api_version_major = VK_API_VERSION_MAJOR(p->apiVersion);
    info->api_version_minor = VK_API_VERSION_MINOR(p->apiVersion);
    info->api_version_patch = VK_API_VERSION_PATCH(p->apiVersion);
    info->driver_version = p->driverVersion;
    if (c->driver.driverID == VK_DRIVER_ID_MESA_LLVMPIPE) {
        // Found by gpu_tests' mesh_pipeline_fragment_shaders_read_push_constants
        // on the CI's lavapipe (Mesa 25.2.8); no fixed release is known, so
        // every version is assumed to have it.
        info->quirks |= (u32)NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS;
        // Found by the renderer's virtual-shadow-map tests and pinned by
        // gpu_tests' an_indirect_draw_after_a_count_draw_draws (Mesa 25.2.8:
        // lvp_execute.c's handle_draw_indirect keeps indirect_draw_count).
        info->quirks |= (u32)NPH_GPU_QUIRK_INDIRECT_DRAW_STALE_COUNT;
    }
    info->device_local_bytes = device_local_bytes(&c->memory);
    info->host_visible_bytes = host_visible_bytes(&c->memory);
    NphGpuLimits *lim = &info->limits;
    lim->max_image_dimension_2d = p->limits.maxImageDimension2D;
    lim->max_image_dimension_3d = p->limits.maxImageDimension3D;
    lim->max_image_dimension_cube = p->limits.maxImageDimensionCube;
    lim->max_image_layers = p->limits.maxImageArrayLayers;
    lim->max_color_attachments =
        nph_min_u32(p->limits.maxColorAttachments, NPH_GPU_MAX_COLOR_ATTACHMENTS);
    lim->max_push_constant_bytes = p->limits.maxPushConstantsSize;
    lim->max_draw_indirect_count = p->limits.maxDrawIndirectCount;
    lim->max_compute_workgroup_invocations = p->limits.maxComputeWorkGroupInvocations;
    for (u32 i = 0; i < 3U; ++i) {
        lim->max_compute_workgroup_size[i] = p->limits.maxComputeWorkGroupSize[i];
        lim->max_compute_workgroup_count[i] = p->limits.maxComputeWorkGroupCount[i];
    }
    lim->subgroup_size = c->p11.subgroupSize;
    const VkSampleCountFlags sc =
        p->limits.framebufferColorSampleCounts & p->limits.framebufferDepthSampleCounts;
    lim->max_sample_count = 1U;
    for (u32 s = 64U; s >= 1U; s >>= 1U) {
        if ((sc & s) != 0U) {
            lim->max_sample_count = s;
            break;
        }
    }
    lim->max_anisotropy = hf->samplerAnisotropy ? p->limits.maxSamplerAnisotropy : 1.0F;
    lim->timestamp_period_ns = p->limits.timestampPeriod;
    lim->min_uniform_buffer_offset_alignment = p->limits.minUniformBufferOffsetAlignment;
    lim->min_storage_buffer_offset_alignment = p->limits.minStorageBufferOffsetAlignment;
    lim->optimal_buffer_copy_offset_alignment = p->limits.optimalBufferCopyOffsetAlignment;
    lim->non_coherent_atom_size = p->limits.nonCoherentAtomSize;
    lim->bindless_sampled_images =
        nph_min_u32(dev->desc.bindless_sampled_images,
                    nph_min_u32(c->p12.maxDescriptorSetUpdateAfterBindSampledImages,
                                c->p12.maxPerStageDescriptorUpdateAfterBindSampledImages));
    lim->bindless_storage_images =
        nph_min_u32(dev->desc.bindless_storage_images,
                    nph_min_u32(c->p12.maxDescriptorSetUpdateAfterBindStorageImages,
                                c->p12.maxPerStageDescriptorUpdateAfterBindStorageImages));
    lim->bindless_samplers =
        nph_min_u32(dev->desc.bindless_samplers,
                    nph_min_u32(c->p12.maxDescriptorSetUpdateAfterBindSamplers,
                                c->p12.maxPerStageDescriptorUpdateAfterBindSamplers));
    if (info->features.mesh_shader) {
        const VkPhysicalDeviceMeshShaderPropertiesEXT *m = &c->mesh_props;
        lim->max_mesh_output_vertices = m->maxMeshOutputVertices;
        lim->max_mesh_output_primitives = m->maxMeshOutputPrimitives;
        lim->max_mesh_workgroup_invocations = m->maxMeshWorkGroupInvocations;
        for (u32 i = 0; i < 3U; ++i) {
            lim->max_mesh_workgroup_count[i] = m->maxMeshWorkGroupCount[i];
        }
        lim->max_mesh_workgroup_total_count = m->maxMeshWorkGroupTotalCount;
        lim->max_preferred_mesh_workgroup_invocations = m->maxPreferredMeshWorkGroupInvocations;
    }
    if (info->features.ray_query) {
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR *a = &c->accel_props;
        lim->accel_scratch_alignment = a->minAccelerationStructureScratchOffsetAlignment;
        lim->accel_max_instances = a->maxInstanceCount;
        lim->accel_max_triangles = a->maxPrimitiveCount;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Timelines
// ---------------------------------------------------------------------------

NphResult gpu_timeline_create_internal(NphGpuDevice *dev, u64 initial, const char *name,
                                       NphGpuTimeline **out)
{
    *out = NULL;
    nph_spin_lock_acquire(&dev->timeline_lock);
    NphGpuTimeline *tl = NULL;
    for (u32 i = 0; i < dev->timeline_cap; ++i) {
        if (!dev->timelines[i].in_use) {
            tl = &dev->timelines[i];
            tl->in_use = 1;
            break;
        }
    }
    nph_spin_lock_release(&dev->timeline_lock);
    if (tl == NULL) {
        return NPH_RESULT_FULL;
    }
    VkSemaphoreTypeCreateInfo type;
    memset(&type, 0, sizeof type);
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initial;
    VkSemaphoreCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    ci.pNext = &type;
    const VkResult r = vkCreateSemaphore(dev->device, &ci, NULL, &tl->vk);
    if (r != VK_SUCCESS) {
        tl->in_use = 0;
        return gpu_vk_fail(dev, r, "vkCreateSemaphore(timeline)");
    }
    nph_atomic_store_u64(&tl->submitted, initial, NPH_MEMORY_ORDER_RELAXED);
    gpu_copy_name(tl->name, name, "timeline", (u32)(tl - dev->timelines));
    gpu_set_name(dev, VK_OBJECT_TYPE_SEMAPHORE, (u64)(uptr)tl->vk, tl->name);
    *out = tl;
    return NPH_RESULT_OK;
}

void gpu_timeline_destroy_internal(NphGpuDevice *dev, NphGpuTimeline *tl, bool deferred)
{
    if (tl == NULL || !tl->in_use) {
        return;
    }
    if (deferred) {
        gpu_defer_handle(dev, NPH_GPU_DEFERRED_SEMAPHORE, tl->vk);
    } else {
        vkDestroySemaphore(dev->device, tl->vk, NULL);
    }
    tl->vk = VK_NULL_HANDLE;
    nph_spin_lock_acquire(&dev->timeline_lock);
    tl->in_use = 0;
    nph_spin_lock_release(&dev->timeline_lock);
}

NphResult nph_gpu_timeline_create(NphGpuDevice *dev, u64 initial_value, const char *name,
                                  NphGpuTimeline **out)
{
    NPH_ASSERT(dev != NULL && out != NULL);
    return gpu_timeline_create_internal(dev, initial_value, name, out);
}

void nph_gpu_timeline_destroy(NphGpuDevice *dev, NphGpuTimeline *timeline)
{
    NPH_ASSERT(dev != NULL);
    for (u32 q = 0; q < dev->queue_count; ++q) {
        NPH_ASSERT_MSG(dev->queue_storage[q].timeline != timeline,
                       "queue timelines are owned by the device");
        if (dev->queue_storage[q].timeline == timeline) {
            GPU_LOG_ERROR("nph_gpu_timeline_destroy: queue %u's timeline is owned by the device; "
                          "ignored",
                          q);
            return;
        }
    }
    gpu_timeline_destroy_internal(dev, timeline, true);
}

u64 nph_gpu_timeline_value(NphGpuDevice *dev, const NphGpuTimeline *timeline)
{
    u64 value = 0U;
    const VkResult r = vkGetSemaphoreCounterValue(dev->device, timeline->vk, &value);
    if (r != VK_SUCCESS) {
        (void)gpu_vk_fail(dev, r, "vkGetSemaphoreCounterValue");
    }
    return value;
}

NphResult nph_gpu_timeline_wait(NphGpuDevice *dev, const NphGpuTimeline *timeline, u64 value,
                                u64 timeout_ns)
{
    VkSemaphoreWaitInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1U;
    info.pSemaphores = &timeline->vk;
    info.pValues = &value;
    const VkResult r = vkWaitSemaphores(dev->device, &info, timeout_ns);
    if (r == VK_TIMEOUT) {
        return NPH_RESULT_BUSY;
    }
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "vkWaitSemaphores");
    }
    return NPH_RESULT_OK;
}

NphResult nph_gpu_timeline_signal(NphGpuDevice *dev, NphGpuTimeline *timeline, u64 value)
{
    VkSemaphoreSignalInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = timeline->vk;
    info.value = value;
    VK_TRY(dev, vkSignalSemaphore(dev->device, &info));
    u64 seen = nph_atomic_load_u64(&timeline->submitted, NPH_MEMORY_ORDER_RELAXED);
    while (seen < value &&
           !nph_atomic_compare_exchange_u64(&timeline->submitted, &seen, value,
                                            NPH_MEMORY_ORDER_RELAXED, NPH_MEMORY_ORDER_RELAXED)) {
    }
    return NPH_RESULT_OK;
}

NphGpuTimeline *nph_gpu_queue_timeline(NphGpuDevice *dev, NphGpuQueueType queue)
{
    NPH_ASSERT((u32)queue < (u32)NPH_GPU_QUEUE_COUNT);
    return dev->queues[queue]->timeline;
}

u64 nph_gpu_queue_submitted_value(NphGpuDevice *dev, NphGpuQueueType queue)
{
    NPH_ASSERT((u32)queue < (u32)NPH_GPU_QUEUE_COUNT);
    return nph_atomic_load_u64(&dev->queues[queue]->timeline->submitted, NPH_MEMORY_ORDER_ACQUIRE);
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

#define SUBMIT_MAX_CMDS       64U
#define SUBMIT_MAX_SEMAPHORES 24U

NphResult nph_gpu_submit(NphGpuDevice *dev, const NphGpuSubmitDesc *desc, u64 *out_value)
{
    NPH_ASSERT(dev != NULL && desc != NULL);
    NPH_ASSERT((u32)desc->queue < (u32)NPH_GPU_QUEUE_COUNT);
    if (out_value != NULL) {
        *out_value = 0U;
    }
    if (dev->lost) {
        return NPH_RESULT_DEVICE_LOST;
    }
    if (desc->cmd_count > SUBMIT_MAX_CMDS || desc->wait_count + 2U > SUBMIT_MAX_SEMAPHORES ||
        desc->signal_count + 2U > SUBMIT_MAX_SEMAPHORES) {
        GPU_LOG_ERROR("submit exceeds the per-submit limits (%u cmds, %u waits, %u signals)",
                      desc->cmd_count, desc->wait_count, desc->signal_count);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuQueue *q = dev->queues[desc->queue];

    VkCommandBufferSubmitInfo cmds[SUBMIT_MAX_CMDS];
    for (u32 i = 0; i < desc->cmd_count; ++i) {
        NphGpuCmd *cmd = desc->cmds[i];
        if (cmd == NULL || !cmd->ended || cmd->queue != desc->queue) {
            GPU_LOG_ERROR("submit: command buffer %u is %s", i,
                          cmd == NULL ? "NULL"
                                      : (!cmd->ended ? "still recording" : "for another queue"));
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        memset(&cmds[i], 0, sizeof cmds[i]);
        cmds[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmds[i].commandBuffer = cmd->vk;
    }

    VkSemaphoreSubmitInfo waits[SUBMIT_MAX_SEMAPHORES];
    VkSemaphoreSubmitInfo signals[SUBMIT_MAX_SEMAPHORES];
    u32 wait_count = 0U;
    u32 signal_count = 0U;
    for (u32 i = 0; i < desc->wait_count; ++i) {
        VkSemaphoreSubmitInfo *w = &waits[wait_count++];
        memset(w, 0, sizeof *w);
        w->sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        w->semaphore = desc->waits[i].timeline->vk;
        w->value = desc->waits[i].value;
        w->stageMask = desc->waits[i].stages == 0U ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                                   : gpu_vk_stages(desc->waits[i].stages);
    }
    for (u32 i = 0; i < desc->signal_count; ++i) {
        VkSemaphoreSubmitInfo *s = &signals[signal_count++];
        memset(s, 0, sizeof *s);
        s->sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        s->semaphore = desc->signals[i].timeline->vk;
        s->value = desc->signals[i].value;
        s->stageMask = desc->signals[i].stages == 0U ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                                                     : gpu_vk_stages(desc->signals[i].stages);
        u64 seen =
            nph_atomic_load_u64(&desc->signals[i].timeline->submitted, NPH_MEMORY_ORDER_RELAXED);
        while (seen < s->value && !nph_atomic_compare_exchange_u64(
                                      &desc->signals[i].timeline->submitted, &seen, s->value,
                                      NPH_MEMORY_ORDER_RELAXED, NPH_MEMORY_ORDER_RELAXED)) {
        }
    }
    if (desc->swapchain != NULL) {
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkSemaphore present = VK_NULL_HANDLE;
        const u32 ops =
            desc->swapchain_ops == 0U
                ? (u32)(NPH_GPU_SWAPCHAIN_WAIT_ACQUIRE | NPH_GPU_SWAPCHAIN_SIGNAL_PRESENT)
                : desc->swapchain_ops;
        if (!gpu_swapchain_submit_semaphores(desc->swapchain, ops, &acquire, &present)) {
            GPU_LOG_ERROR("submit names a swapchain with no acquired image, or repeats a wait "
                          "or signal already made this frame");
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        if (acquire != VK_NULL_HANDLE) {
            // Every stage waits: the first use of the image may be a clear
            // or a copy rather than colour output.
            VkSemaphoreSubmitInfo *w = &waits[wait_count++];
            memset(w, 0, sizeof *w);
            w->sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            w->semaphore = acquire;
            w->stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        if (present != VK_NULL_HANDLE) {
            VkSemaphoreSubmitInfo *s = &signals[signal_count++];
            memset(s, 0, sizeof *s);
            s->sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            s->semaphore = present;
            s->stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    nph_mutex_lock(&q->lock);
    const u64 value =
        nph_atomic_fetch_add_u64(&q->timeline->submitted, 1U, NPH_MEMORY_ORDER_ACQ_REL) + 1U;
    VkSemaphoreSubmitInfo *s = &signals[signal_count++];
    memset(s, 0, sizeof *s);
    s->sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    s->semaphore = q->timeline->vk;
    s->value = value;
    s->stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    info.waitSemaphoreInfoCount = wait_count;
    info.pWaitSemaphoreInfos = waits;
    info.commandBufferInfoCount = desc->cmd_count;
    info.pCommandBufferInfos = cmds;
    info.signalSemaphoreInfoCount = signal_count;
    info.pSignalSemaphoreInfos = signals;
    const VkResult r = vkQueueSubmit2(q->vk, 1U, &info, VK_NULL_HANDLE);
    nph_mutex_unlock(&q->lock);
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "vkQueueSubmit2");
    }
    if (out_value != NULL) {
        *out_value = value;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void destroy_partial(NphGpuDevice *dev);

NphResult nph_gpu_device_create(const NphGpuDeviceDesc *desc, NphGpuDevice **out)
{
    NPH_ASSERT(out != NULL);
    *out = NULL;
    if (desc == NULL || desc->alloc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphGpuDevice *dev = NPH_ALLOC_TYPE(desc->alloc, NphGpuDevice);
    if (dev == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    dev->alloc = desc->alloc;
    dev->desc = *desc;
    fill_defaults(&dev->desc);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        dev->frames[i].frame_index = NPH_U64_MAX;
    }

    const VkResult vr = volkInitialize();
    if (vr != VK_SUCCESS) {
        GPU_LOG_ERROR("no Vulkan loader found (volkInitialize: %s)", nph_gpu_vk_result_name(vr));
        NPH_FREE_TYPE(desc->alloc, NphGpuDevice, dev);
        return NPH_RESULT_UNSUPPORTED;
    }

    bool want_validation = NPH_GPU_VALIDATION_ENABLED != 0;
    if (desc->validation == NPH_GPU_TOGGLE_ON) {
        want_validation = true;
    } else if (desc->validation == NPH_GPU_TOGGLE_OFF) {
        want_validation = false;
    }
    bool want_sync = want_validation;
    if (desc->sync_validation == NPH_GPU_TOGGLE_ON) {
        want_sync = true;
    } else if (desc->sync_validation == NPH_GPU_TOGGLE_OFF) {
        want_sync = false;
    }

    NphResult r = create_instance(dev, want_validation, want_sync);
    if (r != NPH_RESULT_OK) {
        destroy_partial(dev);
        return r;
    }
    Candidate chosen;
    memset(&chosen, 0, sizeof chosen);
    r = pick_physical_device(dev, &chosen);
    if (r == NPH_RESULT_OK) {
        r = create_logical_device(dev, &chosen);
    }
    if (r != NPH_RESULT_OK) {
        destroy_partial(dev);
        return r;
    }

    // Timelines (queues first so their slots are stable)
    dev->timeline_cap = dev->desc.max_timelines;
    dev->timelines = NPH_ALLOC_ARRAY_ZEROED(dev->alloc, NphGpuTimeline, dev->timeline_cap);
    if (dev->timelines == NULL) {
        destroy_partial(dev);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < dev->queue_count; ++i) {
        const char *name = i == 0U   ? "graphics timeline"
                           : i == 1U ? "compute timeline"
                                     : "transfer timeline";
        r = gpu_timeline_create_internal(dev, 0U, name, &dev->queue_storage[i].timeline);
        if (r != NPH_RESULT_OK) {
            destroy_partial(dev);
            return r;
        }
    }

    NphGpuAllocCreateInfo aci;
    memset(&aci, 0, sizeof aci);
    aci.instance = dev->instance;
    aci.physical_device = dev->phys;
    aci.device = dev->device;
    aci.api_version = GPU_API_VERSION;
    aci.memory_budget = dev->info.features.memory_budget;
    aci.get_instance_proc_addr = vkGetInstanceProcAddr;
    aci.get_device_proc_addr = vkGetDeviceProcAddr;
    const VkResult ar = nph_gpu_alloc_create(&aci, &dev->vma);
    if (ar != VK_SUCCESS) {
        (void)nph_gpu_vk_result(ar, "nph_gpu_alloc_create");
        destroy_partial(dev);
        return NPH_RESULT_OUT_OF_MEMORY;
    }

    r = gpu_pool_init(&dev->buffers, dev->alloc, dev->desc.max_buffers, sizeof(NphGpuBufferSlot));
    if (r == NPH_RESULT_OK) {
        r = gpu_pool_init(&dev->images, dev->alloc, dev->desc.max_images, sizeof(NphGpuImageSlot));
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_pool_init(&dev->samplers, dev->alloc, dev->desc.max_samplers,
                          sizeof(NphGpuSamplerSlot));
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_pool_init(&dev->shaders, dev->alloc, dev->desc.max_shaders,
                          sizeof(NphGpuShaderSlot));
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_pool_init(&dev->pipelines, dev->alloc, dev->desc.max_pipelines,
                          sizeof(NphGpuPipelineSlot));
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_pool_init(&dev->accels, dev->alloc, dev->desc.max_accels, sizeof(NphGpuAccelSlot));
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_bindless_init(dev);
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_frames_init(dev);
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_upload_init(dev);
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_sparse_init(dev);
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_indirect_counts_init(dev);
    }
    if (r == NPH_RESULT_OK) {
        r = gpu_pipeline_cache_init(dev);
    }
    if (r != NPH_RESULT_OK) {
        destroy_partial(dev);
        return r;
    }

    const NphGpuDeviceInfo *info = &dev->info;
    GPU_LOG_INFO("using %s: %s %s, Vulkan %u.%u.%u, %llu MiB device-local, queues: graphics "
                 "family %u%s%s",
                 info->name, info->driver_name, info->driver_info, info->api_version_major,
                 info->api_version_minor, info->api_version_patch,
                 (unsigned long long)(info->device_local_bytes >> 20U),
                 dev->queues[NPH_GPU_QUEUE_GRAPHICS]->family,
                 info->features.async_compute ? ", async compute" : "",
                 info->features.dedicated_transfer ? ", dedicated transfer" : "");
    // Two records: a log record keeps 216 bytes.
    GPU_LOG_INFO("features: mesh shader %d, ray query %d, image atomic int64 %d, buffer atomic "
                 "int64 %d, float16 %d, int64 %d, BC %d, filter minmax %d, first instance %d, "
                 "sparse residency %d",
                 info->features.mesh_shader, info->features.ray_query,
                 info->features.image_atomic_int64, info->features.buffer_atomic_int64,
                 info->features.shader_float16, info->features.shader_int64,
                 info->features.texture_compression_bc, info->features.sampler_filter_minmax,
                 info->features.draw_indirect_first_instance, info->features.sparse_residency);
    GPU_LOG_INFO("subgroups of %u (vote %d, ballot %d, arithmetic %d, shuffle %d, quad %d; "
                 "fragment %d); bindless %u sampled / %u storage / %u samplers; push constants "
                 "%u B; timestamps %.3f ns",
                 info->limits.subgroup_size, info->features.subgroup_vote,
                 info->features.subgroup_ballot, info->features.subgroup_arithmetic,
                 info->features.subgroup_shuffle, info->features.subgroup_quad,
                 info->features.subgroup_fragment, info->limits.bindless_sampled_images,
                 info->limits.bindless_storage_images, info->limits.bindless_samplers,
                 info->limits.max_push_constant_bytes, (f64)info->limits.timestamp_period_ns);
    if (info->features.mesh_shader) {
        GPU_LOG_INFO(
            "mesh shaders: %u vertices / %u primitives out, %u invocations (%u "
            "preferred), %u x %u x %u groups (%u in total)",
            info->limits.max_mesh_output_vertices, info->limits.max_mesh_output_primitives,
            info->limits.max_mesh_workgroup_invocations,
            info->limits.max_preferred_mesh_workgroup_invocations,
            info->limits.max_mesh_workgroup_count[0], info->limits.max_mesh_workgroup_count[1],
            info->limits.max_mesh_workgroup_count[2], info->limits.max_mesh_workgroup_total_count);
    }
    if (info->quirks != 0U) {
        GPU_LOG_INFO("driver quirks worked around: 0x%x%s%s", info->quirks,
                     (info->quirks & (u32)NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS) != 0U
                         ? " (mesh pipelines' fragment shaders read no push constants)"
                         : "",
                     (info->quirks & (u32)NPH_GPU_QUIRK_INDIRECT_DRAW_STALE_COUNT) != 0U
                         ? " (indirect draws read a stale count: drawn with counts)"
                         : "");
    }
    *out = dev;
    return NPH_RESULT_OK;
}

static void report_leaks(NphGpuDevice *dev)
{
    const NphGpuLiveCounts live = nph_gpu_device_live_counts(dev);
    if (live.buffers + live.images + live.samplers + live.shaders + live.pipelines + live.accels ==
        0U) {
        return;
    }
    GPU_LOG_ERROR("resource leak: %u buffers, %u images, %u samplers, %u shaders, %u pipelines, "
                  "%u acceleration structures still alive at device destruction",
                  live.buffers, live.images, live.samplers, live.shaders, live.pipelines,
                  live.accels);
    u32 i = 0U;
    void *slot = NULL;
    u32 shown = 0U;
    while (gpu_pool_next_live(&dev->buffers, &i, &slot) && shown++ < 16U) {
        GPU_LOG_ERROR("  leaked buffer '%s' (%llu bytes)", ((NphGpuBufferSlot *)slot)->name,
                      (unsigned long long)((NphGpuBufferSlot *)slot)->desc.size);
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->images, &i, &slot) && shown++ < 32U) {
        GPU_LOG_ERROR("  leaked image '%s' (%ux%u)", ((NphGpuImageSlot *)slot)->name,
                      ((NphGpuImageSlot *)slot)->desc.width,
                      ((NphGpuImageSlot *)slot)->desc.height);
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->samplers, &i, &slot) && shown++ < 40U) {
        GPU_LOG_ERROR("  leaked sampler '%s'", ((NphGpuSamplerSlot *)slot)->name);
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->shaders, &i, &slot) && shown++ < 48U) {
        GPU_LOG_ERROR("  leaked shader '%s'", ((NphGpuShaderSlot *)slot)->name);
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->pipelines, &i, &slot) && shown++ < 56U) {
        GPU_LOG_ERROR("  leaked pipeline '%s'", ((NphGpuPipelineSlot *)slot)->name);
    }
    i = 0U;
    while (gpu_pool_next_live(&dev->accels, &i, &slot) && shown++ < 64U) {
        GPU_LOG_ERROR("  leaked acceleration structure '%s'", ((NphGpuAccelSlot *)slot)->name);
    }
}

static void destroy_partial(NphGpuDevice *dev)
{
    NphAllocator *alloc = dev->alloc;
    if (dev->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(dev->device);
        gpu_pipeline_cache_shutdown(dev);
        gpu_upload_shutdown(dev);
        if (!nph_gpu_buffer_is_null(dev->indirect_counts)) {
            nph_gpu_buffer_destroy(dev, dev->indirect_counts);
            dev->indirect_counts = NPH_GPU_BUFFER_NULL;
        }
        for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
            gpu_run_deferred(dev, i);
        }
        if (dev->buffers.slots != NULL) {
            report_leaks(dev);
            gpu_pipelines_shutdown(dev);
            gpu_resources_shutdown(dev);
        }
        gpu_frames_shutdown(dev);
        gpu_bindless_shutdown(dev);
        gpu_sparse_shutdown(dev);
        if (dev->timelines != NULL) {
            for (u32 i = 0; i < dev->timeline_cap; ++i) {
                gpu_timeline_destroy_internal(dev, &dev->timelines[i], false);
            }
        }
        if (dev->vma != NULL) {
            nph_gpu_alloc_destroy(dev->vma);
            dev->vma = NULL;
        }
        for (u32 i = 0; i < dev->queue_count; ++i) {
            nph_mutex_destroy(&dev->queue_storage[i].lock);
        }
        vkDestroyDevice(dev->device, NULL);
        dev->device = VK_NULL_HANDLE;
    }
    gpu_pool_free(&dev->buffers, alloc);
    gpu_pool_free(&dev->images, alloc);
    gpu_pool_free(&dev->samplers, alloc);
    gpu_pool_free(&dev->shaders, alloc);
    gpu_pool_free(&dev->pipelines, alloc);
    gpu_pool_free(&dev->accels, alloc);
    if (dev->timelines != NULL) {
        NPH_FREE_ARRAY(alloc, NphGpuTimeline, dev->timelines, dev->timeline_cap);
        dev->timelines = NULL;
    }
    if (dev->messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(dev->instance, dev->messenger, NULL);
        dev->messenger = VK_NULL_HANDLE;
    }
    if (dev->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(dev->instance, NULL);
        dev->instance = VK_NULL_HANDLE;
    }
    NPH_FREE_TYPE(alloc, NphGpuDevice, dev);
}

void nph_gpu_device_destroy(NphGpuDevice *dev)
{
    if (dev == NULL) {
        return;
    }
    NPH_ASSERT_MSG(!dev->frame_active, "destroying the device inside a frame");
    const NphGpuValidationStats v = nph_gpu_device_validation_stats(dev);
    GPU_LOG_INFO("device destroyed: %llu frames, validation %llu errors / %llu warnings / %llu "
                 "performance warnings",
                 (unsigned long long)(dev->any_frame_begun ? dev->frame_index + 1U : 0U),
                 (unsigned long long)v.errors, (unsigned long long)v.warnings,
                 (unsigned long long)v.performance_warnings);
    destroy_partial(dev);
}

NphResult nph_gpu_device_wait_idle(NphGpuDevice *dev)
{
    NPH_ASSERT(dev != NULL);
    if (dev->lost) {
        return NPH_RESULT_DEVICE_LOST;
    }
    // Pending uploads are work too: submit them so the wait covers them.
    // Recycling the slots below would otherwise reset their command buffer
    // and release their staging memory under them, losing the copies.
    NPH_TRY(nph_gpu_upload_flush(dev, NULL));
    VK_TRY(dev, vkDeviceWaitIdle(dev->device));
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        gpu_run_deferred(dev, i);
        gpu_upload_release_slot(dev, i);
        if (!dev->frame_active || i != dev->frame_slot) {
            gpu_frames_recycle_slot(dev, i);
            gpu_upload_reset_pools(dev, i);
        }
    }
    return NPH_RESULT_OK;
}

const NphGpuDeviceInfo *nph_gpu_device_info(const NphGpuDevice *dev)
{
    return &dev->info;
}

NphAllocator *nph_gpu_device_allocator(NphGpuDevice *dev)
{
    return dev->alloc;
}

bool nph_gpu_format_supports(const NphGpuDevice *dev, NphGpuFormat format, u32 features)
{
    NPH_ASSERT(dev != NULL);
    if ((u32)format == 0U || (u32)format >= (u32)NPH_GPU_FORMAT_COUNT) {
        return false;
    }
    return (dev->format_features[format] & features) == features;
}

NphGpuValidationStats nph_gpu_device_validation_stats(const NphGpuDevice *dev)
{
    NphGpuValidationStats s;
    s.errors = nph_atomic_load_u64(&dev->val_errors, NPH_MEMORY_ORDER_RELAXED);
    s.warnings = nph_atomic_load_u64(&dev->val_warnings, NPH_MEMORY_ORDER_RELAXED);
    s.performance_warnings = nph_atomic_load_u64(&dev->val_perf, NPH_MEMORY_ORDER_RELAXED);
    s.infos = nph_atomic_load_u64(&dev->val_infos, NPH_MEMORY_ORDER_RELAXED);
    return s;
}

NphGpuLiveCounts nph_gpu_device_live_counts(const NphGpuDevice *dev)
{
    NphGpuLiveCounts c;
    c.buffers = gpu_pool_live(&dev->buffers);
    // The staging ring is the device's own buffer.
    if (c.buffers != 0U && !nph_gpu_buffer_is_null(dev->upload.staging)) {
        c.buffers -= 1U;
    }
    if (c.buffers != 0U && !nph_gpu_buffer_is_null(dev->indirect_counts)) {
        c.buffers -= 1U;
    }
    c.images = gpu_pool_live(&dev->images);
    c.samplers = gpu_pool_live(&dev->samplers);
    c.shaders = gpu_pool_live(&dev->shaders);
    c.pipelines = gpu_pool_live(&dev->pipelines);
    c.accels = gpu_pool_live(&dev->accels);
    c.timelines = 0U;
    for (u32 i = 0; i < dev->timeline_cap; ++i) {
        if (dev->timelines[i].in_use) {
            ++c.timelines;
        }
    }
    // The queues' own and the sparse bindings' are the device's.
    const u32 own = dev->queue_count + (dev->sparse_timeline != NULL ? 1U : 0U);
    c.timelines = c.timelines >= own ? c.timelines - own : 0U;
    c.pending_destroys = 0U;
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        c.pending_destroys += dev->frames[i].deferred_count;
    }
    return c;
}

NphGpuMemoryStats nph_gpu_device_memory_stats(const NphGpuDevice *dev)
{
    NphGpuMemoryStats s;
    memset(&s, 0, sizeof s);
    if (dev->vma != NULL) {
        nph_gpu_alloc_stats(dev->vma, &s);
    }
    return s;
}

// ---------------------------------------------------------------------------
// vk.h accessors
// ---------------------------------------------------------------------------

VkInstance nph_gpu_vk_instance(const NphGpuDevice *dev)
{
    return dev->instance;
}
VkPhysicalDevice nph_gpu_vk_physical_device(const NphGpuDevice *dev)
{
    return dev->phys;
}
VkDevice nph_gpu_vk_device(const NphGpuDevice *dev)
{
    return dev->device;
}
VkQueue nph_gpu_vk_queue(const NphGpuDevice *dev, NphGpuQueueType queue)
{
    return dev->queues[queue]->vk;
}
u32 nph_gpu_vk_queue_family(const NphGpuDevice *dev, NphGpuQueueType queue)
{
    return dev->queues[queue]->family;
}
VkSemaphore nph_gpu_vk_timeline(const NphGpuDevice *dev, const NphGpuTimeline *timeline)
{
    NPH_UNUSED(dev);
    return timeline->vk;
}
VkDescriptorSetLayout nph_gpu_vk_bindless_layout(const NphGpuDevice *dev)
{
    return dev->bindless.layout;
}
VkDescriptorSet nph_gpu_vk_bindless_set(const NphGpuDevice *dev)
{
    return dev->bindless.set;
}
VkPipelineLayout nph_gpu_vk_pipeline_layout(const NphGpuDevice *dev)
{
    return dev->bindless.pipeline_layout;
}
VkPipelineCache nph_gpu_vk_pipeline_cache(const NphGpuDevice *dev)
{
    return dev->pipeline_cache;
}
