// SPDX-License-Identifier: Apache-2.0
#pragma once

// A mesh's levels apart checked against the mesh (render/cluster_lod.h
// "Levels apart"; ADR-0063 "Stage 2, a level is a mesh"), for the proxy
// builder's tests (test_cell_proxy.c) and the cells' (test_vis_cells.c).
#include <nepheline/core/alloc.h>
#include <nepheline/render/cluster_lod.h>

#include <ntest/ntest.h>

#include <string.h>

static inline bool levels_same_bits(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

static inline bool levels_same_f32(f32 a, f32 b)
{
    return nph_f32_to_bits(a) == nph_f32_to_bits(b);
}

// The tree of a level apart: from node 0, every node once and every group
// once, the leaves their records, their maker errors within their clusters'
// makers' (the renderer's checks).
static inline void levels_check_tree(const NphRenderClusterLods *l, const NphRenderLodMaker *makers)
{
    NphAllocator *a = nph_allocator_system();
    u8 *seen_node = NPH_ALLOC_ARRAY_ZEROED(a, u8, l->nodes.len);
    u8 *seen_group = NPH_ALLOC_ARRAY_ZEROED(a, u8, l->groups.len);
    u32 *queue = NPH_ALLOC_ARRAY(a, u32, l->nodes.len);
    NTEST_ASSERT_TRUE(seen_node != NULL && seen_group != NULL && queue != NULL);
    u32 head = 0U;
    u32 tail = 0U;
    queue[tail++] = 0U;
    seen_node[0] = 1U;
    while (head < tail) {
        const NphRenderLodNode *node = &l->nodes.data[queue[head++]];
        for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
            const u32 ch = node->child[i];
            if (ch == NPH_RENDER_LOD_NONE) {
                continue;
            }
            if ((ch & NPH_RENDER_LOD_LEAF) == 0U) {
                NTEST_ASSERT_LT_UINT(ch, l->nodes.len);
                NTEST_ASSERT_EQ_UINT(seen_node[ch], 0U);
                seen_node[ch] = 1U;
                queue[tail++] = ch;
                continue;
            }
            const u32 gi = ch & ~NPH_RENDER_LOD_LEAF;
            NTEST_ASSERT_LT_UINT(gi, l->groups.len);
            NTEST_ASSERT_EQ_UINT(seen_group[gi], 0U);
            seen_group[gi] = 1U;
            const NphRenderLodGroup *rg = &l->groups.data[gi];
            NTEST_EXPECT_TRUE(levels_same_f32(node->sphere[i].w, rg->radius) &&
                              levels_same_f32(node->error[i], rg->error));
            f32 smallest = NPH_F32_MAX;
            for (u32 j = 0; j < rg->cluster_count; ++j) {
                smallest = nph_f32_min(
                    smallest, makers[l->group_clusters.data[rg->cluster_offset + j]].error);
            }
            NTEST_EXPECT_TRUE(node->maker_error[i] <= smallest);
        }
    }
    NTEST_EXPECT_EQ_UINT(tail, l->nodes.len);
    for (u32 x = 0; x < l->groups.len; ++x) {
        NTEST_EXPECT_EQ_UINT(seen_group[x], 1U);
    }
    NPH_FREE_ARRAY(a, u32, queue, l->nodes.len);
    NPH_FREE_ARRAY(a, u8, seen_group, l->groups.len);
    NPH_FREE_ARRAY(a, u8, seen_node, l->nodes.len);
}

// Level `k` of `w` apart (`l`, `makers`) holds the level, record for
// record: its clusters (their bounds, bricks and voxels the same bits),
// each cluster's maker the group that made it (its sphere and error, zeros
// at level 0) and its consumer that group renumbered, with the same
// clusters; its groups the level's; its tree (levels_check_tree).
static inline void levels_check_level(const NphRenderClusterLods *w, u32 k,
                                      const NphRenderClusterLods *l,
                                      const NphRenderLodMakerArray *makers)
{
    u32 first = 0U;
    u32 count = 0U;
    NTEST_ASSERT_TRUE(nph_render_cluster_lods_level_range(w, k, &first, &count));
    NTEST_ASSERT_EQ_UINT(l->clusters.meshlets.len, count);
    NTEST_EXPECT_EQ_UINT(l->levels, 1U);
    NTEST_EXPECT_EQ_UINT(l->level0_clusters, count);
    NTEST_ASSERT_EQ_UINT(makers->len, count);
    for (u32 i = 0; i < count; ++i) {
        const u32 c = first + i;
        const NphRenderMeshlet *m = &w->clusters.meshlets.data[c];
        const NphRenderMeshlet *n = &l->clusters.meshlets.data[i];
        NTEST_ASSERT_EQ_UINT(n->triangle_count, m->triangle_count);
        NTEST_EXPECT_EQ_UINT(n->vertex_count, 0U);
        NTEST_EXPECT_EQ_UINT(n->submesh, m->submesh);
        NTEST_EXPECT_TRUE(levels_same_bits(&l->clusters.bounds.data[i], &w->clusters.bounds.data[c],
                                           sizeof(NphRenderMeshletBounds)));
        for (u32 b = 0; b < m->triangle_count; ++b) {
            const NphRenderVoxelBrick *ob = &w->bricks.data[m->vertex_offset + b];
            const NphRenderVoxelBrick *nb = &l->bricks.data[n->vertex_offset + b];
            NTEST_ASSERT_TRUE(levels_same_bits(&nb->origin, &ob->origin, sizeof ob->origin) &&
                              levels_same_f32(nb->cell, ob->cell) && nb->mask[0] == ob->mask[0] &&
                              nb->mask[1] == ob->mask[1]);
            const u32 v = nph_popcount_u32(ob->mask[0]) + nph_popcount_u32(ob->mask[1]);
            NTEST_ASSERT_TRUE(levels_same_bits(&l->voxels.data[nb->first_voxel],
                                               &w->voxels.data[ob->first_voxel],
                                               (size_t)v * sizeof(NphRenderVoxel)));
        }
        // Its maker as a record, its consumer renumbered.
        const NphRenderClusterLod *lod = &w->lods.data[c];
        const NphRenderLodMaker *mk = &makers->data[i];
        NTEST_EXPECT_EQ_UINT(l->lods.data[i].group, NPH_RENDER_LOD_NONE);
        NTEST_EXPECT_EQ_UINT(l->lods.data[i].level, 0U);
        if (lod->group == NPH_RENDER_LOD_NONE) {
            NTEST_EXPECT_EQ_UINT(k, 0U);
            NTEST_EXPECT_TRUE(levels_same_f32(mk->error, 0.0F) &&
                              levels_same_f32(mk->radius, 0.0F));
        } else {
            const NphRenderLodGroup *maker = &w->groups.data[lod->group];
            NTEST_EXPECT_TRUE(levels_same_bits(&mk->center, &maker->center, sizeof mk->center) &&
                              levels_same_f32(mk->radius, maker->radius) &&
                              levels_same_f32(mk->error, maker->error));
        }
        const NphRenderLodGroup *ow = &w->groups.data[lod->parent];
        const NphRenderLodGroup *nw = &l->groups.data[l->lods.data[i].parent];
        NTEST_EXPECT_TRUE(levels_same_bits(&nw->center, &ow->center, sizeof ow->center) &&
                          levels_same_f32(nw->radius, ow->radius) &&
                          levels_same_f32(nw->error, ow->error));
        NTEST_ASSERT_EQ_UINT(nw->cluster_count, ow->cluster_count);
        for (u32 j = 0; j < ow->cluster_count; ++j) {
            NTEST_EXPECT_EQ_UINT(l->group_clusters.data[nw->cluster_offset + j] + first,
                                 w->group_clusters.data[ow->cluster_offset + j]);
        }
    }
    u32 own = 0U;
    for (u32 x = 0; x < w->groups.len; ++x) {
        own += w->groups.data[x].level == k ? 1U : 0U;
    }
    NTEST_ASSERT_EQ_UINT(l->groups.len, own);
    levels_check_tree(l, makers->data);
}

// Every level of `w` (a mesh of voxels alone) apart, checked
// (levels_check_level); together they hold every cluster and group of it.
static inline void levels_check_all(const NphRenderClusterLods *w)
{
    NphAllocator *a = nph_allocator_system();
    u32 clusters = 0U;
    u32 groups = 0U;
    for (u32 k = 0; k < w->levels; ++k) {
        NphRenderClusterLods l;
        nph_render_cluster_lods_init(&l, a);
        NphRenderLodMakerArray makers;
        NPH_ARRAY_INIT(&makers, a);
        NTEST_ASSERT_EQ_INT(nph_render_cluster_lods_level(w, k, &l, &makers), NPH_RESULT_OK);
        levels_check_level(w, k, &l, &makers);
        clusters += l.clusters.meshlets.len;
        groups += l.groups.len;
        NPH_ARRAY_FREE(&makers);
        nph_render_cluster_lods_free(&l);
    }
    NTEST_EXPECT_EQ_UINT(clusters, w->clusters.meshlets.len);
    NTEST_EXPECT_EQ_UINT(groups, w->groups.len);
}
