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

static bool is_mesh(const PhysShape *s)
{
    return s->geometry.type == NPH_PHYSICS_SHAPE_MESH;
}

// A free contact record, uninitialised: from the free list, or a new one
// with room kept to free every record later without growing.
static NphResult contact_alloc(NphPhysicsWorld *w, u32 *out)
{
    if (w->free_contacts.len > 0U) {
        *out = w->free_contacts.data[w->free_contacts.len - 1U];
        w->free_contacts.len -= 1U;
        return NPH_RESULT_OK;
    }
    if (NPH_ARRAY_PUSH_SLOT(&w->contacts) == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const NphResult r = NPH_ARRAY_RESERVE(&w->free_contacts, w->contacts.len);
    if (r != NPH_RESULT_OK) {
        w->contacts.len -= 1U;
        return r;
    }
    *out = w->contacts.len - 1U;
    return NPH_RESULT_OK;
}

static void contact_init(NphPhysicsWorld *w, u32 index, u32 shape_a, u32 shape_b)
{
    PhysContact *c = &w->contacts.data[index];
    memset(c, 0, sizeof *c);
    c->shape_a = shape_a;
    c->shape_b = shape_b;
    c->body_a = w->shapes.data[shape_a].body;
    c->body_b = w->shapes.data[shape_b].body;
    c->parent = PHYS_NULL;
    c->child = PHYS_NULL;
    c->color = PHYS_NULL;
    c->color_slot = PHYS_NULL;
    c->island = PHYS_NULL;
    c->island_prev = PHYS_NULL;
    c->island_next = PHYS_NULL;
    c->awake_slot = PHYS_NULL;
    c->normal = nph_vec3_unit_y();
}

NphResult phys_contact_create(NphPhysicsWorld *w, u32 shape_a, u32 shape_b)
{
    const PhysShape *sa = &w->shapes.data[shape_a];
    const PhysShape *sb = &w->shapes.data[shape_b];
    // Every record first (a mesh pair's children with its own), so a
    // failure leaves nothing behind.
    const u32 records = is_mesh(sa) || is_mesh(sb) ? NPH_PHYSICS_MESH_MAX_MANIFOLDS : 1U;
    u32 rec[NPH_PHYSICS_MESH_MAX_MANIFOLDS] = {PHYS_NULL, PHYS_NULL, PHYS_NULL, PHYS_NULL};
    NphResult r = NPH_RESULT_OK;
    u32 made = 0U;
    while (made < records && r == NPH_RESULT_OK) {
        r = contact_alloc(w, &rec[made]);
        made += r == NPH_RESULT_OK ? 1U : 0U;
    }
    const u32 index = rec[0];
    if (r == NPH_RESULT_OK) {
        r = nph_hashmap_insert_u64(&w->pairs, pair_key(shape_a, shape_b), &index);
    }
    if (r == NPH_RESULT_OK && w->awake_contacts.cap < w->contacts.len) {
        r = NPH_ARRAY_RESERVE(&w->awake_contacts, w->contacts.len);
        if (r != NPH_RESULT_OK) {
            NPH_VERIFY(nph_hashmap_remove_u64(&w->pairs, pair_key(shape_a, shape_b)));
        }
    }
    if (r != NPH_RESULT_OK) {
        for (u32 k = 0U; k < made; ++k) {
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->free_contacts, rec[k])); // reserved
        }
        return r;
    }
    for (u32 k = 0U; k < records; ++k) {
        contact_init(w, rec[k], shape_a, shape_b);
        w->contacts.data[rec[k]].parent = k == 0U ? PHYS_NULL : index;
        w->contacts.data[rec[k]].child = k + 1U < records ? rec[k + 1U] : PHYS_NULL;
    }
    w->child_contacts += records - 1U;
    PhysContact *c = &w->contacts.data[index];
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
    NPH_ASSERT(c->parent == PHYS_NULL);
    // A mesh pair's children go with it (they hold no events or islands).
    for (u32 k = c->child; k != PHYS_NULL;) {
        PhysContact *child = &w->contacts.data[k];
        const u32 next = child->child;
        if (child->color != PHYS_NULL) {
            phys_graph_remove(w, k);
        }
        child->shape_a = PHYS_NULL;
        child->shape_b = PHYS_NULL;
        child->flags = 0U;
        child->parent = PHYS_NULL;
        child->child = PHYS_NULL;
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->free_contacts, k)); // reserved
        w->child_contacts -= 1U;
        k = next;
    }
    c->child = PHYS_NULL;
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
    const PhysContact *c = &w->contacts.data[contact];
    const PhysBody *a = &w->bodies.data[c->body_a];
    const PhysBody *b = &w->bodies.data[c->body_b];
    const bool awake = body_awake(a) || body_awake(b);
    if (awake) {
        NPH_TRY(awake_list_add(w, contact));
    } else {
        awake_list_remove(w, contact);
    }
    const bool solvable = awake && body_solvable(a) && body_solvable(b);
    // The pair's contact, then its children: in the graph while they touch
    // and their bodies are solved.
    for (u32 k = contact; k != PHYS_NULL; k = w->contacts.data[k].child) {
        const PhysContact *r = &w->contacts.data[k];
        const bool in_graph = solvable && (r->flags & PHYS_CONTACT_TOUCHING) != 0U;
        if (in_graph && r->color == PHYS_NULL) {
            NPH_TRY(phys_graph_add(w, k));
        } else if (!in_graph && r->color != PHYS_NULL) {
            phys_graph_remove(w, k);
        }
    }
    return NPH_RESULT_OK;
}

NphResult phys_body_constraints_sync(NphPhysicsWorld *w, u32 body)
{
    for (u32 key = w->bodies.data[body].contact_head; key != PHYS_NULL;) {
        const u32 c = key >> 1U;
        const u32 next = w->contacts.data[c].next[key & 1U];
        NPH_TRY(contact_sync(w, c));
        key = next;
    }
    for (u32 key = w->bodies.data[body].joint_head; key != PHYS_NULL;) {
        const u32 j = key >> 1U;
        const u32 next = w->joints.data[j].next[key & 1U];
        NPH_TRY(phys_joint_sync(w, j));
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

// ---------------------------------------------------------------------------
// Warm starting
// ---------------------------------------------------------------------------

// A point whose features changed takes the impulses of an old point this
// near on both bodies, as Jolt carries a cached point's impulses to any new
// point within a centimetre (Rouwe, Jolt Physics, PhysicsSettings::
// mContactPointPreserveLambdaMaxDistSq). Ids alone are not enough: where a
// reduction chooses between two near-coincident points (the clipped corners
// of a box turned a little on another), the choice and so the ids flip from
// step to step, and a column of boxes settling lost a twentieth of its warm
// starts to flips between points under a millimetre apart, and drifted
// millimetres aside.
#define WARM_DISTANCE (2.0F * NPH_PHYSICS_LINEAR_SLOP)
// Of a mesh pair's several manifolds, only an old point whose manifold faced
// within 5 degrees of the new one's (the merge angle: manifolds nearer than
// that are one) may pass its impulses on. A box in a crease has a corner in
// each slope's manifold, under a millimetre apart, whose impulses push along
// normals a right angle apart; on a terrain, impulses carried between
// neighbouring manifolds 5 to 15 degrees apart pushed boxes sideways and
// kept them creeping for seconds. A convex pair has one manifold, whose
// points carry their impulses however its normal turns, as ids do (limbs
// tumbling through a pile turn theirs that far in a step).
#define WARM_COS 0.99619470F

// Last step's points of one contact (or of a mesh pair's chain), their
// manifolds' normals (NULL for one manifold), and which of them have passed
// their impulses on.
typedef struct WarmStart {
    const PhysContactPoint *old;
    const NphVec3 *normals; // per old point, or NULL
    u32 count;
    u32 claimed;
} WarmStart;

// The most points a contact's chain holds.
#define CHAIN_POINTS (NPH_PHYSICS_MESH_MAX_MANIFOLDS * NPH_PHYSICS_MANIFOLD_MAX_POINTS)

NPH_STATIC_ASSERT(CHAIN_POINTS < 32U, "a chain's points fit the claimed mask");

static void warm_take(WarmStart *ws, u32 o, PhysContactPoint *p)
{
    p->normal_impulse = ws->old[o].normal_impulse;
    p->tangent_impulse = ws->old[o].tangent_impulse;
    ws->claimed |= 1U << o;
}

// The old point with p's id (the same features), if any.
static bool warm_by_id(WarmStart *ws, PhysContactPoint *p)
{
    for (u32 o = 0U; o < ws->count; ++o) {
        if (ws->old[o].id == p->id) {
            warm_take(ws, o, p);
            return true;
        }
    }
    return false;
}

// For a point no id matched, after every point has tried its id: the
// nearest unclaimed old point within WARM_DISTANCE on both bodies (and, in
// a mesh pair, whose manifold's normal agrees with `normal`). Anchors are in
// world axes, so a body's turn in the step counts as distance.
static void warm_by_place(WarmStart *ws, PhysContactPoint *p, NphVec3 normal)
{
    u32 best = NPH_U32_MAX;
    f32 best_d = WARM_DISTANCE * WARM_DISTANCE;
    for (u32 o = 0U; o < ws->count; ++o) {
        if ((ws->claimed & (1U << o)) != 0U ||
            (ws->normals != NULL && nph_vec3_dot(ws->normals[o], normal) < WARM_COS)) {
            continue;
        }
        const f32 d = nph_f32_max(nph_vec3_distance_sq(ws->old[o].anchor_a, p->anchor_a),
                                  nph_vec3_distance_sq(ws->old[o].anchor_b, p->anchor_b));
        if (d < best_d) {
            best_d = d;
            best = o;
        }
    }
    if (best != NPH_U32_MAX) {
        warm_take(ws, best, p);
    }
}

// A pair with a mesh (ADR-0046 §8, §9): up to four manifolds, the deepest
// into the pair's contact and the rest into its children. A manifold may
// move from one record to another between steps, so impulses carry over
// across the whole chain.
static void mesh_update(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const PhysShape *sa = &w->shapes.data[c->shape_a];
    const PhysShape *sb = &w->shapes.data[c->shape_b];
    const bool mesh_a = is_mesh(sa);
    const PhysShape *sm = mesh_a ? sa : sb;
    const PhysShape *sx = mesh_a ? sb : sa;
    const PhysBody *bm = &w->bodies.data[sm->body];
    const PhysBody *bx = &w->bodies.data[sx->body];
    // The convex shape's place in the mesh's frame, in 64 bits from the
    // bodies' 64-bit origins: exact however far the shape is from the
    // mesh's origin.
    const NphQuat qm = nph_quat_mul(bm->rotation, sm->pose.rotation);
    const NphQuat qx = nph_quat_mul(bx->rotation, sx->pose.rotation);
    const NphDVec3 pm =
        nph_dvec3_add_vec3(bm->origin, nph_quat_rotate(bm->rotation, sm->pose.position));
    const NphDVec3 px =
        nph_dvec3_add_vec3(bx->origin, nph_quat_rotate(bx->rotation, sx->pose.position));
    NphPhysicsMeshManifolds mm;
    nph_physics_collide_mesh_local(&sx->geometry, nph_quat_mul(nph_quat_conjugate(qm), qx),
                                   nph_physics_drotate_inverse(qm, nph_dvec3_sub(px, pm)),
                                   sm->geometry.mesh, NPH_PHYSICS_SPECULATIVE_DISTANCE, &mm);
    // The points come out relative to the convex shape in the mesh's axes;
    // the anchors are from each body's centre of mass, in world axes.
    const NphVec3 shape_from_x = nph_dvec3_relative(px, bx->center);
    const NphVec3 x_from_m = nph_dvec3_relative(bx->center, bm->center);
    // The chain's points of the last step, for warm starting.
    PhysContactPoint old[CHAIN_POINTS];
    NphVec3 old_normals[CHAIN_POINTS];
    u32 old_count = 0U;
    for (u32 k = contact; k != PHYS_NULL; k = w->contacts.data[k].child) {
        const PhysContact *r = &w->contacts.data[k];
        for (u32 j = 0U; j < r->point_count; ++j) {
            old_normals[old_count] = r->normal;
            old[old_count++] = r->points[j];
        }
    }
    WarmStart ws = {old, old_normals, old_count, 0U};
    u32 unmatched = 0U; // bit 4i + j: point j of manifold i
    const f32 friction = nph_f32_sqrt(sa->friction * sb->friction);
    const f32 restitution = nph_f32_max(sa->restitution, sb->restitution);
    bool children_changed = false;
    u32 rec = contact;
    for (u32 i = 0U; rec != PHYS_NULL; ++i) {
        PhysContact *r = &w->contacts.data[rec];
        const bool was = (r->flags & PHYS_CONTACT_TOUCHING) != 0U;
        r->point_count = 0U;
        if (i < mm.count) {
            const NphPhysicsManifold *m = &mm.manifolds[i];
            const NphVec3 n = nph_quat_rotate(qm, m->normal); // from the shape into the mesh
            r->normal = mesh_a ? nph_vec3_neg(n) : n;
            for (u32 j = 0U; j < m->point_count; ++j) {
                PhysContactPoint *p = &r->points[j];
                memset(p, 0, sizeof *p);
                const NphVec3 ax =
                    nph_vec3_add(shape_from_x, nph_quat_rotate(qm, m->points[j].point));
                const NphVec3 am = nph_vec3_add(ax, x_from_m);
                p->anchor_a = mesh_a ? am : ax;
                p->anchor_b = mesh_a ? ax : am;
                p->separation = m->points[j].separation;
                p->id = m->points[j].id;
                if (!warm_by_id(&ws, p)) {
                    unmatched |= 1U << (i * NPH_PHYSICS_MANIFOLD_MAX_POINTS + j);
                }
            }
            r->point_count = m->point_count;
        }
        r->friction = friction;
        r->restitution = restitution;
        const bool touching = r->point_count > 0U;
        r->change = 0U;
        if (touching && !was) {
            r->change = PHYS_CHANGE_STARTED;
        } else if (!touching && was) {
            r->change = PHYS_CHANGE_STOPPED;
        }
        children_changed = children_changed || (i > 0U && r->change != 0U);
        rec = r->child;
    }
    if (unmatched != 0U && ws.claimed != (1U << old_count) - 1U) {
        rec = contact;
        for (u32 i = 0U; rec != PHYS_NULL && i < mm.count; ++i) {
            PhysContact *r = &w->contacts.data[rec];
            for (u32 j = 0U; j < r->point_count; ++j) {
                if ((unmatched & (1U << (i * NPH_PHYSICS_MANIFOLD_MAX_POINTS + j))) != 0U) {
                    warm_by_place(&ws, &r->points[j], r->normal);
                }
            }
            rec = r->child;
        }
    }
    if (children_changed) {
        c->change |= PHYS_CHANGE_CHILDREN;
    }
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
    if (c->child != PHYS_NULL) {
        mesh_update(w, contact);
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
    // Carry impulses to the points that persist, for warm starting: by id,
    // then by place. The tangent impulse is a world vector: it survives a
    // turn of the tangent basis.
    PhysContactPoint points[NPH_PHYSICS_MANIFOLD_MAX_POINTS];
    WarmStart ws = {c->points, NULL, c->point_count, 0U};
    u32 unmatched = 0U;
    for (u32 i = 0U; i < m.point_count; ++i) {
        PhysContactPoint *p = &points[i];
        memset(p, 0, sizeof *p);
        p->anchor_a = m.points[i].point;
        p->anchor_b = nph_vec3_sub(m.points[i].point, ab);
        p->separation = m.points[i].separation;
        p->id = m.points[i].id;
        if (!warm_by_id(&ws, p)) {
            unmatched |= 1U << i;
        }
    }
    if (unmatched != 0U && ws.claimed != (1U << c->point_count) - 1U) {
        for (u32 i = 0U; i < m.point_count; ++i) {
            if ((unmatched & (1U << i)) != 0U) {
                warm_by_place(&ws, &points[i], m.normal);
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
    // A mesh pair's children touch as their manifolds say; their graph
    // membership follows in contact_sync. (The pair's own contact holds the
    // deepest manifold, so it touches whenever any of them does: its events
    // are the pair's.)
    for (u32 k = c->child; k != PHYS_NULL; k = w->contacts.data[k].child) {
        PhysContact *child = &w->contacts.data[k];
        if ((child->change & PHYS_CHANGE_STARTED) != 0U) {
            child->flags |= PHYS_CONTACT_TOUCHING;
        } else if ((child->change & PHYS_CHANGE_STOPPED) != 0U) {
            child->flags &= ~(u32)PHYS_CONTACT_TOUCHING;
        }
        child->change = 0U;
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
    if ((change & PHYS_CHANGE_CHILDREN) != 0U) {
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
    if ((bs->joint_count != 0U && bo->joint_count != 0U) &&
        !phys_joints_allow_contact(w, s->body, o->body)) {
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
