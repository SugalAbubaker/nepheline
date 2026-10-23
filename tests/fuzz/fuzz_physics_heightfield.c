// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for heightfields (engine/physics/heightfield.h; ADR-0046
// §4; CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never
// crash"). The input is a grid for the builder, whose heightfield must
// validate and survive every use, then damage to a copy of it: validation
// must never read past the bytes it is given, and a damaged heightfield it
// accepts must survive every use too, giving finite contacts.
//
// Input (little-endian; fuzz_physics_common.h):
//   u8  flags          bits 0-1 and 4-5 the spacing along X and Z (0.25,
//                      0.5, 1, or 2 m), bit 2 a 30 degree active-edge
//                      angle, bit 3 cell flags follow
//   u8  columns, rows  2 + each % 31
//   i16 heights        columns * rows, in 1/256 m
//   u8  cells          with bit 3: (columns - 1) * (rows - 1), each & 3
//   u16 truncation, then (u16 offset, u8 xor) damage records
#include <nepheline/core/log.h>
#include <nepheline/physics/heightfield.h>

#include "fuzz_physics_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define FUZZ_SIDE 32U

// `user` points at the triangle count.
static bool visit(void *user, u32 triangle)
{
    if (triangle >= *(const u32 *)user) {
        abort(); // a query reported a triangle the heightfield does not have
    }
    return true;
}

// Every use the module makes of a heightfield: its triangles and heights,
// box queries, raycasts, and every convex shape's manifolds against it.
static void exercise(const NphPhysicsHeightfield *h)
{
    const NphPhysicsGeometry g = nph_physics_geometry_heightfield(h);
    if (!nph_physics_geometry_is_valid(&g)) {
        abort(); // a valid heightfield makes valid geometry
    }
    NPH_UNUSED(nph_physics_geometry_aabb(&g, fuzz_pose(nph_vec3_zero(), 4U)));
    NPH_UNUSED(nph_physics_geometry_support(&g, nph_vec3(0.3F, 1.0F, -0.2F)));
    u32 triangles = nph_physics_heightfield_triangle_count(h);
    for (u32 t = 0U; t < triangles; ++t) {
        NphVec3 v[3];
        u32 active = 0U;
        if (nph_physics_heightfield_triangle(h, t, v, &active) &&
            (!nph_vec3_is_finite(v[0]) || !nph_vec3_is_finite(v[1]) || !nph_vec3_is_finite(v[2]) ||
             active > NPH_PHYSICS_MESH_EDGES)) {
            abort();
        }
    }
    const NphVec3 centre = nph_aabb_center(h->bounds);
    const NphVec3 size = nph_vec3_add(nph_aabb_size(h->bounds), nph_vec3_splat(1.0F));
    nph_physics_heightfield_overlap(h, h->bounds, visit, &triangles);
    nph_physics_heightfield_overlap(h,
                                    nph_aabb(nph_vec3_sub(centre, nph_vec3_splat(0.7F)),
                                             nph_vec3_add(centre, nph_vec3_splat(0.7F))),
                                    visit, &triangles);
    // Rays down through the centre and a corner, across, and diagonally.
    const NphVec3 starts[4] = {
        nph_vec3(centre.x, h->bounds.max.y + 1.0F, centre.z),
        nph_vec3(h->bounds.min.x - 1.0F, centre.y, centre.z),
        nph_vec3(0.0F, h->bounds.max.y + 1.0F, 0.0F),
        nph_vec3_sub(h->bounds.min, nph_vec3_splat(0.5F)),
    };
    const NphVec3 deltas[4] = {
        nph_vec3(0.0F, -size.y - 1.0F, 0.0F),
        nph_vec3(size.x + 1.0F, 0.0F, 0.0F),
        nph_vec3(0.0F, -size.y - 1.0F, 0.0F),
        nph_vec3(size.x, size.y * 0.5F, size.z),
    };
    for (u32 i = 0U; i < 4U; ++i) {
        NphPhysicsMeshHit hit;
        if (nph_physics_heightfield_raycast(h, starts[i], deltas[i], &hit) &&
            (hit.triangle >= triangles || !(hit.fraction >= 0.0F && hit.fraction <= 1.0F) ||
             !nph_vec3_is_finite(hit.normal))) {
            abort();
        }
        f32 y = 0.0F;
        NphVec3 n;
        if (nph_physics_heightfield_height_at(h, starts[i].x, starts[i].z, &y, &n) &&
            (!nph_f32_is_finite(y) || !nph_vec3_is_finite(n))) {
            abort();
        }
    }
    // The convex shapes at the centre, over the first sample, and over the
    // far corner.
    NphPhysicsGeometry shapes[4];
    const u32 count = fuzz_convex_shapes(shapes);
    const NphVec3 places[3] = {centre, nph_vec3(0.1F, h->bounds.max.y, 0.1F),
                               nph_vec3(h->bounds.max.x - 0.1F, centre.y, h->bounds.max.z - 0.1F)};
    for (u32 p = 0U; p < 3U; ++p) {
        for (u32 s = 0U; s < count; ++s) {
            NphPhysicsMeshManifolds mm;
            nph_physics_collide_heightfield(&shapes[s], fuzz_pose(places[p], p + s), h,
                                            nph_physics_pose_identity(),
                                            NPH_PHYSICS_SPECULATIVE_DISTANCE, &mm);
            if (mm.count > NPH_PHYSICS_MESH_MAX_MANIFOLDS) {
                abort();
            }
            for (u32 k = 0U; k < mm.count; ++k) {
                fuzz_check_manifold(&mm.manifolds[k]);
            }
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1U << 16U)) {
        return 0;
    }
    // Rejecting input is the common case; logging every rejection would
    // dominate the run.
    nph_log_set_global_level(NPH_LOG_LEVEL_OFF);
    FuzzReader r = {data, size};
    u8 flags;
    u8 c;
    u8 rw;
    if (!fuzz_u8(&r, &flags) || !fuzz_u8(&r, &c) || !fuzz_u8(&r, &rw)) {
        return 0;
    }
    const u32 columns = 2U + c % (FUZZ_SIDE - 1U);
    const u32 rows = 2U + rw % (FUZZ_SIDE - 1U);
    f32 heights[FUZZ_SIDE * FUZZ_SIDE];
    for (u32 i = 0U; i < columns * rows; ++i) {
        if (!fuzz_coordinate(&r, &heights[i])) {
            return 0;
        }
    }
    u8 cells[FUZZ_SIDE * FUZZ_SIDE];
    const bool has_cells = (flags & 8U) != 0U;
    if (has_cells) {
        for (u32 i = 0U; i < (columns - 1U) * (rows - 1U); ++i) {
            u8 cell;
            if (!fuzz_u8(&r, &cell)) {
                return 0;
            }
            cells[i] = (u8)(cell & 3U);
        }
    }
    static const f32 spacings[4] = {0.25F, 0.5F, 1.0F, 2.0F};
    NphPhysicsHeightfieldDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.heights = heights;
    desc.columns = columns;
    desc.rows = rows;
    desc.spacing_x = spacings[flags & 3U];
    desc.spacing_z = spacings[(flags >> 4U) & 3U];
    desc.cells = has_cells ? cells : NULL;
    desc.active_edge_angle = (flags & 4U) != 0U ? 30.0F * 3.14159265F / 180.0F : 0.0F;
    NphAllocator *alloc = nph_allocator_system();
    NphPhysicsHeightfield *field = NULL;
    if (nph_physics_heightfield_build(&desc, alloc, &field) != NPH_RESULT_OK) {
        return 0;
    }
    if (!nph_physics_heightfield_validate(field, field->byte_size)) {
        abort(); // the builder's heightfield fails validation
    }
    exercise(field);
    u32 damaged_size = 0U;
    void *damaged = fuzz_damage(&r, field, field->byte_size, &damaged_size);
    if (damaged != NULL) {
        if (nph_physics_heightfield_validate(damaged, damaged_size)) {
            exercise(damaged);
        }
        fuzz_damage_free(damaged, damaged_size);
    }
    nph_physics_heightfield_free(field, alloc);
    return 0;
}
