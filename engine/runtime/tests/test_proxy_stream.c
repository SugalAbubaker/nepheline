// SPDX-License-Identifier: Apache-2.0
// Proxy streaming's decisions (runtime/proxy_stream.h; ADR-0063 stage 3),
// pure and apart from a renderer: the level a cell is wanted at, what a
// level is worth holding, when one pushes another out past the budget, and
// the picks an update keeps of the levels to load and drop (stage 4).
// The stream itself, over a cooked world, is tests/integration's
// test_proxy_stream.c.
#include <nepheline/math/random.h>
#include <nepheline/math/scalar.h>
#include <nepheline/runtime/proxy_stream.h>

#include <ntest/ntest.h>

#include "runtime_internal.h"

#include <string.h>

// Four levels, finest first, each error twice the finer one's.
static const f32 k_errors[4] = {0.5F, 1.0F, 2.0F, 4.0F};

NTEST(proxy_stream, the_scale_is_the_requests_reach_over_the_threshold)
{
    // 1000 pixels a unit at distance one, a pixel's threshold: from 100 m a
    // quarter nearer is 75 m.
    NTEST_EXPECT_NEAR_F32(proxy_stream_scale(100.0F, 1000.0F, 1.0F, 0.1F), 1000.0F / 75.0F,
                          1.0e-4F);
    NTEST_EXPECT_NEAR_F32(proxy_stream_scale(100.0F, 1000.0F, 2.0F, 0.1F), 1000.0F / 150.0F,
                          1.0e-4F);
    // Never nearer than the near plane.
    NTEST_EXPECT_NEAR_F32(proxy_stream_scale(0.0F, 1000.0F, 1.0F, 0.1F), 10000.0F, 1.0e-2F);
    // No levels of detail: 0.
    NTEST_EXPECT_NEAR_F32(proxy_stream_scale(100.0F, 1000.0F, 0.0F, 0.1F), 0.0F, 0.0F);
    NTEST_EXPECT_NEAR_F32(proxy_stream_scale(100.0F, 1000.0F, -1.0F, 0.1F), 0.0F, 0.0F);
}

NTEST(proxy_stream, a_cell_is_wanted_at_the_level_asked_for)
{
    const f32 scale = 1.0F;
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(0U, k_errors, 4U, 600.0F, 560.0F, scale), 0U);
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(2U, k_errors, 4U, 600.0F, 560.0F, scale), 2U);
    // Past its coarsest: its coarsest. Asked for although near: the views
    // know better.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(9U, k_errors, 4U, 600.0F, 560.0F, scale), 3U);
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(1U, k_errors, 4U, 10.0F, 560.0F, scale), 1U);
    // A view without levels of detail wants its coarsest whatever was asked.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(0U, k_errors, 4U, 600.0F, 560.0F, 0.0F), 3U);
}

NTEST(proxy_stream, unasked_a_cell_is_wanted_a_level_coarser_than_its_cut)
{
    const u32 none = NPH_RENDER_CELL_NO_REQUEST;
    // Nearer than its distance less a quarter: its instances are drawn.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 419.0F, 560.0F, 10.0F), 3U);
    // Past it, the cut's level is the first too coarse (error * scale past
    // 1), and the cell is wanted one coarser.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 420.0F, 560.0F, 2.1F), 1U);
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 420.0F, 560.0F, 1.1F), 2U);
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 420.0F, 560.0F, 0.6F), 3U);
    // A level exactly at the threshold is not too coarse.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 420.0F, 560.0F, 2.0F), 2U);
    // Every level fine enough, or only the coarsest too coarse: the coarsest.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 420.0F, 560.0F, 0.1F), 3U);
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 4U, 420.0F, 560.0F, 0.3F), 3U);
    // One level alone: it.
    NTEST_EXPECT_EQ_UINT(proxy_stream_wanted(none, k_errors, 1U, 420.0F, 560.0F, 100.0F), 0U);
}

NTEST(proxy_stream, a_level_is_worth_the_error_its_cell_shows_without_it)
{
    const ProxyStreamWorth w = proxy_stream_worth(k_errors, 1U, 1U, 3.0F);
    NTEST_EXPECT_TRUE(w.needed);
    NTEST_EXPECT_NEAR_F32(w.error, 3.0F, 0.0F);
    const ProxyStreamWorth spare = proxy_stream_worth(k_errors, 0U, 1U, 3.0F);
    NTEST_EXPECT_FALSE(spare.needed);
    NTEST_EXPECT_NEAR_F32(spare.error, 1.5F, 0.0F);
    // Within a cell a finer level is worth no more than a coarser one.
    for (u32 wanted = 0; wanted < 4U; ++wanted) {
        for (u32 level = 1; level < 4U; ++level) {
            NTEST_EXPECT_FALSE(
                proxy_stream_worth_above(proxy_stream_worth(k_errors, level - 1U, wanted, 2.0F),
                                         proxy_stream_worth(k_errors, level, wanted, 2.0F)));
        }
    }
    // Needed before spare however large the spare one's error; then the
    // larger error.
    const ProxyStreamWorth needed_small = {1, 0.01F};
    const ProxyStreamWorth spare_large = {0, 100.0F};
    NTEST_EXPECT_TRUE(proxy_stream_worth_above(needed_small, spare_large));
    NTEST_EXPECT_FALSE(proxy_stream_worth_above(spare_large, needed_small));
    const ProxyStreamWorth a = {1, 2.0F};
    const ProxyStreamWorth b = {1, 1.0F};
    NTEST_EXPECT_TRUE(proxy_stream_worth_above(a, b));
    NTEST_EXPECT_FALSE(proxy_stream_worth_above(b, a));
    NTEST_EXPECT_FALSE(proxy_stream_worth_above(a, a));
}

NTEST(proxy_stream, a_level_pushes_out_a_spare_one_or_one_worth_a_quarter_less)
{
    const ProxyStreamWorth needed = {1, 1.0F};
    const ProxyStreamWorth spare = {0, 50.0F};
    NTEST_EXPECT_TRUE(proxy_stream_displaces(needed, spare));
    NTEST_EXPECT_FALSE(proxy_stream_displaces(spare, needed));
    const ProxyStreamWorth held = {1, 1.0F};
    const ProxyStreamWorth a_little_more = {1, 1.2F};
    const ProxyStreamWorth a_quarter_more = {1, 1.26F};
    NTEST_EXPECT_FALSE(proxy_stream_displaces(held, held));
    NTEST_EXPECT_FALSE(proxy_stream_displaces(a_little_more, held));
    NTEST_EXPECT_TRUE(proxy_stream_displaces(a_quarter_more, held));
}

NTEST(proxy_stream, the_view_is_the_renderers_scale)
{
    NphRenderView v;
    nph_render_view_default(&v, nph_render_camera_look_at(nph_dvec3(1.0, 2.0, 3.0),
                                                          nph_dvec3(0.0, 0.0, 0.0),
                                                          nph_vec3(0.0F, 1.0F, 0.0F), 0.6F, 0.05F));
    v.lod_error = 1.5F;
    const NphProxyStreamView sv = nph_proxy_stream_view(&v, 1080U);
    NTEST_EXPECT_NEAR_F64(sv.camera.x, 1.0, 0.0);
    NTEST_EXPECT_NEAR_F64(sv.camera.y, 2.0, 0.0);
    NTEST_EXPECT_NEAR_F64(sv.camera.z, 3.0, 0.0);
    // Half the height over tan(yfov / 2).
    NTEST_EXPECT_NEAR_F32(sv.pixels_per_unit, 540.0F / nph_f32_tan(0.3F), 1.0e-2F);
    NTEST_EXPECT_NEAR_F32(sv.lod_error, 1.5F, 0.0F);
    NTEST_EXPECT_NEAR_F32(sv.znear, 0.05F, 0.0F);
}

NTEST(proxy_stream, a_pick_goes_first_by_its_worth_then_by_the_cell_added_first)
{
    const ProxyStreamPick spare_large = {0U, 2U, {0, 9.0F}};
    const ProxyStreamPick needed_small = {1U, 2U, {1, 1.0F}};
    const ProxyStreamPick needed_large = {2U, 1U, {1, 3.0F}};
    const ProxyStreamPick needed_large_later = {3U, 0U, {1, 3.0F}};
    const ProxyStreamPick all[4] = {needed_large_later, spare_large, needed_large, needed_small};
    // To load, the three worth most: needed before spare, then by error, a
    // tie to the cell added first.
    ProxyStreamPick picks[3];
    u32 count = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        proxy_stream_pick_keep(picks, &count, 3U, all[i], false);
    }
    NTEST_ASSERT_EQ_UINT(count, 3U);
    proxy_stream_pick_sort(picks, count, false);
    NTEST_EXPECT_EQ_UINT(picks[0].cell, 2U);
    NTEST_EXPECT_EQ_UINT(picks[1].cell, 3U);
    NTEST_EXPECT_EQ_UINT(picks[2].cell, 1U);
    NTEST_EXPECT_EQ_UINT(picks[2].level, 2U);
    // To drop, the three worth least, in that order.
    count = 0U;
    for (u32 i = 0; i < 4U; ++i) {
        proxy_stream_pick_keep(picks, &count, 3U, all[i], true);
    }
    proxy_stream_pick_sort(picks, count, true);
    NTEST_EXPECT_EQ_UINT(picks[0].cell, 0U);
    NTEST_EXPECT_EQ_UINT(picks[1].cell, 1U);
    NTEST_EXPECT_EQ_UINT(picks[2].cell, 2U);
    // No room keeps nothing.
    count = 0U;
    proxy_stream_pick_keep(picks, &count, 0U, needed_large, false);
    NTEST_EXPECT_EQ_UINT(count, 0U);
}

// The order a scan over the cells picks in: by worth (most first to load,
// least first to drop), then by the cell added first.
static bool scan_before(const ProxyStreamPick *a, const ProxyStreamPick *b, bool drop)
{
    const bool above = proxy_stream_worth_above(a->worth, b->worth);
    const bool below = proxy_stream_worth_above(b->worth, a->worth);
    if (above != below) {
        return drop ? below : above;
    }
    return a->cell < b->cell;
}

NTEST(proxy_stream, the_picks_are_the_first_of_every_candidate_in_order)
{
    // Candidates in random orders, their worths from a few values so that
    // ties are many: the picks kept are the first `cap` of all of them
    // sorted, for loads and drops, under the cap and over it.
    enum {
        CANDIDATES = 400,
        CAP = 72
    };
    static ProxyStreamPick all[CANDIDATES];
    static ProxyStreamPick sorted[CANDIDATES];
    ProxyStreamPick picks[CAP];
    NphPcg32 rng;
    nph_pcg32_seed(&rng, 0x9e3779b97f4a7c15ULL, 7U);
    for (u32 round = 0; round < 200U; ++round) {
        const u32 n = 1U + nph_pcg32_next_bounded(&rng, CANDIDATES);
        const u32 cap = 1U + nph_pcg32_next_bounded(&rng, CAP);
        for (u32 i = 0; i < n; ++i) {
            all[i].cell = i;
            all[i].level = nph_pcg32_next_bounded(&rng, 16U);
            all[i].worth.needed = (b32)nph_pcg32_next_bounded(&rng, 2U);
            all[i].worth.error = (f32)nph_pcg32_next_bounded(&rng, 6U) * 0.5F;
        }
        // Shuffled: the cells are not offered in the order they were added.
        for (u32 i = n - 1U; i > 0U; --i) {
            const u32 j = nph_pcg32_next_bounded(&rng, i + 1U);
            const ProxyStreamPick t = all[i];
            all[i] = all[j];
            all[j] = t;
        }
        for (u32 d = 0; d < 2U; ++d) {
            const bool drop = d != 0U;
            u32 count = 0U;
            for (u32 i = 0; i < n; ++i) {
                proxy_stream_pick_keep(picks, &count, cap, all[i], drop);
            }
            NTEST_ASSERT_EQ_UINT(count, n < cap ? n : cap);
            proxy_stream_pick_sort(picks, count, drop);
            // Every candidate sorted (insertion; the order is total).
            for (u32 i = 0; i < n; ++i) {
                u32 j = i;
                for (; j > 0U && scan_before(&all[i], &sorted[j - 1U], drop); --j) {
                    sorted[j] = sorted[j - 1U];
                }
                sorted[j] = all[i];
            }
            for (u32 k = 0; k < count; ++k) {
                NTEST_ASSERT_EQ_UINT(picks[k].cell, sorted[k].cell);
                NTEST_EXPECT_EQ_UINT(picks[k].level, sorted[k].level);
            }
        }
    }
}
