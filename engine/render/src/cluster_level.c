// SPDX-License-Identifier: Apache-2.0
// A level of a mesh's levels of detail as a mesh of its own (render/
// cluster_lod.h "Levels apart"; ADR-0063 "Stage 2, a level is a mesh").
//
// A level's records point into the level but for one read, a cluster's
// maker, a group of the finer level, which the level apart carries as a
// record instead. Each tree of the hierarchy holds one level's groups
// (lod_build_hierarchy builds a tree per run of groups), so the level's
// nodes are the trees whose groups are all of it, found from the root by the
// levels each node's subtree holds; the rest is the level's runs of
// clusters, groups and voxels, renumbered from 0.
#include <nepheline/core/assert.h>
#include <nepheline/render/cluster_lod.h>

#include <string.h>

bool nph_render_cluster_lods_level_range(const NphRenderClusterLods *l, u32 level, u32 *out_first,
                                         u32 *out_count)
{
    NPH_ASSERT(l != NULL && out_first != NULL && out_count != NULL);
    const u32 n = l->lods.len;
    if (level >= l->levels || n != l->clusters.meshlets.len) {
        return false;
    }
    u32 first = 0U;
    while (first < n && l->lods.data[first].level != level) {
        ++first;
    }
    u32 end = first;
    while (end < n && l->lods.data[end].level == level) {
        ++end;
    }
    for (u32 c = end; c < n; ++c) {
        if (l->lods.data[c].level == level) {
            return false;
        }
    }
    if (first == end) {
        return false;
    }
    *out_first = first;
    *out_count = end - first;
    return true;
}

// The levels each node's subtree holds, a bit a level (`masks`, per node),
// by a walk from the root that finishes a node's children before it. False
// for a child out of range, a node reached twice (not a tree), or a group of
// level 64 or more. `stack` and `state` hold a node each.
static bool subtree_levels(const NphRenderClusterLods *l, u64 *masks, u32 *stack, u8 *state)
{
    const u32 n = l->nodes.len;
    memset(state, 0, (size_t)n);
    u32 top = 0U;
    stack[top++] = 0U;
    state[0] = 1U;
    while (top != 0U) {
        const u32 k = stack[top - 1U];
        const NphRenderLodNode *node = &l->nodes.data[k];
        if (state[k] == 1U) {
            // Its children first.
            state[k] = 2U;
            for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
                const u32 c = node->child[i];
                if (c == NPH_RENDER_LOD_NONE || (c & NPH_RENDER_LOD_LEAF) != 0U) {
                    continue;
                }
                if (c >= n || state[c] != 0U) {
                    return false;
                }
                state[c] = 1U;
                stack[top++] = c;
            }
            continue;
        }
        --top;
        u64 mask = 0U;
        for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
            const u32 c = node->child[i];
            if (c == NPH_RENDER_LOD_NONE) {
                continue;
            }
            if ((c & NPH_RENDER_LOD_LEAF) != 0U) {
                const u32 g = c & ~NPH_RENDER_LOD_LEAF;
                if (g >= l->groups.len || l->groups.data[g].level >= 64U) {
                    return false;
                }
                mask |= 1ULL << l->groups.data[g].level;
            } else {
                mask |= masks[c];
            }
        }
        masks[k] = mask;
    }
    return true;
}

// A tree of the level: a node, or a group (NPH_RENDER_LOD_LEAF), and its
// entry in the node above it (`parent` NPH_RENDER_LOD_NONE for the root).
typedef struct LevelTop {
    u32 ref;
    u32 parent;
    u32 slot;
} LevelTop;

// The level's trees, below the nodes whose subtrees hold more than it: at
// most NPH_RENDER_LOD_NODE_CHILDREN into `tops`, false past that. The root
// itself where it holds the level alone.
static bool level_tops(const NphRenderClusterLods *l, const u64 *masks, u64 bit, u32 *stack,
                       LevelTop *tops, u32 *out_count)
{
    u32 count = 0U;
    if (masks[0] == bit) {
        tops[count++] = (LevelTop){0U, NPH_RENDER_LOD_NONE, 0U};
        *out_count = count;
        return true;
    }
    u32 top = 0U;
    stack[top++] = 0U;
    while (top != 0U) {
        const u32 k = stack[--top];
        const NphRenderLodNode *node = &l->nodes.data[k];
        for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
            const u32 c = node->child[i];
            if (c == NPH_RENDER_LOD_NONE) {
                continue;
            }
            const u64 m = (c & NPH_RENDER_LOD_LEAF) != 0U
                              ? 1ULL << l->groups.data[c & ~NPH_RENDER_LOD_LEAF].level
                              : masks[c];
            if (m == bit) {
                if (count == NPH_RENDER_LOD_NODE_CHILDREN) {
                    return false;
                }
                tops[count++] = (LevelTop){c, k, i};
            } else if ((m & bit) != 0U) {
                stack[top++] = c; // a node holding more levels: the level's trees lie below
            }
        }
    }
    *out_count = count;
    return true;
}

// Marks the nodes of the level's trees in `in_level` (a walk down each).
static void mark_level_nodes(const NphRenderClusterLods *l, const LevelTop *tops, u32 count,
                             u32 *stack, u8 *in_level)
{
    memset(in_level, 0, (size_t)l->nodes.len);
    for (u32 t = 0; t < count; ++t) {
        if ((tops[t].ref & NPH_RENDER_LOD_LEAF) != 0U) {
            continue;
        }
        u32 top = 0U;
        stack[top++] = tops[t].ref;
        while (top != 0U) {
            const u32 k = stack[--top];
            in_level[k] = 1U;
            for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
                const u32 c = l->nodes.data[k].child[i];
                if (c != NPH_RENDER_LOD_NONE && (c & NPH_RENDER_LOD_LEAF) == 0U) {
                    stack[top++] = c;
                }
            }
        }
    }
}

// A child's reference renumbered: a node through `node_map`, a group
// through `group_map`.
static u32 renumber(u32 c, const u32 *node_map, const u32 *group_map)
{
    if (c == NPH_RENDER_LOD_NONE) {
        return c;
    }
    if ((c & NPH_RENDER_LOD_LEAF) != 0U) {
        return group_map[c & ~NPH_RENDER_LOD_LEAF] | NPH_RENDER_LOD_LEAF;
    }
    return node_map[c];
}

// The groups of the level, renumbered in order (`group_map`, NONE for the
// others), their clusters theirs, and each cluster's level record and
// maker. False for a group of the level consuming a cluster of another.
static NphResult level_groups(const NphRenderClusterLods *l, u32 level, u32 first, u32 count,
                              u32 *group_map, NphRenderClusterLods *out,
                              NphRenderLodMakerArray *makers)
{
    for (u32 g = 0; g < l->groups.len; ++g) {
        const NphRenderLodGroup *src = &l->groups.data[g];
        if (src->level != level) {
            group_map[g] = NPH_RENDER_LOD_NONE;
            continue;
        }
        if (src->cluster_offset > l->group_clusters.len ||
            src->cluster_count > l->group_clusters.len - src->cluster_offset) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        group_map[g] = out->groups.len;
        NphRenderLodGroup group = *src;
        group.level = 0U;
        group.cluster_offset = out->group_clusters.len;
        for (u32 i = 0; i < src->cluster_count; ++i) {
            const u32 c = l->group_clusters.data[src->cluster_offset + i];
            if (c < first || c - first >= count) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            NPH_TRY(NPH_ARRAY_PUSH(&out->group_clusters, c - first));
        }
        NPH_TRY(NPH_ARRAY_PUSH(&out->groups, group));
    }
    for (u32 c = first; c < first + count; ++c) {
        const NphRenderClusterLod *src = &l->lods.data[c];
        NphRenderClusterLod lod;
        memset(&lod, 0, sizeof lod);
        lod.group = NPH_RENDER_LOD_NONE;
        lod.parent = NPH_RENDER_LOD_NONE;
        if (src->parent != NPH_RENDER_LOD_NONE) {
            if (src->parent >= l->groups.len || group_map[src->parent] == NPH_RENDER_LOD_NONE) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            lod.parent = group_map[src->parent];
        }
        NPH_TRY(NPH_ARRAY_PUSH(&out->lods, lod));
        NphRenderLodMaker maker;
        memset(&maker, 0, sizeof maker);
        if (src->group != NPH_RENDER_LOD_NONE) {
            if (src->group >= l->groups.len) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            const NphRenderLodGroup *g = &l->groups.data[src->group];
            maker.center = g->center;
            maker.radius = g->radius;
            maker.error = g->error;
        }
        NPH_TRY(NPH_ARRAY_PUSH(makers, maker));
    }
    return NPH_RESULT_OK;
}

// The level's clusters, their bricks and their voxels, renumbered in order.
static NphResult level_voxels(const NphRenderClusterLods *l, u32 first, u32 count,
                              NphRenderClusterLods *out)
{
    for (u32 c = first; c < first + count; ++c) {
        NphRenderMeshlet m = l->clusters.meshlets.data[c];
        if (m.vertex_count != 0U || m.vertex_offset > l->bricks.len ||
            m.triangle_count > l->bricks.len - m.vertex_offset) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        const u32 brick0 = m.vertex_offset;
        m.vertex_offset = out->bricks.len;
        for (u32 k = brick0; k < brick0 + m.triangle_count; ++k) {
            NphRenderVoxelBrick b = l->bricks.data[k];
            const u32 n = nph_popcount_u32(b.mask[0]) + nph_popcount_u32(b.mask[1]);
            if (b.first_voxel > l->voxels.len || n > l->voxels.len - b.first_voxel) {
                return NPH_RESULT_INVALID_ARGUMENT;
            }
            const u32 voxel0 = b.first_voxel;
            b.first_voxel = out->voxels.len;
            NPH_TRY(NPH_ARRAY_PUSH_N(&out->voxels, l->voxels.data + voxel0, n));
            NPH_TRY(NPH_ARRAY_PUSH(&out->bricks, b));
        }
        NPH_TRY(NPH_ARRAY_PUSH(&out->clusters.meshlets, m));
        NPH_TRY(NPH_ARRAY_PUSH(&out->clusters.bounds, l->clusters.bounds.data[c]));
    }
    return NPH_RESULT_OK;
}

// The level's nodes: its one tree's root as node 0, or a new root over its
// trees, holding their entries in the nodes above them; then the trees'
// other nodes in order, every child renumbered.
static NphResult level_nodes(const NphRenderClusterLods *l, const LevelTop *tops, u32 top_count,
                             const u8 *in_level, u32 *node_map, const u32 *group_map,
                             NphRenderClusterLods *out)
{
    const bool one_tree = top_count == 1U && (tops[0].ref & NPH_RENDER_LOD_LEAF) == 0U;
    u32 next = 1U;
    for (u32 k = 0; k < l->nodes.len; ++k) {
        node_map[k] = NPH_RENDER_LOD_NONE;
        if (in_level[k] != 0U) {
            node_map[k] = one_tree && k == tops[0].ref ? 0U : next++;
        }
    }
    NphRenderLodNode root;
    memset(&root, 0, sizeof root);
    for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
        root.child[i] = NPH_RENDER_LOD_NONE;
    }
    if (!one_tree) {
        for (u32 t = 0; t < top_count; ++t) {
            const NphRenderLodNode *parent = &l->nodes.data[tops[t].parent];
            const u32 s = tops[t].slot;
            root.sphere[t] = parent->sphere[s];
            root.error[t] = parent->error[s];
            root.maker_error[t] = parent->maker_error[s];
            root.child[t] = renumber(tops[t].ref, node_map, group_map);
        }
    }
    NPH_TRY(NPH_ARRAY_RESIZE(&out->nodes, next));
    if (!one_tree) {
        out->nodes.data[0] = root;
    }
    for (u32 k = 0; k < l->nodes.len; ++k) {
        if (node_map[k] == NPH_RENDER_LOD_NONE) {
            continue;
        }
        NphRenderLodNode node = l->nodes.data[k];
        for (u32 i = 0; i < NPH_RENDER_LOD_NODE_CHILDREN; ++i) {
            node.child[i] = renumber(node.child[i], node_map, group_map);
        }
        out->nodes.data[node_map[k]] = node;
    }
    return NPH_RESULT_OK;
}

NphResult nph_render_cluster_lods_level(const NphRenderClusterLods *l, u32 level,
                                        NphRenderClusterLods *out, NphRenderLodMakerArray *makers)
{
    NPH_ASSERT(l != NULL && out != NULL && makers != NULL);
    NPH_ASSERT(out->clusters.meshlets.len == 0U && out->nodes.len == 0U && makers->len == 0U);
    u32 first = 0U;
    u32 count = 0U;
    if (l->clusters.vertices.len != 0U || l->clusters.triangles.len != 0U ||
        l->clusters.bounds.len != l->clusters.meshlets.len || l->nodes.len == 0U || level >= 64U ||
        !nph_render_cluster_lods_level_range(l, level, &first, &count)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *a = out->nodes.alloc;
    const u32 nodes = l->nodes.len;
    const u32 groups = nph_max_u32(l->groups.len, 1U);
    u64 *masks = NPH_ALLOC_ARRAY(a, u64, nodes);
    u32 *stack = NPH_ALLOC_ARRAY(a, u32, nodes);
    u8 *state = NPH_ALLOC_ARRAY(a, u8, nodes);
    u32 *node_map = NPH_ALLOC_ARRAY(a, u32, nodes);
    u32 *group_map = NPH_ALLOC_ARRAY(a, u32, groups);
    NphResult r =
        masks != NULL && stack != NULL && state != NULL && node_map != NULL && group_map != NULL
            ? NPH_RESULT_OK
            : NPH_RESULT_OUT_OF_MEMORY;
    LevelTop tops[NPH_RENDER_LOD_NODE_CHILDREN];
    u32 top_count = 0U;
    if (r == NPH_RESULT_OK &&
        (!subtree_levels(l, masks, stack, state) ||
         !level_tops(l, masks, 1ULL << level, stack, tops, &top_count) || top_count == 0U)) {
        r = NPH_RESULT_INVALID_ARGUMENT;
    }
    if (r == NPH_RESULT_OK) {
        r = level_groups(l, level, first, count, group_map, out, makers);
    }
    if (r == NPH_RESULT_OK) {
        r = level_voxels(l, first, count, out);
    }
    if (r == NPH_RESULT_OK) {
        mark_level_nodes(l, tops, top_count, stack, state);
        r = level_nodes(l, tops, top_count, state, node_map, group_map, out);
    }
    if (r == NPH_RESULT_OK) {
        out->levels = 1U;
        out->level0_clusters = count;
    }
    if (masks != NULL) {
        NPH_FREE_ARRAY(a, u64, masks, nodes);
    }
    if (stack != NULL) {
        NPH_FREE_ARRAY(a, u32, stack, nodes);
    }
    if (state != NULL) {
        NPH_FREE_ARRAY(a, u8, state, nodes);
    }
    if (node_map != NULL) {
        NPH_FREE_ARRAY(a, u32, node_map, nodes);
    }
    if (group_map != NULL) {
        NPH_FREE_ARRAY(a, u32, group_map, groups);
    }
    return r;
}
