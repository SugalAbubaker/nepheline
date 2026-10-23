// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for triangle meshes (engine/physics/mesh.h; ADR-0046;
// CLAUDE.md §8.8 "every parser is fuzzed; malformed input must never
// crash"). The input is a triangle soup for the builder, whose mesh must
// validate and survive every use, then damage to a copy of that mesh:
// validation must never read past the bytes it is given, and a damaged mesh
// it accepts must survive every use too, giving finite contacts.
//
// Input (little-endian; fuzz_physics_common.h):
//   u8  flags          bit 0 two-sided, bit 1 weld exact copies only, bit 2 a
//                      30 degree active-edge angle
//   u8  vertex count
//   u8  triangle count
//   i16 x, y, z        each vertex, in 1/256 m
//   u8  i0, i1, i2     each triangle (an index past the vertices is refused)
//   u16 truncation, then (u16 offset, u8 xor) damage records
#include <nepheline/core/log.h>
#include <nepheline/physics/mesh.h>

#include "fuzz_physics_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef struct Visits {
    u32 count;
    u32 triangle_count;
} Visits;

static bool visit(void *user, u32 triangle)
{
    Visits *v = user;
    if (triangle >= v->triangle_count) {
        abort(); // a query reported a triangle the mesh does not have
    }
    v->count += 1U;
    return v->count < 4096U;
}

static void check_hit(const NphPhysicsMesh *m, const NphPhysicsMeshHit *hit)
{
    if (hit->triangle >= m->triangle_count || !(hit->fraction >= 0.0F && hit->fraction <= 1.0F) ||
        !nph_vec3_is_finite(hit->normal)) {
        abort();
    }
}

// Every use the module makes of a mesh: its triangles, box queries,
// raycasts, and every convex shape's manifolds against it.
static void exercise(const NphPhysicsMesh *m)
{
    const NphPhysicsGeometry g = nph_physics_geometry_mesh(m);
    if (!nph_physics_geometry_is_valid(&g)) {
        abort(); // a valid mesh makes valid geometry
    }
    NPH_UNUSED(nph_physics_geometry_aabb(&g, fuzz_pose(nph_vec3_zero(), 4U)));
    NphVec3 tri[3];
    nph_physics_mesh_triangle(m, 0U, tri);
    nph_physics_mesh_triangle(m, m->triangle_count - 1U, tri);
    const NphVec3 centre = nph_aabb_center(m->bounds);
    const NphVec3 first = nph_physics_mesh_vertices(m)[0];
    Visits v = {0U, m->triangle_count};
    nph_physics_mesh_overlap(m, m->bounds, visit, &v);
    v.count = 0U;
    nph_physics_mesh_overlap(m,
                             nph_aabb(nph_vec3_sub(first, nph_vec3_splat(0.5F)),
                                      nph_vec3_add(first, nph_vec3_splat(0.5F))),
                             visit, &v);
    // Rays down through the centre, along each axis, and diagonally across.
    const NphVec3 size = nph_vec3_add(nph_aabb_size(m->bounds), nph_vec3_splat(1.0F));
    const NphVec3 starts[4] = {
        nph_vec3(centre.x, m->bounds.max.y + 1.0F, centre.z),
        nph_vec3(m->bounds.min.x - 1.0F, centre.y, centre.z),
        nph_vec3(centre.x, centre.y, m->bounds.max.z + 1.0F),
        nph_vec3_sub(m->bounds.min, nph_vec3_splat(0.5F)),
    };
    const NphVec3 deltas[4] = {
        nph_vec3(0.0F, -size.y - 1.0F, 0.0F),
        nph_vec3(size.x + 1.0F, 0.0F, 0.0F),
        nph_vec3(0.0F, 0.0F, -size.z - 1.0F),
        nph_vec3_add(size, nph_vec3_splat(0.5F)),
    };
    for (u32 i = 0U; i < 4U; ++i) {
        NphPhysicsMeshHit hit;
        if (nph_physics_mesh_raycast(m, starts[i], deltas[i], &hit)) {
            check_hit(m, &hit);
        }
    }
    // The convex shapes at the centre, at the first vertex, and just above
    // the first triangle, the mesh placed a little turned or far out.
    NphPhysicsGeometry shapes[4];
    const u32 count = fuzz_convex_shapes(shapes);
    nph_physics_mesh_triangle(m, 0U, tri);
    const NphVec3 above = nph_vec3_add(
        nph_vec3_scale(nph_vec3_add(nph_vec3_add(tri[0], tri[1]), tri[2]), 1.0F / 3.0F),
        nph_vec3(0.0F, 0.2F, 0.0F));
    const NphVec3 places[3] = {centre, first, above};
    for (u32 p = 0U; p < 3U; ++p) {
        for (u32 s = 0U; s < count; ++s) {
            const NphPhysicsPose pose = fuzz_pose(places[p], p + s);
            NphPhysicsMeshManifolds mm;
            nph_physics_collide_mesh(&shapes[s], pose, m, nph_physics_pose_identity(),
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
    u8 vertex_count;
    u8 triangle_count;
    if (!fuzz_u8(&r, &flags) || !fuzz_u8(&r, &vertex_count) || !fuzz_u8(&r, &triangle_count)) {
        return 0;
    }
    NphVec3 vertices[255];
    for (u32 i = 0U; i < vertex_count; ++i) {
        if (!fuzz_vec3(&r, &vertices[i])) {
            return 0;
        }
    }
    u32 indices[3U * 255U];
    for (u32 i = 0U; i < 3U * triangle_count; ++i) {
        u8 index;
        if (!fuzz_u8(&r, &index)) {
            return 0;
        }
        indices[i] = index;
    }
    NphPhysicsMeshDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.vertices = vertices;
    desc.vertex_count = vertex_count;
    desc.indices = indices;
    desc.triangle_count = triangle_count;
    desc.two_sided = (flags & 1U) != 0U;
    desc.weld_tolerance = (flags & 2U) != 0U ? -1.0F : 0.0F;
    desc.active_edge_angle = (flags & 4U) != 0U ? 30.0F * 3.14159265F / 180.0F : 0.0F;
    NphAllocator *alloc = nph_allocator_system();
    NphPhysicsMesh *mesh = NULL;
    if (nph_physics_mesh_build(&desc, alloc, &mesh) != NPH_RESULT_OK) {
        return 0;
    }
    if (!nph_physics_mesh_validate(mesh, mesh->byte_size)) {
        abort(); // the builder's mesh fails validation
    }
    exercise(mesh);
    u32 damaged_size = 0U;
    void *damaged = fuzz_damage(&r, mesh, mesh->byte_size, &damaged_size);
    if (damaged != NULL) {
        if (nph_physics_mesh_validate(damaged, damaged_size)) {
            exercise(damaged);
        }
        fuzz_damage_free(damaged, damaged_size);
    }
    nph_physics_mesh_free(mesh, alloc);
    return 0;
}
