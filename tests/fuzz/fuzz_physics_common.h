// SPDX-License-Identifier: Apache-2.0
// What the physics fuzz targets share (fuzz_physics_hull.c,
// fuzz_physics_mesh.c): a little-endian reader over the input, the damage
// the input asks for applied to a copy of a built block, and every convex
// shape the narrow phase takes, to collide with what a block holds.
#pragma once

#include <nepheline/core/alloc.h>
#include <nepheline/physics/collide.h>
#include <nepheline/physics/geometry.h>
#include <nepheline/physics/hull.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct FuzzReader {
    const uint8_t *p;
    size_t n;
} FuzzReader;

static inline bool fuzz_u8(FuzzReader *r, u8 *out)
{
    if (r->n < 1U) {
        return false;
    }
    *out = r->p[0];
    r->p += 1U;
    r->n -= 1U;
    return true;
}

static inline bool fuzz_u16(FuzzReader *r, u16 *out)
{
    if (r->n < 2U) {
        return false;
    }
    *out = (u16)(r->p[0] | (r->p[1] << 8U));
    r->p += 2U;
    r->n -= 2U;
    return true;
}

// A coordinate in 1/256 m, within 128 m of the origin.
static inline bool fuzz_coordinate(FuzzReader *r, f32 *out)
{
    u16 bits;
    if (!fuzz_u16(r, &bits)) {
        return false;
    }
    *out = (f32)(i16)bits * (1.0F / 256.0F);
    return true;
}

static inline bool fuzz_vec3(FuzzReader *r, NphVec3 *out)
{
    return fuzz_coordinate(r, &out->x) && fuzz_coordinate(r, &out->y) &&
           fuzz_coordinate(r, &out->z);
}

// The rest of the input damages a copy of `block`: a u16 truncation (the
// size validation is given; 0 keeps the block's), then three-byte records,
// a u16 offset into the block and a u8 to xor there. The copy is exactly as
// long as the size it is validated with, so a read past it is a sanitizer's
// error; NULL when there is no damage to do.
static inline void *fuzz_damage(FuzzReader *r, const void *block, u32 block_size, u32 *out_size)
{
    u16 truncation;
    if (!fuzz_u16(r, &truncation)) {
        return NULL;
    }
    const u32 size = truncation != 0U && truncation < block_size ? truncation : block_size;
    u8 *copy = NPH_ALLOC(nph_allocator_system(), size, 16U);
    if (copy == NULL) {
        abort();
    }
    memcpy(copy, block, size);
    u16 offset;
    u8 value;
    while (fuzz_u16(r, &offset) && fuzz_u8(r, &value)) {
        copy[offset % size] ^= value;
    }
    *out_size = size;
    return copy;
}

static inline void fuzz_damage_free(void *copy, u32 size)
{
    NPH_FREE(nph_allocator_system(), copy, size, 16U);
}

// A manifold's promises: at most four points, and with any, a finite
// normal and finite points (a NaN would spread through the world).
static inline void fuzz_check_manifold(const NphPhysicsManifold *m)
{
    if (m->point_count > NPH_PHYSICS_MANIFOLD_MAX_POINTS) {
        abort();
    }
    if (m->point_count > 0U && !nph_vec3_is_finite(m->normal)) {
        abort();
    }
    for (u32 i = 0U; i < m->point_count; ++i) {
        if (!nph_vec3_is_finite(m->points[i].point) ||
            !nph_f32_is_finite(m->points[i].separation)) {
            abort();
        }
    }
}

// The convex shapes to collide with: a ball, a capsule, a box, and a box's
// hull (built once).
static inline u32 fuzz_convex_shapes(NphPhysicsGeometry out[4])
{
    static NphPhysicsHull *box_hull = NULL;
    if (box_hull == NULL &&
        nph_physics_hull_make_box(nph_vec3(0.3F, 0.2F, 0.4F), nph_allocator_system(), &box_hull) !=
            NPH_RESULT_OK) {
        abort();
    }
    out[0] = nph_physics_geometry_sphere(0.4F);
    out[1] = nph_physics_geometry_capsule(0.5F, 0.25F);
    out[2] = nph_physics_geometry_box(nph_vec3(0.5F, 0.25F, 0.35F));
    out[3] = nph_physics_geometry_hull(box_hull);
    return 4U;
}

// Poses about `at`: on it, and a little off along each axis and a diagonal,
// turned a different way each.
static inline NphPhysicsPose fuzz_pose(NphVec3 at, u32 k)
{
    static const f32 offsets[6][3] = {{0.0F, 0.0F, 0.0F},  {0.6F, 0.0F, 0.0F}, {0.0F, 0.6F, 0.0F},
                                      {0.0F, 0.0F, -0.6F}, {0.3F, 0.4F, 0.2F}, {-1.5F, 0.1F, 0.0F}};
    const f32 *o = offsets[k % 6U];
    const NphQuat q = nph_quat_from_axis_angle(
        nph_vec3_normalize(nph_vec3(1.0F, 2.0F + (f32)k, 3.0F)), 0.7F * (f32)k);
    return nph_physics_pose(nph_vec3_add(at, nph_vec3(o[0], o[1], o[2])), q);
}
