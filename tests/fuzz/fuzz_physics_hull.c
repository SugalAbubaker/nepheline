// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for convex hulls (engine/physics/hull.h; CLAUDE.md §8.8
// "every parser is fuzzed; malformed input must never crash"). The input is
// a point cloud for the builder, whose hull must validate and survive every
// use, then damage to a copy of that hull: validation must never read past
// the bytes it is given, and a damaged hull it accepts must survive every
// use too, giving finite contacts.
//
// Input (little-endian; fuzz_physics_common.h):
//   u8  budget         the vertex budget: 0 the default, else 4 + budget % 253
//   u8  point count
//   i16 x, y, z        each point, in 1/256 m
//   u16 truncation, then (u16 offset, u8 xor) damage records
#include <nepheline/core/log.h>

#include "fuzz_physics_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Every use the module makes of a hull: the geometry queries, contacts with
// each convex shape and a triangle, and GJK and EPA on its proxy.
static void exercise(const NphPhysicsHull *hull)
{
    const NphPhysicsGeometry g = nph_physics_geometry_hull(hull);
    if (!nph_physics_geometry_is_valid(&g)) {
        abort(); // a valid hull makes valid geometry
    }
    const NphPhysicsPose home = nph_physics_pose_identity();
    NPH_UNUSED(nph_physics_geometry_mass(&g, 1000.0F));
    NPH_UNUSED(nph_physics_geometry_aabb(&g, fuzz_pose(nph_vec3_zero(), 4U)));
    NPH_UNUSED(nph_physics_geometry_inner_radius(&g));
    NPH_UNUSED(nph_physics_geometry_outer_radius(&g));
    for (u32 f = 0U; f < hull->face_count; ++f) {
        NPH_UNUSED(nph_physics_hull_face_vertex_count(hull, f));
    }
    const NphVec3 centre = nph_aabb_center(hull->bounds);
    NphPhysicsGeometry shapes[4];
    const u32 count = fuzz_convex_shapes(shapes);
    NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE];
    NphVec3 own_storage[NPH_PHYSICS_PROXY_STORAGE];
    const NphPhysicsProxy own = nph_physics_geometry_proxy(&g, own_storage);
    for (u32 k = 0U; k < 6U; ++k) {
        // At the centre, and at the surface along a direction.
        const NphVec3 dir = nph_vec3_normalize(nph_vec3((f32)k - 2.5F, 1.0F, 0.5F * (f32)k));
        const NphVec3 at = (k & 1U) != 0U ? nph_physics_geometry_support(&g, dir) : centre;
        for (u32 s = 0U; s < count; ++s) {
            const NphPhysicsPose pose = fuzz_pose(at, k + s);
            NphPhysicsManifold m;
            NphPhysicsCollideCache cache;
            memset(&cache, 0, sizeof cache);
            nph_physics_collide(&g, home, &shapes[s], pose, NPH_PHYSICS_SPECULATIVE_DISTANCE,
                                &cache, &m);
            fuzz_check_manifold(&m);
            // Again from the cache, and in the other order.
            nph_physics_collide(&g, home, &shapes[s], pose, NPH_PHYSICS_SPECULATIVE_DISTANCE,
                                &cache, &m);
            fuzz_check_manifold(&m);
            nph_physics_collide(&shapes[s], pose, &g, home, NPH_PHYSICS_SPECULATIVE_DISTANCE, NULL,
                                &m);
            fuzz_check_manifold(&m);
            const NphPhysicsProxy other = nph_physics_geometry_proxy(&shapes[s], storage);
            NphPhysicsDistance d;
            nph_physics_distance(&own, home, &other, pose, true, NULL, &d);
            NphPhysicsPenetration pen;
            NPH_UNUSED(nph_physics_penetration(&own, home, &other, pose, &pen));
        }
        // The hull against itself, turned.
        NphPhysicsManifold m;
        nph_physics_collide(&g, home, &g, fuzz_pose(nph_vec3_sub(at, centre), k), 0.02F, NULL, &m);
        fuzz_check_manifold(&m);
        // A triangle through the point.
        const NphPhysicsTriangle t = {
            {nph_vec3_add(at, nph_vec3(-1.0F, 0.0F, -1.0F)),
             nph_vec3_add(at, nph_vec3(0.0F, 0.1F, 1.0F)),
             nph_vec3_add(at, nph_vec3(1.0F, -0.1F, -1.0F))},
            NPH_PHYSICS_MESH_EDGES,
            (k & 2U) != 0U,
        };
        nph_physics_collide_triangle(&g, home, &t, NPH_PHYSICS_SPECULATIVE_DISTANCE, &m);
        fuzz_check_manifold(&m);
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
    u8 budget;
    u8 count;
    if (!fuzz_u8(&r, &budget) || !fuzz_u8(&r, &count)) {
        return 0;
    }
    NphVec3 points[255];
    for (u32 i = 0U; i < count; ++i) {
        if (!fuzz_vec3(&r, &points[i])) {
            return 0;
        }
    }
    NphPhysicsHullDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.points = points;
    desc.point_count = count;
    desc.max_vertices = budget == 0U ? 0U : 4U + budget % (NPH_PHYSICS_HULL_MAX_VERTICES - 3U);
    NphAllocator *alloc = nph_allocator_system();
    NphPhysicsHull *hull = NULL;
    if (nph_physics_hull_build(&desc, alloc, &hull) != NPH_RESULT_OK) {
        return 0;
    }
    if (!nph_physics_hull_validate(hull, hull->byte_size, 0.0F)) {
        abort(); // the builder's hull fails validation
    }
    exercise(hull);
    u32 damaged_size = 0U;
    void *damaged = fuzz_damage(&r, hull, hull->byte_size, &damaged_size);
    if (damaged != NULL) {
        if (nph_physics_hull_validate(damaged, damaged_size, 0.0F)) {
            exercise(damaged);
        }
        fuzz_damage_free(damaged, damaged_size);
    }
    nph_physics_hull_free(hull, alloc);
    return 0;
}
