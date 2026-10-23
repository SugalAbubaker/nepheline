// SPDX-License-Identifier: Apache-2.0
// The world: creation, the step's order, events, statistics, the state hash,
// validation, and debug drawing (world.h; docs/physics.md "The world").

#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/profile.h>
#include <nepheline/platform/time.h>

#include "world_internal.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

NphResult phys_slots_alloc(PhysSlots *s, u32 *out_slot, u32 *out_generation)
{
    if (s->free.len > 0U) {
        const u32 slot = s->free.data[s->free.len - 1U];
        s->free.len -= 1U;
        *out_slot = slot;
        *out_generation = s->generation.data[slot];
        return NPH_RESULT_OK;
    }
    if (s->generation.len >= NPH_PHYSICS_MAX_OBJECTS) {
        return NPH_RESULT_FULL;
    }
    NPH_TRY(NPH_ARRAY_PUSH(&s->generation, 1U));
    *out_slot = s->generation.len - 1U;
    *out_generation = 1U;
    return NPH_RESULT_OK;
}

void phys_slots_release(PhysSlots *s, u32 slot)
{
    u32 *g = &s->generation.data[slot];
    *g = *g >= PHYS_GEN_MAX ? 1U : *g + 1U;
    // Cannot fail: the free list never holds more slots than were issued,
    // and its capacity was reserved when they were.
    const NphResult r = NPH_ARRAY_PUSH(&s->free, slot);
    NPH_ASSERT(r == NPH_RESULT_OK);
    NPH_UNUSED(r);
}

static NphResult slots_reserve(PhysSlots *s, u32 count)
{
    NPH_TRY(NPH_ARRAY_RESERVE(&s->generation, count));
    return NPH_ARRAY_RESERVE(&s->free, count);
}

PhysBody *phys_body(const NphPhysicsWorld *w, NphPhysicsBody h)
{
    const u32 slot = phys_handle_slot(h.value);
    if (h.value == 0U || slot >= w->bodies.len || w->bodies.data[slot].handle != h.value) {
        return NULL;
    }
    return &w->bodies.data[slot];
}

PhysShape *phys_shape(const NphPhysicsWorld *w, NphPhysicsShape h)
{
    const u32 slot = phys_handle_slot(h.value);
    if (h.value == 0U || slot >= w->shapes.len || w->shapes.data[slot].handle != h.value) {
        return NULL;
    }
    return &w->shapes.data[slot];
}

NphPhysicsBody phys_body_handle(const NphPhysicsWorld *w, u32 slot)
{
    return (NphPhysicsBody){w->bodies.data[slot].handle};
}

NphPhysicsShape phys_shape_handle(const NphPhysicsWorld *w, u32 slot)
{
    return (NphPhysicsShape){w->shapes.data[slot].handle};
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphPhysicsWorldDesc nph_physics_world_desc_default(void)
{
    NphPhysicsWorldDesc d;
    memset(&d, 0, sizeof d);
    d.gravity = nph_vec3(0.0F, -9.81F, 0.0F);
    d.substeps = 4U;
    d.contact_hertz = 0.0F; // an eighth of the substep rate
    d.contact_damping_ratio = 10.0F;
    d.joint_hertz = 0.0F; // a quarter of the substep rate
    d.joint_damping_ratio = 2.0F;
    d.contact_push_max_velocity = 3.0F;
    d.restitution_threshold = 1.0F;
    d.max_linear_speed = 400.0F;
    d.time_to_sleep = 0.5F;
    d.enable_sleep = 1;
    d.body_capacity = 64U;
    d.shape_capacity = 64U;
    d.contact_capacity = 256U;
    return d;
}

static bool finite_nonneg(f32 v)
{
    return nph_f32_is_finite(v) && v >= 0.0F;
}

NphResult nph_physics_world_create(const NphPhysicsWorldDesc *desc, NphPhysicsWorld **out)
{
    if (desc == NULL || out == NULL || desc->alloc == NULL || desc->substeps == 0U ||
        (u32)desc->solver >= (u32)NPH_PHYSICS_SOLVER_COUNT || desc->substeps > 64U ||
        !nph_vec3_is_finite(desc->gravity) || !finite_nonneg(desc->contact_hertz) ||
        !finite_nonneg(desc->contact_damping_ratio) || !finite_nonneg(desc->joint_hertz) ||
        !finite_nonneg(desc->joint_damping_ratio) ||
        !finite_nonneg(desc->contact_push_max_velocity) ||
        !finite_nonneg(desc->restitution_threshold) || !(desc->max_linear_speed > 0.0F) ||
        !nph_f32_is_finite(desc->max_linear_speed) || !finite_nonneg(desc->time_to_sleep)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAllocator *alloc = desc->alloc;
    NphPhysicsWorld *w = NPH_ALLOC_TYPE(alloc, NphPhysicsWorld);
    if (w == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    w->alloc = alloc;
    w->desc = *desc;
    w->hit_threshold = 1.0F;
    w->kernels = phys_solver_kernels_best();
    w->split_candidate = PHYS_NULL;
    NPH_ARRAY_INIT(&w->body_slots.generation, alloc);
    NPH_ARRAY_INIT(&w->body_slots.free, alloc);
    NPH_ARRAY_INIT(&w->bodies, alloc);
    NPH_ARRAY_INIT(&w->shape_slots.generation, alloc);
    NPH_ARRAY_INIT(&w->shape_slots.free, alloc);
    NPH_ARRAY_INIT(&w->shapes, alloc);
    NPH_ARRAY_INIT(&w->contacts, alloc);
    NPH_ARRAY_INIT(&w->free_contacts, alloc);
    NPH_ARRAY_INIT(&w->joint_slots.generation, alloc);
    NPH_ARRAY_INIT(&w->joint_slots.free, alloc);
    NPH_ARRAY_INIT(&w->joints, alloc);
    NPH_ARRAY_INIT(&w->overflow_joints, alloc);
    NPH_ARRAY_INIT(&w->step_joints, alloc);
    NPH_ARRAY_INIT(&w->islands, alloc);
    NPH_ARRAY_INIT(&w->free_islands, alloc);
    NPH_ARRAY_INIT(&w->awake_bodies, alloc);
    NPH_ARRAY_INIT(&w->awake_contacts, alloc);
    NPH_ARRAY_INIT(&w->awake_islands, alloc);
    NPH_ARRAY_INIT(&w->move_buffer, alloc);
    NPH_ARRAY_INIT(&w->pair_counts, alloc);
    NPH_ARRAY_INIT(&w->pair_buffer, alloc);
    NPH_ARRAY_INIT(&w->overflow, alloc);
    NPH_ARRAY_INIT(&w->solver_bodies, alloc);
    NPH_ARRAY_INIT(&w->solver_prev, alloc);
    NPH_ARRAY_INIT(&w->solver_params, alloc);
    NPH_ARRAY_INIT(&w->groups, alloc);
    NPH_ARRAY_INIT(&w->points, alloc);
    NPH_ARRAY_INIT(&w->stages, alloc);
    NPH_ARRAY_INIT(&w->claims, alloc);
    NPH_ARRAY_INIT(&w->changed, alloc);
    NPH_ARRAY_INIT(&w->scratch, alloc);
    NPH_ARRAY_INIT(&w->scratch2, alloc);
    NPH_ARRAY_INIT(&w->candidates, alloc);
    NPH_ARRAY_INIT(&w->begin_events, alloc);
    NPH_ARRAY_INIT(&w->end_events, alloc);
    NPH_ARRAY_INIT(&w->hit_events, alloc);
    NPH_ARRAY_INIT(&w->move_events, alloc);
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS; ++c) {
        NPH_ARRAY_INIT(&w->colors[c].contacts, alloc);
        NPH_ARRAY_INIT(&w->colors[c].joints, alloc);
    }
    NphResult r = NPH_RESULT_OK;
    for (u32 t = 0U; t < PHYS_TREE_COUNT && r == NPH_RESULT_OK; ++t) {
        r = nph_aabb_tree_init(&w->trees[t], alloc, 16U);
    }
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS && r == NPH_RESULT_OK; ++c) {
        r = nph_bitset_init(&w->colors[c].bodies, alloc, nph_max_u32(desc->body_capacity, 64U));
    }
    if (r == NPH_RESULT_OK) {
        const NphHashMapDesc pairs = nph_hashmap_desc_u64((u32)sizeof(u32));
        r = nph_hashmap_init(&w->pairs, alloc, &pairs);
    }
    if (r == NPH_RESULT_OK) {
        r = slots_reserve(&w->body_slots, desc->body_capacity);
    }
    if (r == NPH_RESULT_OK) {
        r = slots_reserve(&w->shape_slots, desc->shape_capacity);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESERVE(&w->bodies, desc->body_capacity);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESERVE(&w->shapes, desc->shape_capacity);
    }
    if (r == NPH_RESULT_OK) {
        r = NPH_ARRAY_RESERVE(&w->contacts, desc->contact_capacity);
    }
    if (r != NPH_RESULT_OK) {
        nph_physics_world_destroy(w);
        return r;
    }
    *out = w;
    return NPH_RESULT_OK;
}

void nph_physics_world_destroy(NphPhysicsWorld *world)
{
    if (world == NULL) {
        return;
    }
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_world_destroy during a step");
    NphAllocator *alloc = world->alloc;
    NPH_ARRAY_FREE(&world->body_slots.generation);
    NPH_ARRAY_FREE(&world->body_slots.free);
    NPH_ARRAY_FREE(&world->bodies);
    NPH_ARRAY_FREE(&world->shape_slots.generation);
    NPH_ARRAY_FREE(&world->shape_slots.free);
    NPH_ARRAY_FREE(&world->shapes);
    NPH_ARRAY_FREE(&world->contacts);
    NPH_ARRAY_FREE(&world->free_contacts);
    NPH_ARRAY_FREE(&world->joint_slots.generation);
    NPH_ARRAY_FREE(&world->joint_slots.free);
    NPH_ARRAY_FREE(&world->joints);
    NPH_ARRAY_FREE(&world->overflow_joints);
    NPH_ARRAY_FREE(&world->step_joints);
    NPH_ARRAY_FREE(&world->islands);
    NPH_ARRAY_FREE(&world->free_islands);
    NPH_ARRAY_FREE(&world->awake_bodies);
    NPH_ARRAY_FREE(&world->awake_contacts);
    NPH_ARRAY_FREE(&world->awake_islands);
    NPH_ARRAY_FREE(&world->move_buffer);
    NPH_ARRAY_FREE(&world->pair_counts);
    NPH_ARRAY_FREE(&world->pair_buffer);
    NPH_ARRAY_FREE(&world->overflow);
    NPH_ARRAY_FREE(&world->solver_bodies);
    NPH_ARRAY_FREE(&world->solver_prev);
    NPH_ARRAY_FREE(&world->solver_params);
    NPH_ARRAY_FREE(&world->groups);
    NPH_ARRAY_FREE(&world->points);
    NPH_ARRAY_FREE(&world->stages);
    NPH_ARRAY_FREE(&world->claims);
    NPH_ARRAY_FREE(&world->changed);
    NPH_ARRAY_FREE(&world->scratch);
    NPH_ARRAY_FREE(&world->scratch2);
    NPH_ARRAY_FREE(&world->candidates);
    NPH_ARRAY_FREE(&world->begin_events);
    NPH_ARRAY_FREE(&world->end_events);
    NPH_ARRAY_FREE(&world->hit_events);
    NPH_ARRAY_FREE(&world->move_events);
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS; ++c) {
        nph_bitset_free(&world->colors[c].bodies);
        NPH_ARRAY_FREE(&world->colors[c].contacts);
        NPH_ARRAY_FREE(&world->colors[c].joints);
    }
    for (u32 t = 0U; t < PHYS_TREE_COUNT; ++t) {
        nph_aabb_tree_free(&world->trees[t]);
    }
    nph_hashmap_free(&world->pairs);
    NPH_FREE_TYPE(alloc, NphPhysicsWorld, world);
}

void nph_physics_world_set_gravity(NphPhysicsWorld *world, NphVec3 gravity)
{
    NPH_ASSERT(world != NULL && nph_vec3_is_finite(gravity));
    world->desc.gravity = gravity;
}

NphVec3 nph_physics_world_gravity(const NphPhysicsWorld *world)
{
    return world->desc.gravity;
}

void nph_physics_world_set_hit_threshold(NphPhysicsWorld *world, f32 speed)
{
    NPH_ASSERT(world != NULL && finite_nonneg(speed));
    world->hit_threshold = speed;
}

// ---------------------------------------------------------------------------
// The step
// ---------------------------------------------------------------------------

static f64 ms_since(u64 t0)
{
    return (f64)(nph_time_now_ns() - t0) * 1e-6;
}

NphResult nph_physics_world_step(NphPhysicsWorld *world, f32 dt, NphJobSystem *jobs)
{
    NPH_ASSERT(world != NULL);
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_world_step re-entered");
    if (!(dt > 0.0F) || !nph_f32_is_finite(dt)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_PROFILE_BEGIN("physics.step");
    const u64 t0 = nph_time_now_ns();
    world->stepping = true;
    NPH_ARRAY_CLEAR(&world->begin_events);
    NPH_ARRAY_CLEAR(&world->end_events);
    NPH_ARRAY_CLEAR(&world->hit_events);
    NPH_ARRAY_CLEAR(&world->move_events);

    NphResult r = NPH_RESULT_OK;
    u64 t = nph_time_now_ns();
    NPH_PROFILE_BEGIN("physics.pairs");
    r = phys_update_pairs(world, jobs);
    NPH_PROFILE_END();
    world->stats.pairs_ms = ms_since(t);
    if (r == NPH_RESULT_OK) {
        t = nph_time_now_ns();
        NPH_PROFILE_BEGIN("physics.collide");
        r = phys_collide(world, jobs);
        NPH_PROFILE_END();
        world->stats.collide_ms = ms_since(t);
    }
    if (r == NPH_RESULT_OK) {
        t = nph_time_now_ns();
        NPH_PROFILE_BEGIN("physics.solve");
        r = phys_solve(world, dt, jobs);
        NPH_PROFILE_END();
        world->stats.solve_ms = ms_since(t);
    }
    if (r == NPH_RESULT_OK) {
        // Islands merge whether or not they may sleep: their lists stay
        // whole for when sleep is turned on, or a body wakes its island.
        t = nph_time_now_ns();
        NPH_PROFILE_BEGIN("physics.islands");
        r = phys_island_merge_pending(world);
        if (r == NPH_RESULT_OK && world->desc.enable_sleep != 0) {
            r = phys_island_sleep_and_split(world);
        }
        NPH_PROFILE_END();
        world->stats.sleep_ms = ms_since(t);
    }
    world->stepping = false;
    world->stats.steps += 1U;
    world->stats.step_ms = ms_since(t0);
    NPH_PROFILE_END();
    return r;
}

NphPhysicsEvents nph_physics_world_events(const NphPhysicsWorld *world)
{
    NphPhysicsEvents e;
    e.begin = world->begin_events.data;
    e.begin_count = world->begin_events.len;
    e.end = world->end_events.data;
    e.end_count = world->end_events.len;
    e.hit = world->hit_events.data;
    e.hit_count = world->hit_events.len;
    e.move = world->move_events.data;
    e.move_count = world->move_events.len;
    return e;
}

NphPhysicsStats nph_physics_world_stats(const NphPhysicsWorld *world)
{
    NphPhysicsStats s = world->stats;
    s.bodies = world->body_slots.generation.len - world->body_slots.free.len;
    s.awake_bodies = world->awake_bodies.len;
    s.shapes = world->shape_slots.generation.len - world->shape_slots.free.len;
    s.contacts = world->contacts.len - world->free_contacts.len - world->child_contacts;
    s.joints = world->joint_slots.generation.len - world->joint_slots.free.len;
    s.islands = world->islands.len - world->free_islands.len;
    s.awake_islands = world->awake_islands.len;
    u32 touching = 0U;
    u32 points = 0U;
    for (u32 i = 0U; i < world->awake_contacts.len; ++i) {
        const PhysContact *c = &world->contacts.data[world->awake_contacts.data[i]];
        if ((c->flags & PHYS_CONTACT_TOUCHING) != 0U) {
            touching += 1U;
            for (u32 k = world->awake_contacts.data[i]; k != PHYS_NULL;
                 k = world->contacts.data[k].child) {
                points += world->contacts.data[k].point_count;
            }
        }
    }
    s.touching_contacts = touching;
    s.manifold_points = points;
    u32 colors = 0U;
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS; ++c) {
        colors += world->colors[c].contacts.len > 0U ? 1U : 0U;
    }
    s.colors_used = colors;
    s.overflow_constraints = world->overflow.len;
    return s;
}

u64 nph_physics_world_hash(const NphPhysicsWorld *world)
{
    u64 h = 0x6E70687973696373ULL;
    for (u32 i = 0U; i < world->bodies.len; ++i) {
        const PhysBody *b = &world->bodies.data[i];
        if (b->handle == 0U) {
            continue;
        }
        struct {
            u32 handle;
            u32 flags;
            NphDVec3 origin;
            NphQuat rotation;
            NphVec3 v;
            NphVec3 w;
        } rec;
        memset(&rec, 0, sizeof rec);
        rec.handle = b->handle;
        rec.flags = b->flags & PHYS_BODY_AWAKE;
        rec.origin = b->origin;
        rec.rotation = b->rotation;
        rec.v = b->linear_velocity;
        rec.w = b->angular_velocity;
        h = nph_hash64(&rec, sizeof rec, h);
    }
    return h;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

#define VALID(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            NPH_LOG_WARN(&NPH_LOG_PHYSICS, "world validate: %s (%s:%d)", #cond, __FILE__,          \
                         __LINE__);                                                                \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static bool body_awake(const PhysBody *b)
{
    return (b->flags & PHYS_BODY_AWAKE) != 0U;
}

bool nph_physics_world_validate(const NphPhysicsWorld *world)
{
    // Awake bodies.
    for (u32 i = 0U; i < world->awake_bodies.len; ++i) {
        const PhysBody *b = &world->bodies.data[world->awake_bodies.data[i]];
        VALID(b->handle != 0U && b->awake_slot == i && body_awake(b));
        VALID(b->type != NPH_PHYSICS_BODY_STATIC);
    }
    u32 live_bodies = 0U;
    for (u32 i = 0U; i < world->bodies.len; ++i) {
        const PhysBody *b = &world->bodies.data[i];
        if (b->handle == 0U) {
            continue;
        }
        live_bodies += 1U;
        VALID(body_awake(b) == (b->awake_slot != PHYS_NULL));
        VALID(b->type == NPH_PHYSICS_BODY_DYNAMIC ? b->island != PHYS_NULL
                                                  : b->island == PHYS_NULL);
        // Contact list.
        u32 n = 0U;
        for (u32 key = b->contact_head; key != PHYS_NULL; ++n) {
            const PhysContact *c = &world->contacts.data[key >> 1U];
            const u32 side = key & 1U;
            VALID((side == 0U ? c->body_a : c->body_b) == i);
            VALID(n <= b->contact_count);
            key = c->next[side];
        }
        VALID(n == b->contact_count);
        // Joint list.
        u32 nj = 0U;
        for (u32 key = b->joint_head; key != PHYS_NULL; ++nj) {
            const PhysJoint *j = &world->joints.data[key >> 1U];
            const u32 side = key & 1U;
            VALID(j->handle != 0U && (side == 0U ? j->body_a : j->body_b) == i);
            VALID(nj <= b->joint_count);
            key = j->next[side];
        }
        VALID(nj == b->joint_count);
        // Shapes.
        u32 shapes = 0U;
        for (u32 s = b->shape_head; s != PHYS_NULL; s = world->shapes.data[s].next) {
            VALID(world->shapes.data[s].body == i);
            shapes += 1U;
        }
        VALID(shapes == b->shape_count);
    }
    // Contacts: awake list, graph membership, islands.
    for (u32 i = 0U; i < world->awake_contacts.len; ++i) {
        const PhysContact *c = &world->contacts.data[world->awake_contacts.data[i]];
        VALID(c->awake_slot == i);
    }
    u32 children = 0U;
    for (u32 i = 0U; i < world->contacts.len; ++i) {
        const PhysContact *c = &world->contacts.data[i];
        if (c->shape_a == PHYS_NULL) {
            continue;
        }
        const PhysBody *a = &world->bodies.data[c->body_a];
        const PhysBody *b = &world->bodies.data[c->body_b];
        const bool mesh_pair =
            !nph_physics_shape_type_is_convex(world->shapes.data[c->shape_a].geometry.type) ||
            !nph_physics_shape_type_is_convex(world->shapes.data[c->shape_b].geometry.type);
        if (c->parent != PHYS_NULL) {
            // A mesh pair's child: its pair's shapes, found in its pair's
            // chain, outside the awake list, without events or islands.
            const PhysContact *pair = &world->contacts.data[c->parent];
            VALID(mesh_pair && pair->parent == PHYS_NULL && pair->shape_a == c->shape_a &&
                  pair->shape_b == c->shape_b && pair->body_a == c->body_a &&
                  pair->body_b == c->body_b);
            u32 k = pair->child;
            while (k != PHYS_NULL && k != i) {
                k = world->contacts.data[k].child;
            }
            VALID(k == i);
            VALID(c->awake_slot == PHYS_NULL && c->island == PHYS_NULL);
            VALID((c->flags & (u32)~PHYS_CONTACT_TOUCHING) == 0U);
            children += 1U;
        } else {
            // A pair: a mesh pair's chain holds its three children.
            u32 n = 0U;
            for (u32 k = c->child; k != PHYS_NULL && n <= NPH_PHYSICS_MESH_MAX_MANIFOLDS;
                 k = world->contacts.data[k].child) {
                VALID(world->contacts.data[k].parent == i);
                n += 1U;
            }
            VALID(n == (mesh_pair ? NPH_PHYSICS_MESH_MAX_MANIFOLDS - 1U : 0U));
            VALID((c->awake_slot != PHYS_NULL) == (body_awake(a) || body_awake(b)));
        }
        const bool ok_a = body_awake(a) || a->type != NPH_PHYSICS_BODY_DYNAMIC;
        const bool ok_b = body_awake(b) || b->type != NPH_PHYSICS_BODY_DYNAMIC;
        const bool in_graph = (c->flags & PHYS_CONTACT_TOUCHING) != 0U && ok_a && ok_b &&
                              (body_awake(a) || body_awake(b));
        VALID(in_graph == (c->color != PHYS_NULL));
        if (c->color != PHYS_NULL) {
            const NphArrayU32 *list = c->color == PHYS_GRAPH_OVERFLOW
                                          ? &world->overflow
                                          : &world->colors[c->color].contacts;
            VALID(c->color_slot < list->len && list->data[c->color_slot] == i);
            if (c->color != PHYS_GRAPH_OVERFLOW) {
                const NphBitset *bits = &world->colors[c->color].bodies;
                VALID(a->type != NPH_PHYSICS_BODY_DYNAMIC || nph_bitset_test(bits, c->body_a));
                VALID(b->type != NPH_PHYSICS_BODY_DYNAMIC || nph_bitset_test(bits, c->body_b));
            }
        }
        const bool island = (c->flags & PHYS_CONTACT_TOUCHING) != 0U &&
                            a->type == NPH_PHYSICS_BODY_DYNAMIC &&
                            b->type == NPH_PHYSICS_BODY_DYNAMIC;
        VALID(island == ((c->flags & PHYS_CONTACT_ISLAND) != 0U));
        VALID(island == (c->island != PHYS_NULL));
    }
    VALID(children == world->child_contacts);
    // Joints: graph membership and islands, as contacts (always touching).
    u32 live_joints = 0U;
    for (u32 i = 0U; i < world->joints.len; ++i) {
        const PhysJoint *j = &world->joints.data[i];
        if (j->handle == 0U) {
            continue;
        }
        live_joints += 1U;
        const PhysBody *a = &world->bodies.data[j->body_a];
        const PhysBody *b = &world->bodies.data[j->body_b];
        VALID(a->handle != 0U && b->handle != 0U && j->body_a != j->body_b);
        const bool ok_a = body_awake(a) || a->type != NPH_PHYSICS_BODY_DYNAMIC;
        const bool ok_b = body_awake(b) || b->type != NPH_PHYSICS_BODY_DYNAMIC;
        const bool in_graph = ok_a && ok_b && (body_awake(a) || body_awake(b));
        VALID(in_graph == (j->color != PHYS_NULL));
        if (j->color != PHYS_NULL) {
            const NphArrayU32 *list = j->color == PHYS_GRAPH_OVERFLOW
                                          ? &world->overflow_joints
                                          : &world->colors[j->color].joints;
            VALID(j->color_slot < list->len && list->data[j->color_slot] == i);
            if (j->color != PHYS_GRAPH_OVERFLOW) {
                const NphBitset *bits = &world->colors[j->color].bodies;
                VALID(a->type != NPH_PHYSICS_BODY_DYNAMIC || nph_bitset_test(bits, j->body_a));
                VALID(b->type != NPH_PHYSICS_BODY_DYNAMIC || nph_bitset_test(bits, j->body_b));
            }
        }
        const bool island =
            a->type == NPH_PHYSICS_BODY_DYNAMIC && b->type == NPH_PHYSICS_BODY_DYNAMIC;
        VALID(island == ((j->flags & PHYS_JOINT_ISLAND) != 0U));
        VALID(island == (j->island != PHYS_NULL));
    }
    VALID(live_joints == world->joint_slots.generation.len - world->joint_slots.free.len);
    // A colour's constraints share no dynamic body: its bitset holds as many
    // bodies as its constraints have dynamic ends.
    for (u32 c = 0U; c < PHYS_GRAPH_COLORS; ++c) {
        const PhysColor *color = &world->colors[c];
        u32 ends = 0U;
        for (u32 k = 0U; k < color->contacts.len; ++k) {
            const PhysContact *contact = &world->contacts.data[color->contacts.data[k]];
            ends += world->bodies.data[contact->body_a].type == NPH_PHYSICS_BODY_DYNAMIC ? 1U : 0U;
            ends += world->bodies.data[contact->body_b].type == NPH_PHYSICS_BODY_DYNAMIC ? 1U : 0U;
        }
        for (u32 k = 0U; k < color->joints.len; ++k) {
            const PhysJoint *j = &world->joints.data[color->joints.data[k]];
            ends += world->bodies.data[j->body_a].type == NPH_PHYSICS_BODY_DYNAMIC ? 1U : 0U;
            ends += world->bodies.data[j->body_b].type == NPH_PHYSICS_BODY_DYNAMIC ? 1U : 0U;
        }
        VALID(ends == nph_bitset_count(&color->bodies));
    }
    // Islands: bodies agree, and connected pairs share an island root.
    for (u32 i = 0U; i < world->islands.len; ++i) {
        const PhysIsland *is = &world->islands.data[i];
        if (is->live == 0U) {
            continue;
        }
        u32 n = 0U;
        for (u32 b = is->body_head; b != PHYS_NULL; b = world->bodies.data[b].island_next) {
            VALID(world->bodies.data[b].island == i);
            VALID(body_awake(&world->bodies.data[b]) == (is->awake != 0U));
            n += 1U;
        }
        VALID(n == is->body_count && n > 0U);
        u32 m = 0U;
        for (u32 c = is->contact_head; c != PHYS_NULL; c = world->contacts.data[c].island_next) {
            VALID(world->contacts.data[c].island == i);
            m += 1U;
        }
        VALID(m == is->contact_count);
        u32 nj = 0U;
        for (u32 j = is->joint_head; j != PHYS_NULL; j = world->joints.data[j].island_next) {
            VALID(world->joints.data[j].island == i);
            nj += 1U;
        }
        VALID(nj == is->joint_count);
        VALID(is->parent == PHYS_NULL); // merges are applied by the end of a step
    }
    for (u32 t = 0U; t < PHYS_TREE_COUNT; ++t) {
        VALID(nph_aabb_tree_validate(&world->trees[t]));
    }
    VALID(live_bodies == world->body_slots.generation.len - world->body_slots.free.len);
    return true;
}

// ---------------------------------------------------------------------------
// Debug drawing
// ---------------------------------------------------------------------------

static void draw_line(const NphPhysicsDebugDraw *d, NphDVec3 a, NphDVec3 b, u32 rgba)
{
    d->line(d->user, nph_dvec3_relative(a, d->origin), nph_dvec3_relative(b, d->origin), rgba);
}

static void draw_box_lines(const NphPhysicsDebugDraw *d, const NphDVec3 corners[8], u32 rgba)
{
    static const u8 k_edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (u32 e = 0U; e < 12U; ++e) {
        draw_line(d, corners[k_edges[e][0]], corners[k_edges[e][1]], rgba);
    }
}

static NphDVec3 world_point(NphDVec3 origin, NphVec3 rel)
{
    return nph_dvec3_add_vec3(origin, rel);
}

static void draw_circle(const NphPhysicsDebugDraw *d, NphDVec3 base, NphVec3 center, NphVec3 u,
                        NphVec3 v, f32 r, u32 rgba)
{
    enum {
        SEGMENTS = 16
    };
    NphVec3 prev = nph_vec3_mul_add(center, u, r);
    for (u32 i = 1U; i <= (u32)SEGMENTS; ++i) {
        const f32 a = 6.2831853F * (f32)i / (f32)SEGMENTS;
        const NphVec3 p = nph_vec3_add(center, nph_vec3_add(nph_vec3_scale(u, r * nph_f32_cos(a)),
                                                            nph_vec3_scale(v, r * nph_f32_sin(a))));
        draw_line(d, world_point(base, prev), world_point(base, p), rgba);
        prev = p;
    }
}

static void draw_shape(const NphPhysicsDebugDraw *d, const PhysBody *b, const PhysShape *s,
                       u32 rgba)
{
    const NphPhysicsPose pose = phys_shape_pose_relative(b, s, b->origin);
    const NphVec3 x = nph_quat_axis_x(pose.rotation);
    const NphVec3 y = nph_quat_axis_y(pose.rotation);
    const NphVec3 z = nph_quat_axis_z(pose.rotation);
    switch (s->geometry.type) {
    case NPH_PHYSICS_SHAPE_SPHERE: {
        const f32 r = s->geometry.sphere.radius;
        draw_circle(d, b->origin, pose.position, x, y, r, rgba);
        draw_circle(d, b->origin, pose.position, y, z, r, rgba);
        draw_circle(d, b->origin, pose.position, z, x, r, rgba);
        break;
    }
    case NPH_PHYSICS_SHAPE_CAPSULE: {
        const f32 r = s->geometry.capsule.radius;
        const NphVec3 h = nph_vec3_scale(y, s->geometry.capsule.half_height);
        const NphVec3 c0 = nph_vec3_sub(pose.position, h);
        const NphVec3 c1 = nph_vec3_add(pose.position, h);
        draw_circle(d, b->origin, c0, x, z, r, rgba);
        draw_circle(d, b->origin, c1, x, z, r, rgba);
        draw_circle(d, b->origin, c0, x, y, r, rgba);
        draw_circle(d, b->origin, c1, x, y, r, rgba);
        const NphVec3 sides[4] = {x, nph_vec3_neg(x), z, nph_vec3_neg(z)};
        for (u32 k = 0U; k < 4U; ++k) {
            draw_line(d, world_point(b->origin, nph_vec3_mul_add(c0, sides[k], r)),
                      world_point(b->origin, nph_vec3_mul_add(c1, sides[k], r)), rgba);
        }
        break;
    }
    case NPH_PHYSICS_SHAPE_BOX: {
        const NphVec3 he = s->geometry.box.half_extents;
        NphDVec3 corners[8];
        for (u32 i = 0U; i < 8U; ++i) {
            const NphVec3 local =
                nph_vec3((i & 1U) != 0U ? he.x : -he.x, (i & 2U) != 0U ? he.y : -he.y,
                         (i & 4U) != 0U ? he.z : -he.z);
            corners[i] = world_point(b->origin, nph_physics_pose_transform_point(pose, local));
        }
        draw_box_lines(d, corners, rgba);
        break;
    }
    case NPH_PHYSICS_SHAPE_HULL: {
        const NphPhysicsHull *hull = s->geometry.hull;
        const NphVec3 *v = nph_physics_hull_vertices(hull);
        const NphPhysicsHullEdge *e = nph_physics_hull_edges(hull);
        for (u32 i = 0U; i < hull->edge_count; i += 2U) {
            const NphVec3 p = nph_physics_pose_transform_point(pose, v[e[i].origin]);
            const NphVec3 q = nph_physics_pose_transform_point(pose, v[e[i + 1U].origin]);
            draw_line(d, world_point(b->origin, p), world_point(b->origin, q), rgba);
        }
        break;
    }
    case NPH_PHYSICS_SHAPE_MESH: {
        // Every triangle's edges, the active ones (the surface's real
        // edges) brighter.
        const NphPhysicsMesh *mesh = s->geometry.mesh;
        const u8 *active = nph_physics_mesh_edges(mesh);
        for (u32 t = 0U; t < mesh->triangle_count; ++t) {
            NphVec3 tv[3];
            nph_physics_mesh_triangle(mesh, t, tv);
            for (u32 k = 0U; k < 3U; ++k) {
                const NphVec3 p = nph_physics_pose_transform_point(pose, tv[k]);
                const NphVec3 q = nph_physics_pose_transform_point(pose, tv[(k + 1U) % 3U]);
                const u32 color = ((active[t] >> k) & 1U) != 0U ? rgba : (rgba & 0xFF7F7F7FU);
                draw_line(d, world_point(b->origin, p), world_point(b->origin, q), color);
            }
        }
        break;
    }
    case NPH_PHYSICS_SHAPE_HEIGHTFIELD: {
        // As a mesh: every triangle's edges (holes have none), the active
        // ones brighter.
        const NphPhysicsHeightfield *h = s->geometry.heightfield;
        const u32 triangles = nph_physics_heightfield_triangle_count(h);
        for (u32 t = 0U; t < triangles; ++t) {
            NphVec3 tv[3];
            u32 active = 0U;
            if (!nph_physics_heightfield_triangle(h, t, tv, &active)) {
                continue;
            }
            for (u32 k = 0U; k < 3U; ++k) {
                const NphVec3 p = nph_physics_pose_transform_point(pose, tv[k]);
                const NphVec3 q = nph_physics_pose_transform_point(pose, tv[(k + 1U) % 3U]);
                const u32 color = ((active >> k) & 1U) != 0U ? rgba : (rgba & 0xFF7F7F7FU);
                draw_line(d, world_point(b->origin, p), world_point(b->origin, q), color);
            }
        }
        break;
    }
    case NPH_PHYSICS_SHAPE_TYPE_COUNT:
    default:
        break;
    }
}

void nph_physics_world_draw(const NphPhysicsWorld *world, const NphPhysicsDebugDraw *draw)
{
    NPH_ASSERT(world != NULL && draw != NULL && draw->line != NULL);
    NPH_ASSERT_MSG(!world->stepping, "nph_physics_world_draw during a step");
    for (u32 i = 0U; i < world->bodies.len; ++i) {
        const PhysBody *b = &world->bodies.data[i];
        if (b->handle == 0U) {
            continue;
        }
        u32 color = 0xFF80C080U; // dynamic awake: green (ABGR)
        if (b->type == NPH_PHYSICS_BODY_STATIC) {
            color = 0xFF808080U;
        } else if (b->type == NPH_PHYSICS_BODY_KINEMATIC) {
            color = 0xFFC08080U;
        } else if (!body_awake(b) && (draw->flags & NPH_PHYSICS_DRAW_SLEEP) != 0U) {
            color = 0xFF806040U;
        }
        for (u32 s = b->shape_head; s != PHYS_NULL; s = world->shapes.data[s].next) {
            const PhysShape *shape = &world->shapes.data[s];
            if ((draw->flags & NPH_PHYSICS_DRAW_SHAPES) != 0U) {
                draw_shape(draw, b, shape, color);
            }
            if ((draw->flags & NPH_PHYSICS_DRAW_AABBS) != 0U) {
                NphDVec3 corners[8];
                const NphAABB box = shape->fat_aabb;
                for (u32 k = 0U; k < 8U; ++k) {
                    corners[k] = nph_dvec3((k & 1U) != 0U ? (f64)box.max.x : (f64)box.min.x,
                                           (k & 2U) != 0U ? (f64)box.max.y : (f64)box.min.y,
                                           (k & 4U) != 0U ? (f64)box.max.z : (f64)box.min.z);
                }
                draw_box_lines(draw, corners, 0xFF40A0FFU);
            }
        }
        if ((draw->flags & NPH_PHYSICS_DRAW_MASS) != 0U && b->type == NPH_PHYSICS_BODY_DYNAMIC) {
            const f64 k = 0.1;
            draw_line(draw, nph_dvec3_add(b->center, nph_dvec3(-k, 0.0, 0.0)),
                      nph_dvec3_add(b->center, nph_dvec3(k, 0.0, 0.0)), 0xFFFFFFFFU);
            draw_line(draw, nph_dvec3_add(b->center, nph_dvec3(0.0, -k, 0.0)),
                      nph_dvec3_add(b->center, nph_dvec3(0.0, k, 0.0)), 0xFFFFFFFFU);
            draw_line(draw, nph_dvec3_add(b->center, nph_dvec3(0.0, 0.0, -k)),
                      nph_dvec3_add(b->center, nph_dvec3(0.0, 0.0, k)), 0xFFFFFFFFU);
        }
    }
    if ((draw->flags & NPH_PHYSICS_DRAW_JOINTS) != 0U) {
        for (u32 i = 0U; i < world->joints.len; ++i) {
            const PhysJoint *j = &world->joints.data[i];
            if (j->handle == 0U) {
                continue;
            }
            const PhysBody *a = &world->bodies.data[j->body_a];
            const PhysBody *b = &world->bodies.data[j->body_b];
            const NphDVec3 pa =
                world_point(a->origin, nph_quat_rotate(a->rotation, j->frame_a.position));
            const NphDVec3 pb =
                world_point(b->origin, nph_quat_rotate(b->rotation, j->frame_b.position));
            draw_line(draw, a->center, pa, 0xFF00C0FFU);
            draw_line(draw, b->center, pb, 0xFF00C0FFU);
            draw_line(draw, pa, pb, 0xFF0000FFU); // the error, red
            const NphVec3 xa = nph_quat_axis_x(nph_quat_mul(a->rotation, j->frame_a.rotation));
            const NphVec3 xb = nph_quat_axis_x(nph_quat_mul(b->rotation, j->frame_b.rotation));
            draw_line(draw, pa, world_point(pa, nph_vec3_scale(xa, 0.2F)), 0xFFFFFF00U);
            draw_line(draw, pb, world_point(pb, nph_vec3_scale(xb, 0.15F)), 0xFFFF00FFU);
        }
    }
    if ((draw->flags & NPH_PHYSICS_DRAW_CONTACTS) != 0U) {
        for (u32 i = 0U; i < world->contacts.len; ++i) {
            const PhysContact *c = &world->contacts.data[i];
            if (c->shape_a == PHYS_NULL || (c->flags & PHYS_CONTACT_TOUCHING) == 0U) {
                continue;
            }
            const NphDVec3 ca = world->bodies.data[c->body_a].center;
            for (u32 p = 0U; p < c->point_count; ++p) {
                const NphDVec3 at = world_point(ca, c->points[p].anchor_a);
                const u32 rgba = c->points[p].separation > 0.0F ? 0xFF808000U : 0xFF0000FFU;
                draw_line(draw, at, world_point(at, nph_vec3_scale(c->normal, 0.1F)), rgba);
            }
        }
    }
}
