// SPDX-License-Identifier: Apache-2.0
// What a ray of the radiance cache's finds (docs/render.md "Global illumination"; ADR-0039),
// for its update (gi_update.comp) and the reflections' turns (reflect_turns.comp, ADR-0077).
// The includer has the view's records, the cells and ray queries (rt_mask.glsl: its rays
// pass where an alpha-tested surface is cut away).
#ifndef NPH_GI_HIT_GLSL
#define NPH_GI_HIT_GLSL

// The environment's radiance in direction `w`: a level of its cube about 16
// texels across a face.
vec3 environment(ViewRef rview, vec3 w)
{
    if ((rview.v.flags & NPH_RENDER_VIEW_IBL) == 0u) {
        return vec3(0.0);
    }
    const uint tex = rview.v.env_index;
    const uint smp = rview.v.clamp_sampler;
    const float lod = max(float(textureQueryLevels(samplerCube(
                              nph_textures_cube[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]))) -
                              5.0,
                          0.0);
    return textureLod(samplerCube(nph_textures_cube[nonuniformEXT(tex)],
                                  nph_samplers[nonuniformEXT(smp)]),
                      w, lod)
               .rgb *
           rview.v.ibl_intensity;
}

// A material texture at `uv` and level of detail `lod` (before the
// texture's own size), or 1 without the texture.
vec4 material_texel(uint tex, uint smp, vec2 uv, float lod)
{
    if (tex == NPH_RENDER_NO_TEXTURE) {
        return vec4(1.0);
    }
    const vec2 dim = vec2(textureSize(nph_textures_2d[nonuniformEXT(tex)], 0));
    return textureLod(sampler2D(nph_textures_2d[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]),
                      uv, lod + 0.5 * log2(dim.x * dim.y));
}

// The radiance leaving the ray query's committed hit back along `w`. Its textures are read at the
// level whose texels are `footprint` wide there, the width the ray stands for (ADR-0089); 0: the
// size of the hit's cell, for a ray that fills one.
vec3 hit_radiance(VisViewRef view, ViewRef rview, GiViewRef g, rayQueryEXT q, vec3 w, float footprint)
{
    // The hit's triangle: the instance its record names (the custom index
    // is the slot), the submesh its geometry is, the triangle in that.
    const uint slot = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
    const uint geometry = rayQueryGetIntersectionGeometryIndexEXT(q, true);
    const uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
    const vec2 bc = rayQueryGetIntersectionBarycentricsEXT(q, true);
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[slot];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec2 sub = VisUvec2Ref(mesh.submeshes).v[geometry];
    VisUintRef indices = VisUintRef(mesh.indices);
    const uint first = sub.y + 3u * primitive;
    const uvec3 vtx = uvec3(indices.v[first], indices.v[first + 1u], indices.v[first + 2u]);
    const vec3 b = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);

    // Its point, camera relative as the cells are keyed, and its normals.
    const vec3 t =
        vis_relative_translation(inst.transform, view.v.camera_high.xyz, view.v.camera_low.xyz);
    const vec3 w0 = vis_transform_vector(inst.transform, vis_position(mesh, vtx.x)) + t;
    const vec3 w1 = vis_transform_vector(inst.transform, vis_position(mesh, vtx.y)) + t;
    const vec3 w2 = vis_transform_vector(inst.transform, vis_position(mesh, vtx.z)) + t;
    const vec3 x = mat3(w0, w1, w2) * b;
    const vec3 edge_normal = cross(w1 - w0, w2 - w0);
    vec3 face = normalize(edge_normal);
    face = dot(face, w) > 0.0 ? -face : face; // toward the ray's origin
    AttribsRef attribs = AttribsRef(mesh.attribs);
    const uvec4 a0 = attribs.a[vtx.x];
    const uvec4 a1 = attribs.a[vtx.y];
    const uvec4 a2 = attribs.a[vtx.z];
    vec3 n = vis_normal_matrix(inst.transform) *
             (mat3(nph_oct_decode(unpackSnorm2x16(a0.x)), nph_oct_decode(unpackSnorm2x16(a1.x)),
                   nph_oct_decode(unpackSnorm2x16(a2.x))) *
              b);
    n = normalize(dot(n, face) < 0.0 ? -n : n);
    const mat3x2 uvs = mat3x2(unpackHalf2x16(a0.z), unpackHalf2x16(a1.z), unpackHalf2x16(a2.z));
    const vec2 uv = uvs * b;
    const vec3 color =
        (mat3x4(unpackUnorm4x8(a0.w), unpackUnorm4x8(a1.w), unpackUnorm4x8(a2.w)) * b).rgb;

    // Its material, field by field (it holds arrays: docs/build.md
    // "Shaders"), textures at the level the hit's cell resolves.
    const uint mi = vis_instance_material(view, inst, sub.x);
    MaterialsRef materials = MaterialsRef(view.v.materials);
    const float cell = footprint > 0.0 ? footprint : gi_cell_size(g, gi_level(length(x)));
    const vec2 duv1 = uvs[1] - uvs[0];
    const vec2 duv2 = uvs[2] - uvs[0];
    const float uv_area = abs(duv1.x * duv2.y - duv1.y * duv2.x);
    const float lod =
        log2(cell) + 0.5 * log2(max(uv_area, 1e-12) / max(length(edge_normal), 1e-12));
    const vec3 albedo =
        materials.m[mi].base_color.rgb * color *
        material_texel(materials.m[mi].textures[NPH_RENDER_TEX_BASE_COLOR],
                       materials.m[mi].samplers[NPH_RENDER_TEX_BASE_COLOR], uv, lod)
            .rgb *
        (1.0 - clamp(materials.m[mi].metallic, 0.0, 1.0));
    const vec3 emissive = materials.m[mi].emissive.rgb *
                          material_texel(materials.m[mi].textures[NPH_RENDER_TEX_EMISSIVE],
                                         materials.m[mi].samplers[NPH_RENDER_TEX_EMISSIVE], uv, lod)
                              .rgb;

    // The irradiance it reflects: the sun's, and the cache's (the bounces
    // before this frame), else the environment's.
    vec3 irradiance = vec3(0.0);
    const vec4 sun = g.g.sun_direction;
    if (sun.w >= 0.0) {
        const vec3 l = -sun.xyz;
        const float nol = dot(n, l);
        if (nol > 0.0 && dot(face, l) > 0.0) {
            const float lit =
                sun.w > 0.0 ? nph_shadow_ray_masked(view, rview.v.tlas,
                                                    x + rview.v.tlas_offset.xyz, face, l, 1e30)
                            : 1.0;
            irradiance += g.g.sun_color.rgb * (nol * lit);
        }
    }
    const vec3 sky = (rview.v.flags & NPH_RENDER_VIEW_IBL) != 0u
                         ? nph_sh9_irradiance(ShRef(rview.v.sh_address), n) * rview.v.ibl_intensity
                         : vec3(0.0);
    irradiance += gi_irradiance(g, x, face, sky);
    return emissive + albedo * NPH_INV_PI * irradiance;
}

#endif
