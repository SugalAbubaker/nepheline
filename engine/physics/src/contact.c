// SPDX-License-Identifier: Apache-2.0
// Contacts: their lifetime, the broadphase's new pairs, the narrow phase,
// and the serial update of what began and stopped touching (docs/physics.md
// "Contacts").

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <string.h>

static u64 pair_key(u32 a, u32 b)
{
    return a < b ? ((u64)a << 32U) | b : ((u64)b << 32U) | a;
}

static bool body_awake(const PhysBody *b)
{
    return (b->flags & PHYS_BODY_AWAKE) != 0U;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

static void list_insert(NphPhysicsWorld *w, u32 contact, u32 side, u32 body)
{
    PhysBody *b = &w->bodies.data[body];
    PhysContact *c = &w->contacts.data[contact];
    const u32 key = (contact << 1U) | side;
    c->prev[side] = PHYS_NULL;
    c->next[side] = b->contact_head;
    if (b->contact_head != PHYS_NULL) {
        w->contacts.data[b->contact_head >> 1U].prev[b->contact_head & 1U] = key;
    }
    b->contact_head = key;
    b->contact_count += 1U;
}

static void list_remove(NphPhysicsWorld *w, u32 contact, u32 side, u32 body)
{
    PhysBody *b = &w->bodies.data[body];
    PhysContact *c = &w->contacts.data[contact];
    const u32 prev = c->prev[side];
    const u32 next = c->next[side];
    if (prev != PHYS_NULL) {
        w->contacts.data[prev >> 1U].next[prev & 1U] = next;
    } else {
        b->contact_head = next;
    }
    if (next != PHYS_NULL) {
        w->contacts.data[next >> 1U].prev[next & 1U] = prev;
    }
    b->contact_count -= 1U;
}

static NphResult awake_list_add(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    if (c->awake_slot != PHYS_NULL) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(NPH_ARRAY_PUSH(&w->awake_contacts, contact));
    w->contacts.data[contact].awake_slot = w->awake_contacts.len - 1U;
    return NPH_RESULT_OK;
}

static void awake_list_remove(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const u32 slot = c->awake_slot;
    if (slot == PHYS_NULL) {
        return;
    }
    const u32 last = w->awake_contacts.data[w->awake_contacts.len - 1U];
    w->awake_contacts.data[slot] = last;
    w->contacts.data[last].awake_slot = slot;
    w->awake_contacts.len -= 1U;
    c->awake_slot = PHYS_NULL;
}

NphResult phys_contact_create(NphPhysicsWorld *w, u32 shape_a, u32 shape_b)
{
    u32 index;
    if (w->free_contacts.len > 0U) {
        index = w->free_contacts.data[w->free_contacts.len - 1U];
        w->free_contacts.len -= 1U;
    } else {
        if (NPH_ARRAY_PUSH_SLOT(&w->contacts) == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        index = w->contacts.len - 1U;
        // Room to free every contact later without growing.
        const NphResult r = NPH_ARRAY_RESERVE(&w->free_contacts, w->contacts.len);
        if (r != NPH_RESULT_OK) {
            w->contacts.len -= 1U;
            return r;
        }
    }
    NphResult r = nph_hashmap_insert_u64(&w->pairs, pair_key(shape_a, shape_b), &index);
    if (r == NPH_RESULT_OK && w->awake_contacts.cap < w->contacts.len) {
        r = NPH_ARRAY_RESERVE(&w->awake_contacts, w->contacts.len);
    }
    if (r != NPH_RESULT_OK) {
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->free_contacts, index));
        return r;
    }
    const PhysShape *sa = &w->shapes.data[shape_a];
    const PhysShape *sb = &w->shapes.data[shape_b];
    PhysContact *c = &w->contacts.data[index];
    memset(c, 0, sizeof *c);
    c->shape_a = shape_a;
    c->shape_b = shape_b;
    c->body_a = sa->body;
    c->body_b = sb->body;
    c->color = PHYS_NULL;
    c->color_slot = PHYS_NULL;
    c->island = PHYS_NULL;
    c->island_prev = PHYS_NULL;
    c->island_next = PHYS_NULL;
    c->awake_slot = PHYS_NULL;
    c->normal = nph_vec3_unit_y();
    if (((sa->flags | sb->flags) & PHYS_SHAPE_CONTACT_EVENTS) != 0U) {
        c->flags |= PHYS_CONTACT_EVENTS;
    }
    if (((sa->flags | sb->flags) & PHYS_SHAPE_HIT_EVENTS) != 0U) {
        c->flags |= PHYS_CONTACT_HIT_EVENTS;
    }
    list_insert(w, index, 0U, c->body_a);
    list_insert(w, index, 1U, c->body_b);
    if (body_awake(&w->bodies.data[c->body_a]) || body_awake(&w->bodies.data[c->body_b])) {
        // Reserved above: cannot fail.
        NPH_IGNORE_RESULT(awake_list_add(w, index));
    }
    return NPH_RESULT_OK;
}

void phys_contact_destroy(NphPhysicsWorld *w, u32 contact, bool events)
{
    PhysContact *c = &w->contacts.data[contact];
    if ((c->flags & PHYS_CONTACT_TOUCHING) != 0U && events &&
        (c->flags & PHYS_CONTACT_EVENTS) != 0U) {
        NphPhysicsContactEndEvent e;
        e.shape_a = phys_shape_handle(w, c->shape_a);
        e.shape_b = phys_shape_handle(w, c->shape_b);
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->end_events, e));
    }
    if (c->color != PHYS_NULL) {
        phys_graph_remove(w, contact);
    }
    if ((c->flags & PHYS_CONTACT_ISLAND) != 0U) {
        phys_island_unlink_contact(w, contact);
    }
    awake_list_remove(w, contact);
    list_remove(w, contact, 0U, c->body_a);
    list_remove(w, contact, 1U, c->body_b);
    const bool removed = nph_hashmap_remove_u64(&w->pairs, pair_key(c->shape_a, c->shape_b));
    NPH_ASSERT(removed);
    NPH_UNUSED(removed);
    c->shape_a = PHYS_NULL;
    c->shape_b = PHYS_NULL;
    c->flags = 0U;
    // Reserved when the contact was created: cannot fail.
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->free_contacts, contact));
}

// ---------------------------------------------------------------------------
// Graph and awake membership follow the bodies (world_internal.h's rules)
// ---------------------------------------------------------------------------

static bool body_solvable(const PhysBody *b)
{
    return body_awake(b) || b->type != NPH_PHYSICS_BODY_DYNAMIC;
}

// Brings a contact's awake-list and graph membership in line with its
// bodies and its touching state.
static NphResult contact_sync(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const PhysBody *a = &w->bodies.data[c->body_a];
    const PhysBody *b = &w->bodies.data[c->body_b];
    const bool awake = body_awake(a) || body_awake(b);
    if (awake) {
        NPH_TRY(awake_list_add(w, contact));
    } else {
        awake_list_remove(w, contact);
    }
    const bool in_graph =
        (c->flags & PHYS_CONTACT_TOUCHING) != 0U && awake && body_solvable(a) && body_solvable(b);
    if (in_graph && c->color == PHYS_NULL) {
        NPH_TRY(phys_graph_add(w, contact));
    } else if (!in_graph && c->color != PHYS_NULL) {
        phys_graph_remove(w, contact);
    }
    return NPH_RESULT_OK;
}

NphResult phys_body_contacts_sync(NphPhysicsWorld *w, u32 body)
{
    for (u32 key = w->bodies.data[body].contact_head; key != PHYS_NULL;) {
        const u32 c = key >> 1U;
        const u32 next = w->contacts.data[c].next[key & 1U];
        NPH_TRY(contact_sync(w, c));
        key = next;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The narrow phase
// ---------------------------------------------------------------------------

static bool boxes_overlap(NphAABB a, NphAABB b)
{
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y &&
           a.min.z <= b.max.z && b.min.z <= a.max.z;
}

void phys_contact_update(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const PhysShape *sa = &w->shapes.data[c->shape_a];
    const PhysShape *sb = &w->shapes.data[c->shape_b];
    c->change = 0U;
    const bool was_touching = (c->flags & PHYS_CONTACT_TOUCHING) != 0U;
    if (!boxes_overlap(sa->fat_aabb, sb->fat_aabb)) {
        c->change = PHYS_CHANGE_DISJOINT;
        return;
    }
    const PhysBody *ba = &w->bodies.data[c->body_a];
    const PhysBody *bb = &w->bodies.data[c->body_b];
    // The frame of the manifold: world axes, centred on A's centre of mass,
    // so point coordinates are A's anchors and stay small (types.h).
    const NphDVec3 origin = ba->center;
    const NphPhysicsPose pa = phys_shape_pose_relative(ba, sa, origin);
    const NphPhysicsPose pb = phys_shape_pose_relative(bb, sb, origin);
    NphPhysicsManifold m;
    nph_physics_collide(&sa->geometry, pa, &sb->geometry, pb, NPH_PHYSICS_SPECULATIVE_DISTANCE,
                        &c->cache, &m);
    const NphVec3 ab = nph_dvec3_relative(bb->center, origin);
    // Carry impulses to the points that persist (same features), for warm
    // starting. The tangent impulse is a world vector: it survives a turn of
    // the tangent basis.
    PhysContactPoint points[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
    for (u32 i = 0U; i < m.point_count; ++i) {
        PhysContactPoint *p = &points[i];
        memset(p, 0, sizeof *p);
        p->anchor_a = m.points[i].point;
        p->anchor_b = nph_vec3_sub(m.points[i].point, ab);
        p->separation = m.points[i].separation;
        p->id = m.points[i].id;
        for (u32 j = 0U; j < c->point_count; ++j) {
            if (c->points[j].id == p->id) {
                p->normal_impulse = c->points[j].normal_impulse;
                p->tangent_impulse = c->points[j].tangent_impulse;
                break;
            }
        }
    }
    c->normal = m.normal;
    c->point_count = m.point_count;
    memcpy(c->points, points, (size_t)m.point_count * sizeof(PhysContactPoint));
    // Surfaces may change between steps (shape.h).
    c->friction = nph_f32_sqrt(sa->friction * sb->friction);
    c->restitution = nph_f32_max(sa->restitution, sb->restitution);
    const bool touching = m.point_count > 0U;
    if (touching && !was_touching) {
        c->change = PHYS_CHANGE_STARTED;
    } else if (!touching && was_touching) {
        c->change = PHYS_CHANGE_STOPPED;
    }
}

typedef struct CollideTask {
    NphPhysicsWorld *w;
} CollideTask;

static void collide_range(void *user, u32 begin, u32 end)
{
    NphPhysicsWorld *w = ((CollideTask *)user)->w;
    for (u32 i = begin; i < end; ++i) {
        phys_contact_update(w, w->awake_contacts.data[i]);
    }
}

static NphResult apply_change(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const u32 change = c->change;
    c->change = 0U;
    if ((change & PHYS_CHANGE_DISJOINT) != 0U) {
        phys_contact_destroy(w, contact, true);
        return NPH_RESULT_OK;
    }
    const PhysBody *a = &w->bodies.data[c->body_a];
    const PhysBody *b = &w->bodies.data[c->body_b];
    const bool both_dynamic =
        a->type == NPH_PHYSICS_BODY_DYNAMIC && b->type == NPH_PHYSICS_BODY_DYNAMIC;
    if ((change & PHYS_CHANGE_STARTED) != 0U) {
        c->flags |= PHYS_CONTACT_TOUCHING;
        if ((c->flags & PHYS_CONTACT_EVENTS) != 0U) {
            NphPhysicsContactBeginEvent e;
            e.shape_a = phys_shape_handle(w, c->shape_a);
            e.shape_b = phys_shape_handle(w, c->shape_b);
            NPH_TRY(NPH_ARRAY_PUSH(&w->begin_events, e));
        }
        // A sleeping dynamic body touched by an awake one wakes (with its
        // island); the two islands then link.
        if (a->type == NPH_PHYSICS_BODY_DYNAMIC && !body_awake(a)) {
            NPH_TRY(phys_wake_body(w, c->body_a));
        }
        if (b->type == NPH_PHYSICS_BODY_DYNAMIC && !body_awake(b)) {
            NPH_TRY(phys_wake_body(w, c->body_b));
        }
        if (both_dynamic) {
            NPH_TRY(phys_island_link_contact(w, contact));
        }
        return contact_sync(w, contact);
    }
    if ((change & PHYS_CHANGE_STOPPED) != 0U) {
        c->flags &= ~(u32)PHYS_CONTACT_TOUCHING;
        if ((c->flags & PHYS_CONTACT_EVENTS) != 0U) {
            NphPhysicsContactEndEvent e;
            e.shape_a = phys_shape_handle(w, c->shape_a);
            e.shape_b = phys_shape_handle(w, c->shape_b);
            NPH_TRY(NPH_ARRAY_PUSH(&w->end_events, e));
        }
        if ((c->flags & PHYS_CONTACT_ISLAND) != 0U) {
            phys_island_unlink_contact(w, contact);
        }
        return contact_sync(w, contact);
    }
    return NPH_RESULT_OK;
}

NphResult phys_collide(NphPhysicsWorld *w, NphJobSystem *js)
{
    const u32 n = w->awake_contacts.len;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    CollideTask task = {w};
    if (js != NULL && n >= 64U) {
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(js, n, 32U, collide_range, &task, NPH_JOB_PRIORITY_HIGH, &done) ==
            NPH_RESULT_OK) {
            nph_job_wait(js, &done);
        } else {
            nph_job_wait(js, &done);
            collide_range(&task, 0U, n);
        }
    } else {
        collide_range(&task, 0U, n);
    }
    // The changes, in the awake list's order: a fixed order, whatever the
    // threads did.
    NPH_ARRAY_CLEAR(&w->changed);
    for (u32 i = 0U; i < n; ++i) {
        const u32 c = w->awake_contacts.data[i];
        if (w->contacts.data[c].change != 0U) {
            NPH_TRY(NPH_ARRAY_PUSH(&w->changed, c));
        }
    }
    for (u32 i = 0U; i < w->changed.len; ++i) {
        NPH_TRY(apply_change(w, w->changed.data[i]));
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// New pairs from the broadphase
// ---------------------------------------------------------------------------

typedef struct PairQuery {
    const NphPhysicsWorld *w;
    u32 shape;     // the moved shape
    u32 key;       // its proxy key
    PhysPair *out; // NULL for the counting pass
    u32 count;
} PairQuery;

static bool pair_callback(u32 proxy, u64 user_data, void *context)
{
    PairQuery *q = (PairQuery *)context;
    const NphPhysicsWorld *w = q->w;
    const u32 other = (u32)user_data;
    if (other == q->shape) {
        return true;
    }
    const PhysShape *s = &w->shapes.data[q->shape];
    const PhysShape *o = &w->shapes.data[other];
    // Both moved: the one with the lower key reports the pair, once.
    const u32 other_key = phys_proxy_key(proxy, o->tree);
    if ((o->flags & PHYS_SHAPE_IN_MOVE_BUFFER) != 0U && other_key < q->key) {
        return true;
    }
    if (s->body == o->body) {
        return true;
    }
    const PhysBody *bs = &w->bodies.data[s->body];
    const PhysBody *bo = &w->bodies.data[o->body];
    if (bs->type != NPH_PHYSICS_BODY_DYNAMIC && bo->type != NPH_PHYSICS_BODY_DYNAMIC) {
        return true;
    }
    if (!nph_physics_filter_should_collide(s->filter, o->filter)) {
        return true;
    }
    if (nph_hashmap_contains_u64(&w->pairs, pair_key(q->shape, other))) {
        return true;
    }
    if (q->out != NULL) {
        q->out[q->count].shape_a = q->shape;
        q->out[q->count].shape_b = other;
    }
    q->count += 1U;
    return true;
}

static u32 query_moved(const NphPhysicsWorld *w, u32 key, PhysPair *out)
{
    const u32 tree = key & 3U;
    const u32 proxy = key >> 2U;
    PairQuery q;
    q.w = w;
    q.shape = (u32)nph_aabb_tree_proxy_user_data(&w->trees[tree], proxy);
    q.key = key;
    q.out = out;
    q.count = 0U;
    const PhysShape *s = &w->shapes.data[q.shape];
    const NphAABB box = s->fat_aabb;
    // The trees filter by category in the mask; a positive group collides
    // whatever the categories, so it asks for everything.
    const u64 mask = s->filter.group > 0 ? NPH_U64_MAX : s->filter.mask;
    // Dynamic shapes meet every tree; static and kinematic ones only the
    // dynamic shapes.
    if (tree == PHYS_TREE_DYNAMIC) {
        nph_aabb_tree_query(&w->trees[PHYS_TREE_STATIC], box, mask, pair_callback, &q);
        nph_aabb_tree_query(&w->trees[PHYS_TREE_KINEMATIC], box, mask, pair_callback, &q);
    }
    nph_aabb_tree_query(&w->trees[PHYS_TREE_DYNAMIC], box, mask, pair_callback, &q);
    return q.count;
}

typedef struct PairTask {
    NphPhysicsWorld *w;
    bool fill;
} PairTask;

static void pair_range(void *user, u32 begin, u32 end)
{
    const PairTask *t = (const PairTask *)user;
    NphPhysicsWorld *w = t->w;
    for (u32 i = begin; i < end; ++i) {
        const u32 key = w->move_buffer.data[i];
        if (t->fill) {
            const u32 first = i == 0U ? 0U : w->pair_counts.data[i - 1U];
            const u32 n = query_moved(w, key, &w->pair_buffer.data[first]);
            NPH_ASSERT(n == w->pair_counts.data[i] - first);
            NPH_UNUSED(n);
        } else {
            w->pair_counts.data[i] = query_moved(w, key, NULL);
        }
    }
}

static void run_pairs(NphPhysicsWorld *w, NphJobSystem *js, bool fill)
{
    PairTask t = {w, fill};
    const u32 n = w->move_buffer.len;
    if (js != NULL && n >= 32U) {
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(js, n, 16U, pair_range, &t, NPH_JOB_PRIORITY_HIGH, &done) ==
            NPH_RESULT_OK) {
            nph_job_wait(js, &done);
            return;
        }
        nph_job_wait(js, &done);
    }
    pair_range(&t, 0U, n);
}

NphResult phys_update_pairs(NphPhysicsWorld *w, NphJobSystem *js)
{
    // Tighten the subtrees that grew last step (ADR-0042 §10).
    for (u32 t = PHYS_TREE_KINEMATIC; t < PHYS_TREE_COUNT; ++t) {
        u32 placed = 0U;
        NPH_TRY(nph_aabb_tree_rebuild(&w->trees[t], false, &placed));
    }
    const u32 n = w->move_buffer.len;
    w->stats.moved_proxies = n;
    w->stats.new_pairs = 0U;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    // Count, place by prefix sums, fill: each moved proxy writes its own
    // range, so the pairs come out in the move buffer's order whatever the
    // threads do.
    NPH_TRY(NPH_ARRAY_RESIZE(&w->pair_counts, n));
    run_pairs(w, js, false);
    u32 total = 0U;
    for (u32 i = 0U; i < n; ++i) {
        total += w->pair_counts.data[i];
        w->pair_counts.data[i] = total;
    }
    NPH_TRY(NPH_ARRAY_RESIZE(&w->pair_buffer, total));
    if (total > 0U) {
        run_pairs(w, js, true);
    }
    for (u32 i = 0U; i < n; ++i) {
        const u32 key = w->move_buffer.data[i];
        const u32 s = (u32)nph_aabb_tree_proxy_user_data(&w->trees[key & 3U], key >> 2U);
        w->shapes.data[s].flags &= ~(u32)PHYS_SHAPE_IN_MOVE_BUFFER;
    }
    NPH_ARRAY_CLEAR(&w->move_buffer);
    for (u32 i = 0U; i < total; ++i) {
        const PhysPair p = w->pair_buffer.data[i];
        // Each pair is reported once (the lower key of two moved proxies),
        // so this only guards the invariant.
        if (!nph_hashmap_contains_u64(&w->pairs, pair_key(p.shape_a, p.shape_b))) {
            NPH_TRY(phys_contact_create(w, p.shape_a, p.shape_b));
            w->stats.new_pairs += 1U;
        }
    }
    return NPH_RESULT_OK;
}
