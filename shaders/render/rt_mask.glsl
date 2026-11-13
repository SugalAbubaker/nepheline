// SPDX-License-Identifier: Apache-2.0
// Rays and alpha-tested and blended surfaces (docs/render.md "Shadow rays"). The top level
// marks an instance that has a MASK or a BLEND material as not opaque (rt_instances.comp),
// so a ray query hands back each of its triangles the ray meets as a
// candidate, and the ray goes on where the material's alpha test fails there,
// as the raster discards the fragment (vis_raster_frag.glsl): a leaf's card
// stops a ray only where its leaf is. It goes on through a blended surface
// altogether: glass, and a decal a hair before its wall, light as the eye
// passes. Every other instance stays opaque to the
// traversal and costs what it did. The includer has the GPU scene's records
// (vis_common.glsl) and ray queries (rt_common.glsl), and starts its queries
// without gl_RayFlagsOpaqueEXT.
#ifndef NPH_RT_MASK_GLSL
#define NPH_RT_MASK_GLSL

// Whether the candidate triangle of `q` stops its ray: not where its material
// is alpha tested and the base colour's alpha at the hit (the factor, the
// vertex colour, the texture) is under the cutoff, the raster's test. The
// texture is read at the finest level its sampler allows, a point of it and
// not a footprint: a ray is one sample, and the mean of many is the coverage
// the raster's pixels see, which a filtered alpha against the cutoff is not.
bool nph_ray_candidate_covers(VisViewRef view, rayQueryEXT q)
{
    const uint slot = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);
    const uint geometry = rayQueryGetIntersectionGeometryIndexEXT(q, false);
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[slot];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec2 sub = VisUvec2Ref(mesh.submeshes).v[geometry];
    const uint mi = vis_instance_material(view, inst, sub.x);
    MaterialsRef materials = MaterialsRef(view.v.materials);
    if (materials.m[mi].alpha_mode == 2u) {
        return false; // a blended surface (glass, a decal) stops no ray
    }
    if (materials.m[mi].alpha_mode != 1u) {
        return true; // an opaque submesh of an instance that has a masked or blended one
    }
    const uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(q, false);
    const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(q, false);
    const vec3 b = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    VisUintRef indices = VisUintRef(mesh.indices);
    const uint first = sub.y + 3u * primitive;
    AttribsRef attribs = AttribsRef(mesh.attribs);
    const uvec4 a0 = attribs.a[indices.v[first]];
    const uvec4 a1 = attribs.a[indices.v[first + 1u]];
    const uvec4 a2 = attribs.a[indices.v[first + 2u]];
    float alpha = materials.m[mi].base_color.a *
                  dot(vec3(unpackUnorm4x8(a0.w).a, unpackUnorm4x8(a1.w).a, unpackUnorm4x8(a2.w).a),
                      b);
    const uint tex = materials.m[mi].textures[NPH_RENDER_TEX_BASE_COLOR];
    if (tex != NPH_RENDER_NO_TEXTURE) {
        const vec2 uv =
            mat3x2(unpackHalf2x16(a0.z), unpackHalf2x16(a1.z), unpackHalf2x16(a2.z)) * b;
        alpha *=
            nph_sample_2d_lod(tex, materials.m[mi].samplers[NPH_RENDER_TEX_BASE_COLOR], uv, 0.0).a;
    }
    return !(alpha < materials.m[mi].alpha_cutoff);
}

// Runs the query `q` to its end, committing each candidate that stops the ray.
#define NPH_RAY_TRAVERSE(view, q)                                                                  \
    while (rayQueryProceedEXT(q)) {                                                                \
        if (rayQueryGetIntersectionTypeEXT(q, false) ==                                            \
                gl_RayQueryCandidateIntersectionTriangleEXT &&                                     \
            nph_ray_candidate_covers(view, q)) {                                                   \
            rayQueryConfirmIntersectionEXT(q);                                                     \
        }                                                                                          \
    }

// nph_shadow_ray (rt_common.glsl) through alpha-tested surfaces: 1 when
// nothing that stops the ray lies between `p` and the light within `distance`.
float nph_shadow_ray_masked(VisViewRef view, uint64_t tlas, vec3 p, vec3 face_normal, vec3 l,
                            float distance)
{
    const vec3 n = dot(face_normal, l) >= 0.0 ? face_normal : -face_normal;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(tlas), gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFFu, nph_offset_ray(p, n), 0.0, l, distance);
    NPH_RAY_TRAVERSE(view, q)
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0
                                                                                             : 0.0;
}

#endif
