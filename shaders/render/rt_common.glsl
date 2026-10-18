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

// 1 when nothing lies between `p` (in the structure's space, on a surface
// whose geometric normal is `face_normal`) and the light in direction `l`
// within `distance`, else 0. Any hit ends the query: an occluder is an
// occluder.
float nph_shadow_ray(uint64_t tlas, vec3 p, vec3 face_normal, vec3 l, float distance)
{
    const vec3 n = dot(face_normal, l) >= 0.0 ? face_normal : -face_normal;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(tlas),
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFFu,
                          nph_offset_ray(p, n), 0.0, l, distance);
    while (rayQueryProceedEXT(q)) {
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0
                                                                                             : 0.0;
}

#endif
