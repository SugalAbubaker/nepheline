// SPDX-License-Identifier: Apache-2.0
// Tangents by the default rules of MikkTSpace (mesh.h; Mikkelsen,
// "Simulation of Wrinkled Surfaces Revisited", 2008, and the reference
// implementation mikktspace.c, whose structure this follows):
//
// 1. Vertices are identified by position, normal, and texture coordinate
//    only (other attributes do not split a tangent frame).
// 2. Each triangle has a texture-space orientation (the sign of its UV
//    area) and a direction dP/du. A triangle with no UV area or no extent
//    is "degenerate": it contributes nothing but joins whichever group
//    reaches it first, taking that group's orientation.
// 3. Around each vertex, triangles form groups: a group grows across
//    shared edges (both endpoints the same vertex in the sense of 1, so UV
//    seams stop it) through triangles of the same orientation. A vertex
//    on a mirrored seam therefore has two groups.
// 4. A group's tangent is the sum, over its non-degenerate triangles, of
//    their dP/du projected into the plane of the vertex normal and
//    normalised, weighted by the triangle's angle at the vertex; it is
//    normalised at the end. The handedness is the group's orientation.
//
// The sign convention is glTF's rather than MikkTSpace's own: glTF
// texture coordinates grow downwards while its normal textures have +Y
// up, so the bitangent cross(n, t) * w must point along decreasing v.
// That makes w = +1 where the UV triangle is clockwise in (u, v), which
// is MikkTSpace's sign negated (equivalently, MikkTSpace run on 1 - v).
// The tangent directions are the same either way.
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/math/scalar.h>

#include <cook/mesh.h>
#include <string.h>

#define TANGENT_NONE NPH_U32_MAX

typedef struct TangentTri {
    NphVec3 dir;     // normalised dP/du (zero for a degenerate triangle)
    u32 neighbor[3]; // the triangle across edge k (corner k to corner k + 1), or NONE
    u32 group[3];    // the group corner k belongs to, or NONE
    u8 preserving;   // glTF handedness +1 (the UV triangle is clockwise)
    u8 degenerate;   // joins any group, contributes nothing
} TangentTri;

typedef struct TangentGroup {
    u32 vertex; // the identity (step 1) all its corners share
    u8 preserving;
    NphVec3 sum; // angle-weighted directions
} TangentGroup;

typedef NPH_ARRAY(TangentGroup) TangentGroupArray;

// The identity of step 1: the bytes that decide whether two corners are
// the same vertex for tangent purposes.
typedef struct TangentKey {
    NphVec3 position;
    NphVec3 normal;
    NphVec2 uv;
} TangentKey;

NPH_STATIC_ASSERT(sizeof(TangentKey) == 32U, "a tangent key is 32 bytes of floats");

static bool not_zero(f32 v)
{
    return nph_f32_abs(v) > NPH_F32_MIN_NORMAL; // mikktspace.c's NotZero
}

// Identities per corner: corner i of the index list gets ids[i].
static NphResult tangent_identities(const CookMesh *mesh, u32 *ids)
{
    NphHashMap map;
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = sizeof(TangentKey);
    desc.val_size = sizeof(u32);
    NPH_TRY(nph_hashmap_init(&map, mesh->alloc, &desc));
    NphResult r = nph_hashmap_reserve(&map, mesh->vertices.len);
    u32 next = 0U;
    for (u32 i = 0; i < mesh->indices.len && r == NPH_RESULT_OK; ++i) {
        const CookVertex *v = &mesh->vertices.data[mesh->indices.data[i]];
        TangentKey key;
        memset(&key, 0, sizeof key);
        key.position = v->position;
        key.normal = v->normal;
        key.uv = v->uv0;
        f32 *f = (f32 *)&key;
        for (u32 c = 0; c < sizeof key / sizeof(f32); ++c) {
            if (f[c] == 0.0F) {
                f[c] = 0.0F; // -0 and +0 are one value
            }
        }
        const u32 *found = (const u32 *)nph_hashmap_find(&map, &key);
        if (found != NULL) {
            ids[i] = *found;
        } else {
            ids[i] = next;
            r = nph_hashmap_insert(&map, &key, &next);
            ++next;
        }
    }
    nph_hashmap_free(&map);
    return r;
}

// Directed edges by identity: the triangle across edge k of t is the one
// holding the same edge the other way round (consistent winding).
static NphResult tangent_neighbors(NphAllocator *alloc, const u32 *ids, u32 tri_count,
                                   TangentTri *tris)
{
    NphHashMap edges; // (from << 32 | to) -> t * 3 + k
    const NphHashMapDesc desc = nph_hashmap_desc_u64(sizeof(u32));
    NPH_TRY(nph_hashmap_init(&edges, alloc, &desc));
    NphResult r = nph_hashmap_reserve(&edges, tri_count * 3U);
    for (u32 t = 0; t < tri_count && r == NPH_RESULT_OK; ++t) {
        for (u32 k = 0; k < 3U && r == NPH_RESULT_OK; ++k) {
            const u64 key = ((u64)ids[t * 3U + k] << 32U) | ids[t * 3U + (k + 1U) % 3U];
            const u32 slot = t * 3U + k;
            r = nph_hashmap_insert_u64(&edges, key, &slot);
            if (r == NPH_RESULT_ALREADY_EXISTS) {
                r = NPH_RESULT_OK; // a non-manifold edge: the first one wins
            }
        }
    }
    for (u32 t = 0; t < tri_count && r == NPH_RESULT_OK; ++t) {
        for (u32 k = 0; k < 3U; ++k) {
            const u64 back = ((u64)ids[t * 3U + (k + 1U) % 3U] << 32U) | ids[t * 3U + k];
            const u32 *other = (const u32 *)nph_hashmap_find_u64(&edges, back);
            tris[t].neighbor[k] = other != NULL && *other / 3U != t ? *other / 3U : TANGENT_NONE;
        }
    }
    nph_hashmap_free(&edges);
    return r;
}

// Equal as values (the tangents are finite, so -0 == +0 is the only
// difference from comparing bytes, and a sign of zero is no reason to split
// a vertex).
static bool tangents_equal(NphVec4 a, NphVec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static void tangent_triangle_info(const CookMesh *mesh, u32 t, TangentTri *out)
{
    const u32 *tri = &mesh->indices.data[(u64)t * 3U];
    const CookVertex *a = &mesh->vertices.data[tri[0]];
    const CookVertex *b = &mesh->vertices.data[tri[1]];
    const CookVertex *c = &mesh->vertices.data[tri[2]];
    const NphVec3 e1 = nph_vec3_sub(b->position, a->position);
    const NphVec3 e2 = nph_vec3_sub(c->position, a->position);
    const NphVec2 d1 = nph_vec2_sub(b->uv0, a->uv0);
    const NphVec2 d2 = nph_vec2_sub(c->uv0, a->uv0);
    // e1 = T d1.x + B d1.y and e2 = T d2.x + B d2.y, so T = s / area and
    // B = t / area with the two numerators below (mikktspace.c eq. 18, 19).
    const f32 area = d1.x * d2.y - d2.x * d1.y;
    const NphVec3 s = nph_vec3_sub(nph_vec3_scale(e1, d2.y), nph_vec3_scale(e2, d1.y));
    const NphVec3 tv = nph_vec3_sub(nph_vec3_scale(e2, d1.x), nph_vec3_scale(e1, d2.x));
    out->preserving = area < 0.0F ? 1U : 0U; // glTF's sign (see the top of the file)
    out->dir = nph_vec3_zero();
    out->degenerate = 1U;
    const f32 len_s = nph_vec3_length(s);
    const f32 len_t = nph_vec3_length(tv);
    if (not_zero(area) && not_zero(len_s / nph_f32_abs(area)) &&
        not_zero(len_t / nph_f32_abs(area))) {
        out->dir = nph_vec3_scale(s, (area < 0.0F ? -1.0F : 1.0F) / len_s); // dP/du, unit
        out->degenerate = 0U;
    }
}

static u32 corner_of(const u32 *ids, u32 t, u32 vertex)
{
    for (u32 k = 0; k < 3U; ++k) {
        if (ids[t * 3U + k] == vertex) {
            return k;
        }
    }
    return TANGENT_NONE;
}

// Grows group `g` from corner `k` of triangle `t` (mikktspace.c
// Build4RuleGroups and AssignRecur, with an explicit stack in the order
// the recursion would visit).
static NphResult tangent_grow(const u32 *ids, TangentTri *tris, u32 g, const TangentGroup *group,
                              u32 t, u32 k, NphArrayU32 *stack)
{
    tris[t].group[k] = g;
    NPH_ARRAY_CLEAR(stack);
    const u32 right = tris[t].neighbor[(k + 2U) % 3U];
    const u32 left = tris[t].neighbor[k];
    if (right != TANGENT_NONE) {
        NPH_TRY(NPH_ARRAY_PUSH(stack, right));
    }
    if (left != TANGENT_NONE) {
        NPH_TRY(NPH_ARRAY_PUSH(stack, left));
    }
    while (stack->len != 0U) {
        const u32 n = NPH_ARRAY_POP(stack);
        TangentTri *nt = &tris[n];
        const u32 j = corner_of(ids, n, group->vertex);
        if (j == TANGENT_NONE || nt->group[j] != TANGENT_NONE) {
            continue; // already in this group or another
        }
        if (nt->degenerate != 0U && nt->group[0] == TANGENT_NONE && nt->group[1] == TANGENT_NONE &&
            nt->group[2] == TANGENT_NONE) {
            nt->preserving = group->preserving; // the first group to reach it decides
        }
        if (nt->preserving != group->preserving) {
            continue;
        }
        nt->group[j] = g;
        const u32 nr = nt->neighbor[(j + 2U) % 3U];
        const u32 nl = nt->neighbor[j];
        if (nr != TANGENT_NONE) {
            NPH_TRY(NPH_ARRAY_PUSH(stack, nr));
        }
        if (nl != TANGENT_NONE) {
            NPH_TRY(NPH_ARRAY_PUSH(stack, nl));
        }
    }
    return NPH_RESULT_OK;
}

static NphVec3 project_unit(NphVec3 v, NphVec3 n)
{
    return nph_vec3_normalize_or_zero(nph_vec3_sub(v, nph_vec3_scale(n, nph_vec3_dot(n, v))));
}

typedef struct SplitKey {
    u32 vertex;
    NphVec4 tangent;
} SplitKey;

// Gives every corner its tangent, splitting vertices whose corners differ.
static NphResult tangent_apply(CookMesh *mesh, const NphVec4 *corner_tangents)
{
    const u32 n = mesh->vertices.len;
    u8 *assigned = NPH_ALLOC_ARRAY_ZEROED(mesh->alloc, u8, nph_max_u32(n, 1U));
    if (assigned == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphHashMap splits; // SplitKey -> the vertex made for it
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = sizeof(SplitKey);
    desc.val_size = sizeof(u32);
    NphResult r = nph_hashmap_init(&splits, mesh->alloc, &desc);
    for (u32 i = 0; i < mesh->indices.len && r == NPH_RESULT_OK; ++i) {
        const u32 v = mesh->indices.data[i];
        const NphVec4 t = corner_tangents[i];
        if (assigned[v] == 0U) {
            mesh->vertices.data[v].tangent = t;
            assigned[v] = 1U;
            continue;
        }
        if (tangents_equal(mesh->vertices.data[v].tangent, t)) {
            continue;
        }
        SplitKey key;
        memset(&key, 0, sizeof key);
        key.vertex = v;
        key.tangent = t;
        const u32 *found = (const u32 *)nph_hashmap_find(&splits, &key);
        if (found != NULL) {
            mesh->indices.data[i] = *found;
            continue;
        }
        CookVertex copy = mesh->vertices.data[v];
        copy.tangent = t;
        const u32 index = mesh->vertices.len;
        r = NPH_ARRAY_PUSH(&mesh->vertices, copy);
        if (r == NPH_RESULT_OK) {
            r = nph_hashmap_insert(&splits, &key, &index);
            mesh->indices.data[i] = index;
        }
    }
    nph_hashmap_free(&splits);
    NPH_FREE_ARRAY(mesh->alloc, u8, assigned, nph_max_u32(n, 1U));
    return r;
}

NphResult cook_mesh_generate_tangents(CookMesh *mesh)
{
    NPH_ASSERT(mesh != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    if ((mesh->flags & (u32)COOK_MESH_HAS_UV0) == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if ((mesh->flags & (u32)COOK_MESH_HAS_NORMALS) == 0U) {
        // Shared vertices are what the grouping walks: weld the flat corners.
        NPH_TRY(cook_mesh_generate_normals(mesh, COOK_NORMALS_FLAT));
        NPH_TRY(cook_mesh_weld(mesh));
    }
    NphAllocator *alloc = mesh->alloc;
    const u32 corners = mesh->indices.len;
    const u32 tri_count = corners / 3U;
    if (tri_count == 0U) {
        mesh->flags |= (u32)COOK_MESH_HAS_TANGENTS;
        return NPH_RESULT_OK;
    }
    u32 *ids = NPH_ALLOC_ARRAY(alloc, u32, corners);
    TangentTri *tris = NPH_ALLOC_ARRAY(alloc, TangentTri, tri_count);
    NphVec4 *result = NPH_ALLOC_ARRAY(alloc, NphVec4, corners);
    TangentGroupArray groups;
    NPH_ARRAY_INIT(&groups, alloc);
    NphArrayU32 stack;
    NPH_ARRAY_INIT(&stack, alloc);
    NphResult r =
        ids != NULL && tris != NULL && result != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    if (r == NPH_RESULT_OK) {
        r = tangent_identities(mesh, ids);
    }
    if (r == NPH_RESULT_OK) {
        for (u32 t = 0; t < tri_count; ++t) {
            tangent_triangle_info(mesh, t, &tris[t]);
            tris[t].group[0] = TANGENT_NONE;
            tris[t].group[1] = TANGENT_NONE;
            tris[t].group[2] = TANGENT_NONE;
        }
        r = tangent_neighbors(alloc, ids, tri_count, tris);
    }
    // Groups: every corner of a non-degenerate triangle not yet reached
    // starts one (degenerate triangles only ever join).
    for (u32 t = 0; t < tri_count && r == NPH_RESULT_OK; ++t) {
        for (u32 k = 0; k < 3U && r == NPH_RESULT_OK; ++k) {
            if (tris[t].degenerate != 0U || tris[t].group[k] != TANGENT_NONE) {
                continue;
            }
            TangentGroup g;
            g.vertex = ids[t * 3U + k];
            g.preserving = tris[t].preserving;
            g.sum = nph_vec3_zero();
            const u32 index = groups.len;
            r = NPH_ARRAY_PUSH(&groups, g);
            if (r == NPH_RESULT_OK) {
                r = tangent_grow(ids, tris, index, &groups.data[index], t, k, &stack);
            }
        }
    }
    if (r == NPH_RESULT_OK) {
        // Angle-weighted sums of the projected directions.
        for (u32 t = 0; t < tri_count; ++t) {
            if (tris[t].degenerate != 0U) {
                continue;
            }
            for (u32 k = 0; k < 3U; ++k) {
                TangentGroup *g = &groups.data[tris[t].group[k]];
                const CookVertex *v = &mesh->vertices.data[mesh->indices.data[t * 3U + k]];
                const NphVec3 p0 =
                    mesh->vertices.data[mesh->indices.data[t * 3U + (k + 2U) % 3U]].position;
                const NphVec3 p2 =
                    mesh->vertices.data[mesh->indices.data[t * 3U + (k + 1U) % 3U]].position;
                // Unit length for the projections, whatever the source gave.
                const NphVec3 n = nph_vec3_normalize_or(v->normal, nph_vec3_unit_z());
                const NphVec3 v1 = project_unit(nph_vec3_sub(p0, v->position), n);
                const NphVec3 v2 = project_unit(nph_vec3_sub(p2, v->position), n);
                const f32 angle = nph_f32_acos(nph_f32_clamp(nph_vec3_dot(v1, v2), -1.0F, 1.0F));
                g->sum = nph_vec3_add(g->sum, nph_vec3_scale(project_unit(tris[t].dir, n), angle));
            }
        }
        // Every corner takes its group's frame; a corner no group reached
        // (an isolated degenerate triangle) gets an arbitrary perpendicular.
        for (u32 i = 0; i < corners; ++i) {
            const u32 t = i / 3U;
            const u32 k = i % 3U;
            // A source normal of zero (invalid glTF, but finite) is +Z, as
            // the writer encodes it; perpendicular() needs a unit vector.
            const NphVec3 n = nph_vec3_normalize_or(
                mesh->vertices.data[mesh->indices.data[i]].normal, nph_vec3_unit_z());
            const NphVec3 fallback = nph_vec3_perpendicular(n);
            if (tris[t].group[k] == TANGENT_NONE) {
                result[i] = nph_vec4_from_vec3(fallback, 1.0F);
                continue;
            }
            const TangentGroup *g = &groups.data[tris[t].group[k]];
            result[i] = nph_vec4_from_vec3(nph_vec3_normalize_or(g->sum, fallback),
                                           g->preserving != 0U ? 1.0F : -1.0F);
        }
        r = tangent_apply(mesh, result);
    }
    if (r == NPH_RESULT_OK) {
        mesh->flags |= (u32)COOK_MESH_HAS_TANGENTS;
    }
    NPH_ARRAY_FREE(&stack);
    NPH_ARRAY_FREE(&groups);
    if (ids != NULL) {
        NPH_FREE_ARRAY(alloc, u32, ids, corners);
    }
    if (tris != NULL) {
        NPH_FREE_ARRAY(alloc, TangentTri, tris, tri_count);
    }
    if (result != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec4, result, corners);
    }
    return r;
}
