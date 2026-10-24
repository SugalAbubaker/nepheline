// SPDX-License-Identifier: Apache-2.0
// The character controller (character.h; docs/physics.md "The character
// controller"; ADR-0049): a convex shape moved by world queries alone.
//
// The scheme is Box2D v3.1's mover and Unreal's walking movement in 3D:
// cast along the motion, stop where the first surface is a skin off, and
// keep the motion's part along the surfaces met (Quake's slide: one plane,
// along it; two, along their crease; three, stop); while walking, slopes
// too steep to stand on are walls, steps are climbed by casting up,
// forward, and down, and the character is snapped onto the ground below
// it; then push out of anything nearer than the skin, along the contacts'
// normals. Every cast rounds the shape by the skin
// (nph_physics_world_cast_shape_margin_closest), so each stops exactly a
// skin off, along the true closest direction, whatever the shapes, and a
// walk too slow to close the skin in a frame still meets the step it
// presses against. Walking slides only the motion's part across the up
// axis: what pulls it down (gravity) is the snap's to resolve, so a
// character standing on a walkable slope stays put instead of sliding down
// the slope, and never bobs up and down by its skin on flat ground.

#include <nepheline/core/assert.h>
#include <nepheline/physics/body.h>
#include <nepheline/physics/character.h>

// A displacement shorter than this (metres) is none.
#define MOVE_EPSILON 1e-6F
// The most contact planes a push-out keeps, its most sweeps over them, and
// the total push (metres) below which a sweep ends it.
#define PUSH_PLANES     16U
#define PUSH_ITERATIONS 20U
#define PUSH_TOLERANCE  1e-6F
// Ground is what the shape is within this many skins of, along the
// contact's normal, below it.
#define GROUND_PROBE 2.0F
// The steepest contact normal a probe for the ground finds, by its part
// along the up axis: the probe reaches the skins beyond the cast's own
// over this down (a capsule perched on an edge meets it at a slant).
#define GROUND_RISE 0.1F
// Planes a slide remembers for its creases (the newest last), the cosine
// within which a new one is an old one met again, and the most casts a
// slide makes for each of its iterations.
#define SLIDE_PLANES   3U
#define SLIDE_SAME_COS 0.98480775F // 10 degrees
#define SLIDE_CASTS    3U

NphPhysicsCharacterDesc nph_physics_character_desc_default(NphPhysicsGeometry shape)
{
    NphPhysicsCharacterDesc d;
    d.shape = shape;
    d.up = nph_vec3(0.0F, 1.0F, 0.0F);
    d.max_slope = 0.78539816F; // 45 degrees
    d.step_height = 0.3F;
    d.snap_distance = 0.3F;
    d.skin = NPH_PHYSICS_LINEAR_SLOP;
    d.iterations = 4U;
    d.filter = nph_physics_query_filter_default();
    return d;
}

static bool finite_nonneg(f32 v)
{
    return nph_f32_is_finite(v) && v >= 0.0F;
}

static bool dvec3_finite(NphDVec3 v)
{
    return nph_f64_is_finite(v.x) && nph_f64_is_finite(v.y) && nph_f64_is_finite(v.z);
}

NphResult nph_physics_character_desc_validate(const NphPhysicsCharacterDesc *desc)
{
    if (desc == NULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphPhysicsShapeType t = desc->shape.type;
    if (t != NPH_PHYSICS_SHAPE_SPHERE && t != NPH_PHYSICS_SHAPE_CAPSULE &&
        t != NPH_PHYSICS_SHAPE_BOX && t != NPH_PHYSICS_SHAPE_HULL) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!nph_physics_geometry_is_valid(&desc->shape) || !nph_vec3_is_finite(desc->up) ||
        !(nph_f32_abs(nph_vec3_length(desc->up) - 1.0F) <= 1e-4F)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!(desc->max_slope >= 0.0F && desc->max_slope < 1.5707963F) ||
        !finite_nonneg(desc->step_height) || !finite_nonneg(desc->snap_distance) ||
        !(desc->skin > 0.0F && nph_f32_is_finite(desc->skin)) || desc->iterations == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// A move's context
// ---------------------------------------------------------------------------

typedef struct Mover {
    const NphPhysicsWorld *w;
    const NphPhysicsCharacterDesc *d;
    NphQuat rotation;
    f32 cos_slope;
    NphPhysicsCharacterHit *hits;
    u32 max_hits;
    u32 hit_count;
} Mover;

// The shape rounded by the skin, cast: its hits leave it a skin off.
static bool cast(const Mover *m, NphDVec3 at, NphVec3 delta, NphPhysicsQueryHit *hit)
{
    return nph_physics_world_cast_shape_margin_closest(m->w, &m->d->shape, at, m->rotation, delta,
                                                       m->d->skin, &m->d->filter, hit);
}

static bool walkable(const Mover *m, NphVec3 normal)
{
    return nph_vec3_dot(normal, m->d->up) >= m->cos_slope;
}

// Whether a hit below the shape is ground to stand on, and its normal: the
// hit's own, or, where the shape meets an edge of what it hit, the
// surface's there. A capsule's round bottom meets a step's edge at a
// slant, steeper than the slope limit, though the step's top is level: the
// character stands on the edge (perches, as Unreal's walkers do) while it
// climbs or walks off it.
static bool stands_on(const Mover *m, const NphPhysicsQueryHit *hit, NphVec3 *normal)
{
    if (walkable(m, hit->normal)) {
        *normal = hit->normal;
        return true;
    }
    if (!(nph_vec3_dot(hit->normal, m->d->up) > 0.0F)) {
        return false; // beside it or under it
    }
    const NphVec3 surface =
        nph_physics_world_surface_normal(m->w, hit->shape, hit->point, hit->triangle, m->d->up);
    *normal = surface;
    return walkable(m, surface);
}

static NphVec3 across(NphVec3 v, NphVec3 up)
{
    return nph_vec3_sub(v, nph_vec3_scale(up, nph_vec3_dot(v, up)));
}

// How far `p` is above `at`'s lowest point, the character's feet there,
// along the up axis.
static f64 above_feet(const Mover *m, NphDVec3 at, NphDVec3 p)
{
    const NphVec3 down_local = nph_quat_rotate_inverse(m->rotation, nph_vec3_neg(m->d->up));
    const NphVec3 feet =
        nph_quat_rotate(m->rotation, nph_physics_geometry_support(&m->d->shape, down_local));
    const NphVec3 up = m->d->up;
    const NphDVec3 d = nph_dvec3_sub(p, nph_dvec3_add_vec3(at, feet));
    return d.x * (f64)up.x + d.y * (f64)up.y + d.z * (f64)up.z;
}

static void record_hit(Mover *m, const NphPhysicsQueryHit *hit, NphVec3 blocked)
{
    if (m->hit_count < m->max_hits) {
        NphPhysicsCharacterHit *h = &m->hits[m->hit_count];
        h->body = hit->body;
        h->shape = hit->shape;
        h->point = hit->point;
        h->normal = hit->normal;
        h->blocked = blocked;
        m->hit_count += 1U;
    }
}

// ---------------------------------------------------------------------------
// Pushing out
// ---------------------------------------------------------------------------

typedef struct Planes {
    NphVec3 normal[PUSH_PLANES]; // out of the surfaces
    f32 gap[PUSH_PLANES];        // the manifold's least separation
    u32 count;
} Planes;

static bool collect_plane(void *user, const NphPhysicsQueryContact *contact)
{
    Planes *p = (Planes *)user;
    const NphPhysicsManifold *m = &contact->manifold;
    if (m->point_count == 0U || p->count == PUSH_PLANES) {
        return p->count < PUSH_PLANES;
    }
    f32 gap = m->points[0].separation;
    for (u32 i = 1U; i < m->point_count; ++i) {
        gap = nph_f32_min(gap, m->points[i].separation);
    }
    p->normal[p->count] = nph_vec3_neg(m->normal); // the manifold's runs from the character
    p->gap[p->count] = gap;
    p->count += 1U;
    return true;
}

// `at` moved out of what the shape overlaps or is nearer than the skin to:
// each contact a half-space n . delta >= skin - gap, solved as Box2D's
// b2SolvePlanes does, by sweeps over each plane's push, which stays at
// least nothing but may shrink: a push that a later plane's made too large
// is taken back. (Pushes only ever added, in a corner narrower than a right
// angle, left the character within the skin of both walls.)
static NphDVec3 push_out(const Mover *m, NphDVec3 at)
{
    Planes p;
    p.count = 0U;
    nph_physics_world_collide_shape(m->w, &m->d->shape, at, m->rotation, m->d->skin, &m->d->filter,
                                    collect_plane, &p);
    f32 push[PUSH_PLANES];
    for (u32 i = 0U; i < p.count; ++i) {
        push[i] = 0.0F;
    }
    NphVec3 delta = nph_vec3_zero();
    for (u32 iteration = 0U; iteration < PUSH_ITERATIONS; ++iteration) {
        f32 total = 0.0F;
        for (u32 i = 0U; i < p.count; ++i) {
            const f32 gap = p.gap[i] + nph_vec3_dot(delta, p.normal[i]);
            const f32 before = push[i];
            push[i] = nph_f32_max(before + (m->d->skin - gap), 0.0F);
            const f32 change = push[i] - before;
            delta = nph_vec3_add(delta, nph_vec3_scale(p.normal[i], change));
            total += nph_f32_abs(change);
        }
        if (total < PUSH_TOLERANCE) {
            break;
        }
    }
    // Rounding's pushes are none: a character standing a skin off a slope
    // would otherwise creep down it.
    if (!(nph_vec3_length_sq(delta) > PUSH_TOLERANCE * PUSH_TOLERANCE)) {
        return at;
    }
    return nph_dvec3_add_vec3(at, delta);
}

// ---------------------------------------------------------------------------
// Sliding
// ---------------------------------------------------------------------------

// `v` without its part into the plane of unit `n`.
static NphVec3 clip_one(NphVec3 v, NphVec3 n)
{
    const f32 into = nph_vec3_dot(v, n);
    return into < 0.0F ? nph_vec3_sub(v, nph_vec3_scale(n, into)) : v;
}

// `v` kept along the surfaces met (`planes`, the newest last): clipped by
// the newest; if that sends it into an earlier one, along the two's crease;
// if into a third as well, nothing (Quake's PM_SlideMove).
static NphVec3 clip(NphVec3 v, const NphVec3 *planes, u32 count)
{
    const NphVec3 newest = planes[count - 1U];
    NphVec3 r = clip_one(v, newest);
    for (u32 i = 0U; i + 1U < count; ++i) {
        if (!(nph_vec3_dot(r, planes[i]) < -1e-7F)) {
            continue;
        }
        const NphVec3 crease = nph_vec3_cross(planes[i], newest);
        const f32 len = nph_vec3_length(crease);
        if (len > 1e-4F) {
            const NphVec3 c = nph_vec3_scale(crease, 1.0F / len);
            r = nph_vec3_scale(c, nph_vec3_dot(v, c));
        } else {
            r = clip_one(r, planes[i]); // the same plane met again
        }
        for (u32 j = 0U; j + 1U < count; ++j) {
            if (j != i && nph_vec3_dot(r, planes[j]) < -1e-7F) {
                return nph_vec3_zero();
            }
        }
        break;
    }
    return r;
}

// The plane a surface puts in a walking slide: a slope too steep to stand
// on is a wall, its normal laid level (so the character walks along its
// foot, never up it); walls, overhangs, and walkable ground as they are.
static NphVec3 slide_plane(const Mover *m, NphVec3 normal, bool walking)
{
    const f32 rise = nph_vec3_dot(normal, m->d->up);
    if (!walking || rise <= 0.0F || rise >= m->cos_slope) {
        return normal;
    }
    const NphVec3 level = across(normal, m->d->up);
    const f32 len = nph_vec3_length(level);
    return len > 1e-6F ? nph_vec3_scale(level, 1.0F / len) : normal;
}

// From `at`, where the walk stopped a skin off a wall (`wall`, the hit)
// while walking: up by the step height (as far as a ceiling lets it),
// forward by `flat` (what is left of the motion across the up axis) to
// where the walk would have gone, and down onto ground to stand on: there,
// if it got there and went forward, and the ground is a step: no lower
// than where the wall was met (beside a wall the way down finds the floor
// the walk was already on), no higher than the step height above the
// feet, and no higher than the rise (a ceiling that let the character rise
// less than the step would have it perch on the step's edge, its round
// bottom against it).
static bool step_up(const Mover *m, NphDVec3 at, const NphPhysicsQueryHit *wall, NphVec3 flat,
                    NphDVec3 *out)
{
    const NphVec3 up = m->d->up;
    const f32 reach = nph_vec3_length(flat);
    if (!(reach > MOVE_EPSILON)) {
        return false;
    }
    NphPhysicsQueryHit hit;
    f32 rise = m->d->step_height;
    if (cast(m, at, nph_vec3_scale(up, rise), &hit)) {
        rise *= hit.fraction;
    }
    if (!(rise > MOVE_EPSILON)) {
        return false; // a ceiling
    }
    const NphDVec3 top = nph_dvec3_add_vec3(at, nph_vec3_scale(up, rise));
    f32 ahead = 1.0F;
    if (cast(m, top, flat, &hit)) {
        ahead = hit.fraction;
    }
    if (!(ahead * reach > MOVE_EPSILON)) {
        return false; // the wall goes on above the step
    }
    const NphDVec3 over = nph_dvec3_add_vec3(top, nph_vec3_scale(flat, ahead));
    // Down to where it started, and a skin more: what is found that low is
    // the floor the walk was on.
    const f32 drop = rise + m->d->skin;
    NphVec3 ground;
    if (!cast(m, over, nph_vec3_scale(up, -drop), &hit) || !stands_on(m, &hit, &ground)) {
        return false; // nothing to stand on, or too steep to
    }
    const f64 height = above_feet(m, at, hit.point);
    if (height < above_feet(m, at, wall->point) - 1e-4 ||
        height > (f64)nph_f32_min(m->d->step_height, rise)) {
        return false; // not a step: the floor beside a wall, or too high
    }
    *out = nph_dvec3_add_vec3(over, nph_vec3_scale(up, -drop * hit.fraction));
    return true;
}

// `at` slid along `remaining`; how many surfaces it met to `*met`.
// `planes[0, *count)` with `plane` added (the newest last): a plane within
// 10 degrees of one met before is that one again, so a slide along a
// curved wall's facets, or over a mesh's bump, makes no crease of them.
static void remember(NphVec3 *planes, u32 *count, NphVec3 plane)
{
    u32 same = *count;
    for (u32 i = 0U; i < *count; ++i) {
        if (nph_vec3_dot(planes[i], plane) >= SLIDE_SAME_COS) {
            same = i;
        }
    }
    if (same < *count) {
        for (u32 i = same; i + 1U < *count; ++i) {
            planes[i] = planes[i + 1U];
        }
        *count -= 1U;
    } else if (*count == SLIDE_PLANES) {
        planes[0] = planes[1];
        planes[1] = planes[2];
        *count -= 1U;
    }
    planes[(*count)++] = plane;
}

// `v` raised along `up` until it goes into none of the ground planes `n`
// (walkable: each faces up), its part across the up axis kept: walking
// keeps its pace up a slope (Unreal's walkers keep theirs), and over a
// bump in a mesh one lift clears every triangle around its peak met so far
// (clipping to one at a time pressed into the next, a cast each).
static NphVec3 lift(NphVec3 v, const NphVec3 *n, u32 count, NphVec3 up)
{
    f32 rise = 0.0F;
    for (u32 i = 0U; i < count; ++i) {
        const f32 into = -nph_vec3_dot(v, n[i]);
        if (into > 0.0F) {
            rise = nph_f32_max(rise, into / nph_vec3_dot(n[i], up));
        }
    }
    return nph_vec3_mul_add(v, up, rise);
}

static NphDVec3 slide(Mover *m, NphDVec3 at, NphVec3 remaining, bool walking)
{
    NphVec3 planes[SLIDE_PLANES]; // what the motion slides along
    u32 count = 0U;
    NphVec3 grounds[SLIDE_PLANES]; // walking: the ground met, climbed
    u32 ground_count = 0U;
    u32 iteration = 0U;
    for (u32 casts = 0U; iteration < m->d->iterations && casts < SLIDE_CASTS * m->d->iterations;
         ++casts) {
        const f32 len = nph_vec3_length(remaining);
        if (!(len > MOVE_EPSILON)) {
            break;
        }
        NphPhysicsQueryHit hit;
        if (!cast(m, at, remaining, &hit)) {
            at = nph_dvec3_add_vec3(at, remaining);
            break;
        }
        // Up to a skin off the surface.
        at = nph_dvec3_add_vec3(at, nph_vec3_scale(remaining, hit.fraction));
        const NphVec3 rest = nph_vec3_scale(remaining, 1.0F - hit.fraction);
        if (walking && walkable(m, hit.normal)) {
            // Ground rising ahead: up over it, along the walls met. A bump's
            // other faces, met where the walk stands, take no iteration.
            remember(grounds, &ground_count, hit.normal);
            NphVec3 kept = lift(rest, grounds, ground_count, m->d->up);
            if (count > 0U) {
                kept = clip(kept, planes, count);
            }
            record_hit(m, &hit, nph_vec3_sub(rest, kept));
            remaining = kept;
            iteration += hit.fraction > 0.0F ? 1U : 0U;
            continue;
        }
        // A wall while walking: a step, if there is one to climb.
        if (walking && m->d->step_height > 0.0F && nph_vec3_dot(hit.normal, m->d->up) > -1e-3F) {
            NphDVec3 stepped;
            if (step_up(m, at, &hit, rest, &stepped)) {
                at = stepped;
                break;
            }
        }
        remember(planes, &count, slide_plane(m, hit.normal, walking));
        const NphVec3 kept = clip(rest, planes, count);
        record_hit(m, &hit, nph_vec3_sub(rest, kept));
        remaining = kept;
        iteration += 1U;
    }
    return at;
}

// `*at` onto ground to stand on up to the snap distance below, a skin off
// it: true, and the ground (`*ground`, its normal `*normal`), when there.
static bool snap(const Mover *m, NphDVec3 *at, NphPhysicsQueryHit *ground, NphVec3 *normal)
{
    const NphVec3 up = m->d->up;
    const f32 reach = m->d->snap_distance;
    if (!(reach > 0.0F) || !cast(m, *at, nph_vec3_scale(up, -reach), ground) ||
        !stands_on(m, ground, normal)) {
        return false;
    }
    *at = nph_dvec3_add_vec3(*at, nph_vec3_scale(up, -reach * ground->fraction));
    return true;
}

// The character on `hit` (ground of `kind`, its normal `normal`).
static void set_ground(const Mover *m, const NphPhysicsQueryHit *hit,
                       NphPhysicsCharacterGround kind, NphVec3 normal, NphPhysicsCharacter *c)
{
    c->ground = kind;
    c->ground_normal = normal;
    c->ground_point = hit->point;
    c->ground_body = hit->body;
    c->ground_shape = hit->shape;
    const NphVec3 arm =
        nph_dvec3_relative(hit->point, nph_physics_body_center_of_mass(m->w, hit->body));
    c->ground_velocity =
        nph_vec3_add(nph_physics_body_linear_velocity(m->w, hit->body),
                     nph_vec3_cross(nph_physics_body_angular_velocity(m->w, hit->body), arm));
}

static void find_ground(const Mover *m, NphDVec3 at, NphPhysicsCharacter *c)
{
    const NphVec3 up = m->d->up;
    c->ground = NPH_PHYSICS_CHARACTER_AIRBORNE;
    c->ground_normal = up;
    c->ground_point = at;
    c->ground_body = NPH_PHYSICS_BODY_NULL;
    c->ground_shape = NPH_PHYSICS_SHAPE_NULL;
    c->ground_velocity = nph_vec3_zero();
    // Down far enough to find a slanted contact within the probe's skins
    // along its normal (the cast's own skin is the first); ground only if it
    // is that near along it.
    const f32 probe = (GROUND_PROBE - 1.0F) * m->d->skin;
    const f32 reach = probe / GROUND_RISE;
    NphPhysicsQueryHit hit;
    if (!cast(m, at, nph_vec3_scale(up, -reach), &hit)) {
        return;
    }
    const f32 rise = nph_vec3_dot(hit.normal, up);
    if (!(rise > 0.0F) || hit.fraction * reach * rise > probe) {
        return;
    }
    NphVec3 normal;
    if (stands_on(m, &hit, &normal)) {
        set_ground(m, &hit, NPH_PHYSICS_CHARACTER_WALKABLE, normal, c);
    } else {
        set_ground(m, &hit, NPH_PHYSICS_CHARACTER_STEEP, hit.normal, c);
    }
}

static void mover_begin(Mover *m, const NphPhysicsWorld *w, const NphPhysicsCharacterDesc *d,
                        NphQuat rotation, NphPhysicsCharacterHit *hits, u32 max_hits)
{
    m->w = w;
    m->d = d;
    m->rotation = rotation;
    m->cos_slope = nph_f32_cos(d->max_slope);
    m->hits = hits;
    m->max_hits = hits != NULL ? max_hits : 0U;
    m->hit_count = 0U;
}

NphResult nph_physics_character_init(const NphPhysicsWorld *world,
                                     const NphPhysicsCharacterDesc *desc, NphDVec3 position,
                                     NphQuat rotation, NphPhysicsCharacter *out)
{
    if (world == NULL || out == NULL || !dvec3_finite(position)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(nph_physics_character_desc_validate(desc));
    Mover m;
    mover_begin(&m, world, desc, nph_quat_normalize(rotation), NULL, 0U);
    out->position = position;
    out->rotation = m.rotation;
    find_ground(&m, position, out);
    return NPH_RESULT_OK;
}

NphResult nph_physics_character_move(const NphPhysicsWorld *world,
                                     const NphPhysicsCharacterDesc *desc,
                                     NphPhysicsCharacter *character, NphVec3 displacement,
                                     NphPhysicsCharacterHit *hits, u32 max_hits, u32 *hit_count)
{
    if (hit_count != NULL) {
        *hit_count = 0U;
    }
    if (world == NULL || character == NULL || !nph_vec3_is_finite(displacement)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(nph_physics_character_desc_validate(desc));
    Mover m;
    mover_begin(&m, world, desc, character->rotation, hits, max_hits);
    const NphVec3 up = desc->up;
    // Walking: on walkable ground and not leaving it upwards. The pull down
    // is the snap's (header comment).
    const bool walking = character->ground == NPH_PHYSICS_CHARACTER_WALKABLE &&
                         nph_vec3_dot(displacement, up) <= 0.0F;
    // Walking goes along the ground it stands on from the start (Unreal's
    // walk along the floor): up a slope, the first cast then finds no slope
    // to be lifted over.
    const NphVec3 walk =
        walking ? lift(across(displacement, up), &character->ground_normal, 1U, up) : displacement;
    NphDVec3 at = slide(&m, character->position, walk, walking);
    NphPhysicsQueryHit ground;
    NphVec3 ground_normal = up;
    bool snapped = walking && snap(&m, &at, &ground, &ground_normal);
    // Then a skin off everything (Box2D's mover's collide at the target):
    // out of what moved into the character since its last move, and out of
    // a corner's skin, where each surface the slide met left it a skin off
    // along its own normal (in a corner narrower than a right angle, within
    // the other wall's), or where it stopped short of surfaces it never
    // met (walking into a narrow corner, 12 mm a frame, put it 2 mm off
    // both walls).
    {
        const NphDVec3 before = at;
        at = push_out(&m, at);
        snapped = snapped && at.x == before.x && at.y == before.y && at.z == before.z;
    }
    character->position = at;
    // The snap's ground is where it left the character; found again only
    // when there was none, or a push moved it since.
    if (snapped) {
        set_ground(&m, &ground, NPH_PHYSICS_CHARACTER_WALKABLE, ground_normal, character);
    } else {
        find_ground(&m, at, character);
    }
    if (hit_count != NULL) {
        *hit_count = m.hit_count;
    }
    return NPH_RESULT_OK;
}

void nph_physics_character_push(NphPhysicsWorld *world, const NphPhysicsCharacterHit *hits,
                                u32 count, f32 mass, NphVec3 velocity)
{
    if (world == NULL || hits == NULL || !(mass > 0.0F) || !nph_vec3_is_finite(velocity)) {
        return;
    }
    for (u32 i = 0U; i < count; ++i) {
        const NphPhysicsCharacterHit *h = &hits[i];
        if (nph_physics_body_type(world, h->body) != NPH_PHYSICS_BODY_DYNAMIC) {
            continue;
        }
        // The two's speeds into the body at the point (the body's read again
        // for each hit: an earlier one may have moved it).
        const NphVec3 into = nph_vec3_neg(h->normal);
        const NphVec3 arm =
            nph_dvec3_relative(h->point, nph_physics_body_center_of_mass(world, h->body));
        const NphVec3 at_point =
            nph_vec3_add(nph_physics_body_linear_velocity(world, h->body),
                         nph_vec3_cross(nph_physics_body_angular_velocity(world, h->body), arm));
        const f32 closing = nph_vec3_dot(velocity, into) - nph_vec3_dot(at_point, into);
        if (!(closing > 0.0F)) {
            continue;
        }
        const f32 m = nph_physics_body_mass_data(world, h->body).mass;
        const f32 reduced = mass * m / (mass + m);
        nph_physics_body_apply_linear_impulse(world, h->body,
                                              nph_vec3_scale(into, reduced * closing), h->point);
    }
}
