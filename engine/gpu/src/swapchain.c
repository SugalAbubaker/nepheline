// SPDX-License-Identifier: Apache-2.0
// The swapchain: surface, images as engine handles, acquire and present,
// recreation on resize (docs/gpu.md "Swapchain").
#include "gpu_internal.h"

#define SWAPCHAIN_MAX_IMAGES 8U

struct NphGpuSwapchain {
    NphGpuDevice *dev;
    NphWindow *window;
    VkSurfaceKHR surface;
    VkSwapchainKHR vk;
    NphGpuSwapchainDesc desc;
    NphGpuFormat format;
    VkFormat vk_format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR mode;
    u32 width;
    u32 height;
    u32 image_count;
    NphGpuImage images[SWAPCHAIN_MAX_IMAGES];
    VkSemaphore acquire_sems[NPH_GPU_FRAMES_IN_FLIGHT];
    VkSemaphore present_sems[SWAPCHAIN_MAX_IMAGES];
    u32 acquired;
    u32 acquire_slot;
    b32 has_acquired;
    b32 waited;    // the acquire semaphore was consumed by a submit
    b32 submitted; // the present semaphore was signalled by a submit
    b32 needs_recreate;
    b32 vsync;
    u32 generation;
    char name[NPH_GPU_NAME_MAX];
};

static NphGpuFormat engine_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return NPH_GPU_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return NPH_GPU_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return NPH_GPU_FORMAT_A2B10G10R10_UNORM;
    default:
        return NPH_GPU_FORMAT_UNDEFINED;
    }
}

static void destroy_images(NphGpuSwapchain *sc)
{
    for (u32 i = 0; i < sc->image_count; ++i) {
        nph_gpu_image_destroy(sc->dev, sc->images[i]);
        sc->images[i] = NPH_GPU_IMAGE_NULL;
    }
    sc->image_count = 0U;
}

static NphResult choose_format(NphGpuSwapchain *sc)
{
    NphGpuDevice *dev = sc->dev;
    u32 count = 0U;
    VK_TRY(dev, vkGetPhysicalDeviceSurfaceFormatsKHR(dev->phys, sc->surface, &count, NULL));
    if (count == 0U) {
        return NPH_RESULT_UNSUPPORTED;
    }
    VkSurfaceFormatKHR *formats = NPH_ALLOC_ARRAY(dev->alloc, VkSurfaceFormatKHR, count);
    if (formats == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    (void)vkGetPhysicalDeviceSurfaceFormatsKHR(dev->phys, sc->surface, &count, formats);
    const VkFormat preferred_srgb[2] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    const VkFormat preferred_unorm[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    const VkFormat *first = sc->desc.prefer_srgb ? preferred_srgb : preferred_unorm;
    const VkFormat *second = sc->desc.prefer_srgb ? preferred_unorm : preferred_srgb;
    u32 chosen = NPH_U32_MAX;
    for (u32 pass = 0; pass < 2U && chosen == NPH_U32_MAX; ++pass) {
        const VkFormat *want = pass == 0U ? first : second;
        for (u32 w = 0; w < 2U && chosen == NPH_U32_MAX; ++w) {
            for (u32 i = 0; i < count; ++i) {
                if (formats[i].format == want[w] &&
                    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    chosen = i;
                    break;
                }
            }
        }
    }
    if (chosen == NPH_U32_MAX) {
        for (u32 i = 0; i < count; ++i) {
            if (engine_format(formats[i].format) != NPH_GPU_FORMAT_UNDEFINED) {
                chosen = i;
                break;
            }
        }
    }
    NphResult r = NPH_RESULT_OK;
    if (chosen == NPH_U32_MAX) {
        GPU_LOG_ERROR("no supported swapchain format among %u surface formats", count);
        r = NPH_RESULT_UNSUPPORTED;
    } else {
        sc->vk_format = formats[chosen].format;
        sc->color_space = formats[chosen].colorSpace;
        sc->format = engine_format(sc->vk_format);
    }
    NPH_FREE_ARRAY(dev->alloc, VkSurfaceFormatKHR, formats, count);
    return r;
}

static NphResult choose_present_mode(NphGpuSwapchain *sc)
{
    NphGpuDevice *dev = sc->dev;
    sc->mode = VK_PRESENT_MODE_FIFO_KHR; // always available
    if (sc->vsync) {
        return NPH_RESULT_OK;
    }
    u32 count = 0U;
    VK_TRY(dev, vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, sc->surface, &count, NULL));
    VkPresentModeKHR modes[16];
    if (count > NPH_ARRAY_COUNT(modes)) {
        count = NPH_ARRAY_COUNT(modes);
    }
    (void)vkGetPhysicalDeviceSurfacePresentModesKHR(dev->phys, sc->surface, &count, modes);
    bool mailbox = false;
    bool immediate = false;
    for (u32 i = 0; i < count; ++i) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            mailbox = true;
        } else if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            immediate = true;
        }
    }
    if (mailbox) {
        sc->mode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (immediate) {
        sc->mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return NPH_RESULT_OK;
}

// (Re)creates the VkSwapchainKHR and its image handles. NPH_RESULT_EMPTY
// when the window has no area.
static NphResult create_objects(NphGpuSwapchain *sc)
{
    NphGpuDevice *dev = sc->dev;
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(dev, vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->phys, sc->surface, &caps));
    u32 width = caps.currentExtent.width;
    u32 height = caps.currentExtent.height;
    if (width == NPH_U32_MAX) {
        nph_window_get_framebuffer_size(sc->window, &width, &height);
        width = nph_clamp_u32(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        height = nph_clamp_u32(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (width == 0U || height == 0U) {
        sc->needs_recreate = 1;
        return NPH_RESULT_EMPTY;
    }
    NPH_TRY(choose_format(sc));
    NPH_TRY(choose_present_mode(sc));

    u32 image_count = sc->desc.image_count;
    if (image_count == 0U) {
        image_count = sc->mode == VK_PRESENT_MODE_FIFO_KHR ? 2U : 3U;
    }
    image_count = nph_max_u32(image_count, caps.minImageCount);
    if (caps.maxImageCount != 0U) {
        image_count = nph_min_u32(image_count, caps.maxImageCount);
    }
    image_count = nph_min_u32(image_count, SWAPCHAIN_MAX_IMAGES);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    u32 engine_usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT;
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        engine_usage |= (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    }
    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        engine_usage |= (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    }

    VkSwapchainCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = sc->surface;
    ci.minImageCount = image_count;
    ci.imageFormat = sc->vk_format;
    ci.imageColorSpace = sc->color_space;
    ci.imageExtent.width = width;
    ci.imageExtent.height = height;
    ci.imageArrayLayers = 1U;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = sc->mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = sc->vk;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(dev->device, &ci, NULL, &created);
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "vkCreateSwapchainKHR");
    }
    // The previous swapchain, its image handles, and its present
    // semaphores retire with the frame: a semaphore may still be waited
    // on by a presentation of the old swapchain.
    destroy_images(sc);
    if (sc->vk != VK_NULL_HANDLE) {
        gpu_defer_handle(dev, NPH_GPU_DEFERRED_SWAPCHAIN, (u64)(uptr)sc->vk);
    }
    for (u32 i = 0; i < SWAPCHAIN_MAX_IMAGES; ++i) {
        if (sc->present_sems[i] != VK_NULL_HANDLE) {
            gpu_defer_handle(dev, NPH_GPU_DEFERRED_SEMAPHORE, (u64)(uptr)sc->present_sems[i]);
            sc->present_sems[i] = VK_NULL_HANDLE;
        }
    }
    sc->vk = created;
    gpu_set_name(dev, VK_OBJECT_TYPE_SWAPCHAIN_KHR, (u64)(uptr)created, sc->name);

    u32 got = 0U;
    VK_TRY(dev, vkGetSwapchainImagesKHR(dev->device, sc->vk, &got, NULL));
    if (got > SWAPCHAIN_MAX_IMAGES) {
        GPU_LOG_ERROR("the swapchain has %u images; at most %u are supported", got,
                      SWAPCHAIN_MAX_IMAGES);
        return NPH_RESULT_UNSUPPORTED;
    }
    VkImage vk_images[SWAPCHAIN_MAX_IMAGES];
    VK_TRY(dev, vkGetSwapchainImagesKHR(dev->device, sc->vk, &got, vk_images));
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (u32 i = 0; i < got; ++i) {
        VK_TRY(dev, vkCreateSemaphore(dev->device, &si, NULL, &sc->present_sems[i]));
        gpu_set_name(dev, VK_OBJECT_TYPE_SEMAPHORE, (u64)(uptr)sc->present_sems[i],
                     "swapchain present");
    }
    for (u32 i = 0; i < got; ++i) {
        NphGpuImageDesc desc;
        memset(&desc, 0, sizeof desc);
        char name[NPH_GPU_NAME_MAX];
        snprintf(name, sizeof name, "%s image %u", sc->name, i);
        desc.type = NPH_GPU_IMAGE_2D;
        desc.format = sc->format;
        desc.width = width;
        desc.height = height;
        desc.mip_count = 1U;
        desc.usage = engine_usage;
        desc.name = name;
        NPH_TRY(gpu_image_register_external(dev, vk_images[i], &desc, &sc->images[i]));
        sc->image_count = i + 1U;
    }
    sc->width = width;
    sc->height = height;
    sc->needs_recreate = 0;
    sc->has_acquired = 0;
    sc->waited = 0;
    sc->submitted = 0;
    ++sc->generation;
    GPU_LOG_INFO("swapchain '%s': %ux%u, %u images, %s, %s", sc->name, width, height, got,
                 nph_gpu_format_name(sc->format), nph_gpu_swapchain_present_mode_name(sc));
    return NPH_RESULT_OK;
}

NphResult nph_gpu_swapchain_create(NphGpuDevice *dev, const NphGpuSwapchainDesc *desc,
                                   NphGpuSwapchain **out)
{
    NPH_ASSERT(dev != NULL && desc != NULL && out != NULL);
    *out = NULL;
    if (desc->window == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (dev->desc.headless || !dev->has_swapchain_ext) {
        GPU_LOG_ERROR("swapchains need a device created with headless = false");
        return NPH_RESULT_UNSUPPORTED;
    }
    NphGpuSwapchain *sc = NPH_ALLOC_TYPE(dev->alloc, NphGpuSwapchain);
    if (sc == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    sc->dev = dev;
    sc->window = desc->window;
    sc->desc = *desc;
    sc->vsync = desc->vsync;
    gpu_copy_name(sc->name, desc->name, "swapchain", 0U);
    sc->desc.name = sc->name;

    u64 surface = 0U;
    NphResult r = nph_window_vulkan_create_surface(desc->window, dev->instance, NULL, &surface);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(dev->alloc, NphGpuSwapchain, sc);
        return NPH_RESULT_UNSUPPORTED;
    }
    sc->surface = (VkSurfaceKHR)surface;
    VkBool32 supported = VK_FALSE;
    const VkResult sr = vkGetPhysicalDeviceSurfaceSupportKHR(
        dev->phys, dev->queues[NPH_GPU_QUEUE_GRAPHICS]->family, sc->surface, &supported);
    if (sr != VK_SUCCESS || supported == VK_FALSE) {
        GPU_LOG_ERROR("the graphics queue cannot present to this window's surface");
        vkDestroySurfaceKHR(dev->instance, sc->surface, NULL);
        NPH_FREE_TYPE(dev->alloc, NphGpuSwapchain, sc);
        return NPH_RESULT_UNSUPPORTED;
    }
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT && r == NPH_RESULT_OK; ++i) {
        r = nph_gpu_vk_result(vkCreateSemaphore(dev->device, &si, NULL, &sc->acquire_sems[i]),
                              "vkCreateSemaphore(acquire)");
        gpu_set_name(dev, VK_OBJECT_TYPE_SEMAPHORE, (u64)(uptr)sc->acquire_sems[i],
                     "swapchain acquire");
    }
    if (r == NPH_RESULT_OK) {
        r = create_objects(sc);
        if (r == NPH_RESULT_EMPTY) {
            r = NPH_RESULT_OK; // minimised at creation; acquire retries
        }
    }
    if (r != NPH_RESULT_OK) {
        nph_gpu_swapchain_destroy(sc);
        return r;
    }
    *out = sc;
    return NPH_RESULT_OK;
}

void nph_gpu_swapchain_destroy(NphGpuSwapchain *sc)
{
    if (sc == NULL) {
        return;
    }
    NphGpuDevice *dev = sc->dev;
    (void)vkDeviceWaitIdle(dev->device);
    destroy_images(sc);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        gpu_run_deferred(dev, i);
    }
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        if (sc->acquire_sems[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev->device, sc->acquire_sems[i], NULL);
        }
    }
    for (u32 i = 0; i < SWAPCHAIN_MAX_IMAGES; ++i) {
        if (sc->present_sems[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev->device, sc->present_sems[i], NULL);
        }
    }
    if (sc->vk != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev->device, sc->vk, NULL);
    }
    if (sc->surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(dev->instance, sc->surface, NULL);
    }
    NPH_FREE_TYPE(dev->alloc, NphGpuSwapchain, sc);
}

NphResult nph_gpu_swapchain_acquire(NphGpuSwapchain *sc, NphGpuImage *out)
{
    NPH_ASSERT(sc != NULL && out != NULL);
    NPH_ASSERT_MSG(!sc->has_acquired, "acquire called twice without a present");
    NphGpuDevice *dev = sc->dev;
    *out = NPH_GPU_IMAGE_NULL;
    if (dev->lost) {
        return NPH_RESULT_DEVICE_LOST;
    }
    u32 fb_width = 0U;
    u32 fb_height = 0U;
    nph_window_get_framebuffer_size(sc->window, &fb_width, &fb_height);
    if (fb_width == 0U || fb_height == 0U) {
        return NPH_RESULT_EMPTY;
    }
    if (sc->needs_recreate || fb_width != sc->width || fb_height != sc->height ||
        sc->vk == VK_NULL_HANDLE) {
        NPH_TRY(create_objects(sc));
    }
    for (u32 attempt = 0; attempt < 2U; ++attempt) {
        u32 index = 0U;
        const VkResult r =
            vkAcquireNextImageKHR(dev->device, sc->vk, NPH_U64_MAX,
                                  sc->acquire_sems[dev->frame_slot], VK_NULL_HANDLE, &index);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) {
                sc->needs_recreate = 1;
            }
            sc->acquired = index;
            sc->acquire_slot = dev->frame_slot;
            sc->has_acquired = 1;
            sc->waited = 0;
            sc->submitted = 0;
            NphGpuImageSlot *slot = gpu_image(dev, sc->images[index]);
            gpu_image_set_state(slot, 0U, NPH_GPU_ALL_MIPS, NPH_GPU_LAYOUT_UNDEFINED,
                                (u32)NPH_GPU_STAGE_NONE, (u32)NPH_GPU_ACCESS_NONE,
                                (u32)NPH_GPU_QUEUE_GRAPHICS);
            *out = sc->images[index];
            return NPH_RESULT_OK;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0U) {
            const NphResult rr = create_objects(sc);
            if (rr != NPH_RESULT_OK) {
                return rr;
            }
            continue;
        }
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            GPU_LOG_ERROR("swapchain '%s': surface lost", sc->name);
            return NPH_RESULT_UNSUPPORTED;
        }
        return gpu_vk_fail(dev, r, "vkAcquireNextImageKHR");
    }
    return NPH_RESULT_UNKNOWN;
}

bool gpu_swapchain_submit_semaphores(NphGpuSwapchain *sc, u32 ops, VkSemaphore *out_acquire,
                                     VkSemaphore *out_present)
{
    *out_acquire = VK_NULL_HANDLE;
    *out_present = VK_NULL_HANDLE;
    if (!sc->has_acquired) {
        return false;
    }
    if ((ops & (u32)NPH_GPU_SWAPCHAIN_WAIT_ACQUIRE) != 0U) {
        if (sc->waited) {
            return false;
        }
        *out_acquire = sc->acquire_sems[sc->acquire_slot];
        sc->waited = 1;
    }
    if ((ops & (u32)NPH_GPU_SWAPCHAIN_SIGNAL_PRESENT) != 0U) {
        if (sc->submitted) {
            return false;
        }
        *out_present = sc->present_sems[sc->acquired];
        sc->submitted = 1;
    }
    return true;
}

NphResult nph_gpu_swapchain_present(NphGpuSwapchain *sc)
{
    NPH_ASSERT(sc != NULL);
    NphGpuDevice *dev = sc->dev;
    if (!sc->has_acquired) {
        GPU_LOG_ERROR("present without an acquired image");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!sc->submitted) {
        GPU_LOG_ERROR("present before a submit that names the swapchain");
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    VkPresentInfoKHR info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1U;
    info.pWaitSemaphores = &sc->present_sems[sc->acquired];
    info.swapchainCount = 1U;
    info.pSwapchains = &sc->vk;
    info.pImageIndices = &sc->acquired;
    NphGpuQueue *q = dev->queues[NPH_GPU_QUEUE_GRAPHICS];
    nph_mutex_lock(&q->lock);
    const VkResult r = vkQueuePresentKHR(q->vk, &info);
    nph_mutex_unlock(&q->lock);
    sc->has_acquired = 0;
    sc->waited = 0;
    sc->submitted = 0;
    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
        sc->needs_recreate = 1;
        return NPH_RESULT_OK;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        GPU_LOG_ERROR("swapchain '%s': surface lost at present", sc->name);
        return NPH_RESULT_UNSUPPORTED;
    }
    if (r != VK_SUCCESS) {
        return gpu_vk_fail(dev, r, "vkQueuePresentKHR");
    }
    return NPH_RESULT_OK;
}

NphGpuFormat nph_gpu_swapchain_format(const NphGpuSwapchain *sc)
{
    return sc->format;
}

void nph_gpu_swapchain_extent(const NphGpuSwapchain *sc, u32 *out_width, u32 *out_height)
{
    if (out_width != NULL) {
        *out_width = sc->width;
    }
    if (out_height != NULL) {
        *out_height = sc->height;
    }
}

u32 nph_gpu_swapchain_image_count(const NphGpuSwapchain *sc)
{
    return sc->image_count;
}

const char *nph_gpu_swapchain_present_mode_name(const NphGpuSwapchain *sc)
{
    switch (sc->mode) {
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return "immediate";
    default:
        return "fifo";
    }
}

void nph_gpu_swapchain_set_vsync(NphGpuSwapchain *sc, bool vsync)
{
    if ((sc->vsync != 0) != vsync) {
        sc->vsync = vsync ? 1 : 0;
        sc->needs_recreate = 1;
    }
}

bool nph_gpu_swapchain_vsync(const NphGpuSwapchain *sc)
{
    return sc->vsync != 0;
}

u32 nph_gpu_swapchain_generation(const NphGpuSwapchain *sc)
{
    return sc->generation;
}
