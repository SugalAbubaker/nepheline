// SPDX-License-Identifier: Apache-2.0
// Shapes: attachment, broadphase proxies, world boxes, surfaces, filters
// (shape.h; docs/physics.md "Shapes").

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <math.h>
#include <string.h>

NphPhysicsFilter nph_physics_filter_default(void)
{
    NphPhysicsFilter f;
    f.category = 1U;
    f.mask = NPH_U64_MAX;
    f.group = 0;
    return f;
}

bool nph_physics_filter_should_collide(NphPhysicsFilter a, NphPhysicsFilter b)
{
    if (a.group == b.group && a.group != 0) {
        return a.group > 0;
    }
    return (a.category & b.mask) != 0U && (b.category & a.mask) != 0U;
}

NphPhysicsShapeDesc nph_physics_shape_desc_default(NphPhysicsGeometry geometry)
{
    NphPhysicsShapeDesc d;
    memset(&d, 0, sizeof d);
    d.geometry = geometry;
    d.pose = nph_physics_pose_identity();
    d.density = 1000.0F;
    d.friction = 0.6F;
    d.filter = nph_physics_filter_default();
    d.contact_events = 1;
    return d;
}

// ---------------------------------------------------------------------------
// Placement and boxes
// ---------------------------------------------------------------------------

NphPhysicsPose phys_shape_pose_relative(const PhysBody *b, const PhysShape *s, NphDVec3 origin)
{
    const NphVec3 body = nph_dvec3_relative(b->origin, origin);
    return nph_physics_pose(nph_vec3_add(body, nph_quat_rotate(b->rotation, s->pose.position)),
                            nph_quat_mul(b->rotation, s->pose.rotation));
}

// f64 to f32 rounded towards -inf or +inf: a box computed relative to the
// body and converted stays conservative wherever 32 bits are coarse.
static f32 round_down(f64 x)
{
    f32 f = (f32)x;
    if ((f64)f > x) {
        f = nextafterf(f, -NPH_F32_INFINITY);
    }
    return f;
}

static f32 round_up(f64 x)
{
    f32 f = (f32)x;
    if ((f64)f < x) {
        f = nextafterf(f, NPH_F32_INFINITY);
    }
    return f;
}

NphAABB phys_world_box(NphDVec3 origin, NphAABB rel)
{
    NphAABB box;
    box.min = nph_vec3(round_down(origin.x + (f64)rel.min.x), round_down(origin.y + (f64)rel.min.y),
                       round_down(origin.z + (f64)rel.min.z));
    box.max = nph_vec3(round_up(origin.x + (f64)rel.max.x), round_up(origin.y + (f64)rel.max.y),
                       round_up(origin.z + (f64)rel.max.z));
    return box;
}

NphAABB phys_shape_world_aabb(const PhysBody *b, const PhysShape *s)
{
    const NphPhysicsPose pose = phys_shape_pose_relative(b, s, b->origin);
    return phys_world_box(b->origin, nph_physics_geometry_aabb(&s->geometry, pose));
}

static u32 tree_of(const PhysBody *b)
{
    switch (b->type) {
    case NPH_PHYSICS_BODY_STATIC:
        return PHYS_TREE_STATIC;
    case NPH_PHYSICS_BODY_KINEMATIC:
        return PHYS_TREE_KINEMATIC;
    default:
        return PHYS_TREE_DYNAMIC;
    }
}

NphResult phys_move_buffer_push(NphPhysicsWorld *w, u32 shape_slot)
{
    PhysShape *s = &w->shapes.data[shape_slot];
    if ((s->flags & PHYS_SHAPE_IN_MOVE_BUFFER) != 0U) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(NPH_ARRAY_PUSH(&w->move_buffer, phys_proxy_key(s->proxy, s->tree)));
    s->flags |= PHYS_SHAPE_IN_MOVE_BUFFER;
    return NPH_RESULT_OK;
}

NphResult phys_shape_create_proxy(NphPhysicsWorld *w, u32 shape_slot)
{
    PhysShape *s = &w->shapes.data[shape_slot];
    const PhysBody *b = &w->bodies.data[s->body];
    s->tree = tree_of(b);
    s->aabb = phys_shape_world_aabb(b, s);
    // Static shapes do not move: a speculative margin is enough.
    const f32 margin =
        s->tree == PHYS_TREE_STATIC ? NPH_PHYSICS_SPECULATIVE_DISTANCE : NPH_PHYSICS_AABB_MARGIN;
    s->fat_aabb = nph_aabb_grow(s->aabb, margin);
    NPH_TRY(nph_aabb_tree_create_proxy(&w->trees[s->tree], s->fat_aabb, s->filter.category,
                                       shape_slot, &s->proxy));
    const NphResult r = phys_move_buffer_push(w, shape_slot);
    if (r != NPH_RESULT_OK) {
        nph_aabb_tree_destroy_proxy(&w->trees[s->tree], s->proxy);
        s->proxy = PHYS_NULL;
    }
    return r;
}

void phys_shape_destroy_proxy(NphPhysicsWorld *w, u32 shape_slot)
{
    PhysShape *s = &w->shapes.data[shape_slot];
    if (s->proxy == PHYS_NULL) {
        return;
    }
    if ((s->flags & PHYS_SHAPE_IN_MOVE_BUFFER) != 0U) {
        const u32 key = phys_proxy_key(s->proxy, s->tree);
        for (u32 i = 0U; i < w->move_buffer.len; ++i) {
            if (w->move_buffer.data[i] == key) {
                NPH_ARRAY_REMOVE_AT(&w->move_buffer, i); // order kept: pairs stay deterministic
                break;
            }
        }
        s->flags &= ~(u32)PHYS_SHAPE_IN_MOVE_BUFFER;
    }
    nph_aabb_tree_destroy_proxy(&w->trees[s->tree], s->proxy);
    s->proxy = PHYS_NULL;
}

// ---------------------------------------------------------------------------
// Creation and destruction
// ---------------------------------------------------------------------------

static bool pose_valid(NphPhysicsPose p)
{
    return nph_vec3_is_finite(p.position) && nph_f32_is_finite(p.rotation.x) &&
           nph_f32_is_finite(p.rotation.y) && nph_f32_is_finite(p.rotation.z) &&
           nph_f32_is_finite(p.rotation.w) && nph_quat_is_normalized(p.rotation, 1e-3F);
}

NphResult nph_physics_shape_create(NphPhysicsWorld *world, NphPhysicsBody body,
                                   const NphPhysicsShapeDesc *desc, NphPhysicsShape *out)
{
    NPH_ASSERT(world != NULL && out != NULL);
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_shape_create during a step");
    if (desc == NULL || !nph_physics_geometry_is_valid(&desc->geometry) ||
        !pose_valid(desc->pose) || !(desc->density >= 0.0F) || !nph_f32_is_finite(desc->density) ||
        !(desc->friction >= 0.0F) || !nph_f32_is_finite(desc->friction) ||
        !(desc->restitution >= 0.0F) || !(desc->restitution <= 1.0F)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    PhysBody *b = phys_body(world, body);
    if (b == NULL) {
        return NPH_RESULT_NOT_FOUND;
    }
    // A mesh or a heightfield bounds no volume, so it has no mass to move a body with
    // (ADR-0046 §1).
    if (!nph_physics_shape_type_is_convex(desc->geometry.type) &&
        b->type == NPH_PHYSICS_BODY_DYNAMIC) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 body_slot = phys_handle_slot(body.value);
    u32 slot = 0U;
    u32 generation = 0U;
    NPH_TRY(phys_slots_alloc(&world->shape_slots, &slot, &generation));
    NphResult r = NPH_RESULT_OK;
    if (slot >= world->shapes.len) {
        r = NPH_ARRAY_RESIZE(&world->shapes, slot + 1U);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESERVE(&world->shape_slots.free, world->shape_slots.generation.len);
    }
    if (r != NPH_RESULT_OK) {
        phys_slots_release(&world->shape_slots, slot);
        return r;
    }
    b = &world->bodies.data[body_slot]; // stable: bodies did not grow
    PhysShape *s = &world->shapes.data[slot];
    memset(s, 0, sizeof *s);
    s->body = body_slot;
    s->geometry = desc->geometry;
    s->pose = nph_physics_pose(desc->pose.position, nph_quat_normalize(desc->pose.rotation));
    s->density = desc->density;
    s->friction = desc->friction;
    s->restitution = desc->restitution;
    s->filter = desc->filter;
    s->flags = (desc->contact_events != 0 ? PHYS_SHAPE_CONTACT_EVENTS : 0U) |
               (desc->hit_events != 0 ? PHYS_SHAPE_HIT_EVENTS : 0U);
    s->user_data = desc->user_data;
    s->proxy = PHYS_NULL;
    s->next = b->shape_head;
    b->shape_head = slot;
    b->shape_count += 1U;
    phys_body_update_mass(world, b);
    r = phys_shape_create_proxy(world, slot);
    if (r != NPH_RESULT_OK) {
        b->shape_head = s->next;
        b->shape_count -= 1U;
        phys_body_update_mass(world, b);
        phys_slots_release(&world->shape_slots, slot);
        return r;
    }
    s->handle = phys_handle(slot, generation);
    if (b->type != NPH_PHYSICS_BODY_STATIC) {
        NPH_IGNORE_RESULT(phys_wake_body(world, body_slot));
    }
    *out = (NphPhysicsShape){s->handle};
    return NPH_RESULT_OK;
}

void phys_shape_destroy_contacts(NphPhysicsWorld *w, u32 shape_slot)
{
    const u32 body_slot = w->shapes.data[shape_slot].body;
    PhysBody *b = &w->bodies.data[body_slot];
    u32 key = b->contact_head;
    while (key != PHYS_NULL) {
        const u32 c = key >> 1U;
        const PhysContact *contact = &w->contacts.data[c];
        key = contact->next[key & 1U];
        if (contact->shape_a == shape_slot || contact->shape_b == shape_slot) {
            const u32 other = contact->body_a == body_slot ? contact->body_b : contact->body_a;
            const bool touching = (contact->flags & PHYS_CONTACT_TOUCHING) != 0U;
            phys_contact_destroy(w, c, false);
            if (touching) {
                NPH_IGNORE_RESULT(phys_wake_body(w, other));
            }
        }
    }
}

void nph_physics_shape_destroy(NphPhysicsWorld *world, NphPhysicsShape shape)
{
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_shape_destroy during a step");
    PhysShape *s = phys_shape(world, shape);
    NPH_ASSERT_MSG(s != NULL, "nph_physics_shape_destroy: stale shape handle");
    if (s == NULL) {
        return;
    }
    const u32 slot = phys_handle_slot(shape.value);
    const u32 body_slot = s->body;
    phys_shape_destroy_contacts(world, slot);
    phys_shape_destroy_proxy(world, slot);
    PhysBody *b = &world->bodies.data[body_slot];
    u32 *link = &b->shape_head;
    while (*link != slot) {
        link = &world->shapes.data[*link].next;
    }
    *link = s->next;
    b->shape_count -= 1U;
    s->handle = 0U;
    phys_slots_release(&world->shape_slots, slot);
    phys_body_update_mass(world, b);
    if (b->type != NPH_PHYSICS_BODY_STATIC) {
        NPH_IGNORE_RESULT(phys_wake_body(world, body_slot));
    }
}

bool nph_physics_shape_is_valid(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    return phys_shape(world, shape) != NULL;
}

static PhysShape *checked(const NphPhysicsWorld *w, NphPhysicsShape h)
{
    PhysShape *s = phys_shape(w, h);
    NPH_ASSERT_MSG(s != NULL, "physics: stale shape handle 0x%08x", h.value);
    return s;
}

NphPhysicsBody nph_physics_shape_body(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? phys_body_handle(world, s->body) : NPH_PHYSICS_BODY_NULL;
}

NphPhysicsGeometry nph_physics_shape_geometry(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->geometry : nph_physics_geometry_sphere(0.0F);
}

NphPhysicsPose nph_physics_shape_pose(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->pose : nph_physics_pose_identity();
}

f32 nph_physics_shape_friction(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->friction : 0.0F;
}

f32 nph_physics_shape_restitution(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->restitution : 0.0F;
}

void nph_physics_shape_set_friction(NphPhysicsWorld *world, NphPhysicsShape shape, f32 friction)
{
    NPH_ASSERT(nph_f32_is_finite(friction) && friction >= 0.0F);
    PhysShape *s = checked(world, shape);
    if (s != NULL) {
        s->friction = friction;
    }
}

void nph_physics_shape_set_restitution(NphPhysicsWorld *world, NphPhysicsShape shape,
                                       f32 restitution)
{
    NPH_ASSERT(restitution >= 0.0F && restitution <= 1.0F);
    PhysShape *s = checked(world, shape);
    if (s != NULL) {
        s->restitution = restitution;
    }
}

NphPhysicsFilter nph_physics_shape_filter(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->filter : nph_physics_filter_default();
}

void nph_physics_shape_set_filter(NphPhysicsWorld *world, NphPhysicsShape shape,
                                  NphPhysicsFilter filter)
{
    NPH_ASSERT_MSG(!world->stepping, "physics: shape changed during a step");
    PhysShape *s = checked(world, shape);
    if (s == NULL) {
        return;
    }
    const u32 slot = phys_handle_slot(shape.value);
    s->filter = filter;
    nph_aabb_tree_set_category_bits(&world->trees[s->tree], s->proxy, filter.category);
    // Contacts the filter no longer allows end now; new pairs form at the
    // next step (the shape queries the trees again).
    const PhysBody *b = &world->bodies.data[s->body];
    for (u32 key = b->contact_head; key != PHYS_NULL;) {
        const u32 c = key >> 1U;
        const PhysContact *contact = &world->contacts.data[c];
        key = contact->next[key & 1U];
        if (contact->shape_a != slot && contact->shape_b != slot) {
            continue;
        }
        const u32 other = contact->shape_a == slot ? contact->shape_b : contact->shape_a;
        if (!nph_physics_filter_should_collide(filter, world->shapes.data[other].filter)) {
            const u32 other_body = world->shapes.data[other].body;
            const bool touching = (contact->flags & PHYS_CONTACT_TOUCHING) != 0U;
            phys_contact_destroy(world, c, true);
            if (touching) {
                NPH_IGNORE_RESULT(phys_wake_body(world, other_body));
            }
        }
    }
    NPH_IGNORE_RESULT(phys_move_buffer_push(world, slot));
    if (b->type != NPH_PHYSICS_BODY_STATIC) {
        NPH_IGNORE_RESULT(phys_wake_body(world, s->body));
    }
}

u64 nph_physics_shape_user_data(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->user_data : 0U;
}

void nph_physics_shape_set_user_data(NphPhysicsWorld *world, NphPhysicsShape shape, u64 user_data)
{
    PhysShape *s = checked(world, shape);
    if (s != NULL) {
        s->user_data = user_data;
    }
}

NphAABB nph_physics_shape_aabb(const NphPhysicsWorld *world, NphPhysicsShape shape)
{
    const PhysShape *s = checked(world, shape);
    return s != NULL ? s->aabb : nph_aabb_from_point(nph_vec3_zero());
}
