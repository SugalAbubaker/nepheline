// SPDX-License-Identifier: Apache-2.0
// Sparse images (sparse.h; docs/adr/0065): their pages and mip tails, the
// page pools, binding and unbinding, and copies into pages just bound.
// Devices without NphGpuFeatures.sparse_residency (lavapipe on Windows)
// skip all but the test of the feature switched off. A device may keep no
// mip tail (lavapipe on Linux: its tail starts past the last level), its
// small levels then pages of their own.
#include <nepheline/gpu/sparse.h>

#include "test_gpu_support.h"

NTEST_SETUP(GpuFixture)
{
    f->dev = test_gpu_create(NULL);
}

NTEST_TEARDOWN(GpuFixture)
{
    test_gpu_destroy(f->dev);
}

#define REQUIRE_SPARSE(f)                                                                          \
    do {                                                                                           \
        REQUIRE_DEVICE(f);                                                                         \
        if (!nph_gpu_device_info((f)->dev)->features.sparse_residency) {                           \
            NTEST_SKIP("no sparse residency on this device");                                      \
        }                                                                                          \
    } while (0)

static NphGpuImage make_sparse(NphGpuDevice *dev, u32 w, u32 h, NphGpuFormat format, u32 mips,
                               const char *name)
{
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.type = NPH_GPU_IMAGE_2D;
    d.format = format;
    d.width = w;
    d.height = h;
    d.mip_count = mips;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST |
              (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    d.flags = (u32)NPH_GPU_IMAGE_FLAG_SPARSE;
    d.name = name;
    NphGpuImage image = NPH_GPU_IMAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_create(dev, &d, &image), NPH_RESULT_OK);
    return image;
}

static NphGpuPagePool *make_pool(NphGpuDevice *dev, u32 max_pages, u32 block_pages)
{
    NphGpuPagePoolDesc d;
    memset(&d, 0, sizeof d);
    d.max_pages = max_pages;
    d.block_pages = block_pages;
    d.name = "test pages";
    NphGpuPagePool *pool = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_page_pool_create(dev, &d, &pool), NPH_RESULT_OK);
    return pool;
}

// The texel of an RGBA8 test image at (x, y) of mip `mip`, as a word.
static u32 texel(u32 mip, u32 x, u32 y)
{
    return (mip << 28U) ^ (x * 2654435761U) ^ (y * 40503U) ^ 0x5A5A5A5AU;
}

// Uploads the test pattern into the page (px, py) of `mip`.
static void upload_page(NphGpuDevice *dev, NphGpuImage image, u32 mip, u32 px, u32 py)
{
    NphGpuSparseImageInfo info;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_sparse_info(dev, image, &info), NPH_RESULT_OK);
    const NphGpuImageDesc *d = nph_gpu_image_desc(dev, image);
    const u32 w = nph_gpu_mip_extent(d->width, mip);
    const u32 h = nph_gpu_mip_extent(d->height, mip);
    const u32 x0 = px * info.page_width;
    const u32 y0 = py * info.page_height;
    const u32 pw = nph_min_u32(info.page_width, w - x0);
    const u32 ph = nph_min_u32(info.page_height, h - y0);
    u32 *words = NPH_ALLOC_ARRAY(nph_allocator_system(), u32, (u64)pw * ph);
    for (u32 y = 0; y < ph; ++y) {
        for (u32 x = 0; x < pw; ++x) {
            words[y * pw + x] = texel(mip, x0 + x, y0 + y);
        }
    }
    NTEST_ASSERT_EQ_INT(
        nph_gpu_upload_image_region(dev, image, mip, 0U, x0, y0, pw, ph, words, (u64)pw * ph * 4U),
        NPH_RESULT_OK);
    NPH_FREE_ARRAY(nph_allocator_system(), u32, words, (u64)pw * ph);
}

// Reads the page (px, py) of `mip` back (the image stays in GENERAL) and
// counts its texels that are not the pattern, or, with `zero`, not zero.
static u32 page_mismatches(NphGpuDevice *dev, NphGpuImage image, u32 mip, u32 px, u32 py, bool zero)
{
    NphGpuSparseImageInfo info;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_sparse_info(dev, image, &info), NPH_RESULT_OK);
    const NphGpuImageDesc *d = nph_gpu_image_desc(dev, image);
    const u32 w = nph_gpu_mip_extent(d->width, mip);
    const u32 h = nph_gpu_mip_extent(d->height, mip);
    const u32 x0 = px * info.page_width;
    const u32 y0 = py * info.page_height;
    const u32 pw = nph_min_u32(info.page_width, w - x0);
    const u32 ph = nph_min_u32(info.page_height, h - y0);
    const u64 bytes = (u64)pw * ph * 4U;
    NphGpuBuffer rb = test_make_buffer(dev, bytes, (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST,
                                       NPH_GPU_MEMORY_READBACK, "page readback");
    NphGpuCmd *cmd = NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_cmd_begin(dev, NPH_GPU_QUEUE_GRAPHICS, "page readback", &cmd),
                        NPH_RESULT_OK);
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.mip = mip;
    region.x = (i32)x0;
    region.y = (i32)y0;
    region.width = pw;
    region.height = ph;
    region.depth = 1U;
    nph_gpu_cmd_copy_image_to_buffer(cmd, image, NPH_GPU_LAYOUT_GENERAL, rb, &region, 1U);
    nph_gpu_cmd_buffer_barrier(cmd, rb, (u32)NPH_GPU_STAGE_TRANSFER,
                               (u32)NPH_GPU_ACCESS_TRANSFER_WRITE, (u32)NPH_GPU_STAGE_HOST,
                               (u32)NPH_GPU_ACCESS_HOST_READ);
    test_submit_wait(dev, cmd);
    const u32 *words = (const u32 *)nph_gpu_buffer_map(dev, rb);
    u32 wrong = 0U;
    for (u32 y = 0; y < ph; ++y) {
        for (u32 x = 0; x < pw; ++x) {
            const u32 want = zero ? 0U : texel(mip, x0 + x, y0 + y);
            wrong += words[y * pw + x] != want ? 1U : 0U;
        }
    }
    nph_gpu_buffer_destroy(dev, rb);
    return wrong;
}

static NphGpuSparseBind page_bind(NphGpuImage image, u32 mip, u32 x, u32 y, NphGpuPagePool *pool,
                                  NphGpuPage page)
{
    NphGpuSparseBind b;
    b.image = image;
    b.mip = mip;
    b.x = x;
    b.y = y;
    b.pool = pool;
    b.page = page;
    return b;
}

// Whether the device keeps a mip tail for `image`: the levels from the
// first under a page bound together (the tail), or, past the last level,
// none (lavapipe on Linux).
static bool has_tail(NphGpuDevice *dev, NphGpuImage image, const NphGpuSparseImageInfo *info)
{
    return info->tail_first_mip < nph_gpu_image_desc(dev, image)->mip_count;
}

NTEST_F(GpuFixture, a_sparse_image_reports_its_pages_and_its_tail)
{
    REQUIRE_SPARSE(f);
    // BC7: 256 x 256 texel pages; 2048 -> 1024 -> 512 -> 256 paged, 128 and
    // below in the tail, or pages of their own where the device keeps none.
    if (nph_gpu_device_info(f->dev)->features.texture_compression_bc) {
        const NphGpuImage bc7 =
            make_sparse(f->dev, 2048U, 2048U, NPH_GPU_FORMAT_BC7_SRGB, 0U, "bc7");
        NphGpuSparseImageInfo info;
        NTEST_ASSERT_EQ_INT(nph_gpu_image_sparse_info(f->dev, bc7, &info), NPH_RESULT_OK);
        NTEST_LOG("BC7 2048: pages %ux%u, tail from mip %u, %u tail pages", info.page_width,
                  info.page_height, info.tail_first_mip, info.tail_pages);
        NTEST_EXPECT_EQ_UINT(info.page_width, 256U);
        NTEST_EXPECT_EQ_UINT(info.page_height, 256U);
        const bool tail = has_tail(f->dev, bc7, &info);
        NTEST_EXPECT_EQ_UINT(info.tail_first_mip, tail ? 4U : 12U);
        if (tail) {
            NTEST_EXPECT_GE_UINT(info.tail_pages, 1U);
        } else {
            NTEST_EXPECT_EQ_UINT(info.tail_pages, 0U);
        }
        u32 px = 0U;
        u32 py = 0U;
        nph_gpu_image_sparse_pages(f->dev, bc7, 0U, &px, &py);
        NTEST_EXPECT_EQ_UINT(px, 8U);
        NTEST_EXPECT_EQ_UINT(py, 8U);
        nph_gpu_image_sparse_pages(f->dev, bc7, 3U, &px, &py);
        NTEST_EXPECT_EQ_UINT(px, 1U);
        NTEST_EXPECT_EQ_UINT(py, 1U);
        nph_gpu_image_sparse_pages(f->dev, bc7, 4U, &px, &py);
        NTEST_EXPECT_EQ_UINT(px, tail ? 0U : 1U);
        nph_gpu_image_destroy(f->dev, bc7);
    }
    // RGBA8: 128 x 128 pages. 1000 x 600 -> 500 x 300 -> 250 x 150 paged
    // (partial pages at the edges), 125 x 75 the first level in the tail
    // where the device keeps one.
    const NphGpuImage rgba =
        make_sparse(f->dev, 1000U, 600U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 0U, "rgba");
    NphGpuSparseImageInfo info;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_sparse_info(f->dev, rgba, &info), NPH_RESULT_OK);
    NTEST_LOG("RGBA8 1000x600: pages %ux%u, tail from mip %u, %u tail pages", info.page_width,
              info.page_height, info.tail_first_mip, info.tail_pages);
    NTEST_EXPECT_EQ_UINT(info.page_width, 128U);
    NTEST_EXPECT_EQ_UINT(info.page_height, 128U);
    NTEST_EXPECT_EQ_UINT(info.tail_first_mip, has_tail(f->dev, rgba, &info) ? 3U : 10U);
    u32 px = 0U;
    u32 py = 0U;
    nph_gpu_image_sparse_pages(f->dev, rgba, 0U, &px, &py);
    NTEST_EXPECT_EQ_UINT(px, 8U);
    NTEST_EXPECT_EQ_UINT(py, 5U);
    nph_gpu_image_sparse_pages(f->dev, rgba, 2U, &px, &py);
    NTEST_EXPECT_EQ_UINT(px, 2U);
    NTEST_EXPECT_EQ_UINT(py, 2U);
    // Not sparse: no pages.
    const NphGpuImage plain = test_make_image_2d(f->dev, 64U, 64U, NPH_GPU_FORMAT_R8G8B8A8_UNORM,
                                                 1U, (u32)NPH_GPU_IMAGE_USAGE_SAMPLED, "plain");
    NTEST_EXPECT_EQ_INT(nph_gpu_image_sparse_info(f->dev, plain, &info),
                        NPH_RESULT_INVALID_ARGUMENT);
    nph_gpu_image_destroy(f->dev, plain);
    nph_gpu_image_destroy(f->dev, rgba);
}

NTEST_F(GpuFixture, a_sparse_image_is_2d_sampled_and_copied_only)
{
    REQUIRE_SPARSE(f);
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.type = NPH_GPU_IMAGE_2D;
    d.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    d.width = 512U;
    d.height = 512U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    d.flags = (u32)NPH_GPU_IMAGE_FLAG_SPARSE;
    NphGpuImage image = NPH_GPU_IMAGE_NULL;
    d.layer_count = 2U;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(f->dev, &d, &image), NPH_RESULT_INVALID_ARGUMENT);
    d.layer_count = 1U;
    d.usage |= (u32)NPH_GPU_IMAGE_USAGE_STORAGE;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(f->dev, &d, &image), NPH_RESULT_INVALID_ARGUMENT);
    d.usage &= ~(u32)NPH_GPU_IMAGE_USAGE_STORAGE;
    d.flags = 0x80U;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(f->dev, &d, &image), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_TRUE(nph_gpu_image_is_null(image));
}

// Pages bound in one call (pages of three levels and the small levels: the
// tail, or a page each where the device keeps none) hold what is copied
// into them; pages never bound read as zeros where the device says so; a
// page unbound and another bound in its place hold the new page's texels.
NTEST_F(GpuFixture, pages_hold_what_is_copied_into_them)
{
    REQUIRE_SPARSE(f);
    // 512 x 512 RGBA8: 4 x 4 pages at mip 0, 2 x 2 at mip 1, 1 at mip 2, and
    // from mip 3 (64 x 64) the small levels: the tail, or a page each. Blocks
    // of 3 pages, so the pool grows block by block.
    const NphGpuImage image =
        make_sparse(f->dev, 512U, 512U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 0U, "paged");
    NphGpuSparseImageInfo info;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_sparse_info(f->dev, image, &info), NPH_RESULT_OK);
    const u32 small = 3U;
    const u32 mips = nph_gpu_image_desc(f->dev, image)->mip_count;
    NTEST_ASSERT_TRUE(info.tail_first_mip == small || info.tail_first_mip == mips);
    NphGpuPagePool *pool = make_pool(f->dev, 16U, 3U);
    typedef struct Where {
        u32 mip, x, y;
    } Where;
    const Where where[5] = {{0U, 0U, 0U}, {0U, 3U, 1U}, {0U, 2U, 3U}, {1U, 1U, 1U}, {2U, 0U, 0U}};
    NphGpuSparseBind binds[14];
    NphGpuPage pages[14];
    u32 n = 0U;
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &pages[n]), NPH_RESULT_OK);
        binds[n] = page_bind(image, where[i].mip, where[i].x, where[i].y, pool, pages[n]);
        ++n;
    }
    for (u32 t = 0; t < info.tail_pages; ++t) {
        NTEST_ASSERT_LT_UINT(n, 14U);
        NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &pages[n]), NPH_RESULT_OK);
        binds[n] = page_bind(image, NPH_GPU_SPARSE_TAIL, t, 0U, pool, pages[n]);
        ++n;
    }
    for (u32 mip = small; mip < info.tail_first_mip; ++mip) {
        NTEST_ASSERT_LT_UINT(n, 14U);
        NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &pages[n]), NPH_RESULT_OK);
        binds[n] = page_bind(image, mip, 0U, 0U, pool, pages[n]);
        ++n;
    }
    u64 value = 0U;
    NTEST_ASSERT_EQ_INT(nph_gpu_sparse_bind(f->dev, binds, n, &value), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(value, 1U);
    NphGpuPagePoolStats st;
    nph_gpu_page_pool_stats(pool, &st);
    NTEST_EXPECT_EQ_UINT(st.pages_used, n);
    NTEST_EXPECT_EQ_UINT(st.blocks, (n + 2U) / 3U);
    for (u32 i = 0; i < 5U; ++i) {
        upload_page(f->dev, image, where[i].mip, where[i].x, where[i].y);
    }
    // The small levels, whole.
    for (u32 mip = small; mip < mips; ++mip) {
        upload_page(f->dev, image, mip, 0U, 0U);
    }
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_EXPECT_EQ_UINT(
            page_mismatches(f->dev, image, where[i].mip, where[i].x, where[i].y, false), 0U);
    }
    NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, small, 0U, 0U, false), 0U);
    NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, mips - 1U, 0U, 0U, false), 0U);
    // Never bound: zeros where the device says so (residencyNonResidentStrict).
    const bool strict = nph_gpu_device_info(f->dev)->features.sparse_residency_strict != 0;
    if (strict) {
        NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, 0U, 1U, 0U, true), 0U);
        NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, 1U, 0U, 1U, true), 0U);
    }

    // Page (3, 1) leaves; page (1, 2) comes, to a page of its own. Nothing
    // reads either while their bindings change (no frame is in flight).
    NphGpuPage fresh = NPH_GPU_PAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &fresh), NPH_RESULT_OK);
    NphGpuSparseBind change[2] = {page_bind(image, 0U, 3U, 1U, NULL, NPH_GPU_PAGE_NULL),
                                  page_bind(image, 0U, 1U, 2U, pool, fresh)};
    NTEST_ASSERT_EQ_INT(nph_gpu_sparse_bind(f->dev, change, 2U, &value), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(value, 2U);
    nph_gpu_page_free(f->dev, pool, pages[1]);
    upload_page(f->dev, image, 0U, 1U, 2U);
    NTEST_ASSERT_EQ_INT(nph_gpu_upload_flush(f->dev, NULL), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, 0U, 1U, 2U, false), 0U);
    if (strict) {
        NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, 0U, 3U, 1U, true), 0U);
    }
    NTEST_EXPECT_EQ_UINT(page_mismatches(f->dev, image, 0U, 0U, 0U, false), 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(f->dev), NPH_RESULT_OK);
    NTEST_EXPECT_GE_UINT(nph_gpu_sparse_completed(f->dev), 2U);
    NTEST_EXPECT_EQ_UINT(nph_gpu_sparse_submitted(f->dev), 2U);

    nph_gpu_image_destroy(f->dev, image);
    for (u32 i = 0; i < n; ++i) {
        if (i != 1U) {
            nph_gpu_page_free(f->dev, pool, pages[i]);
        }
    }
    nph_gpu_page_free(f->dev, pool, fresh);
    nph_gpu_page_pool_destroy(f->dev, pool);
}

// A freed page comes back once the GPU is done with the frame it was freed
// in; the pool stops at its capacity.
NTEST_F(GpuFixture, a_freed_page_returns_once_the_gpu_is_done_with_it)
{
    REQUIRE_SPARSE(f);
    NphGpuPagePool *pool = make_pool(f->dev, 3U, 2U);
    NphGpuPage p[4];
    for (u32 i = 0; i < 3U; ++i) {
        NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &p[i]), NPH_RESULT_OK);
    }
    NTEST_EXPECT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &p[3]), NPH_RESULT_FULL);
    NphGpuPagePoolStats st;
    nph_gpu_page_pool_stats(pool, &st);
    NTEST_EXPECT_EQ_UINT(st.pages_used, 3U);
    NTEST_EXPECT_EQ_UINT(st.blocks, 2U);
    NTEST_EXPECT_EQ_UINT(st.bytes, 3U * (u64)NPH_GPU_SPARSE_PAGE_BYTES);

    // Inside frames: freed in a frame, back when its slot comes round again.
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
    nph_gpu_page_free(f->dev, pool, p[1]);
    nph_gpu_page_pool_stats(pool, &st);
    NTEST_EXPECT_EQ_UINT(st.pages_used, 2U);
    NTEST_EXPECT_EQ_UINT(st.pages_pending, 1U);
    NTEST_EXPECT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &p[3]), NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(f->dev), NPH_RESULT_OK);
        NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(f->dev), NPH_RESULT_OK);
    }
    nph_gpu_page_pool_stats(pool, &st);
    NTEST_EXPECT_EQ_UINT(st.pages_pending, 0U);
    NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &p[3]), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_UINT(p[3].value, p[1].value);
    for (u32 i = 0; i < 4U; ++i) {
        if (i != 1U) {
            nph_gpu_page_free(f->dev, pool, p[i]);
        }
    }
    // Destroyed with its pages still on their way back: they return first.
    nph_gpu_page_pool_destroy(f->dev, pool);
}

NTEST_F(GpuFixture, bindings_of_pages_out_of_range_or_not_in_use_are_refused)
{
    REQUIRE_SPARSE(f);
    const NphGpuImage image =
        make_sparse(f->dev, 512U, 512U, NPH_GPU_FORMAT_R8G8B8A8_UNORM, 0U, "refused");
    NphGpuSparseImageInfo info;
    NTEST_ASSERT_EQ_INT(nph_gpu_image_sparse_info(f->dev, image, &info), NPH_RESULT_OK);
    NphGpuPagePool *pool = make_pool(f->dev, 4U, 0U);
    NphGpuPage page = NPH_GPU_PAGE_NULL;
    NTEST_ASSERT_EQ_INT(nph_gpu_page_alloc(f->dev, pool, &page), NPH_RESULT_OK);
    const NphGpuSparseBind wrong[5] = {
        page_bind(image, info.tail_first_mip, 0U, 0U, pool, page), // a tail level, not the tail
        page_bind(image, 0U, 4U, 0U, pool, page),                  // past the level's pages
        page_bind(image, NPH_GPU_SPARSE_TAIL, info.tail_pages, 0U, pool, page),
        page_bind(image, 0U, 0U, 0U, pool, (NphGpuPage){3U}), // a page not allocated
        page_bind(NPH_GPU_IMAGE_NULL, 0U, 0U, 0U, pool, page),
    };
    for (u32 i = 0; i < 5U; ++i) {
        NTEST_EXPECT_EQ_INT(nph_gpu_sparse_bind(f->dev, &wrong[i], 1U, NULL),
                            NPH_RESULT_INVALID_ARGUMENT);
    }
    NTEST_EXPECT_EQ_INT(nph_gpu_sparse_bind(f->dev, wrong, 0U, NULL), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_EQ_UINT(nph_gpu_sparse_submitted(f->dev), 0U);
    nph_gpu_image_destroy(f->dev, image);
    nph_gpu_page_free(f->dev, pool, page);
    nph_gpu_page_pool_destroy(f->dev, pool);
}

// The fallback every device without sparse residency takes, tested on any.
NTEST(gpu_sparse, without_the_feature_sparse_images_and_pools_are_refused)
{
    NphGpuDeviceDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.alloc = nph_allocator_system();
    desc.headless = 1;
    desc.disable_features = (u32)NPH_GPU_OPTIONAL_SPARSE_RESIDENCY;
    NphGpuDevice *dev = test_gpu_create(&desc);
    if (dev == NULL) {
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_EXPECT_FALSE(nph_gpu_device_info(dev)->features.sparse_residency);
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.type = NPH_GPU_IMAGE_2D;
    d.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    d.width = 512U;
    d.height = 512U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    d.flags = (u32)NPH_GPU_IMAGE_FLAG_SPARSE;
    NphGpuImage image = NPH_GPU_IMAGE_NULL;
    NTEST_EXPECT_EQ_INT(nph_gpu_image_create(dev, &d, &image), NPH_RESULT_UNSUPPORTED);
    NphGpuPagePoolDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_pages = 4U;
    NphGpuPagePool *pool = NULL;
    NTEST_EXPECT_EQ_INT(nph_gpu_page_pool_create(dev, &pd, &pool), NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_NULL(pool);
    NphGpuSparseBind b = page_bind(image, 0U, 0U, 0U, NULL, NPH_GPU_PAGE_NULL);
    NTEST_EXPECT_EQ_INT(nph_gpu_sparse_bind(dev, &b, 1U, NULL), NPH_RESULT_UNSUPPORTED);
    NTEST_EXPECT_EQ_UINT(nph_gpu_sparse_completed(dev), 0U);
    test_gpu_destroy(dev);
}
