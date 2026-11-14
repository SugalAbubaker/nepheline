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

// What a ray met, for the light it sends back along the ray.
struct GiHit {
    vec3 x;      // the point, camera relative
    vec3 face;   // its triangle's normal, toward the ray's origin
    vec3 n;      // its shading normal, on that side
    vec3 direct; // the radiance it sends back of its own and of the sun's light
    vec3 albedo; // what it reflects evenly of the light it receives
    vec3 cached; // the irradiance the cache holds for it (lux), if `known`
    bool known;  // whether the cache holds a cell for it, with light
    uint age;    // the frames since a sample reached that cell
    int level;   // its level, and its cell's slot and its cell's parent's (GI_NONE: none)
    uint slot;
    uint parent;
};

// The ray query's committed hit, met along `w`. Its textures are read at the level whose texels
// are `footprint` wide there, the width the ray stands for (ADR-0089); 0: the size of the hit's
// cell, for a ray that fills one.
GiHit hit_surface(VisViewRef view, ViewRef rview, GiViewRef g, rayQueryEXT q, vec3 w, float footprint)
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
    vec3 b;
    const uint first = vis_ray_triangle(mesh, sub, primitive, bc, b);
    const uvec3 vtx = uvec3(indices.v[first], indices.v[first + 1u], indices.v[first + 2u]);

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
    // What it reflects evenly of the light it receives: its base colour, a metal's too. A metal
    // reflects by its base colour as a mirror or a lobe does, not evenly; sent on evenly it is
    // the same light in all, where a hit that reflected nothing left metal black in what
    // reflects it and took its light out of the bounce. (Nor is a material's metalness its
    // factor alone: the factor of a material whose metalness is a texture's is 1, and read
    // without the texture every such surface was a metal that reflected nothing. A city whose
    // materials are all so had no bounced light at all: its shade was the sky's alone, and the
    // shaded side of a bus near black: ADR-0092.)
    const vec3 albedo =
        materials.m[mi].base_color.rgb * color *
        material_texel(materials.m[mi].textures[NPH_RENDER_TEX_BASE_COLOR],
                       materials.m[mi].samplers[NPH_RENDER_TEX_BASE_COLOR], uv, lod)
            .rgb;
    const vec3 emissive = materials.m[mi].emissive.rgb *
                          material_texel(materials.m[mi].textures[NPH_RENDER_TEX_EMISSIVE],
                                         materials.m[mi].samplers[NPH_RENDER_TEX_EMISSIVE], uv, lod)
                              .rgb;

    // The sun's light it reflects, and the cache's (the bounces before this frame) where the
    // cache holds its cell.
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
    GiHit h;
    h.x = x;
    h.face = face;
    h.n = n;
    h.direct = emissive + albedo * NPH_INV_PI * irradiance;
    h.albedo = albedo;
    // Its cell's light, or its cell's parent's (ADR-0090), which is there more often: a surface
    // the camera sees at a low angle has samples a pixel apart on the screen and many cells apart
    // on the surface, and most of its cells hold none; and a surface no frame has seen has only
    // the parents the update's rays made for it (gi_update.comp).
    h.level = gi_level(length(x));
    h.slot = gi_find(g, gi_key_at(g, x, face, h.level));
    h.parent = h.level + 1 < NPH_RENDER_GI_LEVELS ? gi_find(g, gi_key_at(g, x, face, h.level + 1))
                                                   : GI_NONE;
    uvec4 held = h.slot != GI_NONE ? GiCellsRef(g.g.cells).c[h.slot] : uvec4(0u);
    if (gi_word_count(held.w) == 0u && h.parent != GI_NONE) {
        held = GiCellsRef(g.g.cells).c[h.parent];
    }
    h.cached = uintBitsToFloat(held.xyz);
    h.known = gi_word_count(held.w) != 0u;
    h.age = gi_word_age(held.w);
    return h;
}

// The radiance leaving the ray query's committed hit back along `w` (hit_surface). Where the
// cache holds no cell for the hit (a surface the frames have not seen), the indirect light it
// reflects is the environment's as if nothing hid it: the reflections' rays', which show the
// surface and little of its light's error; the cache's own update traces on instead
// (gi_update.comp, ADR-0090).
vec3 hit_radiance(VisViewRef view, ViewRef rview, GiViewRef g, rayQueryEXT q, vec3 w, float footprint)
{
    const GiHit h = hit_surface(view, rview, g, q, w, footprint);
    const vec3 sky = (rview.v.flags & NPH_RENDER_VIEW_IBL) != 0u
                         ? nph_sh9_irradiance(ShRef(rview.v.sh_address), h.n) * rview.v.ibl_intensity
                         : vec3(0.0);
    return h.direct + h.albedo * NPH_INV_PI * (h.known ? h.cached : sky);
}

#endif
