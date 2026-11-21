// SPDX-License-Identifier: Apache-2.0
// Picking. See picking.h.
#include "picking.h"

#include <nepheline/math/geometry.h>
#include <nepheline/math/mat.h>
#include <nepheline/scene/transform.h>

#include <float.h>

// The nearest hit of the triangles of `mesh` from index `first`, `count`
// indices, by `local`, nearer than `*best`: true when one is, which lowers
// `*best`.
static bool hit_triangles(const NphMeshView *mesh, NphRay local, u32 first, u32 count, f32 *best)
{
    const NphMeshHeader *h = mesh->header;
    bool hit = false;
    for (u32 i = first; i + 2U < first + count; i += 3U) {
        const NphVec3 a = nph_mesh_dequantize_position(h, mesh->positions[mesh->indices[i]]);
        const NphVec3 b = nph_mesh_dequantize_position(h, mesh->positions[mesh->indices[i + 1U]]);
        const NphVec3 c = nph_mesh_dequantize_position(h, mesh->positions[mesh->indices[i + 2U]]);
        f32 tt = 0.0F;
        if (nph_ray_intersects_triangle(local, a, b, c, false, &tt, NULL, NULL) && tt >= 0.0F &&
            tt < *best) {
            *best = tt;
            hit = true;
        }
    }
    return hit;
}

// The nearest hit of the ray (relative to the entity's origin, in world
// axes) against `mesh` under the entity's linear transform, as a parameter
// along `dir`, and the material slot of the submesh it hit; false for none.
static bool hit_mesh(const NphMeshView *mesh, const NphWorldTransform *t, NphVec3 origin,
                     NphVec3 dir, f32 *out_t, u32 *out_slot)
{
    NphMat3 inv;
    if (!nph_mat3_inverse(nph_world_transform_linear(t), &inv)) {
        return false; // flattened to nothing: nothing to hit
    }
    // In the mesh's space the ray keeps its parameter: points along it are
    // origin + t * dir in both spaces.
    const NphRay local = nph_ray(nph_mat3_mul_vec3(inv, origin), nph_mat3_mul_vec3(inv, dir));
    if (!nph_ray_intersects_aabb(local, mesh->header->bounds, 1e30F, NULL, NULL)) {
        return false;
    }
    const NphMeshHeader *h = mesh->header;
    bool hit = false;
    f32 best = 1e30F;
    u32 slot = 0U;
    if (h->submesh_count == 0U || mesh->submeshes == NULL) {
        hit = hit_triangles(mesh, local, 0U, h->index_count, &best);
    }
    // Submesh by submesh, for the slot: nph_mesh_view checked each one's
    // range against the indices.
    for (u32 s = 0; mesh->submeshes != NULL && s < h->submesh_count; ++s) {
        const NphSubmesh *sm = &mesh->submeshes[s];
        if (hit_triangles(mesh, local, sm->index_offset, sm->index_count, &best)) {
            hit = true;
            slot = sm->material_slot;
        }
    }
    *out_t = best;
    *out_slot = slot;
    return hit;
}

bool editor_pick(const NphWorld *w, const NphRuntimeComponents *rc, EditorMeshLookup lookup,
                 void *user, NphDVec3 eye, NphVec3 dir, EditorPick *out)
{
    out->entity = NPH_ENTITY_NULL;
    out->distance = 0.0;
    out->slot = 0U;
    f32 best = 1e30F;
    for (u32 i = 0; i < nph_world_entity_slot_count(w); ++i) {
        const NphEntity e = nph_world_entity_at(w, i);
        const NphWorldTransform *t =
            NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_WORLD_TRANSFORM, NphWorldTransform);
        if (t == NULL) {
            continue;
        }
        // The ray from the eye, relative to the entity's origin: f32 holds the
        // offset wherever the two are in the world.
        const NphVec3 origin = nph_dvec3_relative(eye, t->translation);
        const NphMeshRenderer *mr = NPH_WORLD_GET(w, e, rc->mesh_renderer, NphMeshRenderer);
        NphMeshView mesh;
        f32 tt = 0.0F;
        u32 slot = 0U;
        bool hit = false;
        if (mr != NULL && lookup != NULL && lookup(user, e, &mesh)) {
            hit = !mr->hidden && hit_mesh(&mesh, t, origin, dir, &tt, &slot);
        } else if (nph_world_has(w, e, rc->light) || nph_world_has(w, e, rc->camera)) {
            const NphSphere marker = {nph_vec3_zero(), EDITOR_PICK_MARKER_RADIUS};
            hit = nph_ray_intersects_sphere(nph_ray(origin, dir), marker, &tt);
        } else if (nph_world_has(w, e, rc->player_start)) {
            // Its figure (the viewport's marker): two balls up its height.
            tt = FLT_MAX;
            for (u32 k = 0U; k < 2U; ++k) {
                const NphSphere part = {
                    nph_vec3(0.0F, EDITOR_PICK_FIGURE_BALL * (1.0F + 2.0F * (f32)k), 0.0F),
                    EDITOR_PICK_FIGURE_BALL};
                f32 tk = 0.0F;
                if (nph_ray_intersects_sphere(nph_ray(origin, dir), part, &tk) && tk < tt) {
                    tt = tk;
                    hit = true;
                }
            }
        }
        if (hit && tt < best) {
            best = tt;
            out->entity = e;
            out->distance = (f64)tt;
            out->slot = slot;
        }
    }
    return !nph_entity_is_null(out->entity);
}
