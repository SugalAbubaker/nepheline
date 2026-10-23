// SPDX-License-Identifier: Apache-2.0
#pragma once

// The dynamic AABB tree (CLAUDE.md §8.9 "Broadphase"; docs/physics.md
// "Broadphase"): a binary bounding volume hierarchy over boxes ("proxies")
// that move, appear, and disappear, answering box overlap and ray queries.
//
// The design follows Catto, "Dynamic Bounding Volume Hierarchies" (GDC
// 2019), after Bittner, Hapala, and Havran, "Fast, Effective BVH Updates for
// Animated Scenes" (I3D 2012):
//
//   - insertion picks the sibling that adds the least surface area to the
//     tree (the surface area heuristic), descending greedily with the cost
//     the ancestors inherit as a lower bound;
//   - every ancestor of a change is refitted and considers a rotation that
//     swaps a child with a grandchild when that shrinks the area;
//   - a proxy can be enlarged in place (its ancestors grow, nothing moves),
//     which keeps small motions cheap and leaves quality to
//   - a rebuild, top-down with binned SAH (Wald, "On fast Construction of
//     SAH-based Bounding Volume Hierarchies", RT 2007), of the whole tree or
//     of the subtrees that enlargements touched.
//
// Each node carries the union of its leaves' category bits, so a query with
// a mask skips subtrees with nothing it wants (Box2D's filtered queries).
// Leaves carry a u64 the caller chooses.
//
// Costs (§1.4): the node array grows by doubling from the tree's allocator
// when a proxy is added to a full tree, and a rebuild uses scratch arrays
// that grow the same way; nothing else allocates. Queries use a fixed stack
// on the caller's stack (4 KiB).
//
// Determinism: the same sequence of calls builds the same tree and every
// query reports in the same order.
//
// Thread-safety: none for changes; queries are NPH_THREADSAFE with each
// other while nothing changes the tree.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>
#include <nepheline/math/geometry.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_AABB_TREE_NULL NPH_U32_MAX

// The deepest tree the queries' fixed stack can walk. Rotations keep real
// trees far shallower (a few times log2 of the proxy count).
#define NPH_AABB_TREE_STACK_SIZE 1024U

typedef struct NphAabbTreeNode {
    NphAABB aabb;      // a leaf's is the caller's (fattened) box
    u64 category_bits; // a leaf's own; the union of its leaves' for an inner node
    union {
        struct {
            u32 child1;
            u32 child2;
        };
        u64 user_data; // a leaf's
    };
    u32 parent; // NPH_AABB_TREE_NULL for the root; the next free node in the free list
    u16 height; // 0 for a leaf
    u16 flags;  // internal
} NphAabbTreeNode;

NPH_STATIC_ASSERT(sizeof(NphAabbTreeNode) == 48U, "a tree node is 48 bytes");

typedef struct NphAabbTree {
    NphAabbTreeNode *nodes;
    u32 root;
    u32 node_count; // nodes in use (leaves and inner nodes)
    u32 node_capacity;
    u32 free_list;
    u32 proxy_count;
    u32 rebuild_capacity;
    // Rebuild scratch, grown on demand.
    u32 *leaf_indices;
    NphAABB *leaf_boxes;
    NphVec3 *leaf_centers;
    u32 *bins_scratch;
    NphAllocator *alloc;
} NphAabbTree;

// An empty tree with room for `initial_capacity` nodes (0: 16).
// NPH_RESULT_INVALID_ARGUMENT for a NULL allocator, NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult nph_aabb_tree_init(NphAabbTree *tree, NphAllocator *alloc,
                                          u32 initial_capacity);

// Releases everything. Safe on a zero-filled tree.
void nph_aabb_tree_free(NphAabbTree *tree);

// Adds a leaf with box `aabb` and returns its proxy id (a node index that
// stays valid until the proxy is destroyed). NPH_RESULT_OUT_OF_MEMORY when
// the node array cannot grow; NPH_RESULT_INVALID_ARGUMENT for a box with
// min > max or a non-finite bound.
NPH_MUST_USE NphResult nph_aabb_tree_create_proxy(NphAabbTree *tree, NphAABB aabb,
                                                  u64 category_bits, u64 user_data, u32 *out_proxy);

// Removes a proxy. The id may be reused by a later create.
void nph_aabb_tree_destroy_proxy(NphAabbTree *tree, u32 proxy);

// Gives a proxy a new box by taking it out and inserting it again (the best
// position for the new box).
void nph_aabb_tree_move_proxy(NphAabbTree *tree, u32 proxy, NphAABB aabb);

// Gives a proxy the box `aabb` without restructuring, O(depth): its
// ancestors grow to contain the new box (and still contain the old one, so
// they stay conservative). For a proxy that moved a little out of its
// fattened box. The ancestors' boxes and the tree's quality decay with many
// such moves; nph_aabb_tree_rebuild(tree, false) restores both.
void nph_aabb_tree_enlarge_proxy(NphAabbTree *tree, u32 proxy, NphAABB aabb);

// Changes a proxy's category bits (and its ancestors' unions).
void nph_aabb_tree_set_category_bits(NphAabbTree *tree, u32 proxy, u64 category_bits);

NPH_INLINE NphAABB nph_aabb_tree_proxy_aabb(const NphAabbTree *tree, u32 proxy)
{
    return tree->nodes[proxy].aabb;
}
NPH_INLINE u64 nph_aabb_tree_proxy_user_data(const NphAabbTree *tree, u32 proxy)
{
    return tree->nodes[proxy].user_data;
}
NPH_INLINE u64 nph_aabb_tree_proxy_category_bits(const NphAabbTree *tree, u32 proxy)
{
    return tree->nodes[proxy].category_bits;
}

// Called for each leaf whose box overlaps the query box and whose category
// bits share a bit with the mask. Return false to end the query.
typedef bool (*NphAabbTreeQueryFn)(u32 proxy, u64 user_data, void *context);

// Every proxy whose box overlaps `aabb` (touching counts) and whose category
// bits intersect `mask_bits`, depth first, in a fixed order.
void nph_aabb_tree_query(const NphAabbTree *tree, NphAABB aabb, u64 mask_bits,
                         NphAabbTreeQueryFn fn, void *context);

typedef struct NphAabbTreeRay {
    NphVec3 origin;
    NphVec3 translation; // the ray is origin + t * translation for t in [0, max_fraction]
    f32 max_fraction;
} NphAabbTreeRay;

// Called for each leaf whose box the ray (clipped to its current maximum)
// enters. Returns what the query does next: a value in [0, current max)
// clips the ray to it (a closer hit was found), the current max or more
// continues unchanged, and a negative value ends the query.
typedef f32 (*NphAabbTreeRayFn)(const NphAabbTreeRay *ray, u32 proxy, u64 user_data, void *context);

// Walks the leaves the ray may hit, nearer children first, skipping boxes
// beyond the current maximum fraction.
void nph_aabb_tree_raycast(const NphAabbTree *tree, const NphAabbTreeRay *ray, u64 mask_bits,
                           NphAabbTreeRayFn fn, void *context);

// Rebuilds with binned SAH: the whole tree (`full`), or only the subtrees
// that enlargements touched since the last rebuild. Returns the number of
// leaves placed again. NPH_RESULT_OUT_OF_MEMORY when the scratch arrays
// cannot grow (the tree is unchanged then).
NPH_MUST_USE NphResult nph_aabb_tree_rebuild(NphAabbTree *tree, bool full, u32 *out_leaves);

// The height of the root (0 for an empty tree or a single leaf).
u32 nph_aabb_tree_height(const NphAabbTree *tree);

// The SAH quality metric: the summed surface area of the inner nodes over
// the root's. Lower is better.
f32 nph_aabb_tree_area_ratio(const NphAabbTree *tree);

// Checks the structure: parents and children agree, heights are consistent,
// every box contains its children's, category bits are the unions, the free
// list and the counts match. For tests.
bool nph_aabb_tree_validate(const NphAabbTree *tree);

#ifdef __cplusplus
}
#endif
