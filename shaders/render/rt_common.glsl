// SPDX-License-Identifier: Apache-2.0
// Ray queries against the renderer's top-level acceleration structure
// (docs/render.md "Shadows"), for the shaders compiled with them
// (NPH_RAY_QUERY: the *_rt variants). The structure holds every instance
// in a space of its own, the camera's shifted by NphRenderViewData.
// tlas_offset (a point of the view is p + tlas_offset there), and is
// reached by its device address (NphRenderViewData.tlas).
// The includer enables GL_EXT_ray_query right after its #version (an
// extension directive must come before any declaration).
#ifndef NPH_RT_COMMON_GLSL
#define NPH_RT_COMMON_GLSL

#ifndef GL_EXT_ray_query
#error "enable GL_EXT_ray_query before including rt_common.glsl"
#endif

// A ray's origin moved off the surface at `p` along `n` (the side the ray
// leaves from) by a few units in the last place of each coordinate, so a
// ray never hits the surface it starts on whatever its distance from the
// camera, and by a fixed amount near zero, where units in the last place
// vanish (Wächter & Binder, "A Fast and Robust Method for Avoiding
// Self-Intersection", Ray Tracing Gems, 2019, chapter 6).
vec3 nph_offset_ray(vec3 p, vec3 n)
{
    const float origin = 1.0 / 32.0;
    const float float_scale = 1.0 / 65536.0;
    const float int_scale = 256.0;
    const ivec3 of = ivec3(int_scale * n);
    const vec3 pi = vec3(intBitsToFloat(floatBitsToInt(p.x) + (p.x < 0.0 ? -of.x : of.x)),
                         intBitsToFloat(floatBitsToInt(p.y) + (p.y < 0.0 ? -of.y : of.y)),
                         intBitsToFloat(floatBitsToInt(p.z) + (p.z < 0.0 ? -of.z : of.z)));
    return vec3(abs(p.x) < origin ? p.x + float_scale * n.x : pi.x,
                abs(p.y) < origin ? p.y + float_scale * n.y : pi.y,
                abs(p.z) < origin ? p.z + float_scale * n.z : pi.z);
}

#ifdef NPH_VIS_COMMON_GLSL
// Whether the ray query's candidate triangle stops its ray. The production
// path's top level forces the triangles of an instance with no alpha-tested
// material opaque (rt_instances.comp), so a candidate is a triangle of an
// instance that has one: a MASK material's stops the ray where its alpha (the
// factor's, the vertex colour's, the base colour texture's) is not below the
// cutoff, as the raster tests it (vis_raster_frag.glsl), and any other
// material's always does. The texture is read at its finest level: what the
// material draws up close, where the raster's level depends on the pixel.
// `scene` is the GPU scene's view record (NphVisViewData), whose tables name
// the candidate's material.
bool nph_rt_candidate_stops(uint64_t scene, rayQueryEXT q)
{
    VisViewRef view = VisViewRef(scene);
    const NphVisInstance inst =
        VisInstancesRef(view.v.instances).i[rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec2 sub = VisUvec2Ref(mesh.submeshes).v[rayQueryGetIntersectionGeometryIndexEXT(q, false)];
    const uint mi = vis_instance_material(view, inst, sub.x);
    MaterialsRef materials = MaterialsRef(view.v.materials);
    if (materials.m[mi].alpha_mode != 1u) {
        return true;
    }
    const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(q, false);
    const vec3 b = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    VisUintRef indices = VisUintRef(mesh.indices);
    const uint first = sub.y + 3u * rayQueryGetIntersectionPrimitiveIndexEXT(q, false);
    AttribsRef attribs = AttribsRef(mesh.attribs);
    const uvec4 a0 = attribs.a[indices.v[first]];
    const uvec4 a1 = attribs.a[indices.v[first + 1u]];
    const uvec4 a2 = attribs.a[indices.v[first + 2u]];
    float alpha = materials.m[mi].base_color.a *
                  dot(vec3(unpackUnorm4x8(a0.w).a, unpackUnorm4x8(a1.w).a, unpackUnorm4x8(a2.w).a), b);
    const uint tex = materials.m[mi].textures[NPH_RENDER_TEX_BASE_COLOR];
    if (tex != NPH_RENDER_NO_TEXTURE) {
        const uint smp = materials.m[mi].samplers[NPH_RENDER_TEX_BASE_COLOR];
        const vec2 uv = mat3x2(unpackHalf2x16(a0.z), unpackHalf2x16(a1.z), unpackHalf2x16(a2.z)) * b;
        alpha *= textureLod(sampler2D(nph_textures_2d[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]),
                            uv, 0.0)
                     .a;
    }
    return !(alpha < materials.m[mi].alpha_cutoff);
}

// Runs the ray query `q` to its end, each candidate confirmed where it stops
// the ray (nph_rt_candidate_stops; with `scene` 0, every one does).
#define NPH_RT_PROCEED(q, scene)                                                                   \
    while (rayQueryProceedEXT(q)) {                                                                \
        if ((scene) == 0ul || nph_rt_candidate_stops((scene), (q))) {                              \
            rayQueryConfirmIntersectionEXT(q);                                                     \
        }                                                                                          \
    }
#else
// Without the GPU scene's tables (the reference renderer's shaders): every
// candidate stops the ray, as every triangle does.
#define NPH_RT_PROCEED(q, scene)                                                                   \
    while (rayQueryProceedEXT(q)) {                                                                \
        rayQueryConfirmIntersectionEXT(q);                                                         \
    }
#endif

// 1 when nothing lies between `p` (in the structure's space, on a surface
// whose geometric normal is `face_normal`) and the light in direction `l`
// within `distance`, else 0. Any hit that stops the ray ends the query: an
// occluder is an occluder. `scene` is the GPU scene's view record, through
// which the rays pass the alpha-tested materials where their alpha does
// (nph_rt_candidate_stops); 0, every triangle is opaque (the reference
// renderer, whose shaders do not have those tables).
float nph_shadow_ray(uint64_t tlas, uint64_t scene, vec3 p, vec3 face_normal, vec3 l, float distance)
{
    const vec3 n = dot(face_normal, l) >= 0.0 ? face_normal : -face_normal;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(tlas),
                          gl_RayFlagsTerminateOnFirstHitEXT | (scene == 0ul ? gl_RayFlagsOpaqueEXT : 0u),
                          0xFFu, nph_offset_ray(p, n), 0.0, l, distance);
    NPH_RT_PROCEED(q, scene)
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0
                                                                                             : 0.0;
}

#endif
