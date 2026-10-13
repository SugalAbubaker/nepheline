// SPDX-License-Identifier: Apache-2.0
#pragma once

// Buffers, images, and samplers (CLAUDE.md §8.5 "Memory" and "Bindless").
//
// Memory comes from the Vulkan Memory Allocator behind nph_gpu_alloc_*
// (an internal interface; the allocator never shows in the public API).
// Every buffer has a device address and every sampled image, storage image
// mip, and sampler has an index in the one global descriptor set, so
// shaders receive addresses and indices through push constants and never
// see a descriptor set of their own (docs/adr/0014-gpu-module.md).
//
// Destruction is deferred: the handle is invalidated at once (stale use is
// asserted), the Vulkan objects are released when the frame being recorded
// has completed on the GPU, or at the next nph_gpu_device_wait_idle. The
// bindless slot is released at the same time, so a shader in flight never
// sees a slot change under it.
//
// Thread-safety: NPH_THREADSAFE. Creation and destruction take a spin lock
// per pool; the accessors read immutable slot data.

#include <nepheline/gpu/device.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

// NPH_RESULT_INVALID_ARGUMENT for a zero size or no usage; NPH_RESULT_FULL
// past max_buffers; NPH_RESULT_OUT_OF_MEMORY when the GPU heap is
// exhausted. UPLOAD and READBACK buffers are mapped for their lifetime.
NPH_MUST_USE NphResult nph_gpu_buffer_create(NphGpuDevice *dev, const NphGpuBufferDesc *desc,
                                             NphGpuBuffer *out);

// Deferred, see above. Null and stale handles are ignored (stale asserted).
void nph_gpu_buffer_destroy(NphGpuDevice *dev, NphGpuBuffer buffer);

bool nph_gpu_buffer_is_valid(const NphGpuDevice *dev, NphGpuBuffer buffer);
const NphGpuBufferDesc *nph_gpu_buffer_desc(const NphGpuDevice *dev, NphGpuBuffer buffer);
u64 nph_gpu_buffer_size(const NphGpuDevice *dev, NphGpuBuffer buffer);

// The buffer's device address, for push constants (GL_EXT_buffer_reference).
u64 nph_gpu_buffer_address(const NphGpuDevice *dev, NphGpuBuffer buffer);

// The persistent host mapping of an UPLOAD or READBACK buffer; NULL for
// DEVICE memory. Writes to UPLOAD memory are visible to the GPU at the
// next submit (coherent); reads of READBACK memory are valid once the
// producing work has completed (a timeline wait).
void *nph_gpu_buffer_map(const NphGpuDevice *dev, NphGpuBuffer buffer);

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// NPH_RESULT_INVALID_ARGUMENT for a zero extent, an UNDEFINED format, no
// usage, or a usage the format does not support (logged);
// NPH_RESULT_FULL past max_images or when the bindless arrays are full;
// NPH_RESULT_OUT_OF_MEMORY when the heap is exhausted. The image starts
// in NPH_GPU_LAYOUT_UNDEFINED.
NPH_MUST_USE NphResult nph_gpu_image_create(NphGpuDevice *dev, const NphGpuImageDesc *desc,
                                            NphGpuImage *out);

void nph_gpu_image_destroy(NphGpuDevice *dev, NphGpuImage image);

bool nph_gpu_image_is_valid(const NphGpuDevice *dev, NphGpuImage image);

// The description with the defaults filled in (depth, mip_count,
// layer_count, samples).
const NphGpuImageDesc *nph_gpu_image_desc(const NphGpuDevice *dev, NphGpuImage image);

// The bindless slot of the whole image (all mips and layers) in the sampled
// image array; NPH_GPU_INVALID_INDEX without SAMPLED usage.
u32 nph_gpu_image_sampled_index(const NphGpuDevice *dev, NphGpuImage image);

// The bindless slot of one mip in the storage image array;
// NPH_GPU_INVALID_INDEX without STORAGE usage or for a mip out of range.
u32 nph_gpu_image_storage_index(const NphGpuDevice *dev, NphGpuImage image, u32 mip);

// The layout the device believes mip `mip` is in: the result of the last
// recorded barrier, upload, or render-graph execution. Recording order is
// assumed to be submission order (true on one thread; the render graph
// keeps its own view when recording in parallel).
NphGpuLayout nph_gpu_image_layout(const NphGpuDevice *dev, NphGpuImage image, u32 mip);

// ---------------------------------------------------------------------------
// Samplers
// ---------------------------------------------------------------------------

// NPH_RESULT_FULL past max_samplers or the bindless sampler array;
// NPH_RESULT_UNSUPPORTED for a reduction mode without the feature.
NPH_MUST_USE NphResult nph_gpu_sampler_create(NphGpuDevice *dev, const NphGpuSamplerDesc *desc,
                                              NphGpuSampler *out);
void nph_gpu_sampler_destroy(NphGpuDevice *dev, NphGpuSampler sampler);
bool nph_gpu_sampler_is_valid(const NphGpuDevice *dev, NphGpuSampler sampler);

// The bindless slot in the sampler array.
u32 nph_gpu_sampler_index(const NphGpuDevice *dev, NphGpuSampler sampler);

// Fills a description with trilinear, repeat, no anisotropy.
void nph_gpu_sampler_desc_default(NphGpuSamplerDesc *desc);

#ifdef __cplusplus
}
#endif
