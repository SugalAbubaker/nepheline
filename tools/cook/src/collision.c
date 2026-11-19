// SPDX-License-Identifier: Apache-2.0
// Collision cooked from glTF (cook/collision.h; ADR-0107).
#include <nepheline/asset/material.h>
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/affine.h>
#include <nepheline/math/scalar.h>
#include <nepheline/physics/hull.h>
#include <nepheline/physics/mesh.h>

#include <cook/collision.h>
#include <cook/cook.h>
#include <stdio.h>
#include <string.h>

static const char *const k_prefixes[] = {"UCX_", "UBX_", "USP_", "UCP_"};

bool cook_collision_proxy_name(const char *name, const char **out_rest)
{
    NPH_ASSERT(name != NULL && out_rest != NULL);
    for (u32 i = 0; i < NPH_ARRAY_COUNT(k_prefixes); ++i) {
        if (strncmp(name, k_prefixes[i], 4U) == 0 && name[4] != '\0') {
            *out_rest = name + 4;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

// `s` without a last `<sep><digits>` ("Crate_01" -> "Crate" for '_'); `s` itself when it has
// none, or nothing before it.
static NphStr strip_count(NphStr s, char sep)
{
    u64 i = s.len;
    while (i > 0U && s.data[i - 1U] >= '0' && s.data[i - 1U] <= '9') {
        --i;
    }
    if (i == s.len || i < 2U || s.data[i - 1U] != sep) {
        return s;
    }
    return nph_str(s.data, i - 1U);
}

static u64 name_hash(NphStr s)
{
    return nph_hash64(s.data, s.len, NPH_HASH_DEFAULT_SEED);
}

// The node named `name` among those `names` holds (a name's hash to its first node), else
// NPH_SCENE_NO_NODE.
static u32 find_node(const CookGltfScene *scene, const NphHashMap *names, NphStr name)
{
    const u32 *at = (const u32 *)nph_hashmap_find_u64(names, name_hash(name));
    if (at == NULL) {
        return NPH_SCENE_NO_NODE;
    }
    const char *found = scene->nodes[*at].name;
    return strlen(found) == name.len && memcmp(found, name.data, (size_t)name.len) == 0
               ? *at
               : NPH_SCENE_NO_NODE;
}

// The node a proxy's name stands for (cook/collision.h), else NPH_SCENE_NO_NODE.
static u32 proxy_target(const CookGltfScene *scene, const NphHashMap *names, const char *rest)
{
    const NphStr whole = nph_str_from_cstr(rest);
    u32 t = find_node(scene, names, whole);
    const NphStr undup = strip_count(whole, '.');
    if (t == NPH_SCENE_NO_NODE && undup.len != whole.len) {
        t = find_node(scene, names, undup);
    }
    const NphStr uncounted = strip_count(undup, '_');
    if (t == NPH_SCENE_NO_NODE && uncounted.len != undup.len) {
        t = find_node(scene, names, uncounted);
    }
    return t;
}

// Whether the mesh draws: it has triangles in submeshes.
static bool draws(const CookGltfScene *scene, i32 mesh)
{
    return mesh >= 0 && (u32)mesh < scene->mesh_count &&
           scene->meshes[mesh].mesh.submeshes.len != 0U &&
           scene->meshes[mesh].mesh.vertices.len >= 3U &&
           scene->meshes[mesh].mesh.indices.len >= 3U;
}

// Whether a material of the mesh's is double-sided: its triangles then collide from both sides.
static bool mesh_two_sided(const CookGltfScene *scene, u32 mesh)
{
    const CookGltfMesh *gm = &scene->meshes[mesh];
    for (u32 s = 0; s < gm->material_count; ++s) {
        const i32 m = gm->materials[s];
        if (m >= 0 && (u32)m < scene->material_count &&
            (scene->materials[m].header.flags & NPH_MATERIAL_FLAG_DOUBLE_SIDED) != 0U) {
            return true;
        }
    }
    return false;
}

// A proxy found, before the plan groups the proxies by their node.
typedef struct Found {
    u32 proxy;  // its node
    u32 target; // the node it stands for
} Found;

typedef NPH_ARRAY(Found) FoundArray;

// The proxy's geometry in its target's frame: `to_target` applied to its mesh's positions.
static NphResult take_geometry(const CookGltfScene *scene, NphAllocator *alloc, u32 node,
                               NphAffine to_target, CookCollisionProxy *out)
{
    const CookMesh *m = &scene->meshes[scene->nodes[node].mesh].mesh;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", scene->nodes[node].name);
    // Each count set with its allocation, so the plan's release frees what a failure leaves.
    out->points = NPH_ALLOC_ARRAY(alloc, NphVec3, m->vertices.len);
    out->point_count = out->points != NULL ? m->vertices.len : 0U;
    out->indices = NPH_ALLOC_ARRAY(alloc, u32, m->indices.len);
    out->index_count = out->indices != NULL ? m->indices.len : 0U;
    if (out->points == NULL || out->indices == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 v = 0; v < m->vertices.len; ++v) {
        out->points[v] = nph_affine_transform_point(to_target, m->vertices.data[v].position);
    }
    memcpy(out->indices, m->indices.data, (size_t)m->indices.len * sizeof(u32));
    return NPH_RESULT_OK;
}

// Takes the proxies `found` lists out of the scene's node list, where they have no child,
// camera or light; the others stay, drawing nothing. A mesh that only proxies drew stops
// drawing. `remap` (a node's new index, NPH_SCENE_NO_NODE when taken out) is the caller's.
static void take_out(CookGltfScene *scene, const u8 *is_proxy, u32 *remap, u8 *has_child)
{
    const u32 n = scene->node_count;
    memset(has_child, 0, (size_t)n);
    for (u32 i = 0; i < n; ++i) {
        if (scene->nodes[i].parent != NPH_SCENE_NO_NODE) {
            has_child[scene->nodes[i].parent] = 1U;
        }
    }
    // The meshes the proxies drew, and whether anything else draws them.
    u8 *mesh_state = has_child + n; // 1: a proxy drew it; 2: something else does
    memset(mesh_state, 0, (size_t)scene->mesh_count);
    for (u32 i = 0; i < n; ++i) {
        const i32 m = scene->nodes[i].mesh;
        if (m >= 0 && (u32)m < scene->mesh_count) {
            mesh_state[m] = is_proxy[i] != 0U ? (u8)(mesh_state[m] | 1U) : (u8)(mesh_state[m] | 2U);
        }
    }
    u32 kept = 0U;
    for (u32 i = 0; i < n; ++i) {
        CookGltfNode *node = &scene->nodes[i];
        if (is_proxy[i] != 0U) {
            node->mesh = -1;
            if (has_child[i] == 0U && node->camera < 0 && node->light < 0) {
                remap[i] = NPH_SCENE_NO_NODE;
                continue;
            }
        }
        remap[i] = kept;
        if (kept != i) {
            scene->nodes[kept] = *node;
        }
        CookGltfNode *moved = &scene->nodes[kept];
        // A node taken out has no child: a parent is always kept.
        moved->parent =
            moved->parent != NPH_SCENE_NO_NODE ? remap[moved->parent] : NPH_SCENE_NO_NODE;
        kept += 1U;
    }
    scene->node_count = kept;
    for (u32 k = 0; k < scene->camera_count; ++k) {
        scene->cameras[k].node = remap[scene->cameras[k].node];
    }
    for (u32 k = 0; k < scene->light_count; ++k) {
        scene->lights[k].node = remap[scene->lights[k].node];
    }
    for (u32 m = 0; m < scene->mesh_count; ++m) {
        if (mesh_state[m] == 1U) {
            NPH_ARRAY_CLEAR(&scene->meshes[m].mesh.submeshes);
            scene->meshes[m].material_count = 0U;
        }
    }
}

static void warn(CookCollisionPlan *plan, CookGltfScene *scene)
{
    plan->warnings += 1U;
    scene->warnings += 1U;
}

// Finds the proxies and their nodes, and gathers their geometry in their nodes' frames, in node
// order: `found[k]` is `proxies[k]`'s.
static NphResult find_proxies(CookGltfScene *scene, NphAllocator *alloc, const NphAffine *world,
                              const NphHashMap *names, u8 *is_proxy, FoundArray *found,
                              CookCollisionPlan *plan)
{
    u32 cap = 0U;
    for (u32 i = 0; i < scene->node_count; ++i) {
        const char *rest = NULL;
        cap += cook_collision_proxy_name(scene->nodes[i].name, &rest) ? 1U : 0U;
    }
    if (cap == 0U) {
        return NPH_RESULT_OK;
    }
    plan->proxies = NPH_ALLOC_ARRAY_ZEROED(alloc, CookCollisionProxy, cap);
    if (plan->proxies == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    plan->proxy_capacity = cap;
    for (u32 i = 0; i < scene->node_count; ++i) {
        const CookGltfNode *node = &scene->nodes[i];
        const char *rest = NULL;
        if (!cook_collision_proxy_name(node->name, &rest)) {
            continue;
        }
        if (!draws(scene, node->mesh)) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "collision proxy %s has no mesh; left as it is",
                         node->name);
            warn(plan, scene);
            continue;
        }
        const u32 target = proxy_target(scene, names, rest);
        NphAffine inverse;
        if (target == NPH_SCENE_NO_NODE) {
            NPH_LOG_WARN(&NPH_LOG_COOK,
                         "collision proxy %s: no node is named %s; drawn as it is (docs/cook.md "
                         "\"Collision\")",
                         node->name, rest);
            warn(plan, scene);
            continue;
        }
        if (!nph_affine_inverse(world[target], &inverse)) {
            NPH_LOG_WARN(&NPH_LOG_COOK,
                         "collision proxy %s: %s has a scale of zero, so no frame; drawn as it is",
                         node->name, scene->nodes[target].name);
            warn(plan, scene);
            continue;
        }
        const NphAffine to_target = nph_affine_mul(inverse, world[i]);
        // Counted before it is filled, so the plan's release frees a half-made one.
        CookCollisionProxy *proxy = &plan->proxies[plan->proxy_count++];
        NPH_TRY(take_geometry(scene, alloc, i, to_target, proxy));
        is_proxy[i] = 1U;
        const Found f = {i, target};
        NPH_TRY(NPH_ARRAY_PUSH(found, f));
    }
    return NPH_RESULT_OK;
}

// Groups the proxies by their node, in node order (a node's in the order they were found), and
// sets the plan's entries for the nodes that have some; `remap` gives the nodes' new indices.
static NphResult group_proxies(CookCollisionPlan *plan, NphAllocator *alloc,
                               const FoundArray *found, const u32 *remap)
{
    const u32 n = plan->proxy_count;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    NPH_ASSERT(found->data != NULL && found->len == n); // one a proxy taken
    CookCollisionProxy *sorted = NPH_ALLOC_ARRAY_ZEROED(alloc, CookCollisionProxy, n);
    u32 *order = NPH_ALLOC_ARRAY(alloc, u32, n);
    if (sorted == NULL || order == NULL) {
        if (sorted != NULL) {
            NPH_FREE_ARRAY(alloc, CookCollisionProxy, sorted, n);
        }
        if (order != NULL) {
            NPH_FREE_ARRAY(alloc, u32, order, n);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    // A stable insertion sort by target: the proxies are few beside the nodes.
    for (u32 k = 0; k < n; ++k) {
        u32 at = k;
        while (at > 0U && found->data[order[at - 1U]].target > found->data[k].target) {
            order[at] = order[at - 1U];
            --at;
        }
        order[at] = k;
    }
    for (u32 k = 0; k < n; ++k) {
        sorted[k] = plan->proxies[order[k]];
        const u32 target = remap[found->data[order[k]].target];
        NPH_ASSERT(target != NPH_SCENE_NO_NODE); // a proxy's node is never a proxy
        CookNodeCollision *last =
            plan->node_count != 0U ? &plan->nodes[plan->node_count - 1U] : NULL;
        if (last != NULL && last->node == target) {
            last->proxy_count += 1U;
        } else {
            CookNodeCollision *e = &plan->nodes[plan->node_count++];
            memset(e, 0, sizeof *e);
            e->node = target;
            e->kind = COOK_COLLISION_NONE;
            e->first_proxy = k;
            e->proxy_count = 1U;
        }
    }
    NPH_FREE_ARRAY(alloc, CookCollisionProxy, plan->proxies, plan->proxy_capacity);
    plan->proxies = sorted;
    plan->proxy_capacity = n;
    NPH_FREE_ARRAY(alloc, u32, order, n);
    return NPH_RESULT_OK;
}

// The entries of the nodes without proxies, merged into the plan in node order.
static NphResult add_mesh_entries(CookCollisionPlan *plan, CookGltfScene *scene,
                                  CookCollisionKind scene_kind, NphAllocator *alloc)
{
    const u32 with_proxies = plan->node_count;
    const u32 cap = nph_max_u32(scene->node_count, 1U);
    CookNodeCollision *merged = NPH_ALLOC_ARRAY_ZEROED(alloc, CookNodeCollision, cap);
    if (merged == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 count = 0U;
    u32 p = 0U;
    for (u32 i = 0; i < scene->node_count; ++i) {
        if (p < with_proxies && plan->nodes[p].node == i) {
            merged[count++] = plan->nodes[p++];
            continue;
        }
        const CookGltfNode *node = &scene->nodes[i];
        const CookCollisionKind kind =
            node->collision >= 0 ? (CookCollisionKind)node->collision : scene_kind;
        if (kind == COOK_COLLISION_NONE) {
            continue;
        }
        if (!draws(scene, node->mesh)) {
            if (node->collision >= 0) {
                NPH_LOG_WARN(&NPH_LOG_COOK,
                             "node %s: collision \"%s\" asked, but it draws no "
                             "mesh; none made",
                             node->name, cook_collision_kind_name(kind));
                warn(plan, scene);
            }
            continue;
        }
        CookNodeCollision *e = &merged[count++];
        e->node = i;
        e->kind = kind;
        e->mesh = (u32)node->mesh;
    }
    NPH_FREE_ARRAY(alloc, CookNodeCollision, plan->nodes, plan->node_capacity);
    plan->nodes = merged;
    plan->node_count = count;
    plan->node_capacity = cap;
    return NPH_RESULT_OK;
}

NphResult cook_collision_plan(CookGltfScene *scene, const CookSceneImport *settings,
                              NphAllocator *alloc, CookCollisionPlan *out)
{
    NPH_ASSERT(scene != NULL && settings != NULL && alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    out->hull_vertices = cook_scene_import_hull_vertices(settings);
    const u32 n = scene->node_count;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    // The plan's entries, at most one a node; every node's world transform; the names of the
    // nodes that are not proxies; and the scratch the taking out needs.
    out->nodes = NPH_ALLOC_ARRAY_ZEROED(alloc, CookNodeCollision, n);
    out->node_capacity = out->nodes != NULL ? n : 0U;
    NphAffine *world = NPH_ALLOC_ARRAY(alloc, NphAffine, n);
    u32 *remap = NPH_ALLOC_ARRAY(alloc, u32, n);
    const u32 scratch = n * 2U + scene->mesh_count;
    u8 *bytes = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, scratch);
    NphHashMap names;
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));
    NphResult r = nph_hashmap_init(&names, alloc, &desc);
    if (r == NPH_RESULT_OK &&
        (out->nodes == NULL || world == NULL || remap == NULL || bytes == NULL)) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < n && r == NPH_RESULT_OK; ++i) {
        const CookGltfNode *node = &scene->nodes[i];
        const NphAffine local = nph_affine_from_trs(node->translation, node->rotation, node->scale);
        world[i] =
            node->parent == NPH_SCENE_NO_NODE ? local : nph_affine_mul(world[node->parent], local);
        const char *rest = NULL;
        if (!cook_collision_proxy_name(node->name, &rest)) {
            const u32 index = i;
            r = nph_hashmap_insert_u64(&names, name_hash(nph_str_from_cstr(node->name)), &index);
            if (r == NPH_RESULT_ALREADY_EXISTS) {
                r = NPH_RESULT_OK; // the first of a name is the one a proxy finds
            }
        }
    }
    FoundArray found;
    NPH_ARRAY_INIT(&found, alloc);
    u8 *is_proxy = bytes;
    u8 *has_child = bytes + n; // and the meshes' state after it
    if (r == NPH_RESULT_OK) {
        r = find_proxies(scene, alloc, world, &names, is_proxy, &found, out);
    }
    if (r == NPH_RESULT_OK && out->proxy_count != 0U) {
        take_out(scene, is_proxy, remap, has_child);
        r = group_proxies(out, alloc, &found, remap);
    }
    if (r == NPH_RESULT_OK) {
        r = add_mesh_entries(out, scene, cook_scene_import_collision(settings), alloc);
    }
    for (u32 k = 0; k < out->node_count && r == NPH_RESULT_OK; ++k) {
        CookNodeCollision *e = &out->nodes[k];
        e->two_sided = e->kind == COOK_COLLISION_MESH && mesh_two_sided(scene, e->mesh) ? 1 : 0;
    }
    NPH_ARRAY_FREE(&found);
    nph_hashmap_free(&names);
    if (bytes != NULL) {
        NPH_FREE_ARRAY(alloc, u8, bytes, scratch);
    }
    if (remap != NULL) {
        NPH_FREE_ARRAY(alloc, u32, remap, n);
    }
    if (world != NULL) {
        NPH_FREE_ARRAY(alloc, NphAffine, world, n);
    }
    if (r != NPH_RESULT_OK) {
        cook_collision_plan_free(out);
    }
    return r;
}

void cook_collision_plan_free(CookCollisionPlan *plan)
{
    NPH_ASSERT(plan != NULL);
    NphAllocator *alloc = plan->alloc;
    if (alloc == NULL) {
        return;
    }
    for (u32 k = 0; k < plan->proxy_capacity; ++k) {
        CookCollisionProxy *p = &plan->proxies[k];
        if (p->points != NULL) {
            NPH_FREE_ARRAY(alloc, NphVec3, p->points, p->point_count);
        }
        if (p->indices != NULL) {
            NPH_FREE_ARRAY(alloc, u32, p->indices, p->index_count);
        }
    }
    if (plan->proxies != NULL) {
        NPH_FREE_ARRAY(alloc, CookCollisionProxy, plan->proxies, plan->proxy_capacity);
    }
    if (plan->nodes != NULL) {
        NPH_FREE_ARRAY(alloc, CookNodeCollision, plan->nodes, plan->node_capacity);
    }
    memset(plan, 0, sizeof *plan);
}

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

void cook_collision_shapes_init(CookCollisionShapes *s, NphAllocator *alloc)
{
    NPH_ASSERT(s != NULL && alloc != NULL);
    memset(s, 0, sizeof *s);
    s->alloc = alloc;
    NPH_ARRAY_INIT(&s->blocks, alloc);
}

void cook_collision_shapes_free(CookCollisionShapes *s)
{
    NPH_ASSERT(s != NULL);
    NPH_ARRAY_FOREACH (CookCollisionBlock, b, &s->blocks) {
        if (b->kind == NPH_COLLISION_SHAPE_HULL) {
            nph_physics_hull_free((NphPhysicsHull *)b->block, s->alloc);
        } else {
            nph_physics_mesh_free((NphPhysicsMesh *)b->block, s->alloc);
        }
    }
    NPH_ARRAY_FREE(&s->blocks);
}

// Every coordinate finite and within what the physics module takes.
static bool coordinates_ok(const NphVec3 *points, u32 count)
{
    for (u32 i = 0; i < count; ++i) {
        const f32 c[3] = {points[i].x, points[i].y, points[i].z};
        for (u32 k = 0; k < 3U; ++k) {
            if (!(nph_f32_abs(c[k]) <= NPH_PHYSICS_MAX_COORDINATE)) {
                return false; // NaN fails too
            }
        }
    }
    return true;
}

NphResult cook_collision_add_mesh(CookCollisionShapes *s, const NphVec3 *points, u32 point_count,
                                  const u32 *indices, u32 index_count, bool two_sided)
{
    NPH_ASSERT(s != NULL);
    if (s->blocks.len >= NPH_COLLISION_MAX_SHAPES) {
        return NPH_RESULT_FULL;
    }
    if (index_count < 3U || index_count % 3U != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphPhysicsMeshDesc d;
    memset(&d, 0, sizeof d);
    d.vertices = points;
    d.vertex_count = point_count;
    d.indices = indices;
    d.triangle_count = index_count / 3U;
    d.two_sided = two_sided ? 1 : 0;
    NphPhysicsMesh *mesh = NULL;
    NPH_TRY(nph_physics_mesh_build(&d, s->alloc, &mesh));
    CookCollisionBlock b;
    memset(&b, 0, sizeof b);
    b.kind = NPH_COLLISION_SHAPE_MESH;
    b.block = mesh;
    b.size = mesh->byte_size;
    b.bounds = mesh->bounds;
    const NphResult r = NPH_ARRAY_PUSH(&s->blocks, b);
    if (r != NPH_RESULT_OK) {
        nph_physics_mesh_free(mesh, s->alloc);
    }
    return r;
}

NphResult cook_collision_add_hull(CookCollisionShapes *s, const NphVec3 *points, u32 point_count,
                                  const u32 *indices, u32 index_count, u32 max_vertices,
                                  bool *out_flat)
{
    NPH_ASSERT(s != NULL && out_flat != NULL);
    NPH_ASSERT(max_vertices >= 4U && max_vertices <= NPH_PHYSICS_HULL_MAX_VERTICES);
    *out_flat = false;
    if (s->blocks.len >= NPH_COLLISION_MAX_SHAPES) {
        return NPH_RESULT_FULL;
    }
    if (!coordinates_ok(points, point_count)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphPhysicsHullDesc d;
    memset(&d, 0, sizeof d);
    d.points = points;
    d.point_count = point_count;
    d.max_vertices = max_vertices;
    NphPhysicsHull *hull = NULL;
    const NphResult built = point_count >= 4U ? nph_physics_hull_build(&d, s->alloc, &hull)
                                              : NPH_RESULT_INVALID_ARGUMENT;
    if (built == NPH_RESULT_INVALID_ARGUMENT) {
        // With every coordinate in range and the budget good, no volume: a flat thing, which
        // collides as its triangles, from both sides.
        *out_flat = true;
        return cook_collision_add_mesh(s, points, point_count, indices, index_count, true);
    }
    NPH_TRY(built);
    CookCollisionBlock b;
    memset(&b, 0, sizeof b);
    b.kind = NPH_COLLISION_SHAPE_HULL;
    b.block = hull;
    b.size = hull->byte_size;
    b.bounds = hull->bounds;
    const NphResult r = NPH_ARRAY_PUSH(&s->blocks, b);
    if (r != NPH_RESULT_OK) {
        nph_physics_hull_free(hull, s->alloc);
    }
    return r;
}

NphResult cook_collision_write(const CookCollisionShapes *s, NphLzLevel lz, NphAssetWriter *w)
{
    NPH_ASSERT(s != NULL && w != NULL);
    const u32 n = s->blocks.len;
    if (n == 0U || n > NPH_COLLISION_MAX_SHAPES) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphCollisionHeader h;
    memset(&h, 0, sizeof h);
    h.shape_count = n;
    h.bounds = s->blocks.data[0].bounds;
    NphCollisionShape *shapes = NPH_ALLOC_ARRAY_ZEROED(s->alloc, NphCollisionShape, n);
    if (shapes == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    u32 next[2] = {0U, 0U}; // hulls, meshes
    for (u32 i = 0; i < n; ++i) {
        const CookCollisionBlock *b = &s->blocks.data[i];
        const u32 k = b->kind == NPH_COLLISION_SHAPE_HULL ? 0U : 1U;
        shapes[i].kind = (u32)b->kind;
        shapes[i].block = next[k]++;
        h.bounds = nph_aabb_union(h.bounds, b->bounds);
    }
    NphResult r = nph_asset_writer_add_blob(w, NPH_COLLISION_TAG_HEADER, 0U, &h, sizeof h,
                                            NPH_ASSET_CODEC_NONE, 0U);
    if (r == NPH_RESULT_OK) {
        r = nph_asset_writer_add_blob(w, NPH_COLLISION_TAG_SHAPES, 0U, shapes,
                                      (u64)n * sizeof *shapes, NPH_ASSET_CODEC_NONE, 0U);
    }
    nph_asset_writer_set_lz_level(w, lz);
    for (u32 i = 0; i < n && r == NPH_RESULT_OK; ++i) {
        const CookCollisionBlock *b = &s->blocks.data[i];
        const u32 tag =
            b->kind == NPH_COLLISION_SHAPE_HULL ? NPH_COLLISION_TAG_HULL : NPH_COLLISION_TAG_MESH;
        r = nph_asset_writer_add_blob(w, tag, shapes[i].block, b->block, b->size,
                                      NPH_ASSET_CODEC_LZ, 16U);
    }
    NPH_FREE_ARRAY(s->alloc, NphCollisionShape, shapes, n);
    return r;
}
