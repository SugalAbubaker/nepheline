// SPDX-License-Identifier: Apache-2.0
// Texture streaming (runtime/texture_stream.h; ADR-0065 stage 3): its
// decisions, pure; the stream over a stand-in for the renderer's paging,
// on any device: the order it installs in, its budget, what it evicts and
// when, the page memory and the staging filling up, removals, and textures
// destroyed under it; and the stream over a paged texture drawn on a plane:
// it installs the levels the pixels ask for, coarse first, until the image
// is the whole texture's; keeps out the levels a far view does not ask for;
// leaves a texture short, without trading levels, where the budget is full
// of wanted ones; and gives the room of a texture no longer asked for to one
// that is. The GPU tests skip without texture paging (lavapipe).
#include <nepheline/gpu/sparse.h>
#include <nepheline/render/primitives.h>
#include <nepheline/runtime/texture_stream.h>

#include "runtime_internal.h"
#include "test_render_support.h"

#define NEVER NPH_U64_MAX

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

NTEST(texture_stream, a_texture_wants_the_finest_level_asked_lately)
{
    const u64 asked[4] = {NEVER, 100U, 90U, NEVER};
    NTEST_EXPECT_EQ_UINT(texture_stream_wanted(asked, 4U, 120U, 30U), 1U);
    // Level 1 asked too long ago: level 2 is too, so the small levels alone.
    NTEST_EXPECT_EQ_UINT(texture_stream_wanted(asked, 4U, 120U, 15U), 4U);
    NTEST_EXPECT_EQ_UINT(texture_stream_wanted(asked, 4U, 105U, 15U), 1U);
    // Only the levels below the small ones count.
    NTEST_EXPECT_EQ_UINT(texture_stream_wanted(asked, 1U, 120U, 30U), 1U);
    const u64 never[4] = {NEVER, NEVER, NEVER, NEVER};
    NTEST_EXPECT_EQ_UINT(texture_stream_wanted(never, 4U, 120U, 30U), 4U);
    NTEST_EXPECT_EQ_UINT(texture_stream_last_asked(asked, 4U), 100U);
    NTEST_EXPECT_EQ_UINT(texture_stream_last_asked(asked, 1U), NEVER);
    NTEST_EXPECT_EQ_UINT(texture_stream_last_asked(never, 4U), NEVER);
}

NTEST(texture_stream, the_texture_furthest_from_its_wanted_level_installs_first)
{
    const TextureStreamNeed far = {3U, 10U};
    const TextureStreamNeed near_recent = {1U, 50U};
    const TextureStreamNeed near_old = {1U, 20U};
    const TextureStreamNeed near_never = {1U, NEVER};
    NTEST_EXPECT_TRUE(texture_stream_installs_before(far, near_recent));
    NTEST_EXPECT_FALSE(texture_stream_installs_before(near_recent, far));
    NTEST_EXPECT_TRUE(texture_stream_installs_before(near_recent, near_old));
    NTEST_EXPECT_TRUE(texture_stream_installs_before(near_old, near_never));
    NTEST_EXPECT_FALSE(texture_stream_installs_before(near_never, near_old));
    NTEST_EXPECT_FALSE(texture_stream_installs_before(near_old, near_old));
}

NTEST(texture_stream, the_least_recently_asked_texture_gives_its_spare_level_first)
{
    NTEST_EXPECT_TRUE(texture_stream_evicts_before(10U, 20U));
    NTEST_EXPECT_FALSE(texture_stream_evicts_before(20U, 10U));
    NTEST_EXPECT_TRUE(texture_stream_evicts_before(NEVER, 10U));
    NTEST_EXPECT_FALSE(texture_stream_evicts_before(10U, NEVER));
    NTEST_EXPECT_FALSE(texture_stream_evicts_before(10U, 10U));
}

NTEST(texture_stream, a_pages_texels_lie_in_the_levels_rows)
{
    // BC7 (16-byte blocks of 4 x 4), 1024 wide: level 1 is 128 blocks a row
    // (2,048 bytes); 256 x 256 pages are 64 blocks a side, so page (1, 2)
    // starts 128 block rows down and 64 blocks in.
    static const u8 level[1] = {0U};
    u64 pitch = 0U;
    const u8 *at = texture_stream_page_texels(level, nph_gpu_format_info(NPH_GPU_FORMAT_BC7_SRGB),
                                              1024U, 1U, 256U, 256U, 1U, 2U, &pitch);
    NTEST_EXPECT_EQ_UINT(pitch, 2048U);
    NTEST_EXPECT_EQ_UINT((u64)(at - level), 128U * 2048U + 64U * 16U);
    // RGBA8 (a texel a block), 1000 wide: level 0 rows of 4,000 bytes.
    at = texture_stream_page_texels(level, nph_gpu_format_info(NPH_GPU_FORMAT_R8G8B8A8_UNORM),
                                    1000U, 0U, 128U, 128U, 2U, 1U, &pitch);
    NTEST_EXPECT_EQ_UINT(pitch, 4000U);
    NTEST_EXPECT_EQ_UINT((u64)(at - level), 128U * 4000U + 256U * 4U);
}

// ---------------------------------------------------------------------------
// The stream over a stand-in for the renderer's paging
// ---------------------------------------------------------------------------

// Square RGBA8 textures (pages of 128 x 128 texels), small from the first
// level under a page: 512 wide pages levels 0 to 2 (16, 4, and 1 pages),
// 1024 wide levels 0 to 3 (64, 16, 4, 1). A page memory of `capacity`
// pages, whose evicted pages are free RETURN_FRAMES frames later; a
// staging of `staging` pages a frame; and the frame each level was last
// asked for, as the test sets it.
#define FAKE_TEXTURES 8U
#define FAKE_LEVELS   4U
#define FAKE_LOG      1024U
#define RETURN_FRAMES 2U
#define PAGE          ((u64)NPH_GPU_SPARSE_PAGE_BYTES)

// The levels' texels, level m's from byte 64 m: the stream reads none of
// them, the stand-in checks where each page's would be.
static u8 g_texels[4U << 20U];

typedef struct FakeTexture {
    b32 live;
    b32 whole; // not paged
    u32 width;
    u32 levels; // below the small ones
    u8 installed[FAKE_LEVELS][64];
    u64 asked[NPH_GPU_MAX_MIPS];
} FakeTexture;

typedef struct FakeInstall {
    u32 texture;
    u32 mip;
    u32 x;
    u32 y;
} FakeInstall;

typedef struct FakeFixture {
    FakeTexture textures[FAKE_TEXTURES];
    u32 count;
    u32 capacity;
    u32 used;                     // pages installed
    u32 returning[RETURN_FRAMES]; // evicted, by frame % RETURN_FRAMES
    u32 staging;
    u32 staging_left;
    u64 frame;
    NphResult fail; // the installs' result, other than OK
    u32 installs;   // in all
    u32 evicts;
    u32 bad_texels; // installs whose texels or pitch were not the page's
    FakeInstall log[FAKE_LOG];
    u32 log_count;
    const void *mips[NPH_GPU_MAX_MIPS];
    NphTextureStream *stream;
} FakeFixture;

NTEST_SETUP(FakeFixture)
{
    memset(f, 0, sizeof *f);
    f->capacity = 4096U;
    f->staging = 64U;
    f->staging_left = f->staging;
    f->frame = 100U;
    for (u32 m = 0; m < NPH_GPU_MAX_MIPS; ++m) {
        f->mips[m] = g_texels + (u64)m * 64U;
    }
}

NTEST_TEARDOWN(FakeFixture)
{
    nph_texture_stream_destroy(f->stream);
}

static FakeTexture *fake_texture(FakeFixture *f, NphRenderTexture texture)
{
    const u32 i = texture.value - 1U;
    return texture.value != 0U && i < f->count && f->textures[i].live ? &f->textures[i] : NULL;
}

static u32 fake_returning(const FakeFixture *f)
{
    u32 n = 0U;
    for (u32 i = 0; i < RETURN_FRAMES; ++i) {
        n += f->returning[i];
    }
    return n;
}

static u32 fake_pages_x(const FakeTexture *t, u32 mip)
{
    return (t->width >> mip) / 128U;
}

static NphResult fake_pages(void *user, NphRenderTexture texture, NphRenderTexturePages *out)
{
    memset(out, 0, sizeof *out);
    const FakeTexture *t = fake_texture((FakeFixture *)user, texture);
    if (t == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (t->whole) {
        return NPH_RESULT_OK;
    }
    out->paged = 1;
    out->format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    out->width = t->width;
    out->height = t->width;
    out->page_width = 128U;
    out->page_height = 128U;
    out->tail_first_mip = t->levels;
    for (u32 m = 0; m < t->levels; ++m) {
        out->pages_x[m] = fake_pages_x(t, m);
        out->pages_y[m] = fake_pages_x(t, m);
    }
    return NPH_RESULT_OK;
}

static NphResult fake_levels_requested(void *user, NphRenderTexture texture, u64 *out)
{
    const FakeTexture *t = fake_texture((FakeFixture *)user, texture);
    for (u32 m = 0; m < NPH_GPU_MAX_MIPS; ++m) {
        out[m] = t != NULL ? t->asked[m] : NEVER;
    }
    return t != NULL ? NPH_RESULT_OK : NPH_RESULT_INVALID_ARGUMENT;
}

static NphResult fake_install(void *user, NphRenderTexture texture, u32 mip, u32 x, u32 y,
                              const void *texels, u64 row_pitch)
{
    FakeFixture *f = (FakeFixture *)user;
    FakeTexture *t = fake_texture(f, texture);
    if (t == NULL || mip >= t->levels || x >= fake_pages_x(t, mip) || y >= fake_pages_x(t, mip)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (f->fail != NPH_RESULT_OK) {
        return f->fail;
    }
    u8 *page = &t->installed[mip][y * fake_pages_x(t, mip) + x];
    if (*page != 0U) {
        return NPH_RESULT_OK;
    }
    if (f->staging_left == 0U || f->used + fake_returning(f) == f->capacity) {
        return NPH_RESULT_FULL;
    }
    // The page's rectangle in its level's rows: 128 rows down a row of
    // pages, 128 texels in a page.
    const u64 pitch = (u64)(t->width >> mip) * 4U;
    const u8 *at = g_texels + (u64)mip * 64U + (u64)y * 128U * pitch + (u64)x * 128U * 4U;
    f->bad_texels += (const u8 *)texels != at || row_pitch != pitch ? 1U : 0U;
    *page = 1U;
    f->used += 1U;
    f->staging_left -= 1U;
    f->installs += 1U;
    if (f->log_count < FAKE_LOG) {
        f->log[f->log_count++] = (FakeInstall){texture.value - 1U, mip, x, y};
    }
    return NPH_RESULT_OK;
}

static void fake_evict(void *user, NphRenderTexture texture, u32 mip, u32 x, u32 y)
{
    FakeFixture *f = (FakeFixture *)user;
    FakeTexture *t = fake_texture(f, texture);
    if (t == NULL || mip >= t->levels) {
        return; // a texture destroyed: its pages went with it
    }
    u8 *page = &t->installed[mip][y * fake_pages_x(t, mip) + x];
    if (*page == 0U) {
        return;
    }
    *page = 0U;
    f->used -= 1U;
    f->returning[f->frame % RETURN_FRAMES] += 1U;
    f->evicts += 1U;
}

static void fake_room(void *user, NphRenderPageRoom *out)
{
    const FakeFixture *f = (const FakeFixture *)user;
    out->pages = f->capacity;
    out->pages_returning = fake_returning(f);
    out->pages_free = f->capacity - f->used - out->pages_returning;
    out->staging_free = f->staging_left;
}

static u64 fake_frame_index(void *user)
{
    return ((const FakeFixture *)user)->frame;
}

static TextureStreamPaging fake_paging(FakeFixture *f)
{
    const TextureStreamPaging p = {
        f, fake_pages, fake_levels_requested, fake_install, fake_evict, fake_room, fake_frame_index,
    };
    return p;
}

// A texture `width` wide (512 or 1024), never asked for.
static NphRenderTexture fake_add_texture(FakeFixture *f, u32 width)
{
    FakeTexture *t = &f->textures[f->count++];
    t->live = 1;
    t->width = width;
    t->levels = width == 1024U ? 4U : 3U;
    for (u32 m = 0; m < NPH_GPU_MAX_MIPS; ++m) {
        t->asked[m] = NEVER;
    }
    return (NphRenderTexture){f->count};
}

static void fake_stream(FakeFixture *f, u64 budget_pages, u32 max_pages, u32 hold)
{
    NphTextureStreamDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.max_textures = FAKE_TEXTURES;
    d.budget_bytes = budget_pages * PAGE;
    d.max_pages = max_pages;
    d.hold_frames = hold;
    const TextureStreamPaging paging = fake_paging(f);
    NTEST_ASSERT_EQ_INT(texture_stream_create_with(&d, &paging, &f->stream), NPH_RESULT_OK);
}

static void fake_stream_add(FakeFixture *f, NphRenderTexture texture)
{
    const NphTextureStreamTexture st = {texture, f->mips};
    NTEST_ASSERT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_OK);
}

// The pixels ask for level `mip` of `texture` this frame.
static void fake_ask(FakeFixture *f, NphRenderTexture texture, u32 mip)
{
    f->textures[texture.value - 1U].asked[mip] = f->frame;
}

// An update, then the next frame: the staging empty again, and the pages
// evicted RETURN_FRAMES frames before it free.
static NphTextureStreamStats fake_update(FakeFixture *f)
{
    NTEST_ASSERT_EQ_INT(nph_texture_stream_update(f->stream), NPH_RESULT_OK);
    NphTextureStreamStats st;
    nph_texture_stream_stats(f->stream, &st);
    f->frame += 1U;
    f->returning[f->frame % RETURN_FRAMES] = 0U;
    f->staging_left = f->staging;
    return st;
}

static NphTextureStreamTextureState fake_state(const FakeFixture *f, NphRenderTexture texture)
{
    NphTextureStreamTextureState st;
    NTEST_ASSERT_EQ_INT(nph_texture_stream_texture_state(f->stream, texture, &st), NPH_RESULT_OK);
    return st;
}

static NphTextureStreamStats stream_stats(const FakeFixture *f)
{
    NphTextureStreamStats st;
    nph_texture_stream_stats(f->stream, &st);
    return st;
}

// Asked for level 0, a texture installs its levels from the coarsest, each
// whole before the next begins, a level an update, within the update's
// pages, each page's texels its rectangle of the level.
NTEST_F(FakeFixture, the_levels_install_coarse_first_within_the_pages_of_an_update)
{
    const NphRenderTexture t = fake_add_texture(f, 1024U);
    fake_stream(f, 1000U, 8U, 0U);
    fake_stream_add(f, t);
    u32 updates = 0U;
    for (; updates < 40U && fake_state(f, t).held != 0U; ++updates) {
        fake_ask(f, t, 0U);
        NTEST_EXPECT_LE_UINT(fake_update(f).installed, 8U);
    }
    // Levels 3 and 2 an update each, then level 1 in 2, level 0 in 8.
    NTEST_EXPECT_EQ_UINT(updates, 12U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, t).held, 0U);
    NTEST_EXPECT_EQ_UINT(f->installs, 85U);
    NTEST_EXPECT_EQ_UINT(f->bad_texels, 0U);
    for (u32 i = 1; i < f->log_count; ++i) {
        const FakeInstall a = f->log[i - 1U];
        const FakeInstall b = f->log[i];
        NTEST_EXPECT_TRUE(b.mip < a.mip || (b.mip == a.mip && b.y * 8U + b.x > a.y * 8U + a.x));
    }
    NphTextureStreamStats st = stream_stats(f);
    NTEST_EXPECT_EQ_UINT(st.textures, 1U);
    NTEST_EXPECT_EQ_UINT(st.held_levels, 4U);
    NTEST_EXPECT_EQ_UINT(st.held_bytes, 85U * PAGE);
    NTEST_EXPECT_EQ_UINT(st.short_textures, 0U);
    NTEST_EXPECT_EQ_UINT(st.asked, 1U);
    // Settled, nothing moves.
    fake_ask(f, t, 0U);
    st = fake_update(f);
    NTEST_EXPECT_EQ_UINT(st.installed + st.evicted, 0U);
}

// Eight textures at their small levels, wanting levels 0, 1, or 2, asked
// at different frames: each installs its first page in the order of their
// needs, the furthest from its wanted level first, then the most recently
// asked, then the texture held longer.
NTEST_F(FakeFixture, the_textures_furthest_from_their_wanted_level_install_first)
{
    fake_stream(f, 1000U, 8U, 50U);
    static const u32 wanted[8] = {2U, 0U, 1U, 0U, 2U, 1U, 0U, 1U};
    static const u32 age[8] = {5U, 9U, 1U, 2U, 0U, 1U, 9U, 30U}; // frames since asked
    for (u32 i = 0; i < 8U; ++i) {
        const NphRenderTexture t = fake_add_texture(f, 512U);
        fake_stream_add(f, t);
        f->textures[i].asked[wanted[i]] = f->frame - age[i];
    }
    const NphTextureStreamStats st = fake_update(f);
    // Wanting level 0: 3 (2 frames), then 1 and 6 (9, the order held);
    // level 1: 2 and 5 (1), then 7 (30); level 2: 4 (0), then 0 (5).
    static const u32 expected[8] = {3U, 1U, 6U, 2U, 5U, 7U, 4U, 0U};
    NTEST_ASSERT_EQ_UINT(f->log_count, 8U);
    for (u32 i = 0; i < 8U; ++i) {
        NTEST_EXPECT_EQ_UINT(f->log[i].texture, expected[i]);
        NTEST_EXPECT_EQ_UINT(f->log[i].mip, 2U);
    }
    NTEST_EXPECT_EQ_UINT(st.asked, 8U);
    NTEST_EXPECT_EQ_UINT(st.short_textures, 6U); // those wanting level 2 have it
}

// Room for two textures' levels: A and B held whole, then neither asked
// for, A the longer ago, and C asked for: A's levels leave for C's, the
// finest first, while B's stay. Within the budget, nothing left before.
NTEST_F(FakeFixture, past_the_budget_what_no_texture_wants_leaves_the_least_recently_asked_first)
{
    const NphRenderTexture a = fake_add_texture(f, 512U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    const NphRenderTexture c = fake_add_texture(f, 512U);
    fake_stream(f, 42U, 32U, 4U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    fake_stream_add(f, c);
    for (u32 i = 0; i < 8U; ++i) {
        fake_ask(f, a, 0U);
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_EQ_UINT(fake_state(f, a).held, 0U);
    NTEST_ASSERT_EQ_UINT(fake_state(f, b).held, 0U);
    for (u32 i = 0; i < 3U; ++i) {
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    for (u32 i = 0; i < 5U; ++i) {
        (void)fake_update(f); // past the hold of B's last ask
    }
    NTEST_EXPECT_EQ_UINT(fake_state(f, a).wanted, 3U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).wanted, 3U);
    NTEST_EXPECT_EQ_UINT(f->evicts, 0U);
    NTEST_EXPECT_EQ_UINT(stream_stats(f).asked, 0U);
    for (u32 i = 0; i < 6U; ++i) {
        fake_ask(f, c, 0U);
        (void)fake_update(f);
    }
    NTEST_EXPECT_EQ_UINT(fake_state(f, c).held, 0U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, a).held, 3U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).held, 0U);
    NTEST_EXPECT_EQ_UINT(f->evicts, 21U);
    for (u32 p = 0; p < 16U; ++p) {
        NTEST_EXPECT_EQ_UINT(f->textures[0].installed[0][p], 0U);
        NTEST_EXPECT_EQ_UINT(f->textures[1].installed[0][p], 1U);
    }
    const NphTextureStreamStats st = stream_stats(f);
    NTEST_EXPECT_EQ_UINT(st.held_bytes, 42U * PAGE);
    NTEST_EXPECT_EQ_UINT(st.held_levels, 6U);
}

// A budget too small for two textures' wanted levels: the one ahead holds
// its own, the other stays short, and nothing moves from then on.
NTEST_F(FakeFixture, a_budget_full_of_wanted_levels_leaves_a_texture_short_without_trading)
{
    const NphRenderTexture a = fake_add_texture(f, 512U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    fake_stream(f, 30U, 32U, 0U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    NphTextureStreamStats st;
    memset(&st, 0, sizeof st);
    for (u32 i = 0; i < 8U; ++i) {
        fake_ask(f, a, 0U);
        fake_ask(f, b, 0U);
        st = fake_update(f);
    }
    NTEST_EXPECT_EQ_UINT(fake_state(f, a).held, 0U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).held, 1U);
    NTEST_EXPECT_EQ_UINT(st.short_textures, 1U);
    NTEST_EXPECT_EQ_UINT(st.held_bytes, 26U * PAGE);
    for (u32 i = 0; i < 20U; ++i) {
        fake_ask(f, a, 0U);
        fake_ask(f, b, 0U);
        st = fake_update(f);
        NTEST_EXPECT_EQ_UINT(st.installed + st.evicted, 0U);
    }
    NTEST_EXPECT_EQ_UINT(f->evicts, 0U);
}

// A texture that stops wanting the level it is installing, a page an
// update: once level 0's last ask is past the hold, the pages of it
// installed leave, and their room with them.
NTEST_F(FakeFixture, a_level_its_texture_stops_wanting_leaves_while_installing)
{
    const NphRenderTexture t = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 1U, 4U);
    fake_stream_add(f, t);
    for (u32 i = 0; i < 6U; ++i) {
        fake_ask(f, t, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_EQ_UINT(fake_state(f, t).held, 1U);
    NTEST_ASSERT_TRUE(fake_state(f, t).loading != 0);
    // Level 1 alone asked for: level 0 is still wanted for the hold's 4
    // updates (5 of its pages in all), then leaves.
    for (u32 i = 0; i < 5U; ++i) {
        fake_ask(f, t, 1U);
        (void)fake_update(f);
    }
    const NphTextureStreamTextureState st = fake_state(f, t);
    NTEST_EXPECT_EQ_UINT(st.wanted, 1U);
    NTEST_EXPECT_EQ_UINT(st.held, 1U);
    NTEST_EXPECT_FALSE(st.loading != 0);
    NTEST_EXPECT_EQ_UINT(f->evicts, 5U);
    for (u32 p = 0; p < 16U; ++p) {
        NTEST_EXPECT_EQ_UINT(f->textures[0].installed[0][p], 0U);
    }
    const NphTextureStreamStats ss = stream_stats(f);
    NTEST_EXPECT_EQ_UINT(ss.held_bytes, 5U * PAGE);
    NTEST_EXPECT_EQ_UINT(ss.loading, 0U);
}

// A page memory that holds less than the budget: full of pages none of
// which are returning, a level no texture wants leaves, and only as much
// as the texture asked for needs: A gives its level 0 alone, and B's
// installs wait for its pages to return.
NTEST_F(FakeFixture, a_page_memory_smaller_than_the_budget_lets_spare_levels_go)
{
    f->capacity = 30U;
    const NphRenderTexture a = fake_add_texture(f, 512U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 32U, 2U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    for (u32 i = 0; i < 4U; ++i) {
        fake_ask(f, a, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_EQ_UINT(fake_state(f, a).held, 0U);
    u32 frames = 0U;
    for (; frames < 30U && fake_state(f, b).held != 0U; ++frames) {
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    NTEST_EXPECT_LT_UINT(frames, 30U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).held, 0U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, a).held, 1U);
    NTEST_EXPECT_EQ_UINT(f->evicts, 16U);
    NTEST_EXPECT_EQ_UINT(f->used, 26U);
}

// A staging of two pages a frame, with a spare texture held whole: the
// installs wait for the staging, and nothing is evicted for it.
NTEST_F(FakeFixture, a_full_staging_only_waits)
{
    f->staging = 2U;
    f->staging_left = 2U;
    const NphRenderTexture a = fake_add_texture(f, 512U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 32U, 2U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    u32 frames = 0U;
    for (; frames < 40U && fake_state(f, a).held != 0U; ++frames) {
        fake_ask(f, a, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_EQ_UINT(fake_state(f, a).held, 0U);
    for (frames = 0U; frames < 40U && fake_state(f, b).held != 0U; ++frames) {
        fake_ask(f, b, 0U);
        NTEST_EXPECT_LE_UINT(fake_update(f).installed, 2U);
    }
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).held, 0U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, a).held, 0U);
    NTEST_EXPECT_EQ_UINT(f->evicts, 0U);
}

// A texture removed while installing: what the stream installed of it
// leaves at once, and the rest stream on.
NTEST_F(FakeFixture, a_texture_removed_gives_back_what_the_stream_installed)
{
    const NphRenderTexture a = fake_add_texture(f, 512U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 4U, 0U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    for (u32 i = 0; i < 4U; ++i) {
        fake_ask(f, a, 0U);
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_EQ_UINT(fake_state(f, a).held, 1U);
    NTEST_ASSERT_TRUE(fake_state(f, a).loading != 0);
    nph_texture_stream_remove(f->stream, a);
    NTEST_EXPECT_EQ_UINT(f->evicts, 9U); // levels 2 and 1, and 4 pages of level 0
    NphTextureStreamTextureState st;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_texture_state(f->stream, a, &st), NPH_RESULT_NOT_FOUND);
    const NphTextureStreamStats ss = stream_stats(f);
    NTEST_EXPECT_EQ_UINT(ss.textures, 1U);
    NTEST_EXPECT_EQ_UINT(ss.held_levels, 2U);
    NTEST_EXPECT_EQ_UINT(ss.held_bytes, 5U * PAGE);
    NTEST_EXPECT_EQ_UINT(ss.evicted, 9U);
    nph_texture_stream_remove(f->stream, a);
    NTEST_EXPECT_EQ_UINT(f->evicts, 9U);
    for (u32 i = 0; i < 8U; ++i) {
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).held, 0U);
    for (u32 m = 0; m < 3U; ++m) {
        for (u32 p = 0; p < 16U; ++p) {
            NTEST_EXPECT_EQ_UINT(f->textures[0].installed[m][p], 0U);
        }
    }
}

// A texture destroyed under the stream (its pages with it): the update lets
// it go, evicting nothing, and the texture that takes its place among
// those held streams on.
NTEST_F(FakeFixture, a_texture_destroyed_under_the_stream_is_let_go)
{
    const NphRenderTexture a = fake_add_texture(f, 512U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    const NphRenderTexture c = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 32U, 0U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    fake_stream_add(f, c);
    for (u32 i = 0; i < 4U; ++i) {
        fake_ask(f, a, 0U);
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_EQ_UINT(fake_state(f, a).held, 0U);
    f->textures[0].live = 0;
    f->used -= 21U;
    for (u32 i = 0; i < 4U; ++i) {
        fake_ask(f, b, 0U);
        fake_ask(f, c, 0U);
        (void)fake_update(f);
    }
    NTEST_EXPECT_EQ_UINT(f->evicts, 0U);
    NphTextureStreamTextureState st;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_texture_state(f->stream, a, &st), NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_UINT(fake_state(f, b).held, 0U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, c).held, 0U);
    const NphTextureStreamStats ss = stream_stats(f);
    NTEST_EXPECT_EQ_UINT(ss.textures, 2U);
    NTEST_EXPECT_EQ_UINT(ss.held_levels, 6U);
    NTEST_EXPECT_EQ_UINT(ss.held_bytes, 42U * PAGE);
}

// Destroyed with levels held and one being installed, the stream evicts
// every page it installed.
NTEST_F(FakeFixture, destroying_the_stream_evicts_every_page_it_installed)
{
    const NphRenderTexture a = fake_add_texture(f, 1024U);
    const NphRenderTexture b = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 8U, 0U);
    fake_stream_add(f, a);
    fake_stream_add(f, b);
    for (u32 i = 0; i < 5U; ++i) {
        fake_ask(f, a, 0U);
        fake_ask(f, b, 0U);
        (void)fake_update(f);
    }
    NTEST_ASSERT_TRUE(fake_state(f, a).loading != 0);
    NTEST_ASSERT_GT_UINT(f->used, 0U);
    nph_texture_stream_destroy(f->stream);
    f->stream = NULL;
    NTEST_EXPECT_EQ_UINT(f->used, 0U);
    NTEST_EXPECT_EQ_UINT(f->evicts, f->installs);
}

// What create and add refuse; what a lookup of a texture not held says.
NTEST_F(FakeFixture, the_stream_refuses_what_it_cannot_take)
{
    NphTextureStreamDesc d;
    memset(&d, 0, sizeof d);
    d.alloc = nph_allocator_system();
    d.max_textures = 2U;
    d.budget_bytes = PAGE;
    NphTextureStream *s = NULL;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_create(&d, &s), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(s);
    const TextureStreamPaging paging = fake_paging(f);
    d.alloc = NULL;
    NTEST_EXPECT_EQ_INT(texture_stream_create_with(&d, &paging, &s), NPH_RESULT_INVALID_ARGUMENT);
    d.alloc = nph_allocator_system();
    d.max_textures = 0U;
    NTEST_EXPECT_EQ_INT(texture_stream_create_with(&d, &paging, &s), NPH_RESULT_INVALID_ARGUMENT);
    d.max_textures = 2U;
    d.budget_bytes = 0U;
    NTEST_EXPECT_EQ_INT(texture_stream_create_with(&d, &paging, &s), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_EXPECT_NULL(s);
    d.budget_bytes = PAGE;
    NTEST_ASSERT_EQ_INT(texture_stream_create_with(&d, &paging, &f->stream), NPH_RESULT_OK);

    const NphRenderTexture whole = fake_add_texture(f, 512U);
    f->textures[0].whole = 1;
    const NphRenderTexture gone = fake_add_texture(f, 512U);
    f->textures[1].live = 0;
    const NphRenderTexture t = fake_add_texture(f, 512U);
    const NphRenderTexture u = fake_add_texture(f, 512U);
    const NphRenderTexture v = fake_add_texture(f, 512U);
    NphTextureStreamTexture st = {whole, f->mips};
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_INVALID_ARGUMENT);
    st.texture = gone;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_INVALID_ARGUMENT);
    st.texture = t;
    st.mips = NULL;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_INVALID_ARGUMENT);
    const void *missing[3] = {g_texels, NULL, g_texels};
    st.mips = missing;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_INVALID_ARGUMENT);
    st.mips = f->mips;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_OK);
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_ALREADY_EXISTS);
    st.texture = u;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_OK);
    st.texture = v;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_add(f->stream, &st), NPH_RESULT_FULL);
    NphTextureStreamTextureState state;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_texture_state(f->stream, v, &state),
                        NPH_RESULT_NOT_FOUND);
    NTEST_EXPECT_EQ_UINT(state.held + state.wanted, 0U);
    nph_texture_stream_remove(f->stream, v);
    NTEST_EXPECT_EQ_UINT(stream_stats(f).textures, 2U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, t).held, 3U);
    NTEST_EXPECT_EQ_UINT(fake_state(f, t).wanted, 3U);
}

// An install the renderer refuses other than for room fails the update.
NTEST_F(FakeFixture, an_install_refused_otherwise_fails_the_update)
{
    const NphRenderTexture t = fake_add_texture(f, 512U);
    fake_stream(f, 1000U, 32U, 0U);
    fake_stream_add(f, t);
    fake_ask(f, t, 0U);
    f->fail = NPH_RESULT_OUT_OF_MEMORY;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_update(f->stream), NPH_RESULT_OUT_OF_MEMORY);
    f->fail = NPH_RESULT_OK;
    NTEST_EXPECT_EQ_INT(nph_texture_stream_update(f->stream), NPH_RESULT_OK);
}

// ---------------------------------------------------------------------------
// The stream over a paged texture
// ---------------------------------------------------------------------------

#define TEX      1024U // RGBA8: 128 x 128 pages, 8 x 8 at level 0, small from level 4
#define PIXELS   ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)
#define MAX_MIPS 11U

typedef struct StreamFixture {
    RenderFixture rf;
    u32 count;
    u8 *data[MAX_MIPS];
    u64 size[MAX_MIPS];
    NphRenderMesh plane;
    NphRenderMaterial material;
    NphRenderInstance instance;
    NphRenderLight light;
} StreamFixture;

NTEST_SETUP(StreamFixture)
{
    memset(f, 0, sizeof *f);
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.max_anisotropy = 8.0F;
    test_render_setup(&f->rf, &d);
    // A checker of 8-texel squares, each level tinted its own (its green,
    // 40 + 20 m), so that any level drawn shows.
    f->count = nph_gpu_mip_count(TEX, TEX, 1U);
    for (u32 m = 0; m < f->count; ++m) {
        const u32 w = nph_gpu_mip_extent(TEX, m);
        f->size[m] = (u64)w * w * 4U;
        f->data[m] = NPH_ALLOC_ARRAY(nph_allocator_system(), u8, f->size[m]);
        NTEST_ASSERT_NOT_NULL(f->data[m]);
        for (u32 y = 0; y < w; ++y) {
            for (u32 x = 0; x < w; ++x) {
                u8 *t = f->data[m] + ((u64)y * w + x) * 4U;
                t[0] = (u8)((((x >> 3U) ^ (y >> 3U)) & 1U) != 0U ? 230U : 30U);
                t[1] = (u8)(40U + 20U * m);
                t[2] = (u8)((x * 7U + y * 3U) & 0xFFU);
                t[3] = 255U;
            }
        }
    }
}

NTEST_TEARDOWN(StreamFixture)
{
    if (f->rf.renderer != NULL && f->instance.value != 0U) {
        nph_render_light_destroy(f->rf.renderer, f->light);
        nph_render_instance_destroy(f->rf.renderer, f->instance);
        nph_render_material_destroy(f->rf.renderer, f->material);
        nph_render_mesh_destroy(f->rf.renderer, f->plane);
    }
    for (u32 m = 0; m < f->count; ++m) {
        NPH_FREE_ARRAY(nph_allocator_system(), u8, f->data[m], f->size[m]);
    }
    test_render_teardown(&f->rf);
}

#define REQUIRE_PAGING(f)                                                                          \
    do {                                                                                           \
        REQUIRE_RENDERER(&(f)->rf);                                                                \
        if (!nph_renderer_texture_paging_supported((f)->rf.renderer)) {                            \
            NTEST_SKIP("no texture paging on this device");                                        \
        }                                                                                          \
    } while (0)

static NphRenderTexture make_texture(StreamFixture *f, bool paged)
{
    NphRenderTextureDesc td;
    memset(&td, 0, sizeof td);
    td.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    td.width = TEX;
    td.height = TEX;
    td.mip_count = f->count;
    td.mips = (const void *const *)f->data;
    td.mip_sizes = f->size;
    td.flags = paged ? (u32)NPH_RENDER_TEXTURE_FLAG_PAGED : 0U;
    td.name = paged ? "streamed texture" : "whole texture";
    NphRenderTexture t = NPH_RENDER_TEXTURE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_texture_create(f->rf.renderer, &td, &t), NPH_RESULT_OK);
    return t;
}

static void set_texture(StreamFixture *f, NphRenderTexture t)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.roughness = 0.6F;
    md.textures[NPH_RENDER_SLOT_BASE_COLOR].texture = t;
    if (f->material.value == 0U) {
        NTEST_ASSERT_EQ_INT(nph_render_material_create(f->rf.renderer, &md, &f->material),
                            NPH_RESULT_OK);
    } else {
        NTEST_ASSERT_EQ_INT(nph_render_material_update(f->rf.renderer, f->material, &md),
                            NPH_RESULT_OK);
    }
}

// The plane (+Y up, through the origin, 4 m) with `t` on it, lit from above.
static void make_scene(StreamFixture *f, NphRenderTexture t)
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(nph_render_primitive_plane(nph_allocator_system(), 4.0F, 1U, 1.0F, &data),
                        NPH_RESULT_OK);
    f->plane = test_upload_mesh(f->rf.renderer, &data);
    nph_render_mesh_data_free(&data);
    set_texture(f, t);
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = f->plane;
    id.materials = &f->material;
    id.material_count = 1U;
    id.transform = nph_render_transform_identity();
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->rf.renderer, &id, &f->instance),
                        NPH_RESULT_OK);
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 2.0F;
    ld.direction = nph_vec3_normalize(nph_vec3(0.2F, -1.0F, -0.3F));
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->rf.renderer, &ld, &f->light), NPH_RESULT_OK);
}

// The plane seen from `height` above its centre, unlit.
static NphRenderView view_from(f64 height)
{
    const NphRenderCamera cam =
        nph_render_camera_look_at(nph_dvec3(0.0, height, 0.0), nph_dvec3(0.01, 0.0, -0.01),
                                  nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.02F);
    NphRenderView v;
    nph_render_view_default(&v, cam);
    v.ev100 = 0.0F;
    v.tonemapper = NPH_RENDER_TONEMAPPER_NONE;
    v.background = NPH_RENDER_BACKGROUND_SOLID;
    v.background_color = nph_vec3_zero();
    v.disable_ibl = 1;
    v.anti_aliasing = NPH_RENDER_AA_NONE;
    v.ao_radius = 0.0F;
    v.gi = 0;
    v.debug_view = NPH_RENDER_VIEW_BASE_COLOR;
    return v;
}

// A frame of `v` with the stream's update between the frame's begin and
// its rendering, read back into f->rf.pixels.
static NphRenderFrameStats stream_frame(StreamFixture *f, NphTextureStream *s,
                                        const NphRenderView *v)
{
    RenderFixture *rf = &f->rf;
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_begin(rf->dev), NPH_RESULT_OK);
    if (s != NULL) {
        NTEST_ASSERT_EQ_INT(nph_texture_stream_update(s), NPH_RESULT_OK);
    }
    nph_gpu_rg_begin(rf->graph);
    NTEST_ASSERT_EQ_INT(nph_render_frame(rf->renderer, v, rf->graph, rf->target, NULL, &stats),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_compile(rf->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_rg_execute(rf->graph), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_frame_end(rf->dev), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_gpu_device_wait_idle(rf->dev), NPH_RESULT_OK);
    test_read_image(rf->dev, rf->target, 0U, rf->pixels, PIXELS);
    return stats;
}

static NphTextureStream *make_stream(StreamFixture *f, u64 budget, u32 hold)
{
    NphTextureStreamDesc sd;
    memset(&sd, 0, sizeof sd);
    sd.alloc = nph_allocator_system();
    sd.renderer = f->rf.renderer;
    sd.max_textures = 4U;
    sd.budget_bytes = budget;
    sd.hold_frames = hold;
    NphTextureStream *s = NULL;
    NTEST_ASSERT_EQ_INT(nph_texture_stream_create(&sd, &s), NPH_RESULT_OK);
    return s;
}

static void add_texture(StreamFixture *f, NphTextureStream *s, NphRenderTexture t)
{
    NphTextureStreamTexture st;
    memset(&st, 0, sizeof st);
    st.texture = t;
    st.mips = (const void *const *)f->data;
    NTEST_ASSERT_EQ_INT(nph_texture_stream_add(s, &st), NPH_RESULT_OK);
}

static NphTextureStreamTextureState state_of(const NphTextureStream *s, NphRenderTexture t)
{
    NphTextureStreamTextureState st;
    NTEST_ASSERT_EQ_INT(nph_texture_stream_texture_state(s, t, &st), NPH_RESULT_OK);
    return st;
}

static u32 bytes_differing(const u8 *a, const u8 *b)
{
    u32 n = 0U;
    for (u64 i = 0; i < PIXELS; ++i) {
        n += a[i] != b[i] ? 1U : 0U;
    }
    return n;
}

// From near, level 0 under the view's centre is asked for: the stream
// installs levels 3, 2, 1 and 0 in turn, and the image ends the whole
// texture's, byte for byte.
NTEST_F(StreamFixture, the_stream_installs_the_levels_the_pixels_ask_for)
{
    REQUIRE_PAGING(f);
    const NphRenderTexture whole = make_texture(f, false);
    const NphRenderTexture paged = make_texture(f, true);
    make_scene(f, whole);
    const NphRenderView near = view_from(0.15);
    static u8 expected[PIXELS];
    for (u32 i = 0; i < 4U; ++i) {
        (void)stream_frame(f, NULL, &near);
    }
    memcpy(expected, f->rf.pixels, PIXELS);
    set_texture(f, paged);
    NphTextureStream *s = make_stream(f, 1000U * PAGE, 0U);
    add_texture(f, s, paged);
    u32 held_seen[5] = {0U, 0U, 0U, 0U, 0U};
    u32 frames = 0U;
    for (; frames < 80U; ++frames) {
        (void)stream_frame(f, s, &near);
        const NphTextureStreamTextureState st = state_of(s, paged);
        held_seen[st.held < 4U ? st.held : 4U] = 1U;
        if (st.held == 0U && nph_render_texture_first_resident_mip(f->rf.renderer, paged) == 0U &&
            bytes_differing(expected, f->rf.pixels) == 0U) {
            break;
        }
    }
    NphTextureStreamStats stats;
    nph_texture_stream_stats(s, &stats);
    const NphTextureStreamTextureState st = state_of(s, paged);
    NTEST_LOG("settled in %u frames: held %u, wanted %u, %u levels, %llu bytes", frames, st.held,
              st.wanted, stats.held_levels, (unsigned long long)stats.held_bytes);
    NTEST_EXPECT_LT_UINT(frames, 80U);
    NTEST_EXPECT_EQ_UINT(st.wanted, 0U);
    NTEST_EXPECT_EQ_UINT(st.held, 0U);
    NTEST_EXPECT_EQ_UINT(stats.held_levels, 4U);
    NTEST_EXPECT_EQ_UINT(stats.held_bytes, 85U * PAGE);
    NTEST_EXPECT_EQ_UINT(bytes_differing(expected, f->rf.pixels), 0U);
    // Coarse first: every level was held on the way.
    for (u32 m = 0; m < 4U; ++m) {
        NTEST_EXPECT_EQ_UINT(held_seen[m], 1U);
    }
    // Settled, it moves nothing more.
    for (u32 i = 0; i < 4U; ++i) {
        (void)stream_frame(f, s, &near);
        nph_texture_stream_stats(s, &stats);
        NTEST_EXPECT_EQ_UINT(stats.installed + stats.evicted, 0U);
    }
    nph_texture_stream_destroy(s);
    set_texture(f, whole);
    nph_render_texture_destroy(f->rf.renderer, paged);
    nph_render_texture_destroy(f->rf.renderer, whole);
}

// From 3 m, the pixels ask for level 2 and coarser (the centre's footprint
// 11.6 texels across at level 0, and anisotropic filtering's two probes
// halving it: lambda 2.5): levels 0 and 1 stay out.
NTEST_F(StreamFixture, a_far_view_keeps_the_finer_levels_out)
{
    REQUIRE_PAGING(f);
    const NphRenderTexture paged = make_texture(f, true);
    make_scene(f, paged);
    NphTextureStream *s = make_stream(f, 1000U * PAGE, 0U);
    add_texture(f, s, paged);
    const NphRenderView far = view_from(3.0);
    for (u32 i = 0; i < 40U; ++i) {
        (void)stream_frame(f, s, &far);
    }
    const NphTextureStreamTextureState st = state_of(s, paged);
    NTEST_LOG("far: held %u, wanted %u", st.held, st.wanted);
    NTEST_EXPECT_GE_UINT(st.wanted, 2U);
    NTEST_EXPECT_EQ_UINT(st.held, st.wanted);
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->rf.renderer, paged, 0U, 3U, 3U),
                        NPH_RENDER_PAGE_ABSENT);
    NTEST_EXPECT_EQ_INT(nph_render_texture_page_state(f->rf.renderer, paged, 1U, 1U, 1U),
                        NPH_RENDER_PAGE_ABSENT);
    nph_texture_stream_destroy(s);
    nph_render_texture_destroy(f->rf.renderer, paged);
}

// A budget that levels 1 to 3 fill (21 pages): from near, level 0 does not
// fit, and the texture stays short at level 1, with nothing moving.
NTEST_F(StreamFixture, a_budget_full_of_wanted_levels_leaves_the_texture_short)
{
    REQUIRE_PAGING(f);
    const NphRenderTexture paged = make_texture(f, true);
    make_scene(f, paged);
    NphTextureStream *s = make_stream(f, 21U * PAGE, 0U);
    add_texture(f, s, paged);
    const NphRenderView near = view_from(0.15);
    for (u32 i = 0; i < 40U; ++i) {
        (void)stream_frame(f, s, &near);
    }
    NphTextureStreamStats stats;
    nph_texture_stream_stats(s, &stats);
    const NphTextureStreamTextureState st = state_of(s, paged);
    NTEST_LOG("short: held %u, wanted %u, %llu bytes", st.held, st.wanted,
              (unsigned long long)stats.held_bytes);
    NTEST_EXPECT_EQ_UINT(st.wanted, 0U);
    NTEST_EXPECT_EQ_UINT(st.held, 1U);
    NTEST_EXPECT_EQ_UINT(stats.short_textures, 1U);
    NTEST_EXPECT_LE_UINT(stats.held_bytes, 21U * PAGE);
    NTEST_EXPECT_EQ_UINT(stats.installed + stats.evicted, 0U);
    nph_texture_stream_destroy(s);
    nph_render_texture_destroy(f->rf.renderer, paged);
}

// Two textures, room for one's levels: the plane first shows `a` from near,
// then `b`. Once `a` has gone unasked past the hold, its levels leave for
// `b`'s, and the image is `b`'s whole.
NTEST_F(StreamFixture, a_texture_no_longer_asked_gives_its_room_to_one_that_is)
{
    REQUIRE_PAGING(f);
    const NphRenderTexture a = make_texture(f, true);
    const NphRenderTexture b = make_texture(f, true);
    make_scene(f, a);
    NphTextureStream *s = make_stream(f, 85U * PAGE, 8U);
    add_texture(f, s, a);
    add_texture(f, s, b);
    const NphRenderView near = view_from(0.15);
    for (u32 i = 0; i < 40U; ++i) {
        (void)stream_frame(f, s, &near);
    }
    NTEST_EXPECT_EQ_UINT(state_of(s, a).held, 0U);
    set_texture(f, b);
    u32 frames = 0U;
    for (; frames < 80U; ++frames) {
        (void)stream_frame(f, s, &near);
        if (state_of(s, b).held == 0U &&
            nph_render_texture_first_resident_mip(f->rf.renderer, b) == 0U) {
            break;
        }
    }
    const NphTextureStreamTextureState sa = state_of(s, a);
    const NphTextureStreamTextureState sb = state_of(s, b);
    NTEST_LOG("after %u frames: a held %u (wanted %u), b held %u (wanted %u)", frames, sa.held,
              sa.wanted, sb.held, sb.wanted);
    NTEST_EXPECT_LT_UINT(frames, 80U);
    NTEST_EXPECT_EQ_UINT(sb.held, 0U);
    NTEST_EXPECT_EQ_UINT(sa.wanted, 4U);
    NTEST_EXPECT_GE_UINT(sa.held, 1U);
    NphTextureStreamStats stats;
    nph_texture_stream_stats(s, &stats);
    NTEST_EXPECT_LE_UINT(stats.held_bytes, 85U * PAGE);
    nph_texture_stream_destroy(s);
    set_texture(f, a);
    nph_render_texture_destroy(f->rf.renderer, b);
    nph_render_texture_destroy(f->rf.renderer, a);
}
