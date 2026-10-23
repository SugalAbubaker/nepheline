// SPDX-License-Identifier: Apache-2.0
// Mass properties, bounds, support points, and proxies of the collision
// geometries (geometry.h, types.h).

#include <nepheline/core/assert.h>
#include <nepheline/physics/geometry.h>

#define NPH_PI_F64 3.14159265358979323846

// ---------------------------------------------------------------------------
// Mass properties
// ---------------------------------------------------------------------------

NphMat3 nph_physics_inertia_offset(f32 mass, NphVec3 r)
{
    // mass (|r|^2 E - r r^T): the tensor of a point mass about a point at -r.
    const f32 rr = nph_vec3_dot(r, r);
    const NphVec3 c0 = nph_vec3(rr - r.x * r.x, -r.y * r.x, -r.z * r.x);
    const NphVec3 c1 = nph_vec3(-r.x * r.y, rr - r.y * r.y, -r.z * r.y);
    const NphVec3 c2 = nph_vec3(-r.x * r.z, -r.y * r.z, rr - r.z * r.z);
    return nph_mat3_scale(nph_mat3_from_cols(c0, c1, c2), mass);
}

NphPhysicsMassData nph_physics_mass_transform(NphPhysicsMassData m, NphPhysicsPose pose)
{
    const NphMat3 r = nph_mat3_from_quat(pose.rotation);
    NphPhysicsMassData out;
    out.mass = m.mass;
    out.center = nph_physics_pose_transform_point(pose, m.center);
    out.inertia = nph_mat3_mul(nph_mat3_mul(r, m.inertia), nph_mat3_transpose(r));
    return out;
}

NphPhysicsMassData nph_physics_mass_add(NphPhysicsMassData a, NphPhysicsMassData b)
{
    if (a.mass <= 0.0F) {
        return b;
    }
    if (b.mass <= 0.0F) {
        return a;
    }
    NphPhysicsMassData out;
    out.mass = a.mass + b.mass;
    out.center = nph_vec3_scale(
        nph_vec3_add(nph_vec3_scale(a.center, a.mass), nph_vec3_scale(b.center, b.mass)),
        1.0F / out.mass);
    const NphMat3 ia = nph_mat3_add(
        a.inertia, nph_physics_inertia_offset(a.mass, nph_vec3_sub(a.center, out.center)));
    const NphMat3 ib = nph_mat3_add(
        b.inertia, nph_physics_inertia_offset(b.mass, nph_vec3_sub(b.center, out.center)));
    out.inertia = nph_mat3_add(ia, ib);
    return out;
}

static NphPhysicsMassData mass_diagonal(f32 mass, NphVec3 center, f32 ixx, f32 iyy, f32 izz)
{
    NphPhysicsMassData m;
    m.mass = mass;
    m.center = center;
    m.inertia = nph_mat3_from_diagonal(nph_vec3(ixx, iyy, izz));
    return m;
}

static NphPhysicsMassData sphere_mass(f32 radius, f32 density)
{
    const f64 r = (f64)radius;
    const f64 mass = (f64)density * (4.0 / 3.0) * NPH_PI_F64 * r * r * r;
    const f64 i = 0.4 * mass * r * r;
    return mass_diagonal((f32)mass, nph_vec3_zero(), (f32)i, (f32)i, (f32)i);
}

static NphPhysicsMassData capsule_mass(f32 half_height, f32 radius, f32 density)
{
    // A cylinder of height 2h and two hemispheres. A hemisphere's tensor
    // about a diameter of its flat face is (2/5) m r^2; moved through its
    // centre of mass (3r/8 from the face) to the capsule's centre, h from the
    // face, it becomes (2/5) m r^2 + m (h^2 + 3 h r / 4).
    const f64 h = (f64)half_height;
    const f64 r = (f64)radius;
    const f64 rho = (f64)density;
    const f64 m_cyl = rho * NPH_PI_F64 * r * r * (2.0 * h);
    const f64 m_hemi = rho * (2.0 / 3.0) * NPH_PI_F64 * r * r * r;
    const f64 axial = m_cyl * r * r * 0.5 + 2.0 * (0.4 * m_hemi * r * r);
    const f64 cyl_perp = m_cyl * ((2.0 * h) * (2.0 * h) / 12.0 + r * r * 0.25);
    const f64 hemi_perp = 0.4 * m_hemi * r * r + m_hemi * (h * h + 0.75 * h * r);
    const f64 perp = cyl_perp + 2.0 * hemi_perp;
    return mass_diagonal((f32)(m_cyl + 2.0 * m_hemi), nph_vec3_zero(), (f32)perp, (f32)axial,
                         (f32)perp);
}

static NphPhysicsMassData box_mass(NphVec3 he, f32 density)
{
    const f64 x = (f64)he.x;
    const f64 y = (f64)he.y;
    const f64 z = (f64)he.z;
    const f64 mass = (f64)density * 8.0 * x * y * z;
    const f64 k = mass / 3.0;
    return mass_diagonal((f32)mass, nph_vec3_zero(), (f32)(k * (y * y + z * z)),
                         (f32)(k * (x * x + z * z)), (f32)(k * (x * x + y * y)));
}

static NphPhysicsMassData hull_mass(const NphPhysicsHull *hull, f32 density)
{
    NphPhysicsMassData m;
    m.mass = hull->volume * density;
    m.center = hull->centroid;
    m.inertia = nph_mat3_scale(hull->inertia, density);
    return m;
}

NphPhysicsMassData nph_physics_geometry_mass(const NphPhysicsGeometry *g, f32 density)
{
    NPH_ASSERT(g != NULL);
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        return sphere_mass(g->sphere.radius, density);
    case NPH_PHYSICS_SHAPE_CAPSULE:
        return capsule_mass(g->capsule.half_height, g->capsule.radius, density);
    case NPH_PHYSICS_SHAPE_BOX:
        return box_mass(g->box.half_extents, density);
    case NPH_PHYSICS_SHAPE_HULL:
        return hull_mass(g->hull, density);
    case NPH_PHYSICS_SHAPE_MESH:
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD:
        // A surface bounds no volume (static and kinematic bodies only).
        return mass_diagonal(0.0F, nph_vec3_zero(), 0.0F, 0.0F, 0.0F);
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
    NPH_ASSERT_MSG(false, "nph_physics_geometry_mass: unknown shape type %u", (u32)g->type);
    return mass_diagonal(0.0F, nph_vec3_zero(), 0.0F, 0.0F, 0.0F);
}

// ---------------------------------------------------------------------------
// Validity, bounds, support
// ---------------------------------------------------------------------------

// Positive, finite, and at most NPH_PHYSICS_MAX_COORDINATE.
static bool size_ok(f32 v)
{
    return v > 0.0F && v <= NPH_PHYSICS_MAX_COORDINATE;
}

bool nph_physics_geometry_is_valid(const NphPhysicsGeometry *g)
{
    if (g == NULL) {
        return false;
    }
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        return size_ok(g->sphere.radius);
    case NPH_PHYSICS_SHAPE_CAPSULE:
        // A capsule of zero height is a sphere and is allowed.
        return size_ok(g->capsule.radius) && g->capsule.half_height >= 0.0F &&
               g->capsule.half_height <= NPH_PHYSICS_MAX_COORDINATE;
    case NPH_PHYSICS_SHAPE_BOX:
        return size_ok(g->box.half_extents.x) && size_ok(g->box.half_extents.y) &&
               size_ok(g->box.half_extents.z);
    case NPH_PHYSICS_SHAPE_HULL:
        return g->hull != NULL && g->hull->vertex_count >= 4U;
    case NPH_PHYSICS_SHAPE_MESH:
        return g->mesh != NULL && g->mesh->triangle_count > 0U;
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD:
        return g->heightfield != NULL && g->heightfield->columns >= 2U &&
               g->heightfield->rows >= 2U;
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        return false;
    }
}

// A box placed by `pose`: the extent along each axis is |R| times the half
// extents (Arvo, "Transforming Axis-Aligned Bounding Boxes", Graphics Gems
// 1990).
static NphAABB transform_box(NphAABB b, NphPhysicsPose pose)
{
    const NphMat3 m = nph_mat3_from_quat(pose.rotation);
    const NphVec3 h = nph_vec3_scale(nph_vec3_sub(b.max, b.min), 0.5F);
    const NphVec3 c = nph_physics_pose_transform_point(pose, nph_aabb_center(b));
    const NphVec3 e = nph_vec3_add(nph_vec3_add(nph_vec3_scale(nph_vec3_abs(m.c[0]), h.x),
                                                nph_vec3_scale(nph_vec3_abs(m.c[1]), h.y)),
                                   nph_vec3_scale(nph_vec3_abs(m.c[2]), h.z));
    return nph_aabb(nph_vec3_sub(c, e), nph_vec3_add(c, e));
}

NphAABB nph_physics_geometry_aabb(const NphPhysicsGeometry *g, NphPhysicsPose pose)
{
    NPH_ASSERT(g != NULL);
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE: {
        const NphVec3 r = nph_vec3_splat(g->sphere.radius);
        return nph_aabb(nph_vec3_sub(pose.position, r), nph_vec3_add(pose.position, r));
    }
    case NPH_PHYSICS_SHAPE_CAPSULE: {
        const NphVec3 axis = nph_vec3_scale(nph_quat_axis_y(pose.rotation), g->capsule.half_height);
        const NphVec3 a = nph_vec3_sub(pose.position, axis);
        const NphVec3 b = nph_vec3_add(pose.position, axis);
        const NphVec3 r = nph_vec3_splat(g->capsule.radius);
        return nph_aabb(nph_vec3_sub(nph_vec3_min(a, b), r), nph_vec3_add(nph_vec3_max(a, b), r));
    }
    case NPH_PHYSICS_SHAPE_BOX:
        return transform_box(nph_aabb(nph_vec3_neg(g->box.half_extents), g->box.half_extents),
                             pose);
    case NPH_PHYSICS_SHAPE_HULL: {
        const NphPhysicsHull *hull = g->hull;
        const NphVec3 *v = nph_physics_hull_vertices(hull);
        NphAABB box = nph_aabb_from_point(nph_physics_pose_transform_point(pose, v[0]));
        for (u32 i = 1U; i < hull->vertex_count; ++i) {
            const NphVec3 p = nph_physics_pose_transform_point(pose, v[i]);
            box.min = nph_vec3_min(box.min, p);
            box.max = nph_vec3_max(box.max, p);
        }
        return box;
    }
    case NPH_PHYSICS_SHAPE_MESH:
        return transform_box(g->mesh->bounds, pose);
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD:
        return transform_box(g->heightfield->bounds, pose);
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
    NPH_ASSERT_MSG(false, "nph_physics_geometry_aabb: unknown shape type %u", (u32)g->type);
    return nph_aabb_from_point(pose.position);
}

// sign(x) with +1 for zero, so a zero direction still picks a corner.
static f32 support_sign(f32 x)
{
    return x < 0.0F ? -1.0F : 1.0F;
}

static NphVec3 rounding(NphVec3 direction, f32 radius)
{
    const f32 len_sq = nph_vec3_length_sq(direction);
    if (len_sq <= 0.0F) {
        return nph_vec3(0.0F, radius, 0.0F);
    }
    return nph_vec3_scale(direction, radius / nph_f32_sqrt(len_sq));
}

NphVec3 nph_physics_geometry_support(const NphPhysicsGeometry *g, NphVec3 direction)
{
    NPH_ASSERT(g != NULL);
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        return rounding(direction, g->sphere.radius);
    case NPH_PHYSICS_SHAPE_CAPSULE: {
        const NphVec3 core =
            nph_vec3(0.0F, support_sign(direction.y) * g->capsule.half_height, 0.0F);
        return nph_vec3_add(core, rounding(direction, g->capsule.radius));
    }
    case NPH_PHYSICS_SHAPE_BOX: {
        const NphVec3 h = g->box.half_extents;
        return nph_vec3(support_sign(direction.x) * h.x, support_sign(direction.y) * h.y,
                        support_sign(direction.z) * h.z);
    }
    case NPH_PHYSICS_SHAPE_HULL:
        return nph_physics_hull_vertices(g->hull)[nph_physics_hull_support(g->hull, direction)];
    case NPH_PHYSICS_SHAPE_MESH: {
        const NphVec3 *v = nph_physics_mesh_vertices(g->mesh);
        u32 best = 0U;
        for (u32 i = 1U; i < g->mesh->vertex_count; ++i) {
            if (nph_vec3_dot(v[i], direction) > nph_vec3_dot(v[best], direction)) {
                best = i;
            }
        }
        return v[best];
    }
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD: {
        const NphPhysicsHeightfield *h = g->heightfield;
        const f32 *heights = nph_physics_heightfield_heights(h);
        NphVec3 best = nph_vec3(0.0F, heights[0], 0.0F);
        for (u32 j = 0U; j < h->rows; ++j) {
            for (u32 i = 0U; i < h->columns; ++i) {
                const NphVec3 p = nph_vec3((f32)i * h->spacing_x, heights[(u64)j * h->columns + i],
                                           (f32)j * h->spacing_z);
                if (nph_vec3_dot(p, direction) > nph_vec3_dot(best, direction)) {
                    best = p;
                }
            }
        }
        return best;
    }
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
    NPH_ASSERT_MSG(false, "nph_physics_geometry_support: unknown shape type %u", (u32)g->type);
    return nph_vec3_zero();
}

f32 nph_physics_geometry_inner_radius(const NphPhysicsGeometry *g)
{
    NPH_ASSERT(g != NULL);
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        return g->sphere.radius;
    case NPH_PHYSICS_SHAPE_CAPSULE:
        return g->capsule.radius;
    case NPH_PHYSICS_SHAPE_BOX:
        return nph_vec3_min_component(g->box.half_extents);
    case NPH_PHYSICS_SHAPE_HULL:
        return g->hull->inner_radius;
    case NPH_PHYSICS_SHAPE_MESH: // a surface holds no ball
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD:
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        return 0.0F;
    }
}

f32 nph_physics_geometry_outer_radius(const NphPhysicsGeometry *g)
{
    NPH_ASSERT(g != NULL);
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        return g->sphere.radius;
    case NPH_PHYSICS_SHAPE_CAPSULE:
        return g->capsule.half_height + g->capsule.radius;
    case NPH_PHYSICS_SHAPE_BOX:
        return nph_vec3_length(g->box.half_extents);
    case NPH_PHYSICS_SHAPE_HULL:
        return g->hull->outer_radius;
    case NPH_PHYSICS_SHAPE_MESH:
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD: {
        // The farthest corner of the vertices' box: a bound, in constant time.
        const NphAABB b =
            g->type == NPH_PHYSICS_SHAPE_MESH ? g->mesh->bounds : g->heightfield->bounds;
        return nph_vec3_length(nph_vec3_max(nph_vec3_abs(b.min), nph_vec3_abs(b.max)));
    }
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        return 0.0F;
    }
}

// ---------------------------------------------------------------------------
// Proxies
// ---------------------------------------------------------------------------

NphPhysicsProxy nph_physics_geometry_proxy(const NphPhysicsGeometry *g,
                                           NphVec3 storage[NPH_PHYSICS_PROXY_STORAGE])
{
    NPH_ASSERT(g != NULL && storage != NULL);
    NphPhysicsProxy p;
    switch (g->type) {
    case NPH_PHYSICS_SHAPE_SPHERE:
        storage[0] = nph_vec3_zero();
        p.points = storage;
        p.count = 1U;
        p.radius = g->sphere.radius;
        return p;
    case NPH_PHYSICS_SHAPE_CAPSULE:
        storage[0] = nph_vec3(0.0F, -g->capsule.half_height, 0.0F);
        storage[1] = nph_vec3(0.0F, g->capsule.half_height, 0.0F);
        p.points = storage;
        p.count = g->capsule.half_height > 0.0F ? 2U : 1U;
        p.radius = g->capsule.radius;
        return p;
    case NPH_PHYSICS_SHAPE_BOX: {
        const NphVec3 h = g->box.half_extents;
        for (u32 i = 0U; i < 8U; ++i) {
            storage[i] = nph_vec3((i & 1U) != 0U ? h.x : -h.x, (i & 2U) != 0U ? h.y : -h.y,
                                  (i & 4U) != 0U ? h.z : -h.z);
        }
        p.points = storage;
        p.count = 8U;
        p.radius = 0.0F;
        return p;
    }
    case NPH_PHYSICS_SHAPE_HULL:
        p.points = nph_physics_hull_vertices(g->hull);
        p.count = g->hull->vertex_count;
        p.radius = 0.0F;
        return p;
    case NPH_PHYSICS_SHAPE_MESH: // not convex
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD:
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
    NPH_ASSERT_MSG(false, "nph_physics_geometry_proxy: unknown shape type %u", (u32)g->type);
    storage[0] = nph_vec3_zero();
    p.points = storage;
    p.count = 1U;
    p.radius = 0.0F;
    return p;
}

u32 nph_physics_proxy_support(const NphPhysicsProxy *p, NphVec3 direction)
{
    NPH_ASSERT(p != NULL && p->count >= 1U);
    u32 best = 0U;
    f32 best_dot = nph_vec3_dot(p->points[0], direction);
    for (u32 i = 1U; i < p->count; ++i) {
        const f32 d = nph_vec3_dot(p->points[i], direction);
        if (d > best_dot) {
            best_dot = d;
            best = i;
        }
    }
    return best;
}
