// SPDX-License-Identifier: Apache-2.0
// The range allocator (core/range_alloc.h): TLSF over offsets with
// out-of-band nodes (Masmano et al., ECRTS 2004; Aaltonen,
// OffsetAllocator, 2023). See the header for the contract.
#include <nepheline/core/assert.h>
#include <nepheline/core/range_alloc.h>

#include <string.h>

struct NphRangeNode {
    u32 offset;
    u32 size;
    u32 class_prev; // free-list links (free nodes only)
    u32 class_next;
    u32 phys_prev; // the ranges just below and above this one
    u32 phys_next;
    u32 used;
};

#define MANTISSA_VALUE (1U << NPH_RANGE_MANTISSA_BITS)
#define MANTISSA_MASK  (MANTISSA_VALUE - 1U)
#define TOP_COUNT      (NPH_RANGE_CLASS_COUNT >> NPH_RANGE_MANTISSA_BITS)

// ---------------------------------------------------------------------------
// Size classes: 8-bit floats with 3 mantissa bits. Sizes below 8 are their
// own class (exponent 0); from 8 up, the exponent counts how far the
// leading bit sits above bit 3 (plus one) and the mantissa is the next
// three bits, so class c covers [class_size(c), class_size(c + 1)).
// ---------------------------------------------------------------------------

u32 nph_range_class_round_down(u32 count)
{
    if (count < MANTISSA_VALUE) {
        return count;
    }
    const u32 highest = nph_log2_floor_u32(count);
    const u32 mantissa_start = highest - NPH_RANGE_MANTISSA_BITS;
    const u32 exponent = mantissa_start + 1U;
    const u32 mantissa = (count >> mantissa_start) & MANTISSA_MASK;
    return (exponent << NPH_RANGE_MANTISSA_BITS) | mantissa;
}

u32 nph_range_class_round_up(u32 count)
{
    if (count < MANTISSA_VALUE) {
        return count;
    }
    const u32 highest = nph_log2_floor_u32(count);
    const u32 mantissa_start = highest - NPH_RANGE_MANTISSA_BITS;
    const u32 exponent = mantissa_start + 1U;
    u32 mantissa = (count >> mantissa_start) & MANTISSA_MASK;
    if ((count & ((1U << mantissa_start) - 1U)) != 0U) {
        mantissa += 1U; // a carry out of the mantissa moves to the next exponent
    }
    return (exponent << NPH_RANGE_MANTISSA_BITS) + mantissa;
}

u64 nph_range_class_size(u32 size_class)
{
    const u32 exponent = size_class >> NPH_RANGE_MANTISSA_BITS;
    const u32 mantissa = size_class & MANTISSA_MASK;
    if (exponent == 0U) {
        return mantissa;
    }
    return (u64)(mantissa | MANTISSA_VALUE) << (exponent - 1U);
}

// ---------------------------------------------------------------------------
// Free lists
// ---------------------------------------------------------------------------

static void class_insert(NphRangeAllocator *ra, u32 n)
{
    NphRangeNode *node = &ra->nodes[n];
    const u32 c = nph_range_class_round_down(node->size);
    node->class_prev = NPH_RANGE_NONE;
    node->class_next = ra->class_heads[c];
    if (node->class_next != NPH_RANGE_NONE) {
        ra->nodes[node->class_next].class_prev = n;
    }
    ra->class_heads[c] = n;
    ra->used_classes[c >> NPH_RANGE_MANTISSA_BITS] |= (u8)(1U << (c & MANTISSA_MASK));
    ra->used_top |= 1U << (c >> NPH_RANGE_MANTISSA_BITS);
}

static void class_remove(NphRangeAllocator *ra, u32 n)
{
    NphRangeNode *node = &ra->nodes[n];
    const u32 c = nph_range_class_round_down(node->size);
    if (node->class_prev != NPH_RANGE_NONE) {
        ra->nodes[node->class_prev].class_next = node->class_next;
    } else {
        ra->class_heads[c] = node->class_next;
    }
    if (node->class_next != NPH_RANGE_NONE) {
        ra->nodes[node->class_next].class_prev = node->class_prev;
    }
    node->class_prev = NPH_RANGE_NONE;
    node->class_next = NPH_RANGE_NONE;
    if (ra->class_heads[c] == NPH_RANGE_NONE) {
        const u32 top = c >> NPH_RANGE_MANTISSA_BITS;
        ra->used_classes[top] &= (u8) ~(1U << (c & MANTISSA_MASK));
        if (ra->used_classes[top] == 0U) {
            ra->used_top &= ~(1U << top);
        }
    }
}

// The first non-empty class at or above `min_class`, or NPH_RANGE_NONE.
static u32 find_class(const NphRangeAllocator *ra, u32 min_class)
{
    if (min_class >= NPH_RANGE_CLASS_COUNT) {
        return NPH_RANGE_NONE;
    }
    u32 top = min_class >> NPH_RANGE_MANTISSA_BITS;
    const u32 leaf = min_class & MANTISSA_MASK;
    const u32 leaf_bits = (u32)ra->used_classes[top] & (0xFFU << leaf) & 0xFFU;
    if (leaf_bits != 0U) {
        return (top << NPH_RANGE_MANTISSA_BITS) | nph_ctz_u32(leaf_bits);
    }
    if (top + 1U >= TOP_COUNT) {
        return NPH_RANGE_NONE;
    }
    const u32 top_bits = ra->used_top & (NPH_U32_MAX << (top + 1U));
    if (top_bits == 0U) {
        return NPH_RANGE_NONE;
    }
    top = nph_ctz_u32(top_bits);
    return (top << NPH_RANGE_MANTISSA_BITS) | nph_ctz_u32((u32)ra->used_classes[top]);
}

static u32 take_spare(NphRangeAllocator *ra)
{
    NPH_ASSERT(ra->spare_count != 0U);
    return ra->spare[--ra->spare_count];
}

static void give_spare(NphRangeAllocator *ra, u32 n)
{
    NPH_ASSERT(ra->spare_count < ra->node_count);
    memset(&ra->nodes[n], 0, sizeof ra->nodes[n]);
    ra->spare[ra->spare_count++] = n;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void nph_range_alloc_reset(NphRangeAllocator *ra)
{
    NPH_ASSERT(ra != NULL && ra->nodes != NULL);
    ra->spare_count = 0U;
    for (u32 i = ra->node_count; i > 1U; --i) {
        ra->spare[ra->spare_count++] = i - 1U;
    }
    memset(ra->used_classes, 0, sizeof ra->used_classes);
    ra->used_top = 0U;
    for (u32 c = 0; c < NPH_RANGE_CLASS_COUNT; ++c) {
        ra->class_heads[c] = NPH_RANGE_NONE;
    }
    NphRangeNode *first = &ra->nodes[0];
    memset(first, 0, sizeof *first);
    first->size = ra->size;
    first->phys_prev = NPH_RANGE_NONE;
    first->phys_next = NPH_RANGE_NONE;
    ra->head = 0U;
    ra->free_total = ra->size;
    ra->live = 0U;
    class_insert(ra, 0U);
}

NphResult nph_range_alloc_init(NphRangeAllocator *ra, NphAllocator *alloc, u32 size,
                               u32 max_allocations)
{
    NPH_ASSERT(ra != NULL && alloc != NULL);
    memset(ra, 0, sizeof *ra);
    if (size == 0U || max_allocations == 0U || max_allocations > (1U << 30U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // Every live range is a node, and free ranges are separated by live
    // ones, so there is at most one more free range than live ones.
    const u32 nodes = max_allocations * 2U + 1U;
    ra->nodes = NPH_ALLOC_ARRAY(alloc, NphRangeNode, nodes);
    ra->spare = NPH_ALLOC_ARRAY(alloc, u32, nodes);
    if (ra->nodes == NULL || ra->spare == NULL) {
        if (ra->nodes != NULL) {
            NPH_FREE_ARRAY(alloc, NphRangeNode, ra->nodes, nodes);
        }
        if (ra->spare != NULL) {
            NPH_FREE_ARRAY(alloc, u32, ra->spare, nodes);
        }
        memset(ra, 0, sizeof *ra);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    ra->alloc = alloc;
    ra->node_count = nodes;
    ra->size = size;
    ra->max_allocations = max_allocations;
    nph_range_alloc_reset(ra);
    return NPH_RESULT_OK;
}

void nph_range_alloc_destroy(NphRangeAllocator *ra)
{
    if (ra == NULL || ra->nodes == NULL) {
        return;
    }
    NPH_FREE_ARRAY(ra->alloc, NphRangeNode, ra->nodes, ra->node_count);
    NPH_FREE_ARRAY(ra->alloc, u32, ra->spare, ra->node_count);
    memset(ra, 0, sizeof *ra);
}

// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------

NphResult nph_range_alloc(NphRangeAllocator *ra, u32 count, NphRange *out)
{
    NPH_ASSERT(ra != NULL && ra->nodes != NULL && out != NULL);
    *out = NPH_RANGE_NULL;
    if (count == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (ra->live == ra->max_allocations) {
        return NPH_RESULT_FULL;
    }
    const u32 c = find_class(ra, nph_range_class_round_up(count));
    if (c == NPH_RANGE_NONE) {
        return NPH_RESULT_FULL;
    }
    const u32 n = ra->class_heads[c];
    NphRangeNode *node = &ra->nodes[n];
    NPH_ASSERT(node->used == 0U && node->size >= count);
    const u32 rest = node->size - count;
    if (rest != 0U && ra->spare_count == 0U) {
        return NPH_RESULT_FULL; // unreachable while live < max_allocations
    }
    class_remove(ra, n);
    node->used = 1U;
    node->size = count;
    if (rest != 0U) {
        // The rest of the range stays free, just above the allocation.
        const u32 r = take_spare(ra);
        NphRangeNode *tail = &ra->nodes[r];
        tail->offset = node->offset + count;
        tail->size = rest;
        tail->used = 0U;
        tail->phys_prev = n;
        tail->phys_next = node->phys_next;
        if (node->phys_next != NPH_RANGE_NONE) {
            ra->nodes[node->phys_next].phys_prev = r;
        }
        node->phys_next = r;
        class_insert(ra, r);
    }
    ra->free_total -= count;
    ra->live += 1U;
    out->offset = node->offset;
    out->node = n;
    return NPH_RESULT_OK;
}

static bool range_is_live(const NphRangeAllocator *ra, NphRange range)
{
    return range.node < ra->node_count && ra->nodes[range.node].used != 0U &&
           ra->nodes[range.node].offset == range.offset;
}

void nph_range_free(NphRangeAllocator *ra, NphRange range)
{
    NPH_ASSERT(ra != NULL && ra->nodes != NULL);
    if (nph_range_is_null(range)) {
        return;
    }
    NPH_ASSERT_MSG(range_is_live(ra, range), "freeing a range that is not live");
    if (!range_is_live(ra, range)) {
        return;
    }
    const u32 n = range.node;
    NphRangeNode *node = &ra->nodes[n];
    ra->free_total += node->size;
    ra->live -= 1U;
    node->used = 0U;
    const u32 prev = node->phys_prev;
    if (prev != NPH_RANGE_NONE && ra->nodes[prev].used == 0U) {
        class_remove(ra, prev);
        node->offset = ra->nodes[prev].offset;
        node->size += ra->nodes[prev].size;
        node->phys_prev = ra->nodes[prev].phys_prev;
        if (node->phys_prev != NPH_RANGE_NONE) {
            ra->nodes[node->phys_prev].phys_next = n;
        } else {
            ra->head = n;
        }
        give_spare(ra, prev);
    }
    const u32 next = node->phys_next;
    if (next != NPH_RANGE_NONE && ra->nodes[next].used == 0U) {
        class_remove(ra, next);
        node->size += ra->nodes[next].size;
        node->phys_next = ra->nodes[next].phys_next;
        if (node->phys_next != NPH_RANGE_NONE) {
            ra->nodes[node->phys_next].phys_prev = n;
        }
        give_spare(ra, next);
    }
    class_insert(ra, n);
}

u32 nph_range_size(const NphRangeAllocator *ra, NphRange range)
{
    NPH_ASSERT(ra != NULL);
    return ra->nodes != NULL && range_is_live(ra, range) ? ra->nodes[range.node].size : 0U;
}

u32 nph_range_alloc_largest_free(const NphRangeAllocator *ra)
{
    NPH_ASSERT(ra != NULL);
    if (ra->nodes == NULL || ra->used_top == 0U) {
        return 0U;
    }
    const u32 top = nph_log2_floor_u32(ra->used_top);
    const u32 leaf = nph_log2_floor_u32((u32)ra->used_classes[top]);
    u32 largest = 0U;
    for (u32 n = ra->class_heads[(top << NPH_RANGE_MANTISSA_BITS) | leaf]; n != NPH_RANGE_NONE;
         n = ra->nodes[n].class_next) {
        largest = nph_max_u32(largest, ra->nodes[n].size);
    }
    return largest;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

static NphResult corrupt(const char **reason, const char *why)
{
    if (reason != NULL) {
        *reason = why;
    }
    return NPH_RESULT_CORRUPT;
}

NphResult nph_range_alloc_validate(const NphRangeAllocator *ra, const char **reason)
{
    NPH_ASSERT(ra != NULL);
    if (reason != NULL) {
        *reason = NULL;
    }
    if (ra->nodes == NULL) {
        return corrupt(reason, "not initialised");
    }
    // The physical list tiles [0, size).
    u64 expected = 0U;
    u32 free_nodes = 0U;
    u32 used_nodes = 0U;
    u64 free_units = 0U;
    u32 prev = NPH_RANGE_NONE;
    bool prev_free = false;
    for (u32 n = ra->head; n != NPH_RANGE_NONE; n = ra->nodes[n].phys_next) {
        if (n >= ra->node_count || free_nodes + used_nodes > ra->node_count) {
            return corrupt(reason, "physical list leaves the node array or loops");
        }
        const NphRangeNode *node = &ra->nodes[n];
        if (node->phys_prev != prev) {
            return corrupt(reason, "physical links disagree");
        }
        if (node->offset != expected || node->size == 0U) {
            return corrupt(reason, "ranges do not tile the space");
        }
        expected += node->size;
        if (node->used == 0U) {
            if (prev_free) {
                return corrupt(reason, "two free ranges touch");
            }
            ++free_nodes;
            free_units += node->size;
        } else {
            ++used_nodes;
        }
        prev_free = node->used == 0U;
        prev = n;
    }
    if (expected != ra->size) {
        return corrupt(reason, "ranges do not cover the space");
    }
    if (free_units != ra->free_total || used_nodes != ra->live) {
        return corrupt(reason, "counts disagree with the ranges");
    }
    if (free_nodes + used_nodes + ra->spare_count != ra->node_count) {
        return corrupt(reason, "nodes are lost");
    }
    // The class lists hold exactly the free ranges, each under its class.
    u32 listed = 0U;
    u32 top_bits = 0U;
    for (u32 c = 0; c < NPH_RANGE_CLASS_COUNT; ++c) {
        u32 back = NPH_RANGE_NONE;
        for (u32 n = ra->class_heads[c]; n != NPH_RANGE_NONE; n = ra->nodes[n].class_next) {
            if (n >= ra->node_count || listed > free_nodes) {
                return corrupt(reason, "a class list leaves the node array or loops");
            }
            const NphRangeNode *node = &ra->nodes[n];
            if (node->used != 0U || nph_range_class_round_down(node->size) != c ||
                node->class_prev != back) {
                return corrupt(reason, "a class list holds a wrong range");
            }
            back = n;
            ++listed;
        }
        const bool bit =
            (((u32)ra->used_classes[c >> NPH_RANGE_MANTISSA_BITS] >> (c & MANTISSA_MASK)) & 1U) !=
            0U;
        if (bit != (ra->class_heads[c] != NPH_RANGE_NONE)) {
            return corrupt(reason, "a class bit disagrees with its list");
        }
        if (bit) {
            top_bits |= 1U << (c >> NPH_RANGE_MANTISSA_BITS);
        }
    }
    if (listed != free_nodes) {
        return corrupt(reason, "free ranges are missing from the class lists");
    }
    if (top_bits != ra->used_top) {
        return corrupt(reason, "the top bitmap disagrees with the class bits");
    }
    return NPH_RESULT_OK;
}
