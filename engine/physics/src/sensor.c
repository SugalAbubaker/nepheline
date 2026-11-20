// SPDX-License-Identifier: Apache-2.0
// Sensors (shape.h `sensor`; docs/physics.md "Sensors and ghosts"; ADR-0108): after each step's
// solve, each sensor finds the shapes it overlaps by the world's overlap query, one way (the
// visitor's category against the sensor's mask), and the step reports which began and which
// ended. Box2D 3.1 finds its sensors' overlaps the same way, at the end of the step, not as
// contacts (Catto, 2025).
#include <nepheline/jobs/jobs.h>
#include <nepheline/physics/query.h>

#include "world_internal.h"

NphResult phys_sensor_reserve(NphPhysicsWorld *w)
{
    return NPH_ARRAY_RESERVE(&w->sensors, w->sensors.len + 1U);
}

// The sensor of the shape in `shape_slot`: sensors are sorted by their shapes' slots.
static u32 sensor_lower_bound(const NphPhysicsWorld *w, u32 shape_slot)
{
    u32 lo = 0U;
    u32 hi = w->sensors.len;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2U;
        if (w->sensors.data[mid].shape < shape_slot) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void phys_sensor_add(NphPhysicsWorld *w, u32 shape_slot)
{
    // Kept sorted by slot: the step's events come in that order.
    const u32 at = sensor_lower_bound(w, shape_slot);
    // A new sensor's room in the step's buffer grows to the power of two that holds what it
    // finds, and never shrinks, so a sensor's population settles into it.
    const PhysSensor s = {shape_slot, 0U, 0U, PHYS_SENSOR_FIRST_ROOM};
    NPH_ASSERT(w->sensors.len < w->sensors.cap); // reserved
    NPH_IGNORE_RESULT(NPH_ARRAY_INSERT(&w->sensors, at, s));
}

static NphPhysicsSensorEvent sensor_event(const PhysShape *sensor, const PhysSensorVisitor *v)
{
    NphPhysicsSensorEvent e;
    e.sensor.value = sensor->handle;
    e.visitor.value = v->handle;
    e.sensor_user_data = sensor->user_data;
    e.visitor_user_data = v->user_data;
    return e;
}

void phys_sensor_shape_gone(NphPhysicsWorld *w, u32 shape_slot)
{
    const PhysShape *s = &w->shapes.data[shape_slot];
    if ((s->flags & PHYS_SHAPE_SENSOR) == 0U) {
        // A visitor: the next step's sets cannot hold it, and their walk ends it with the
        // handle and user data the last step's sets kept. Nothing to do now.
        return;
    }
    const u32 i = sensor_lower_bound(w, shape_slot);
    const bool found = i < w->sensors.len && w->sensors.data[i].shape == shape_slot;
    NPH_ASSERT(found); // registered when made: phys_sensor_add cannot fail
    if (!found) {
        return;
    }
    const PhysSensor *ps = &w->sensors.data[i];
    for (u32 k = 0U; k < ps->count; ++k) {
        const NphPhysicsSensorEvent e = sensor_event(s, &w->sensor_visitors.data[ps->first + k]);
        // Out of memory loses the event: the destroy that made it cannot fail.
        NPH_IGNORE_RESULT(NPH_ARRAY_PUSH(&w->sensor_pending_ends, e));
    }
    NPH_ARRAY_REMOVE_AT(&w->sensors, i);
}

// One sensor's query: every visitor counted, the first `room` written.
typedef struct SensorGather {
    const NphPhysicsWorld *w;
    PhysSensorVisitor *out;
    u32 room;
    u32 count;
} SensorGather;

static bool gather(void *user, NphPhysicsShape shape, NphPhysicsBody body)
{
    NPH_UNUSED(body);
    SensorGather *g = (SensorGather *)user;
    if (g->count < g->room) {
        const PhysShape *o = &g->w->shapes.data[phys_handle_slot(shape.value)];
        PhysSensorVisitor *v = &g->out[g->count];
        v->handle = o->handle;
        v->pad = 0U;
        v->user_data = o->user_data;
    }
    g->count += 1U;
    return true;
}

static void sift_down(PhysSensorVisitor *v, u32 root, u32 n)
{
    for (;;) {
        u32 child = 2U * root + 1U;
        if (child >= n) {
            return;
        }
        if (child + 1U < n && v[child + 1U].handle > v[child].handle) {
            ++child;
        }
        if (v[root].handle >= v[child].handle) {
            return;
        }
        const PhysSensorVisitor t = v[root];
        v[root] = v[child];
        v[child] = t;
        root = child;
    }
}

// Sorted by handle (a set holds each once): by insertion when few, else by a heap sort; in place
// either way, allocating nothing.
static void sort_visitors(PhysSensorVisitor *v, u32 n)
{
    if (n <= 16U) {
        for (u32 i = 1U; i < n; ++i) {
            const PhysSensorVisitor x = v[i];
            u32 j = i;
            while (j > 0U && v[j - 1U].handle > x.handle) {
                v[j] = v[j - 1U];
                --j;
            }
            v[j] = x;
        }
        return;
    }
    for (u32 i = n / 2U; i-- > 0U;) {
        sift_down(v, i, n);
    }
    for (u32 end = n - 1U; end > 0U; --end) {
        const PhysSensorVisitor t = v[0];
        v[0] = v[end];
        v[end] = t;
        sift_down(v, 0U, end);
    }
}

// What overlaps each sensor in [begin, end) now, by the world's overlap query, which reads the
// world only: each sensor writes its own room of the step's buffer and its own count, so the
// sets come out the same whatever the threads do.
static void sensor_range(void *user, u32 begin, u32 end)
{
    NphPhysicsWorld *w = (NphPhysicsWorld *)user;
    for (u32 i = begin; i < end; ++i) {
        const PhysSensor *ps = &w->sensors.data[i];
        const PhysShape *s = &w->shapes.data[ps->shape];
        const PhysBody *b = &w->bodies.data[s->body];
        // The sensor's frame in the world: its body's, then its pose in it.
        const NphDVec3 position =
            nph_dvec3_add_vec3(b->origin, nph_quat_rotate(b->rotation, s->pose.position));
        const NphQuat rotation = nph_quat_mul(b->rotation, s->pose.rotation);
        NphPhysicsQueryFilter filter = nph_physics_query_filter_default();
        filter.category = s->filter.category;
        filter.mask = s->filter.mask;
        filter.body_types = (u32)NPH_PHYSICS_QUERY_DYNAMIC | (u32)NPH_PHYSICS_QUERY_KINEMATIC;
        filter.ignore.value = b->handle;
        filter.sensors = 0;
        SensorGather g = {w, &w->sensor_next.data[w->sensor_place.data[i]], ps->room, 0U};
        phys_overlap_one_way(w, &s->geometry, position, rotation, &filter, gather, &g);
        w->sensor_found.data[i] = g.count;
        if (g.count <= ps->room) {
            sort_visitors(g.out, g.count);
        }
    }
}

static void run_sensors(NphPhysicsWorld *w, NphJobSystem *js)
{
    const u32 n = w->sensors.len;
    if (js != NULL && n >= 8U) {
        NphJobCounter done = NPH_JOB_COUNTER_INIT;
        if (nph_job_parallel_for(js, n, 2U, sensor_range, w, NPH_JOB_PRIORITY_HIGH, &done) ==
            NPH_RESULT_OK) {
            nph_job_wait(js, &done);
            return;
        }
        nph_job_wait(js, &done);
    }
    sensor_range(w, 0U, n);
}

static NphResult sensors_pass(NphPhysicsWorld *w, NphJobSystem *js, u32 *out_visitors)
{
    const u32 n = w->sensors.len;
    NPH_TRY(NPH_ARRAY_RESIZE(&w->sensor_place, n));
    NPH_TRY(NPH_ARRAY_RESIZE(&w->sensor_found, n));
    // One query a sensor. A sensor that finds more than its room gets the power of two that holds
    // them, and the pass runs again: the world is the same, so the second round fits.
    bool fits = false;
    while (!fits) {
        u64 total = 0U;
        for (u32 i = 0U; i < n; ++i) {
            w->sensor_place.data[i] = (u32)total;
            total += w->sensors.data[i].room;
        }
        if (total > (u64)NPH_U32_MAX) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        NPH_TRY(NPH_ARRAY_RESIZE(&w->sensor_next, (u32)total));
        run_sensors(w, js);
        fits = true;
        for (u32 i = 0U; i < n; ++i) {
            PhysSensor *ps = &w->sensors.data[i];
            if (w->sensor_found.data[i] > ps->room) {
                ps->room = nph_next_pow2_u32(w->sensor_found.data[i]);
                fits = false;
            }
        }
    }
    // What changed, sensor by sensor, by walking the last step's sorted set beside this one's. A
    // destroyed visitor is in the last only, so it ends here with what that set kept.
    for (u32 i = 0U; i < n; ++i) {
        const PhysSensor *ps = &w->sensors.data[i];
        const PhysShape *s = &w->shapes.data[ps->shape];
        const u32 place = w->sensor_place.data[i];
        const u32 count = w->sensor_found.data[i];
        // Entries are taken by index where the sets hold them (an empty set may have no storage).
        const u32 first = ps->first;
        const u32 last_count = ps->count;
        u32 a = 0U;
        u32 c = 0U;
        while (a < last_count && c < count) {
            const PhysSensorVisitor *old = &w->sensor_visitors.data[first + a];
            const PhysSensorVisitor *cur = &w->sensor_next.data[place + c];
            if (old->handle < cur->handle) {
                NPH_TRY(NPH_ARRAY_PUSH(&w->sensor_end_events, sensor_event(s, old)));
                ++a;
            } else if (cur->handle < old->handle) {
                NPH_TRY(NPH_ARRAY_PUSH(&w->sensor_begin_events, sensor_event(s, cur)));
                ++c;
            } else {
                ++a;
                ++c;
            }
        }
        for (; a < last_count; ++a) {
            const PhysSensorVisitor *old = &w->sensor_visitors.data[first + a];
            NPH_TRY(NPH_ARRAY_PUSH(&w->sensor_end_events, sensor_event(s, old)));
        }
        for (; c < count; ++c) {
            const PhysSensorVisitor *cur = &w->sensor_next.data[place + c];
            NPH_TRY(NPH_ARRAY_PUSH(&w->sensor_begin_events, sensor_event(s, cur)));
        }
    }
    // This step's sets become the last step's only now, so a step that fails keeps the sets of the
    // last that succeeded.
    u32 visitors = 0U;
    for (u32 i = 0U; i < n; ++i) {
        PhysSensor *ps = &w->sensors.data[i];
        ps->first = w->sensor_place.data[i];
        ps->count = w->sensor_found.data[i];
        visitors += ps->count;
    }
    const PhysSensorVisitorArray swap = w->sensor_visitors;
    w->sensor_visitors = w->sensor_next;
    w->sensor_next = swap;
    *out_visitors = visitors;
    return NPH_RESULT_OK;
}

NphResult phys_sensors_step(NphPhysicsWorld *w, NphJobSystem *js)
{
    u32 visitors = 0U;
    NPH_TRY(sensors_pass(w, js, &visitors));
    w->stats.sensors = w->sensors.len;
    w->stats.sensor_visitors = visitors;
    return NPH_RESULT_OK;
}
