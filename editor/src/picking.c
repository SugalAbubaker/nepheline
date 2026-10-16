// SPDX-License-Identifier: Apache-2.0
// Picking. See picking.h.
#include "picking.h"

#include <nepheline/math/geometry.h>
#include <nepheline/math/mat.h>
#include <nepheline/scene/transform.h>

// The nearest hit of the ray (relative to the entity's origin, in world
// axes) against `mesh` under the entity's linear transform, as a parameter
// along `dir`; false for none.
static bool hit_mesh(const NphMeshView *mesh, const NphWorldTransform *t, NphVec3 origin,
                     NphVec3 dir, f32 *out_t)
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
    bool hit = false;
    f32 best = 1e30F;
    const NphMeshHeader *h = mesh->header;
    for (u32 i = 0; i + 2U < h->index_count; i += 3U) {
        const NphVec3 a = nph_mesh_dequantize_position(h, mesh->positions[mesh->indices[i]]);
        const NphVec3 b = nph_mesh_dequantize_position(h, mesh->positions[mesh->indices[i + 1U]]);
        const NphVec3 c = nph_mesh_dequantize_position(h, mesh->positions[mesh->indices[i + 2U]]);
        f32 tt = 0.0F;
        if (nph_ray_intersects_triangle(local, a, b, c, false, &tt, NULL, NULL) && tt >= 0.0F &&
            tt < best) {
            best = tt;
            hit = true;
        }
    }
    *out_t = best;
    return hit;
}

bool editor_pick(const NphWorld *w, const NphRuntimeComponents *rc, EditorMeshLookup lookup,
                 void *user, NphDVec3 eye, NphVec3 dir, EditorPick *out)
{
    out->entity = NPH_ENTITY_NULL;
    out->distance = 0.0;
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
        bool hit = false;
        if (mr != NULL && lookup != NULL && lookup(user, e, &mesh)) {
            hit = !mr->hidden && hit_mesh(&mesh, t, origin, dir, &tt);
        } else if (nph_world_has(w, e, rc->light) || nph_world_has(w, e, rc->camera)) {
            const NphSphere marker = {nph_vec3_zero(), EDITOR_PICK_MARKER_RADIUS};
            hit = nph_ray_intersects_sphere(nph_ray(origin, dir), marker, &tt);
        }
        if (hit && tt < best) {
            best = tt;
            out->entity = e;
            out->distance = (f64)tt;
        }
    }
    return !nph_entity_is_null(out->entity);
}
