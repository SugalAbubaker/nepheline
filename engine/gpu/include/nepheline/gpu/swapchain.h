// SPDX-License-Identifier: Apache-2.0
#pragma once

// The swapchain (CLAUDE.md §8.5 "Swapchain: resize handling, present mode
// selection, HDR output (later)").
//
// A swapchain is created for a window (platform/window.h supplies the
// surface) and owns its images, which it exposes as ordinary NphGpuImage
// handles so barriers, attachments, and the render graph treat them like
// any other image. Each frame: nph_gpu_swapchain_acquire, record and
// submit with NphGpuSubmitDesc.swapchain set (which waits for the acquire
// and signals the present semaphore), nph_gpu_swapchain_present.
//
// Resizing is handled inside acquire and present: an out-of-date or
// suboptimal swapchain, or a framebuffer whose size changed, is recreated
// at the next acquire (the images get new handles; the old ones are
// destroyed when their frames complete). A minimised window (zero extent)
// makes acquire return NPH_RESULT_EMPTY: skip rendering this frame.
//
// Present modes: vsync on is FIFO; vsync off prefers MAILBOX (no tearing,
// lowest latency) and falls back to IMMEDIATE, then FIFO.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY (the frame-owning thread), like the
// window it belongs to.

#include <nepheline/gpu/resources.h>
#include <nepheline/platform/window.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NphGpuSwapchainDesc {
    NphWindow *window; // required
    b32 vsync;
    // Prefer an sRGB-encoded format (B8G8R8A8_SRGB) so linear shader output
    // is encoded by the hardware; otherwise a UNORM format and the shader
    // encodes. The format actually chosen is reported by
    // nph_gpu_swapchain_format.
    b32 prefer_srgb;
    u32 image_count; // 0: 3 with mailbox or immediate, 2 with FIFO (clamped to the surface's range)
    const char *name;
} NphGpuSwapchainDesc;

// NPH_RESULT_UNSUPPORTED on a headless device or when the window's surface
// cannot be created or presented to by the graphics queue;
// NPH_RESULT_OUT_OF_MEMORY otherwise on failure.
NPH_MUST_USE NphResult nph_gpu_swapchain_create(NphGpuDevice *dev, const NphGpuSwapchainDesc *desc,
                                                NphGpuSwapchain **out);

// Waits for the device to go idle, then destroys the swapchain, its
// images, and its surface. Safe on NULL.
void nph_gpu_swapchain_destroy(NphGpuSwapchain *swapchain);

// Acquires the image to render this frame into (tracked layout UNDEFINED:
// its contents are stale). NPH_RESULT_EMPTY when the window is minimised,
// NPH_RESULT_DEVICE_LOST on loss, NPH_RESULT_UNSUPPORTED when the surface
// was lost and cannot be recreated. Once per frame, before the submit that
// renders into it.
NPH_MUST_USE NphResult nph_gpu_swapchain_acquire(NphGpuSwapchain *swapchain, NphGpuImage *out);

// Presents the acquired image. The image must have been transitioned to
// NPH_GPU_LAYOUT_PRESENT by a submit that named this swapchain.
// NPH_RESULT_DEVICE_LOST on loss; a suboptimal or out-of-date result is
// handled at the next acquire and reported as OK.
NPH_MUST_USE NphResult nph_gpu_swapchain_present(NphGpuSwapchain *swapchain);

NphGpuFormat nph_gpu_swapchain_format(const NphGpuSwapchain *swapchain);
void nph_gpu_swapchain_extent(const NphGpuSwapchain *swapchain, u32 *out_width, u32 *out_height);
u32 nph_gpu_swapchain_image_count(const NphGpuSwapchain *swapchain);

// The present mode in use, as a static name ("fifo", "mailbox", "immediate").
const char *nph_gpu_swapchain_present_mode_name(const NphGpuSwapchain *swapchain);

// Changes vsync; takes effect at the next acquire (the swapchain is recreated).
void nph_gpu_swapchain_set_vsync(NphGpuSwapchain *swapchain, bool vsync);
bool nph_gpu_swapchain_vsync(const NphGpuSwapchain *swapchain);

// Times the swapchain was (re)created, for tests of the resize path.
u32 nph_gpu_swapchain_generation(const NphGpuSwapchain *swapchain);

#ifdef __cplusplus
}
#endif
