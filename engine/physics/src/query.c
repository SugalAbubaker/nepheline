// SPDX-License-Identifier: Apache-2.0
// World queries (query.h; docs/physics.md "Queries"; ADR-0047): the
// broadphase's trees walked, then each shape found tested exactly in a frame
// centred on the query's origin.

#include <nepheline/core/assert.h>
#include <nepheline/physics/query.h>

#include "world_internal.h"

#include <string.h>

NphPhysicsQueryFilter nph_physics_query_filter_default(void)
{
    NphPhysicsQueryFilter f;
    f.category = 1U;
    f.mask = NPH_U64_MAX;
    f.body_types = NPH_PHYSICS_QUERY_ALL_BODIES;
    f.ignore = NPH_PHYSICS_BODY_NULL;
    return f;
}

// The trees in the order queries walk them, each with the kind of body whose
// shapes it holds.
static const u32 k_trees[PHYS_TREE_COUNT][2] = {
    {PHYS_TREE_STATIC, NPH_PHYSICS_QUERY_STATIC},
    {PHYS_TREE_KINEMATIC, NPH_PHYSICS_QUERY_KINEMATIC},
    {PHYS_TREE_DYNAMIC, NPH_PHYSICS_QUERY_DYNAMIC},
};

// What every query carries.
typedef struct Query {
    const NphPhysicsWorld *w;
    NphPhysicsQueryFilter filter;
    NphDVec3 origin; // the frame shapes are tested in
    bool ended;      // a callback ended it
} Query;

static void query_begin(Query *q, const NphPhysicsWorld *w, const NphPhysicsQueryFilter *filter,
                        NphDVec3 origin)
{
    NPH_ASSERT_MSG(!w->stepping, "physics: a query during a step");
    q->w = w;
    q->filter = filter != NULL ? *filter : nph_physics_query_filter_default();
    q->origin = origin;
    q->ended = false;
}

static bool walks(const Query *q, u32 k)
{
    return !q->ended && (q->filter.body_types & k_trees[k][1]) != 0U;
}

// The shape in a tree's leaf, if the filter passes it (its body's kind chose
// the tree), and its body.
static const PhysShape *query_shape(const Query *q, u64 leaf, const PhysBody **body)
{
    const PhysShape *s = &q->w->shapes.data[(u32)leaf];
    const PhysBody *b = &q->w->bodies.data[s->body];
    if ((s->filter.category & q->filter.mask) == 0U ||
        (s->filter.mask & q->filter.category) == 0U || b->handle == q->filter.ignore.value) {
        return NULL;
    }
    *body = b;
    return s;
}

static bool is_surface(const PhysShape *s)
{
    return s->geometry.type == NPH_PHYSICS_SHAPE_MESH ||
           s->geometry.type == NPH_PHYSICS_SHAPE_HEIGHTFIELD;
}

// The shape's frame relative to the query's origin: its rotation, and its
// origin's offset, in 64 bits from the body's 64-bit origin.
static void shape_frame(const Query *q, const PhysBody *b, const PhysShape *s, NphQuat *rotation,
                        NphDVec3 *offset)
{
    *rotation = nph_quat_mul(b->rotation, s->pose.rotation);
    *offset = nph_dvec3_sub(
        nph_dvec3_add_vec3(b->origin, nph_quat_rotate(b->rotation, s->pose.position)), q->origin);
}

// The query's origin in the shape's frame, in 64 bits (a surface's queries
// take it so, as its contacts do).
static NphDVec3 origin_in_shape(NphQuat rotation, NphDVec3 offset)
{
    return nph_physics_drotate_inverse(rotation, nph_dvec3_neg(offset));
}

// What a box test of a query's 32-bit stand-in adds to stay conservative:
// the stand-in's rounding and the slab test's, a few units in the last place
// of the largest coordinate on the way, and a micrometre.
static f32 rounding(NphVec3 at, NphVec3 translation)
{
    return 1e-6F * (nph_vec3_max_component(nph_vec3_abs(at)) +
                    nph_vec3_max_component(nph_vec3_abs(translation))) +
           1e-6F;
}

static bool dvec3_is_finite(NphDVec3 v)
{
    return nph_f64_is_finite(v.x) && nph_f64_is_finite(v.y) && nph_f64_is_finite(v.z);
}

// A query's shape: valid convex geometry, and a rotation near unit length.
static bool shape_input_ok(const NphPhysicsGeometry *g, NphDVec3 position, NphQuat rotation)
{
    const bool ok = g != NULL && nph_physics_shape_type_is_convex(g->type) &&
                    nph_physics_geometry_is_valid(g) && dvec3_is_finite(position) &&
                    nph_quat_is_normalized(rotation, 1e-3F);
    NPH_ASSERT_MSG(ok, "physics: a shape query needs valid convex geometry and a unit rotation");
    return ok;
}

// ---------------------------------------------------------------------------
// Casts
// ---------------------------------------------------------------------------

// A cast, by a segment (`g` NULL) or a shape.
typedef struct CastQuery {
    Query q;
    const NphPhysicsGeometry *g;
    NphQuat rotation;
    NphVec3 translation;
    f32 max; // the fraction still searched
    NphPhysicsQueryCastFn fn;
    void *user;
} CastQuery;

// The hit to the caller, whose answer clips the search or ends it.
static f32 report(CastQuery *c, const PhysShape *s, const PhysBody *b,
                  const NphPhysicsCastHit *local)
{
    NphPhysicsQueryHit hit;
    hit.shape.value = s->handle;
    hit.body.value = b->handle;
    hit.fraction = local->fraction;
    hit.point = nph_dvec3_add_vec3(c->q.origin, local->point);
    hit.normal = local->normal;
    hit.triangle = local->triangle;
    const f32 r = c->fn(c->user, &hit);
    if (r < 0.0F) {
        c->q.ended = true;
        return -1.0F;
    }
    c->max = nph_f32_min(c->max, r);
    return c->max;
}

static f32 ray_leaf(const NphAabbTreeRay *ray, u32 proxy, u64 leaf, void *context)
{
    NPH_UNUSED(ray);
    NPH_UNUSED(proxy);
    CastQuery *c = (CastQuery *)context;
    const PhysBody *b = NULL;
    const PhysShape *s = query_shape(&c->q, leaf, &b);
    if (s == NULL) {
        return c->max;
    }
    NphQuat rotation;
    NphDVec3 offset;
    shape_frame(&c->q, b, s, &rotation, &offset);
    // The whole segment in the shape's frame, whatever the trees' order
    // (so a shape's answer is its own); hits past the nearest so far are
    // dropped.
    NphPhysicsCastHit local;
    if (!nph_physics_raycast_local(&s->geometry,
                                   nph_dvec3_to_vec3(origin_in_shape(rotation, offset)),
                                   nph_quat_rotate_inverse(rotation, c->translation), &local) ||
        local.fraction > c->max) {
        return c->max;
    }
    local.point = nph_vec3_scale(c->translation, local.fraction);
    local.normal = nph_quat_rotate(rotation, local.normal);
    return report(c, s, b, &local);
}

static f32 cast_leaf(const NphAabbTreeRay *ray, u32 proxy, u64 leaf, void *context)
{
    NPH_UNUSED(ray);
    NPH_UNUSED(proxy);
    CastQuery *c = (CastQuery *)context;
    const PhysBody *b = NULL;
    const PhysShape *s = query_shape(&c->q, leaf, &b);
    if (s == NULL) {
        return c->max;
    }
    NphQuat rotation;
    NphDVec3 offset;
    shape_frame(&c->q, b, s, &rotation, &offset);
    // The whole motion, up to the nearest hit so far (as for rays).
    NphPhysicsCastHit local;
    if (is_surface(s)) {
        const NphDVec3 position = origin_in_shape(rotation, offset);
        const NphQuat q = nph_quat_mul(nph_quat_conjugate(rotation), c->rotation);
        const NphVec3 d = nph_quat_rotate_inverse(rotation, c->translation);
        const bool hit = s->geometry.type == NPH_PHYSICS_SHAPE_MESH
                             ? nph_physics_cast_mesh_local(c->g, q, position, d, s->geometry.mesh,
                                                           c->max, &local)
                             : nph_physics_cast_heightfield_local(
                                   c->g, q, position, d, s->geometry.heightfield, c->max, &local);
        if (!hit) {
            return c->max;
        }
        local.point = nph_quat_rotate(rotation, local.point);
        local.normal = nph_quat_rotate(rotation, local.normal);
    } else if (!nph_physics_cast_convex(c->g, nph_physics_pose(nph_vec3_zero(), c->rotation),
                                        c->translation, &s->geometry,
                                        nph_physics_pose(nph_dvec3_to_vec3(offset), rotation),
                                        c->max, &local)) {
        return c->max;
    }
    return report(c, s, b, &local);
}

// Walks every tree the filter wants with the box of half extents `extent`
// centred at `at` swept along the translation.
static void cast_walk(CastQuery *c, NphVec3 at, NphVec3 extent, NphAabbTreeRayFn leaf)
{
    for (u32 k = 0U; k < PHYS_TREE_COUNT; ++k) {
        if (!walks(&c->q, k)) {
            continue;
        }
        const NphAabbTreeRay ray = {at, c->translation, c->max};
        nph_aabb_tree_box_cast(&c->q.w->trees[k_trees[k][0]], &ray, extent, c->q.filter.mask, leaf,
                               c);
    }
}

typedef struct Closest {
    NphPhysicsQueryHit hit;
    bool found;
} Closest;

static f32 keep_closest(void *user, const NphPhysicsQueryHit *hit)
{
    Closest *c = (Closest *)user;
    if (!c->found || hit->fraction < c->hit.fraction) {
        c->hit = *hit;
        c->found = true;
    }
    // Nothing comes nearer than a hit at the start (a shape touched there).
    return c->hit.fraction > 0.0F ? c->hit.fraction : -1.0F;
}

void nph_physics_world_raycast(const NphPhysicsWorld *world, NphDVec3 origin, NphVec3 translation,
                               const NphPhysicsQueryFilter *filter, NphPhysicsQueryCastFn fn,
                               void *user)
{
    NPH_ASSERT(world != NULL && fn != NULL);
    if (!dvec3_is_finite(origin) || !nph_vec3_is_finite(translation) ||
        !(nph_vec3_length_sq(translation) > 0.0F)) {
        return;
    }
    CastQuery c;
    query_begin(&c.q, world, filter, origin);
    c.g = NULL;
    c.rotation = nph_quat_identity();
    c.translation = translation;
    c.max = 1.0F;
    c.fn = fn;
    c.user = user;
    const NphVec3 at = nph_dvec3_to_vec3(origin);
    cast_walk(&c, at, nph_vec3_splat(rounding(at, translation)), ray_leaf);
}

bool nph_physics_world_raycast_closest(const NphPhysicsWorld *world, NphDVec3 origin,
                                       NphVec3 translation, const NphPhysicsQueryFilter *filter,
                                       NphPhysicsQueryHit *out)
{
    NPH_ASSERT(out != NULL);
    Closest c;
    memset(&c, 0, sizeof c);
    nph_physics_world_raycast(world, origin, translation, filter, keep_closest, &c);
    if (c.found) {
        *out = c.hit;
    }
    return c.found;
}

void nph_physics_world_cast_shape(const NphPhysicsWorld *world, const NphPhysicsGeometry *geometry,
                                  NphDVec3 position, NphQuat rotation, NphVec3 translation,
                                  const NphPhysicsQueryFilter *filter, NphPhysicsQueryCastFn fn,
                                  void *user)
{
    NPH_ASSERT(world != NULL && fn != NULL);
    if (!shape_input_ok(geometry, position, rotation) || !nph_vec3_is_finite(translation) ||
        !(nph_vec3_length_sq(translation) > 0.0F)) {
        return;
    }
    CastQuery c;
    query_begin(&c.q, world, filter, position);
    c.g = geometry;
    c.rotation = nph_quat_normalize(rotation);
    c.translation = translation;
    c.max = 1.0F;
    c.fn = fn;
    c.user = user;
    // The shape's box swept along the translation, grown by the narrow
    // phase's reach (shapes it starts touching) and the rounding.
    const NphAABB rel =
        nph_physics_geometry_aabb(geometry, nph_physics_pose(nph_vec3_zero(), c.rotation));
    const NphVec3 mid = nph_vec3_scale(nph_vec3_add(rel.min, rel.max), 0.5F);
    const NphVec3 half = nph_vec3_scale(nph_vec3_sub(rel.max, rel.min), 0.5F);
    const NphVec3 at = nph_dvec3_to_vec3(nph_dvec3_add_vec3(position, mid));
    const NphVec3 extent =
        nph_vec3_add(half, nph_vec3_splat(NPH_PHYSICS_CAST_REACH + rounding(at, translation)));
    cast_walk(&c, at, extent, cast_leaf);
}

bool nph_physics_world_cast_shape_closest(const NphPhysicsWorld *world,
                                          const NphPhysicsGeometry *geometry, NphDVec3 position,
                                          NphQuat rotation, NphVec3 translation,
                                          const NphPhysicsQueryFilter *filter,
                                          NphPhysicsQueryHit *out)
{
    NPH_ASSERT(out != NULL);
    Closest c;
    memset(&c, 0, sizeof c);
    nph_physics_world_cast_shape(world, geometry, position, rotation, translation, filter,
                                 keep_closest, &c);
    if (c.found) {
        *out = c.hit;
    }
    return c.found;
}

// ---------------------------------------------------------------------------
// Overlaps and contacts
// ---------------------------------------------------------------------------

typedef struct ShapeQuery {
    Query q;
    const NphPhysicsGeometry *g;
    NphQuat rotation;
    NphAABB box; // overlap_aabb's
    f32 speculative;
    NphPhysicsQueryShapeFn shape_fn;
    NphPhysicsQueryContactFn contact_fn;
    void *user;
} ShapeQuery;

static bool found(ShapeQuery *sq, const PhysShape *s, const PhysBody *b)
{
    if (!sq->shape_fn(sq->user, (NphPhysicsShape){s->handle}, (NphPhysicsBody){b->handle})) {
        sq->q.ended = true;
        return false;
    }
    return true;
}

static bool overlap_leaf(u32 proxy, u64 leaf, void *context)
{
    NPH_UNUSED(proxy);
    ShapeQuery *sq = (ShapeQuery *)context;
    const PhysBody *b = NULL;
    const PhysShape *s = query_shape(&sq->q, leaf, &b);
    if (s == NULL) {
        return true;
    }
    NphQuat rotation;
    NphDVec3 offset;
    shape_frame(&sq->q, b, s, &rotation, &offset);
    bool touch = false;
    if (is_surface(s)) {
        const NphDVec3 position = origin_in_shape(rotation, offset);
        const NphQuat q = nph_quat_mul(nph_quat_conjugate(rotation), sq->rotation);
        touch = s->geometry.type == NPH_PHYSICS_SHAPE_MESH
                    ? nph_physics_overlap_mesh_local(sq->g, q, position, s->geometry.mesh)
                    : nph_physics_overlap_heightfield_local(sq->g, q, position,
                                                            s->geometry.heightfield);
    } else {
        touch = nph_physics_overlap(sq->g, nph_physics_pose(nph_vec3_zero(), sq->rotation),
                                    &s->geometry,
                                    nph_physics_pose(nph_dvec3_to_vec3(offset), rotation));
    }
    return !touch || found(sq, s, b);
}

static bool aabb_leaf(u32 proxy, u64 leaf, void *context)
{
    NPH_UNUSED(proxy);
    ShapeQuery *sq = (ShapeQuery *)context;
    const PhysBody *b = NULL;
    const PhysShape *s = query_shape(&sq->q, leaf, &b);
    return s == NULL || !nph_aabb_overlaps(s->aabb, sq->box) || found(sq, s, b);
}

static bool contact(ShapeQuery *sq, const PhysShape *s, const PhysBody *b,
                    const NphPhysicsManifold *m)
{
    NphPhysicsQueryContact c;
    c.shape.value = s->handle;
    c.body.value = b->handle;
    c.manifold = *m;
    if (!sq->contact_fn(sq->user, &c)) {
        sq->q.ended = true;
        return false;
    }
    return true;
}

static bool collide_leaf(u32 proxy, u64 leaf, void *context)
{
    NPH_UNUSED(proxy);
    ShapeQuery *sq = (ShapeQuery *)context;
    const PhysBody *b = NULL;
    const PhysShape *s = query_shape(&sq->q, leaf, &b);
    if (s == NULL) {
        return true;
    }
    NphQuat rotation;
    NphDVec3 offset;
    shape_frame(&sq->q, b, s, &rotation, &offset);
    if (!is_surface(s)) {
        NphPhysicsManifold m;
        nph_physics_collide(sq->g, nph_physics_pose(nph_vec3_zero(), sq->rotation), &s->geometry,
                            nph_physics_pose(nph_dvec3_to_vec3(offset), rotation), sq->speculative,
                            NULL, &m);
        return m.point_count == 0U || contact(sq, s, b, &m);
    }
    const NphDVec3 position = origin_in_shape(rotation, offset);
    const NphQuat q = nph_quat_mul(nph_quat_conjugate(rotation), sq->rotation);
    NphPhysicsMeshManifolds mm;
    if (s->geometry.type == NPH_PHYSICS_SHAPE_MESH) {
        nph_physics_collide_mesh_local(sq->g, q, position, s->geometry.mesh, sq->speculative, &mm);
    } else {
        nph_physics_collide_heightfield_local(sq->g, q, position, s->geometry.heightfield,
                                              sq->speculative, &mm);
    }
    for (u32 i = 0U; i < mm.count; ++i) {
        // From the surface's axes to the world's; the points stay relative
        // to the query's position.
        NphPhysicsManifold *m = &mm.manifolds[i];
        m->normal = nph_quat_rotate(rotation, m->normal);
        for (u32 j = 0U; j < m->point_count; ++j) {
            m->points[j].point = nph_quat_rotate(rotation, m->points[j].point);
        }
        if (!contact(sq, s, b, m)) {
            return false;
        }
    }
    return true;
}

static void shape_walk(ShapeQuery *sq, NphAABB box, NphAabbTreeQueryFn leaf)
{
    for (u32 k = 0U; k < PHYS_TREE_COUNT; ++k) {
        if (walks(&sq->q, k)) {
            nph_aabb_tree_query(&sq->q.w->trees[k_trees[k][0]], box, sq->q.filter.mask, leaf, sq);
        }
    }
}

static void shape_query_begin(ShapeQuery *sq, const NphPhysicsWorld *world,
                              const NphPhysicsGeometry *geometry, NphDVec3 position,
                              NphQuat rotation, const NphPhysicsQueryFilter *filter, void *user)
{
    query_begin(&sq->q, world, filter, position);
    sq->g = geometry;
    sq->rotation = nph_quat_normalize(rotation);
    sq->speculative = 0.0F;
    sq->shape_fn = NULL;
    sq->contact_fn = NULL;
    sq->user = user;
}

void nph_physics_world_overlap_shape(const NphPhysicsWorld *world,
                                     const NphPhysicsGeometry *geometry, NphDVec3 position,
                                     NphQuat rotation, const NphPhysicsQueryFilter *filter,
                                     NphPhysicsQueryShapeFn fn, void *user)
{
    NPH_ASSERT(world != NULL && fn != NULL);
    if (!shape_input_ok(geometry, position, rotation)) {
        return;
    }
    ShapeQuery sq;
    shape_query_begin(&sq, world, geometry, position, rotation, filter, user);
    sq.shape_fn = fn;
    const NphAABB rel =
        nph_physics_geometry_aabb(geometry, nph_physics_pose(nph_vec3_zero(), sq.rotation));
    shape_walk(&sq, phys_world_box(position, rel), overlap_leaf);
}

void nph_physics_world_overlap_aabb(const NphPhysicsWorld *world, NphAABB box,
                                    const NphPhysicsQueryFilter *filter, NphPhysicsQueryShapeFn fn,
                                    void *user)
{
    NPH_ASSERT(world != NULL && fn != NULL);
    if (!nph_vec3_is_finite(box.min) || !nph_vec3_is_finite(box.max)) {
        return;
    }
    ShapeQuery sq;
    shape_query_begin(&sq, world, NULL, nph_dvec3_zero(), nph_quat_identity(), filter, user);
    sq.box = box;
    sq.shape_fn = fn;
    shape_walk(&sq, box, aabb_leaf);
}

void nph_physics_world_collide_shape(const NphPhysicsWorld *world,
                                     const NphPhysicsGeometry *geometry, NphDVec3 position,
                                     NphQuat rotation, f32 speculative_distance,
                                     const NphPhysicsQueryFilter *filter,
                                     NphPhysicsQueryContactFn fn, void *user)
{
    NPH_ASSERT(world != NULL && fn != NULL);
    NPH_ASSERT(speculative_distance >= 0.0F);
    if (!shape_input_ok(geometry, position, rotation) || !(speculative_distance >= 0.0F) ||
        !nph_f32_is_finite(speculative_distance)) {
        return;
    }
    ShapeQuery sq;
    shape_query_begin(&sq, world, geometry, position, rotation, filter, user);
    sq.speculative = speculative_distance;
    sq.contact_fn = fn;
    const NphAABB rel =
        nph_physics_geometry_aabb(geometry, nph_physics_pose(nph_vec3_zero(), sq.rotation));
    shape_walk(&sq, phys_world_box(position, nph_aabb_grow(rel, speculative_distance)),
               collide_leaf);
}
