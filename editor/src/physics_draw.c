// SPDX-License-Identifier: Apache-2.0
// The viewport's outlines of the physics components (docs/editor.md
// "Viewport"; ADR-0108), drawn where the engine makes their bodies, so a
// level shows what it collides with before it plays:
//   - a Collider's shapes in the frame its body takes (runtime/components.h
//     nph_body_axes_rotation: a root's rotation and signed scale, an entity
//     under a parent its world axes), mirrored where the entity mirrors; an
//     ASSET Collider's hulls and meshes from its collision asset, loaded here
//     and held, at the scale and turn the engine bakes into its copies, a
//     mesh left out where the engine refuses it (a dynamic body, a trigger);
//     a trigger's in the physics module's sensor colour;
//   - a CharacterController's upright capsule, a skin over its feet, in the
//     ghost colour (its body's capsule is a ghost);
//   - a Joint's anchor joined to its entity and to the other body (a
//     distance joint's: to the other anchor), and its axis.
// An entity with a CharacterController is a character, whose Collider makes
// no body: it is not drawn. Lines go to a sink, which the viewport points at
// the renderer's debug lines and the tests at a counter; a sink that fills
// up stops the outline (a big mesh would otherwise cost its every triangle
// a frame).
#include <nepheline/asset/collision.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/scalar.h>
#include <nepheline/physics/character.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>
#include <nepheline/physics/world.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/scene/transform.h>

#include "editor_internal.h"

#include <string.h>

#define COLOR_COLLIDER    NPH_RENDER_RGBA(90, 230, 120, 255)
#define COLOR_JOINT       NPH_RENDER_RGBA(255, 192, 0, 255) // the physics module's anchor links
#define COLOR_JOINT_AXIS  NPH_RENDER_RGBA(0, 255, 255, 255) // and its frames' X axes
#define JOINT_AXIS_LENGTH 0.3F                              // metres
#define JOINT_CROSS       0.05F

enum {
    COLLISION_LOADING = 0,
    COLLISION_READY = 1,  // held, and every block checked
    COLLISION_FAILED = 2, // not drawn (logged once)
};

struct EditorCollisionEntry {
    NphGuid guid;
    NphAssetHandle asset; // a reference held while the entry is
    u32 state;
};

// ---------------------------------------------------------------------------
// The collision assets outlined
// ---------------------------------------------------------------------------

void editor_physics_draw_init(Editor *ed)
{
    EditorCollisionCache *c = &ed->collisions;
    memset(c, 0, sizeof *c);
    NphHashMapDesc d;
    memset(&d, 0, sizeof d);
    d.key_size = (u32)sizeof(NphGuid);
    d.val_size = (u32)sizeof(u32);
    // Allocates nothing until the first insert: it cannot fail but on the sizes.
    c->indexed = nph_hashmap_init(&c->index, ed->alloc, &d) == NPH_RESULT_OK;
}

void editor_physics_draw_free(Editor *ed)
{
    EditorCollisionCache *c = &ed->collisions;
    NphAssetLoader *loader = ed->engine != NULL ? nph_engine_loader(ed->engine) : NULL;
    for (u32 i = 0U; i < c->count && loader != NULL; ++i) {
        nph_asset_release(loader, c->entries[i].asset);
    }
    if (c->entries != NULL) {
        NPH_FREE_ARRAY(ed->alloc, EditorCollisionEntry, c->entries, c->cap);
    }
    if (c->indexed) {
        nph_hashmap_free(&c->index);
    }
    memset(c, 0, sizeof *c);
}

static void log_collision(NphGuid guid, const char *what, NphResult r)
{
    char text[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(guid, text);
    NPH_LOG_WARN(&LOG_EDITOR, "collision asset %s %s: %s; its Colliders are not outlined", text,
                 what, nph_result_string(r));
}

// A loaded entry's blocks, each checked once by the physics module's
// validators (the outline reads their indices).
static NphResult check_collision(const NphAsset *asset)
{
    NphCollisionView v;
    NPH_TRY(nph_collision_view(asset, &v));
    for (u32 i = 0U; i < v.header->shape_count; ++i) {
        const NphCollisionBlock b = nph_collision_shape_block(asset, &v, i);
        const bool valid = v.shapes[i].kind == (u32)NPH_COLLISION_SHAPE_HULL
                               ? nph_physics_hull_validate(b.data, b.size, 0.0F)
                               : nph_physics_mesh_validate(b.data, b.size);
        if (!valid) {
            return NPH_RESULT_CORRUPT;
        }
    }
    return NPH_RESULT_OK;
}

// The collision asset `guid` when it is loaded and checked; its load begun
// when it is new. NULL until then, and for one that cannot be read.
static const NphAsset *collision_of(Editor *ed, NphGuid guid)
{
    EditorCollisionCache *c = &ed->collisions;
    NphAssetLoader *loader = nph_engine_loader(ed->engine);
    if (!c->indexed || loader == NULL || nph_guid_is_null(guid)) {
        return NULL;
    }
    const u32 *known = NPH_HASHMAP_FIND(&c->index, u32, &guid);
    if (known == NULL) {
        if (c->count == c->cap) {
            const u32 cap = c->cap == 0U ? 16U : c->cap * 2U;
            EditorCollisionEntry *grown = NPH_ALLOC_ARRAY(ed->alloc, EditorCollisionEntry, cap);
            if (grown == NULL) {
                return NULL;
            }
            if (c->count > 0U) {
                memcpy(grown, c->entries, sizeof *grown * c->count);
            }
            if (c->entries != NULL) {
                NPH_FREE_ARRAY(ed->alloc, EditorCollisionEntry, c->entries, c->cap);
            }
            c->entries = grown;
            c->cap = cap;
        }
        EditorCollisionEntry *e = &c->entries[c->count];
        e->guid = guid;
        e->asset = NPH_ASSET_HANDLE_NULL;
        e->state = (u32)COLLISION_LOADING;
        const NphResult r = nph_asset_load_guid_async(loader, guid, NULL, &e->asset);
        if (r != NPH_RESULT_OK) {
            e->state = (u32)COLLISION_FAILED; // kept, so that it is not asked for every frame
            e->asset = NPH_ASSET_HANDLE_NULL;
            log_collision(guid, "cannot be loaded", r);
        }
        const u32 index = c->count;
        if (nph_hashmap_insert(&c->index, &guid, &index) != NPH_RESULT_OK) {
            nph_asset_release(loader, e->asset);
            return NULL;
        }
        c->count += 1U;
        return NULL; // outlined once it has loaded
    }
    EditorCollisionEntry *e = &c->entries[*known];
    if (e->state == (u32)COLLISION_LOADING) {
        const NphAssetState st = nph_asset_state(loader, e->asset);
        if (st == NPH_ASSET_STATE_LOADING) {
            return NULL;
        }
        NphResult r = nph_asset_error(loader, e->asset);
        if (st == NPH_ASSET_STATE_READY) {
            r = check_collision(nph_asset_get(loader, e->asset));
        } else if (r == NPH_RESULT_OK) {
            r = NPH_RESULT_NOT_FOUND;
        }
        if (r != NPH_RESULT_OK) {
            nph_asset_release(loader, e->asset);
            e->asset = NPH_ASSET_HANDLE_NULL;
            e->state = (u32)COLLISION_FAILED;
            log_collision(guid, "cannot be read", r);
            return NULL;
        }
        e->state = (u32)COLLISION_READY;
    }
    return e->state == (u32)COLLISION_READY ? nph_asset_get(loader, e->asset) : NULL;
}

void editor_physics_draw_finished(Editor *ed, const EditorImportResult *res)
{
    // A collision asset cooked again is read again at its next outline: the
    // loader forgets the old one, and the entry goes.
    EditorCollisionCache *c = &ed->collisions;
    NphAssetLoader *loader = nph_engine_loader(ed->engine);
    for (u32 i = 0U; c->indexed && loader != NULL && i < res->written_count; ++i) {
        const u32 *known = NPH_HASHMAP_FIND(&c->index, u32, &res->written[i]);
        if (known == NULL) {
            continue;
        }
        const u32 index = *known;
        nph_asset_forget(loader, c->entries[index].asset);
        nph_asset_release(loader, c->entries[index].asset);
        NPH_IGNORE_RESULT(nph_hashmap_remove(&c->index, &res->written[i]));
        const u32 last = c->count - 1U;
        if (index != last) {
            c->entries[index] = c->entries[last];
            NPH_IGNORE_RESULT(nph_hashmap_set(&c->index, &c->entries[index].guid, &index));
        }
        c->count = last;
    }
}

u32 editor_physics_draw_collisions(const Editor *ed)
{
    return ed->collisions.count;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Where an entity's body goes, as the engine makes it (physics_bridge.c
// entity_pose): a root's LocalTransform turn (unit to rounding as it is, else
// normalised) and signed scale; under a parent, its world axes.
typedef struct BodyFrame {
    NphDVec3 position;
    NphQuat rotation;
    NphVec3 scale; // per local axis, negative where the entity mirrors
} BodyFrame;

static BodyFrame body_frame(const NphWorld *w, NphEntity e, const NphWorldTransform *t)
{
    BodyFrame f;
    f.position = t->translation;
    const NphLocalTransform *lt =
        NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
    const NphEntity parent = nph_world_parent(w, e);
    const bool root =
        nph_entity_is_null(parent) ||
        NPH_WORLD_GET(w, parent, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform) == NULL;
    if (root && lt != NULL) {
        const NphQuat q = lt->rotation;
        f.rotation = nph_quat_is_normalized(q, 1e-6F) ? q : nph_quat_normalize_or_identity(q);
        f.scale = lt->scale;
    } else {
        f.rotation = nph_body_axes_rotation(t->x_axis, t->y_axis);
        f.scale = nph_body_axes_scale(t->x_axis, t->y_axis, t->z_axis);
    }
    return f;
}

// A point of the entity's frame in the world, relative to `origin`.
static NphVec3 frame_point(const BodyFrame *f, NphVec3 local, NphDVec3 origin)
{
    const NphVec3 at = nph_dvec3_relative(f->position, origin);
    return nph_vec3_add(at, nph_quat_rotate(f->rotation, nph_vec3_mul(local, f->scale)));
}

static NphVec3 finite_or_zero(NphVec3 v)
{
    return nph_vec3_is_finite(v) ? v : nph_vec3_zero();
}

static f32 nonneg(f32 v)
{
    return nph_f32_is_finite(v) && v > 0.0F ? v : 0.0F;
}

// ---------------------------------------------------------------------------
// Lines
// ---------------------------------------------------------------------------

// The sink behind the physics module's line callback, which cannot say it
// is full: `full` records that it was.
typedef struct Lines {
    const EditorLineSink *sink;
    bool full;
} Lines;

static void put(Lines *l, NphVec3 a, NphVec3 b, u32 rgba)
{
    if (!l->full && !l->sink->line(l->sink->user, a, b, rgba)) {
        l->full = true;
    }
}

static void physics_line(void *user, NphVec3 a, NphVec3 b, u32 rgba)
{
    put((Lines *)user, a, b, rgba);
}

static NphPhysicsDebugDraw physics_draw_of(Lines *l)
{
    NphPhysicsDebugDraw d;
    memset(&d, 0, sizeof d);
    d.line = physics_line;
    d.user = l;
    d.origin = l->sink->origin;
    return d;
}

// ---------------------------------------------------------------------------
// Colliders
// ---------------------------------------------------------------------------

// The type the body a Collider shapes will be: its entity's RigidBody's, or
// that of the nearest one above it (a part's), else static.
static u32 body_type_of(const NphWorld *w, const NphRuntimeComponents *rc, NphEntity e)
{
    for (u32 depth = 0U; !nph_entity_is_null(e) && depth < 256U; ++depth) {
        const NphRigidBody *rb = NPH_WORLD_GET(w, e, rc->rigid_body, NphRigidBody);
        if (rb != NULL) {
            return rb->type;
        }
        e = nph_world_parent(w, e);
    }
    return (u32)NPH_RIGID_BODY_STATIC;
}

// Where an ASSET Collider puts a point of its asset's blocks: as the
// engine's copy of the block does (physics_bridge.c copy_of: the Collider's
// turn, then the entity's scale, mirrored where it mirrors), from the
// Collider's centre, in the body's frame.
typedef struct AssetPlace {
    NphVec3 at;     // the body, relative to the sink's origin
    NphQuat body;   // its turn
    NphVec3 scale;  // the entity's, signed
    NphVec3 center; // the Collider's centre, mirrored with the entity
    NphQuat turn;   // the Collider's turn, unit
} AssetPlace;

static NphVec3 asset_point(const AssetPlace *a, NphVec3 p)
{
    const NphVec3 local =
        nph_vec3_add(a->center, nph_vec3_mul(nph_quat_rotate(a->turn, p), a->scale));
    return nph_vec3_add(a->at, nph_quat_rotate(a->body, local));
}

static void draw_hull(const AssetPlace *a, const NphPhysicsHull *hull, u32 rgba, Lines *l)
{
    const NphVec3 *pts = nph_physics_hull_vertices(hull);
    const NphPhysicsHullEdge *edges = nph_physics_hull_edges(hull);
    for (u32 k = 0U; k + 1U < hull->edge_count && !l->full; k += 2U) {
        put(l, asset_point(a, pts[edges[k].origin]), asset_point(a, pts[edges[k + 1U].origin]),
            rgba);
    }
}

// Every triangle's edges, the active ones brighter: as the physics module
// draws a mesh (physics/world.h).
static void draw_mesh(const AssetPlace *a, const NphPhysicsMesh *mesh, u32 rgba, Lines *l)
{
    const u8 *active = nph_physics_mesh_edges(mesh);
    for (u32 t = 0U; t < mesh->triangle_count && !l->full; ++t) {
        NphVec3 tv[3];
        nph_physics_mesh_triangle(mesh, t, tv);
        NphVec3 w[3];
        for (u32 k = 0U; k < 3U; ++k) {
            w[k] = asset_point(a, tv[k]);
        }
        for (u32 k = 0U; k < 3U; ++k) {
            const u32 color = ((active[t] >> k) & 1U) != 0U ? rgba : (rgba & 0xFF7F7F7FU);
            put(l, w[k], w[(k + 1U) % 3U], color);
        }
    }
}

static void draw_asset(Editor *ed, const NphCollider *c, const BodyFrame *f, bool meshes, u32 rgba,
                       Lines *l)
{
    const NphAsset *asset = collision_of(ed, c->collision);
    NphCollisionView v;
    if (asset == NULL || nph_collision_view(asset, &v) != NPH_RESULT_OK) {
        return;
    }
    // The Collider's place (components.c nph_collider_shape): its centre
    // mirrored with the entity, its turn unit.
    NphCollider as_box = *c;
    as_box.shape = (u32)NPH_COLLIDER_BOX;
    as_box.half_extents = nph_vec3_one();
    NphPhysicsGeometry unused;
    NphPhysicsPose place;
    NPH_IGNORE_RESULT(nph_collider_shape(&as_box, f->scale, &unused, &place));
    AssetPlace a;
    a.at = nph_dvec3_relative(f->position, l->sink->origin);
    a.body = f->rotation;
    a.scale = f->scale;
    a.center = place.position;
    a.turn = place.rotation;
    for (u32 i = 0U; i < v.header->shape_count && !l->full; ++i) {
        const NphCollisionBlock b = nph_collision_shape_block(asset, &v, i);
        if (v.shapes[i].kind == (u32)NPH_COLLISION_SHAPE_HULL) {
            draw_hull(&a, (const NphPhysicsHull *)b.data, rgba, l);
        } else if (meshes) {
            draw_mesh(&a, (const NphPhysicsMesh *)b.data, rgba, l);
        }
    }
}

static void draw_collider(Editor *ed, const NphWorld *w, NphEntity e, const NphCollider *c,
                          const BodyFrame *f, Lines *l)
{
    const NphRuntimeComponents *rc = nph_engine_components(ed->engine);
    const bool trigger = c->trigger != 0;
    const u32 rgba = trigger ? NPH_PHYSICS_DRAW_SENSOR_RGBA : COLOR_COLLIDER;
    if (c->shape == (u32)NPH_COLLIDER_ASSET) {
        // The engine leaves a mesh out of a dynamic body and of a trigger.
        const bool meshes = !trigger && body_type_of(w, rc, e) != (u32)NPH_RIGID_BODY_DYNAMIC;
        draw_asset(ed, c, f, meshes, rgba, l);
        return;
    }
    NphPhysicsGeometry g;
    NphPhysicsPose local;
    if (!nph_collider_shape(c, f->scale, &g, &local)) {
        return; // no shape: nothing the body has
    }
    NphPhysicsPose pose;
    pose.position = nph_quat_rotate(f->rotation, local.position);
    pose.rotation = nph_quat_normalize(nph_quat_mul(f->rotation, local.rotation));
    const NphPhysicsDebugDraw d = physics_draw_of(l);
    nph_physics_geometry_draw(&d, &g, f->position, pose, rgba);
}

// ---------------------------------------------------------------------------
// Characters and joints
// ---------------------------------------------------------------------------

// The capsule the engine stands on the entity's origin (physics_bridge.c
// character_desc, character_rise): upright whatever the entity's turn and
// scale, a skin over its feet.
static void draw_character(const NphCharacterController *cc, NphDVec3 feet, Lines *l)
{
    if (!(nph_f32_is_finite(cc->radius) && cc->radius > 0.0F)) {
        return; // no capsule: the engine makes none either
    }
    const f32 half = nph_f32_max(0.5F * nonneg(cc->height) - cc->radius, 0.0F);
    const NphPhysicsGeometry g = nph_physics_geometry_capsule(half, cc->radius);
    const NphPhysicsCharacterDesc desc = nph_physics_character_desc_default(g);
    NphPhysicsPose pose;
    pose.position = nph_vec3(0.0F, desc.skin + half + cc->radius, 0.0F);
    pose.rotation = nph_quat_identity();
    const NphPhysicsDebugDraw d = physics_draw_of(l);
    nph_physics_geometry_draw(&d, &g, feet, pose, NPH_PHYSICS_DRAW_GHOST_RGBA);
}

static void draw_cross(Lines *l, NphVec3 at, f32 r, u32 rgba)
{
    put(l, nph_vec3_add(at, nph_vec3(-r, 0.0F, 0.0F)), nph_vec3_add(at, nph_vec3(r, 0.0F, 0.0F)),
        rgba);
    put(l, nph_vec3_add(at, nph_vec3(0.0F, -r, 0.0F)), nph_vec3_add(at, nph_vec3(0.0F, r, 0.0F)),
        rgba);
    put(l, nph_vec3_add(at, nph_vec3(0.0F, 0.0F, -r)), nph_vec3_add(at, nph_vec3(0.0F, 0.0F, r)),
        rgba);
}

// A Joint where the engine frames it (physics_bridge.c entity_point,
// joint_frames): its anchor in the entity's frame, joined to the entity and
// to the other body's (none for the world's), or for a distance joint to
// the other anchor, in the other's frame or the world's; its axis from the
// anchor.
static void draw_joint(const NphWorld *w, const NphJoint *j, const BodyFrame *f, Lines *l)
{
    const NphDVec3 origin = l->sink->origin;
    const NphVec3 anchor = frame_point(f, finite_or_zero(j->anchor), origin);
    const NphVec3 axis = nph_vec3_normalize_or(
        nph_quat_rotate(f->rotation, nph_vec3_mul(finite_or_zero(j->axis), f->scale)),
        nph_quat_rotate(f->rotation, nph_vec3_unit_x()));
    put(l, nph_dvec3_relative(f->position, origin), anchor, COLOR_JOINT);
    draw_cross(l, anchor, JOINT_CROSS, COLOR_JOINT);
    put(l, anchor, nph_vec3_mul_add(anchor, axis, JOINT_AXIS_LENGTH), COLOR_JOINT_AXIS);
    const bool distance = j->kind == (u32)NPH_JOINT_DISTANCE;
    const NphVec3 other_anchor = finite_or_zero(j->other_anchor);
    const NphWorldTransform *ot =
        nph_entity_is_null(j->other) || !nph_world_is_alive(w, j->other)
            ? NULL
            : NPH_WORLD_GET(w, j->other, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    if (ot != NULL) {
        const BodyFrame of = body_frame(w, j->other, ot);
        const NphVec3 end = distance ? frame_point(&of, other_anchor, origin)
                                     : nph_dvec3_relative(of.position, origin);
        put(l, anchor, end, COLOR_JOINT);
        if (distance) {
            draw_cross(l, end, JOINT_CROSS, COLOR_JOINT);
        }
    } else if (distance && nph_entity_is_null(j->other)) {
        const NphVec3 end = nph_dvec3_relative(nph_dvec3_from_vec3(other_anchor), origin);
        put(l, anchor, end, COLOR_JOINT);
        draw_cross(l, end, JOINT_CROSS, COLOR_JOINT);
    }
}

bool editor_physics_outline(Editor *ed, NphEntity e, const EditorLineSink *sink)
{
    NPH_ASSERT(ed != NULL && sink != NULL && sink->line != NULL);
    const NphWorld *w = nph_engine_world(ed->engine);
    const NphRuntimeComponents *rc = nph_engine_components(ed->engine);
    const NphWorldTransform *t =
        NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
    if (t == NULL) {
        return true;
    }
    const NphCharacterController *cc =
        NPH_WORLD_GET(w, e, rc->character_controller, NphCharacterController);
    const NphCollider *c = NPH_WORLD_GET(w, e, rc->collider, NphCollider);
    const NphJoint *j = NPH_WORLD_GET(w, e, rc->joint, NphJoint);
    if (cc == NULL && c == NULL && j == NULL) {
        return true;
    }
    Lines l = {sink, false};
    const BodyFrame f = body_frame(w, e, t);
    if (cc != NULL) {
        draw_character(cc, t->translation, &l); // its Collider makes no body
    } else if (c != NULL) {
        draw_collider(ed, w, e, c, &f, &l);
    }
    if (j != NULL) {
        draw_joint(w, j, &f, &l);
    }
    return !l.full;
}
