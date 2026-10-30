// SPDX-License-Identifier: Apache-2.0
#pragma once

// Sparse images: pages of memory bound and unbound while the image lives
// (docs/adr/0065-virtual-texturing.md; Vulkan "Sparse Resources").
//
// An image created with NPH_GPU_IMAGE_FLAG_SPARSE has no memory of its own.
// Its levels are cut into pages of the format's standard sparse block shape
// (64 KiB: 256 x 256 texels of a 16-byte BC block, 512 x 256 of an 8-byte
// one, 128 x 128 of RGBA8), and its smallest levels, those under a page in
// either dimension, form the mip tail, bound as a whole. A page is backed by
// a page of an NphGpuPagePool: 64 KiB of device-local memory in blocks the
// pool allocates as it grows.
//
// nph_gpu_sparse_bind changes the bindings of pages in one queue operation
// on the sparse queue (the transfer queue where its family binds, else the
// graphics queue) and signals the sparse timeline. The bindings complete in
// the order they were made: each operation waits for the one before. Copies
// into newly bound pages (nph_gpu_upload_image_region) are ordered after
// the binding: the upload flush that records them waits for the latest
// binding's value. A frame that begins waits, as for its queues' work, for
// the sparse timeline's value at the end of the frame its slot last held.
//
// Vulkan requires that no other queue access memory while its binding
// changes. Nothing here can tell which pages a shader will read, so the
// caller makes sure no GPU work in flight reads a page whose binding it
// changes: the renderer clears a page's residency before unbinding it and
// unbinds only once the frames that could have read it are done (ADR-0065
// point 3). Reads of unbound pages return zeros (residencyNonResidentStrict,
// part of NphGpuFeatures.sparse_residency).
//
// A page freed (nph_gpu_page_free) returns to its pool once the frame being
// recorded, and the bindings made before its end, are complete; or at the
// next nph_gpu_device_wait_idle.
//
// Thread-safety: NPH_THREADSAFE. A pool takes its own lock; binding takes
// the sparse queue's lock (shared with submits to that queue) and then the
// upload lock.

#include <nepheline/gpu/resources.h>

#ifdef __cplusplus
extern "C" {
#endif

// Every standard sparse block shape is 64 KiB.
#define NPH_GPU_SPARSE_PAGE_BYTES 65536U

// NphGpuSparseBind.mip for the mip tail.
#define NPH_GPU_SPARSE_TAIL NPH_U32_MAX

typedef struct NphGpuSparseImageInfo {
    u32 page_width; // texels of a page (whole blocks of a compressed format)
    u32 page_height;
    // The first level of the mip tail: the levels below it are paged. The
    // image's mip_count when every level is paged.
    u32 tail_first_mip;
    // Pages the mip tail takes (0 without a tail); bound one by one, its
    // pages numbered by NphGpuSparseBind.x.
    u32 tail_pages;
} NphGpuSparseImageInfo;

// NPH_RESULT_INVALID_ARGUMENT for an image that is not sparse.
NPH_MUST_USE NphResult nph_gpu_image_sparse_info(const NphGpuDevice *dev, NphGpuImage image,
                                                 NphGpuSparseImageInfo *out);

// The pages of level `mip` (below tail_first_mip): a page per started page
// of its extent, in each direction.
void nph_gpu_image_sparse_pages(const NphGpuDevice *dev, NphGpuImage image, u32 mip,
                                u32 *out_pages_x, u32 *out_pages_y);

// ---------------------------------------------------------------------------
// Page pools
// ---------------------------------------------------------------------------

typedef struct NphGpuPagePool NphGpuPagePool;

// A page of a pool: index + 1 (0 is no page).
typedef struct NphGpuPage {
    u32 value;
} NphGpuPage;

#define NPH_GPU_PAGE_NULL ((NphGpuPage){0U})

typedef struct NphGpuPagePoolDesc {
    u32 max_pages;   // the pool's capacity (> 0)
    u32 block_pages; // pages a device memory block holds (0: 1024, 64 MiB)
    const char *name;
} NphGpuPagePoolDesc;

typedef struct NphGpuPagePoolStats {
    u32 pages_used;    // allocated and not freed
    u32 pages_pending; // freed, waiting for the GPU to be done with them
    u32 blocks;        // device memory blocks allocated
    u64 bytes;         // of those blocks
} NphGpuPagePoolStats;

// NPH_RESULT_UNSUPPORTED without NphGpuFeatures.sparse_residency;
// NPH_RESULT_INVALID_ARGUMENT for no capacity. No memory is allocated until
// pages are.
NPH_MUST_USE NphResult nph_gpu_page_pool_create(NphGpuDevice *dev, const NphGpuPagePoolDesc *desc,
                                                NphGpuPagePool **out);

// Releases the pool's blocks, deferred past the frames in flight. Pages
// still allocated are released with it (logged): the images bound to them
// must be destroyed or unbound first.
void nph_gpu_page_pool_destroy(NphGpuDevice *dev, NphGpuPagePool *pool);

// A free page, allocating a block when every block is full.
// NPH_RESULT_FULL at the pool's capacity; NPH_RESULT_OUT_OF_MEMORY when
// the device has no memory for another block.
NPH_MUST_USE NphResult nph_gpu_page_alloc(NphGpuDevice *dev, NphGpuPagePool *pool, NphGpuPage *out);

// Returns a page to its pool once the frame being recorded and the
// bindings made before its end are complete (above). Null pages are
// ignored.
void nph_gpu_page_free(NphGpuDevice *dev, NphGpuPagePool *pool, NphGpuPage page);

// Takes the pool's lock.
void nph_gpu_page_pool_stats(NphGpuPagePool *pool, NphGpuPagePoolStats *out);

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

typedef struct NphGpuSparseBind {
    NphGpuImage image; // sparse
    u32 mip;           // a paged level, or NPH_GPU_SPARSE_TAIL
    u32 x;             // the page's column (in the tail: which of its pages)
    u32 y;             // the page's row (0 in the tail)
    // The memory to bind: a page of `pool`, or NPH_GPU_PAGE_NULL to unbind
    // (`pool` then unused).
    NphGpuPagePool *pool;
    NphGpuPage page;
} NphGpuSparseBind;

// Changes the bindings of `count` pages (> 0) in one operation on the sparse
// queue. `out_value` (may be NULL) receives the sparse timeline value that
// completes it. The caller guarantees that no GPU work reads any of these
// pages while the operation runs (above). NPH_RESULT_INVALID_ARGUMENT for an
// image that is not sparse, a level or page out of range, or a page not
// allocated from `pool`.
NPH_MUST_USE NphResult nph_gpu_sparse_bind(NphGpuDevice *dev, const NphGpuSparseBind *binds,
                                           u32 count, u64 *out_value);

// Records, in the open upload batch, the copy of a page's texels from `src`
// at `src_offset` into page (x, y) of the paged level `mip` (a tail is
// uploaded whole, by nph_gpu_upload_image). The texels are tightly packed:
// the page's rectangle, which ends with the level at its right and bottom
// edges. Like every copy into a sparse image, it lands after the latest
// binding. `src` needs TRANSFER_SRC usage and must hold the bytes until the
// batch completes: the frame recording now, or the flush's return outside
// a frame. NPH_RESULT_INVALID_ARGUMENT for a page out of range or a source
// too small. Thread-safety: the upload lock (upload.h).
NPH_MUST_USE NphResult nph_gpu_upload_sparse_page(NphGpuDevice *dev, NphGpuImage dst, u32 mip,
                                                  u32 x, u32 y, NphGpuBuffer src, u64 src_offset);

// The sparse timeline: the value of the latest binding made, and the value
// the GPU has completed (it polls).
u64 nph_gpu_sparse_submitted(const NphGpuDevice *dev);
u64 nph_gpu_sparse_completed(NphGpuDevice *dev);

// Waits until the sparse timeline reaches `value`; NPH_RESULT_TIMEOUT
// after `timeout_ns`. Immediate for 0.
NPH_MUST_USE NphResult nph_gpu_sparse_wait(NphGpuDevice *dev, u64 value, u64 timeout_ns);

#ifdef __cplusplus
}
#endif
