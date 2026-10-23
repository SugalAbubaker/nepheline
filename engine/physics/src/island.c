// SPDX-License-Identifier: Apache-2.0
// The constraint graph's colours, islands, waking, and sleep (docs/physics.md
// "Islands and sleep"; Catto, "Simulation Islands", 2023).
//
// Islands are persistent. A contact that begins touching between two
// dynamic bodies links their islands (a union recorded as a parent, merged
// at the end of the step); one that stops touching only counts a possible
// split. An island is split when it is about to fall asleep, so the work of
// finding components is spent only where it buys sleep, and an island that
// is too big while awake costs nothing but a later sleep.

#include <nepheline/core/assert.h>

#include "world_internal.h"

#include <string.h>

enum {
    PHYS_BODY_VISITED = 1U << 8U,
    PHYS_CONTACT_VISITED = 1U << 8U,
};

static bool body_awake(const PhysBody *b)
{
    return (b->flags & PHYS_BODY_AWAKE) != 0U;
}

// ---------------------------------------------------------------------------
// The constraint graph
// ---------------------------------------------------------------------------

NphResult phys_graph_add(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    NPH_ASSERT(c->color == PHYS_NULL);
    const bool dyn_a = w->bodies.data[c->body_a].type == NPH_PHYSICS_BODY_DYNAMIC;
    const bool dyn_b = w->bodies.data[c->body_b].type == NPH_PHYSICS_BODY_DYNAMIC;
    u32 color = PHYS_GRAPH_OVERFLOW;
    if (dyn_a && dyn_b) {
        for (u32 k = 0U; k < PHYS_GRAPH_DYNAMIC_COLORS; ++k) {
            NphBitset *bits = &w->colors[k].bodies;
            if (!nph_bitset_test(bits, c->body_a) && !nph_bitset_test(bits, c->body_b)) {
                color = k;
                break;
            }
        }
    } else {
        // One dynamic body: colour 0 stays for pairs, which are the harder
        // to place (Box2D's graph).
        const u32 body = dyn_a ? c->body_a : c->body_b;
        for (u32 k = 1U; k < PHYS_GRAPH_COLORS; ++k) {
            if (!nph_bitset_test(&w->colors[k].bodies, body)) {
                color = k;
                break;
            }
        }
    }
    NphArrayU32 *list = color == PHYS_GRAPH_OVERFLOW ? &w->overflow : &w->colors[color].contacts;
    NPH_TRY(NPH_ARRAY_PUSH(list, contact));
    if (color != PHYS_GRAPH_OVERFLOW) {
        if (dyn_a) {
            nph_bitset_set(&w->colors[color].bodies, c->body_a);
        }
        if (dyn_b) {
            nph_bitset_set(&w->colors[color].bodies, c->body_b);
        }
    }
    c->color = color;
    c->color_slot = list->len - 1U;
    return NPH_RESULT_OK;
}

void phys_graph_remove(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const u32 color = c->color;
    NPH_ASSERT(color != PHYS_NULL);
    NphArrayU32 *list = color == PHYS_GRAPH_OVERFLOW ? &w->overflow : &w->colors[color].contacts;
    const u32 slot = c->color_slot;
    const u32 last = list->data[list->len - 1U];
    list->data[slot] = last;
    w->contacts.data[last].color_slot = slot;
    list->len -= 1U;
    if (color != PHYS_GRAPH_OVERFLOW) {
        if (w->bodies.data[c->body_a].type == NPH_PHYSICS_BODY_DYNAMIC) {
            nph_bitset_clear(&w->colors[color].bodies, c->body_a);
        }
        if (w->bodies.data[c->body_b].type == NPH_PHYSICS_BODY_DYNAMIC) {
            nph_bitset_clear(&w->colors[color].bodies, c->body_b);
        }
    }
    c->color = PHYS_NULL;
    c->color_slot = PHYS_NULL;
}

// ---------------------------------------------------------------------------
// The awake lists
// ---------------------------------------------------------------------------

static void awake_islands_remove(NphPhysicsWorld *w, u32 island)
{
    PhysIsland *is = &w->islands.data[island];
    const u32 slot = is->awake_slot;
    if (slot == PHYS_NULL) {
        return;
    }
    const u32 last = w->awake_islands.data[w->awake_islands.len - 1U];
    w->awake_islands.data[slot] = last;
    w->islands.data[last].awake_slot = slot;
    w->awake_islands.len -= 1U;
    is->awake_slot = PHYS_NULL;
}

static NphResult awake_islands_add(NphPhysicsWorld *w, u32 island)
{
    PhysIsland *is = &w->islands.data[island];
    if (is->awake_slot != PHYS_NULL) {
        return NPH_RESULT_OK;
    }
    NPH_TRY(NPH_ARRAY_PUSH(&w->awake_islands, island));
    w->islands.data[island].awake_slot = w->awake_islands.len - 1U;
    return NPH_RESULT_OK;
}

// Adds a body to or removes it from the awake list, without its contacts.
static NphResult body_list_set(NphPhysicsWorld *w, u32 body, bool awake)
{
    PhysBody *b = &w->bodies.data[body];
    if (awake) {
        b->sleep_time = 0.0F;
        if (b->awake_slot != PHYS_NULL) {
            return NPH_RESULT_OK;
        }
        NPH_TRY(NPH_ARRAY_PUSH(&w->awake_bodies, body));
        b->awake_slot = w->awake_bodies.len - 1U;
        b->flags |= PHYS_BODY_AWAKE;
        return NPH_RESULT_OK;
    }
    if (b->awake_slot == PHYS_NULL) {
        return NPH_RESULT_OK;
    }
    const u32 slot = b->awake_slot;
    const u32 last = w->awake_bodies.data[w->awake_bodies.len - 1U];
    w->awake_bodies.data[slot] = last;
    w->bodies.data[last].awake_slot = slot;
    w->awake_bodies.len -= 1U;
    b->awake_slot = PHYS_NULL;
    b->flags &= ~(u32)PHYS_BODY_AWAKE;
    // Asleep is at rest: what little motion is left is dropped, so the body
    // wakes exactly where and how it fell asleep.
    b->linear_velocity = nph_vec3_zero();
    b->angular_velocity = nph_vec3_zero();
    b->force = nph_vec3_zero();
    b->torque = nph_vec3_zero();
    return NPH_RESULT_OK;
}

NphResult phys_body_set_awake_list(NphPhysicsWorld *w, u32 body, bool awake)
{
    NPH_TRY(body_list_set(w, body, awake));
    return phys_body_contacts_sync(w, body);
}

// ---------------------------------------------------------------------------
// Islands
// ---------------------------------------------------------------------------

static NphResult island_alloc(NphPhysicsWorld *w, bool awake, u32 *out)
{
    u32 index;
    if (w->free_islands.len > 0U) {
        index = w->free_islands.data[w->free_islands.len - 1U];
        w->free_islands.len -= 1U;
    } else {
        if (NPH_ARRAY_PUSH_SLOT(&w->islands) == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        index = w->islands.len - 1U;
        const NphResult r = NPH_ARRAY_RESERVE(&w->free_islands, w->islands.len);
        if (r != NPH_RESULT_OK) {
            w->islands.len -= 1U;
            return r;
        }
    }
    PhysIsland *is = &w->islands.data[index];
    memset(is, 0, sizeof *is);
    is->live = 1U;
    is->body_head = PHYS_NULL;
    is->body_tail = PHYS_NULL;
    is->contact_head = PHYS_NULL;
    is->contact_tail = PHYS_NULL;
    is->parent = PHYS_NULL;
    is->awake_slot = PHYS_NULL;
    if (awake) {
        is->awake = 1U;
        const NphResult r = awake_islands_add(w, index);
        if (r != NPH_RESULT_OK) {
            is->live = 0U;
            NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->free_islands, index));
            return r;
        }
    }
    *out = index;
    return NPH_RESULT_OK;
}

static void island_free(NphPhysicsWorld *w, u32 island)
{
    awake_islands_remove(w, island);
    w->islands.data[island].live = 0U;
    // Reserved when the island was made: cannot fail.
    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->free_islands, island));
}

static void island_append_body(NphPhysicsWorld *w, u32 island, u32 body)
{
    PhysIsland *is = &w->islands.data[island];
    PhysBody *b = &w->bodies.data[body];
    b->island = island;
    b->island_prev = is->body_tail;
    b->island_next = PHYS_NULL;
    if (is->body_tail != PHYS_NULL) {
        w->bodies.data[is->body_tail].island_next = body;
    } else {
        is->body_head = body;
    }
    is->body_tail = body;
    is->body_count += 1U;
}

static void island_append_contact(NphPhysicsWorld *w, u32 island, u32 contact)
{
    PhysIsland *is = &w->islands.data[island];
    PhysContact *c = &w->contacts.data[contact];
    c->island = island;
    c->island_prev = is->contact_tail;
    c->island_next = PHYS_NULL;
    if (is->contact_tail != PHYS_NULL) {
        w->contacts.data[is->contact_tail].island_next = contact;
    } else {
        is->contact_head = contact;
    }
    is->contact_tail = contact;
    is->contact_count += 1U;
}

NphResult phys_island_add_body(NphPhysicsWorld *w, u32 body)
{
    u32 island = PHYS_NULL;
    // Every new island starts awake; a body created asleep puts its island
    // to sleep through the awake list (body.c).
    NPH_TRY(island_alloc(w, true, &island));
    island_append_body(w, island, body);
    return NPH_RESULT_OK;
}

void phys_island_remove_body(NphPhysicsWorld *w, u32 body)
{
    PhysBody *b = &w->bodies.data[body];
    const u32 island = b->island;
    PhysIsland *is = &w->islands.data[island];
    if (b->island_prev != PHYS_NULL) {
        w->bodies.data[b->island_prev].island_next = b->island_next;
    } else {
        is->body_head = b->island_next;
    }
    if (b->island_next != PHYS_NULL) {
        w->bodies.data[b->island_next].island_prev = b->island_prev;
    } else {
        is->body_tail = b->island_prev;
    }
    is->body_count -= 1U;
    b->island = PHYS_NULL;
    b->island_prev = PHYS_NULL;
    b->island_next = PHYS_NULL;
    if (is->body_count == 0U) {
        NPH_ASSERT(is->contact_count == 0U);
        island_free(w, island);
    } else {
        // Its contacts were destroyed first; what is left may fall apart.
        is->constraint_remove_count += 1U;
    }
}

u32 phys_island_root(NphPhysicsWorld *w, u32 island)
{
    u32 r = island;
    while (w->islands.data[r].parent != PHYS_NULL) {
        r = w->islands.data[r].parent;
    }
    // Path compression keeps later finds short.
    u32 i = island;
    while (w->islands.data[i].parent != PHYS_NULL) {
        const u32 next = w->islands.data[i].parent;
        w->islands.data[i].parent = r;
        i = next;
    }
    return r;
}

NphResult phys_island_link_contact(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const u32 ia = phys_island_root(w, w->bodies.data[c->body_a].island);
    const u32 ib = phys_island_root(w, w->bodies.data[c->body_b].island);
    NPH_ASSERT(w->islands.data[ia].awake != 0U && w->islands.data[ib].awake != 0U);
    u32 root = ia;
    if (ia != ib) {
        // The bigger island absorbs the smaller (ties: the lower index), so
        // a merge moves the fewest records.
        const PhysIsland *a = &w->islands.data[ia];
        const PhysIsland *b = &w->islands.data[ib];
        const bool a_wins =
            a->body_count > b->body_count || (a->body_count == b->body_count && ia < ib);
        root = a_wins ? ia : ib;
        w->islands.data[a_wins ? ib : ia].parent = root;
    }
    island_append_contact(w, root, contact);
    c->flags |= PHYS_CONTACT_ISLAND;
    return NPH_RESULT_OK;
}

void phys_island_unlink_contact(NphPhysicsWorld *w, u32 contact)
{
    PhysContact *c = &w->contacts.data[contact];
    const u32 island = c->island;
    PhysIsland *is = &w->islands.data[island];
    if (c->island_prev != PHYS_NULL) {
        w->contacts.data[c->island_prev].island_next = c->island_next;
    } else {
        is->contact_head = c->island_next;
    }
    if (c->island_next != PHYS_NULL) {
        w->contacts.data[c->island_next].island_prev = c->island_prev;
    } else {
        is->contact_tail = c->island_prev;
    }
    is->contact_count -= 1U;
    is->constraint_remove_count += 1U;
    c->island = PHYS_NULL;
    c->island_prev = PHYS_NULL;
    c->island_next = PHYS_NULL;
    c->flags &= ~(u32)PHYS_CONTACT_ISLAND;
}

// Appends island `from`'s bodies and contacts to `into` and frees `from`.
static void island_absorb(NphPhysicsWorld *w, u32 into, u32 from)
{
    PhysIsland *src = &w->islands.data[from];
    for (u32 b = src->body_head; b != PHYS_NULL;) {
        const u32 next = w->bodies.data[b].island_next;
        island_append_body(w, into, b);
        b = next;
    }
    for (u32 c = src->contact_head; c != PHYS_NULL;) {
        const u32 next = w->contacts.data[c].island_next;
        island_append_contact(w, into, c);
        c = next;
    }
    w->islands.data[into].constraint_remove_count += src->constraint_remove_count;
    src->body_head = PHYS_NULL;
    src->body_tail = PHYS_NULL;
    src->contact_head = PHYS_NULL;
    src->contact_tail = PHYS_NULL;
    src->body_count = 0U;
    src->contact_count = 0U;
    island_free(w, from);
}

NphResult phys_island_merge_pending(NphPhysicsWorld *w)
{
    // The children and their final roots first, then the merges: freeing
    // an island must not cut a later child's path to its root.
    NPH_ARRAY_CLEAR(&w->scratch);
    for (u32 i = 0U; i < w->awake_islands.len; ++i) {
        const u32 island = w->awake_islands.data[i];
        if (w->islands.data[island].parent != PHYS_NULL) {
            NPH_TRY(NPH_ARRAY_PUSH(&w->scratch, island));
            NPH_TRY(NPH_ARRAY_PUSH(&w->scratch, phys_island_root(w, island)));
        }
    }
    for (u32 i = 0U; i < w->scratch.len; i += 2U) {
        const u32 child = w->scratch.data[i];
        const u32 root = w->scratch.data[i + 1U];
        w->islands.data[child].parent = PHYS_NULL;
        island_absorb(w, root, child);
    }
    return NPH_RESULT_OK;
}

NphResult phys_island_wake(NphPhysicsWorld *w, u32 island)
{
    PhysIsland *is = &w->islands.data[island];
    if (is->awake != 0U) {
        // Already awake: a wake call still restarts the bodies' sleep clocks.
        for (u32 b = is->body_head; b != PHYS_NULL; b = w->bodies.data[b].island_next) {
            w->bodies.data[b].sleep_time = 0.0F;
        }
        is->sleep_time = 0.0F;
        return NPH_RESULT_OK;
    }
    is->awake = 1U;
    is->sleep_time = 0.0F;
    NPH_TRY(awake_islands_add(w, island));
    for (u32 b = w->islands.data[island].body_head; b != PHYS_NULL;
         b = w->bodies.data[b].island_next) {
        NPH_TRY(body_list_set(w, b, true));
    }
    for (u32 b = w->islands.data[island].body_head; b != PHYS_NULL;
         b = w->bodies.data[b].island_next) {
        NPH_TRY(phys_body_contacts_sync(w, b));
    }
    return NPH_RESULT_OK;
}

NphResult phys_wake_body(NphPhysicsWorld *w, u32 body)
{
    PhysBody *b = &w->bodies.data[body];
    if (b->type == NPH_PHYSICS_BODY_STATIC) {
        return NPH_RESULT_OK;
    }
    if (b->type == NPH_PHYSICS_BODY_DYNAMIC) {
        return phys_island_wake(w, phys_island_root(w, b->island));
    }
    if (body_awake(b)) {
        b->sleep_time = 0.0F;
        return NPH_RESULT_OK;
    }
    return phys_body_set_awake_list(w, body, true);
}

static NphResult island_sleep(NphPhysicsWorld *w, u32 island)
{
    PhysIsland *is = &w->islands.data[island];
    NPH_ASSERT(is->parent == PHYS_NULL);
    for (u32 b = is->body_head; b != PHYS_NULL; b = w->bodies.data[b].island_next) {
        PhysBody *body = &w->bodies.data[b];
        // Its move event of this step says so (the runtime stops mirroring).
        if (body->move_event < w->move_events.len &&
            w->move_events.data[body->move_event].body.value == body->handle) {
            w->move_events.data[body->move_event].fell_asleep = 1U;
        }
        NPH_TRY(body_list_set(w, b, false));
    }
    for (u32 b = is->body_head; b != PHYS_NULL; b = w->bodies.data[b].island_next) {
        NPH_TRY(phys_body_contacts_sync(w, b));
    }
    is->awake = 0U;
    awake_islands_remove(w, island);
    return NPH_RESULT_OK;
}

// Splits an island into its connected components (bodies joined by
// touching contacts), depth first from its bodies in list order.
static NphResult island_split(NphPhysicsWorld *w, u32 island)
{
    PhysIsland *is = &w->islands.data[island];
    NPH_ARRAY_CLEAR(&w->scratch);
    for (u32 b = is->body_head; b != PHYS_NULL; b = w->bodies.data[b].island_next) {
        NPH_TRY(NPH_ARRAY_PUSH(&w->scratch, b));
    }
    NPH_TRY(NPH_ARRAY_RESERVE(&w->scratch2, w->scratch.len));
    const u32 body_count = w->scratch.len;
    // Detach everything from the old island; the components rebuild lists.
    for (u32 c = is->contact_head; c != PHYS_NULL;) {
        const u32 next = w->contacts.data[c].island_next;
        w->contacts.data[c].island = PHYS_NULL;
        c = next;
    }
    is->body_head = PHYS_NULL;
    is->body_tail = PHYS_NULL;
    is->contact_head = PHYS_NULL;
    is->contact_tail = PHYS_NULL;
    is->body_count = 0U;
    is->contact_count = 0U;
    NphResult result = NPH_RESULT_OK;
    for (u32 i = 0U; i < body_count && result == NPH_RESULT_OK; ++i) {
        const u32 seed = w->scratch.data[i];
        if ((w->bodies.data[seed].flags & PHYS_BODY_VISITED) != 0U) {
            continue;
        }
        u32 part = PHYS_NULL;
        // The first component reuses the island's record.
        if (w->islands.data[island].body_count == 0U) {
            part = island;
        } else {
            result = island_alloc(w, true, &part);
            if (result != NPH_RESULT_OK) {
                break;
            }
        }
        NPH_ARRAY_CLEAR(&w->scratch2);
        w->bodies.data[seed].flags |= PHYS_BODY_VISITED;
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->scratch2, seed)); // reserved
        while (w->scratch2.len > 0U) {
            w->scratch2.len -= 1U;
            const u32 x = w->scratch2.data[w->scratch2.len];
            island_append_body(w, part, x);
            for (u32 key = w->bodies.data[x].contact_head; key != PHYS_NULL;) {
                PhysContact *c = &w->contacts.data[key >> 1U];
                const u32 contact = key >> 1U;
                const u32 other = (key & 1U) == 0U ? c->body_b : c->body_a;
                key = c->next[key & 1U];
                if ((c->flags & PHYS_CONTACT_ISLAND) == 0U ||
                    (c->flags & PHYS_CONTACT_VISITED) != 0U) {
                    continue;
                }
                c->flags |= PHYS_CONTACT_VISITED;
                island_append_contact(w, part, contact);
                PhysBody *ob = &w->bodies.data[other];
                if ((ob->flags & PHYS_BODY_VISITED) == 0U) {
                    ob->flags |= PHYS_BODY_VISITED;
                    NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->scratch2, other)); // reserved
                }
            }
        }
    }
    for (u32 i = 0U; i < body_count; ++i) {
        PhysBody *b = &w->bodies.data[w->scratch.data[i]];
        b->flags &= ~(u32)PHYS_BODY_VISITED;
        for (u32 key = b->contact_head; key != PHYS_NULL;) {
            PhysContact *c = &w->contacts.data[key >> 1U];
            c->flags &= ~(u32)PHYS_CONTACT_VISITED;
            key = c->next[key & 1U];
        }
    }
    w->islands.data[island].constraint_remove_count = 0U;
    return result;
}

NphResult phys_island_sleep_now(NphPhysicsWorld *w, u32 island)
{
    if (w->islands.data[island].awake == 0U) {
        return NPH_RESULT_OK;
    }
    return island_sleep(w, island);
}

NphResult phys_island_sleep_and_split(NphPhysicsWorld *w)
{
    const f32 time_to_sleep = w->desc.time_to_sleep;
    // Each awake island's sleep clock is its stillest-moving body's: one
    // body in motion keeps the island awake.
    NPH_ARRAY_CLEAR(&w->candidates);
    for (u32 i = 0U; i < w->awake_islands.len; ++i) {
        const u32 island = w->awake_islands.data[i];
        PhysIsland *is = &w->islands.data[island];
        f32 t = 1e30F;
        for (u32 b = is->body_head; b != PHYS_NULL; b = w->bodies.data[b].island_next) {
            const PhysBody *body = &w->bodies.data[b];
            const f32 bt = (body->flags & PHYS_BODY_ENABLE_SLEEP) != 0U ? body->sleep_time : 0.0F;
            t = nph_f32_min(t, bt);
        }
        is->sleep_time = t;
        if (t >= time_to_sleep) {
            NPH_TRY(NPH_ARRAY_PUSH(&w->candidates, island));
        }
    }
    // Candidates in the awake list's order: islands that may have fallen
    // apart split first (one a step, the work is proportional to its size)
    // and are reconsidered next step; whole ones fall asleep.
    bool split_done = false;
    for (u32 i = 0U; i < w->candidates.len; ++i) {
        const u32 island = w->candidates.data[i];
        const PhysIsland *is = &w->islands.data[island];
        if (is->live == 0U || is->awake == 0U) {
            continue;
        }
        if (is->constraint_remove_count > 0U) {
            if (!split_done) {
                split_done = true;
                NPH_TRY(island_split(w, island));
            }
            continue;
        }
        NPH_TRY(island_sleep(w, island));
    }
    // Kinematic bodies sleep on their own clocks.
    for (u32 i = 0U; i < w->awake_bodies.len;) {
        const u32 b = w->awake_bodies.data[i];
        const PhysBody *body = &w->bodies.data[b];
        if (body->type == NPH_PHYSICS_BODY_KINEMATIC &&
            (body->flags & PHYS_BODY_ENABLE_SLEEP) != 0U && body->sleep_time >= time_to_sleep) {
            NPH_TRY(phys_body_set_awake_list(w, b, false));
            continue; // the slot now holds another body
        }
        i += 1U;
    }
    return NPH_RESULT_OK;
}
