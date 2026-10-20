// SPDX-License-Identifier: Apache-2.0
#pragma once

// Uploads to device memory (CLAUDE.md §8.5 "Upload via staging ring
// buffers. Transfer-queue uploads use timeline-semaphore sync").
//
// nph_gpu_upload_* copy the caller's bytes into the device's persistently
// mapped staging ring and record the copy into the frame's transfer
// command buffer. nph_gpu_upload_flush submits that command buffer on the
// TRANSFER queue (the graphics queue when there is no transfer family)
// and signals the transfer timeline; the next graphics submit waits on
// that value, and images are handed over to the graphics queue family
// with a release/acquire pair (exclusive sharing keeps the hardware's
// framebuffer compression, CLAUDE.md §10.3.5), so by the time the frame
// renders every upload has landed. Buffers are shared across queue
// families and need no ownership transfer. When the TRANSFER queue is
// the graphics queue (no transfer family, or NphGpuDeviceDesc
// .single_queue) the upload command buffer carries the barriers itself:
// one before the copies against everything the queue did earlier, one
// after them for everything it does later.
//
// After an image upload its mips are in SHADER_READ_ONLY when the image
// has SAMPLED usage and GENERAL otherwise. Uploads larger than the free
// staging space get a dedicated staging buffer that is destroyed when the
// frame completes.
//
// Outside a frame (no nph_gpu_frame_begin active) a flush is synchronous:
// it waits for the transfer to complete before returning, so tests and
// tools can upload, flush, and read back without frame bookkeeping. There
// an upload that the ring cannot take now but would take empty flushes
// the pending uploads first, so a load between frames of any size streams
// through the ring; only an upload larger than the whole ring (or one
// that meets the ring held by frames still in flight) takes a dedicated
// buffer. Inside a frame the uploads stay batched for the frame.
//
// Thread-safety: NPH_THREADSAFE through one lock on the upload state
// (the only lock on this path); intended for the main thread and loading
// jobs, not for every worker every frame.

#include <nepheline/gpu/resources.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copies `size` bytes to `dst` at `dst_offset`. NPH_RESULT_INVALID_ARGUMENT
// when the range exceeds the buffer; NPH_RESULT_OUT_OF_MEMORY when no
// staging memory could be found.
NPH_MUST_USE NphResult nph_gpu_upload_buffer(NphGpuDevice *dev, NphGpuBuffer dst, u64 dst_offset,
                                             const void *data, u64 size);

// Copies tightly packed texels for `layer_count` layers (0: 1) of mip
// `mip`; `size` must equal the mip's bytes times the layer count. The
// image must have TRANSFER_DST usage. The mip's layout is transitioned
// from whatever the device tracks (UNDEFINED discards nothing we care
// about: the upload overwrites the mip).
NPH_MUST_USE NphResult nph_gpu_upload_image(NphGpuDevice *dev, NphGpuImage dst, u32 mip,
                                            u32 base_layer, u32 layer_count, const void *data,
                                            u64 size);

// Copies tightly packed texels into the rectangle (x, y, width, height) of
// mip `mip`, layer `layer`, keeping the rest of the mip: a texture that
// changes in parts (the editor UI's font atlas, which grows glyph by
// glyph). Compressed formats need block-aligned rectangles; `size` must be
// the rectangle's bytes. NPH_RESULT_INVALID_ARGUMENT for a rectangle
// outside the mip, a misaligned one, a wrong size, an image without
// TRANSFER_DST usage, or an exclusive image (an attachment) on a device
// whose transfer queue has a family of its own: that upload cannot take
// the image from its owner, so it cannot keep the texels it does not
// cover. (On a device with one queue family every image is exclusive,
// and nothing changes owner.)
NPH_MUST_USE NphResult nph_gpu_upload_image_region(NphGpuDevice *dev, NphGpuImage dst, u32 mip,
                                                   u32 layer, u32 x, u32 y, u32 width, u32 height,
                                                   const void *data, u64 size);

// Submits the pending uploads. `out_value` (may be NULL) receives the
// transfer-queue timeline value that completes them; 0 when nothing was
// pending. Outside a frame the call also waits for completion.
NPH_MUST_USE NphResult nph_gpu_upload_flush(NphGpuDevice *dev, u64 *out_value);

// Bytes of staging ring currently free, for tests and the streaming budget.
u64 nph_gpu_upload_staging_free(const NphGpuDevice *dev);

#ifdef __cplusplus
}
#endif
