// SPDX-License-Identifier: Apache-2.0
// glTF import (gltf.h): cgltf parses and validates; this file converts the
// document into the cooker's intermediate form and checks every value it
// takes from it.
#include <nepheline/core/assert.h>
#include <nepheline/core/hash.h>
#include <nepheline/math/affine.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/path.h>

#include <cgltf.h>
#include <cook/gltf.h>
#include <stdio.h>
#include <string.h>

// Positions per primitive: beyond this the importer refuses rather than
// allocate for a hostile count (cgltf has checked the bytes exist).
#define GLTF_MAX_VERTICES (1U << 24U)

// ---------------------------------------------------------------------------
// cgltf callbacks: memory through the caller's allocator, files through
// the engine's file API
// ---------------------------------------------------------------------------

static void *gltf_alloc(void *user, cgltf_size size)
{
    return cook_block_alloc((NphAllocator *)user, (u64)size);
}

static void gltf_free(void *user, void *ptr)
{
    cook_block_free((NphAllocator *)user, ptr);
}

static cgltf_result gltf_file_read(const struct cgltf_memory_options *memory_options,
                                   const struct cgltf_file_options *file_options, const char *path,
                                   cgltf_size *size, void **data)
{
    NPH_UNUSED(file_options);
    NphAllocator *alloc = (NphAllocator *)memory_options->user_data;
    NphFile f;
    if (nph_file_open(path, NPH_FILE_READ | NPH_FILE_SEQUENTIAL, &f) != NPH_RESULT_OK) {
        return cgltf_result_file_not_found;
    }
    u64 file_size = 0U;
    if (nph_file_size(&f, &file_size) != NPH_RESULT_OK) {
        nph_file_close(&f);
        return cgltf_result_io_error;
    }
    void *buf = cook_block_alloc(alloc, file_size);
    if (buf == NULL) {
        nph_file_close(&f);
        return cgltf_result_out_of_memory;
    }
    u64 got = 0U;
    const NphResult r = nph_file_read(&f, buf, file_size, &got);
    nph_file_close(&f);
    if (r != NPH_RESULT_OK || got != file_size) {
        cook_block_free(alloc, buf);
        return cgltf_result_io_error;
    }
    *size = (cgltf_size)file_size;
    *data = buf;
    return cgltf_result_success;
}

static void gltf_file_release(const struct cgltf_memory_options *memory_options,
                              const struct cgltf_file_options *file_options, void *data)
{
    NPH_UNUSED(file_options);
    cook_block_free((NphAllocator *)memory_options->user_data, data);
}

static void gltf_options(NphAllocator *alloc, cgltf_options *out)
{
    memset(out, 0, sizeof *out);
    out->memory.alloc_func = gltf_alloc;
    out->memory.free_func = gltf_free;
    out->memory.user_data = alloc;
    out->file.read = gltf_file_read;
    out->file.release = gltf_file_release;
}

static const char *gltf_result_name(cgltf_result r)
{
    switch (r) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "data too short";
    case cgltf_result_unknown_format:
        return "unknown format";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "legacy glTF 1.0";
    default:
        return "unknown error";
    }
}

static NphResult gltf_result_to_nph(cgltf_result r)
{
    switch (r) {
    case cgltf_result_success:
        return NPH_RESULT_OK;
    case cgltf_result_file_not_found:
        return NPH_RESULT_NOT_FOUND;
    case cgltf_result_io_error:
        return NPH_RESULT_IO_ERROR;
    case cgltf_result_out_of_memory:
        return NPH_RESULT_OUT_OF_MEMORY;
    default:
        return NPH_RESULT_PARSE_ERROR;
    }
}

// ---------------------------------------------------------------------------
// Extensions
// ---------------------------------------------------------------------------

// Extensions whose content the importer implements (or, for mesh
// quantisation, reads through cgltf's component conversion).
static bool extension_supported(const char *name)
{
    static const char *const supported[] = {
        "KHR_lights_punctual",
        "KHR_materials_unlit",
        "KHR_materials_emissive_strength",
        "KHR_mesh_quantization",
    };
    for (u32 i = 0; i < NPH_ARRAY_COUNT(supported); ++i) {
        if (strcmp(name, supported[i]) == 0) {
            return true;
        }
    }
    return false;
}

static NphResult check_extensions(CookGltfScene *scene, const cgltf_data *data)
{
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const char *name = data->extensions_required[i];
        if (name == NULL || !extension_supported(name)) {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "glTF requires %s, which the importer does not implement",
                          name != NULL ? name : "(unnamed)");
            return NPH_RESULT_UNSUPPORTED;
        }
    }
    for (cgltf_size i = 0; i < data->extensions_used_count; ++i) {
        const char *name = data->extensions_used[i];
        if (name != NULL && !extension_supported(name)) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "glTF uses %s, which the importer ignores", name);
            scene->warnings += 1U;
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Names and attributes
// ---------------------------------------------------------------------------

static void copy_name(char *out, u64 cap, const char *name, const char *kind, u64 index)
{
    if (name != NULL && name[0] != '\0') {
        const u64 n = strlen(name);
        const u64 keep = n < cap - 1U ? n : cap - 1U;
        memcpy(out, name, (size_t)keep);
        out[keep] = '\0';
    } else {
        snprintf(out, (size_t)cap, "%s_%llu", kind, (unsigned long long)index);
    }
}

static const cgltf_accessor *find_attribute(const cgltf_primitive *prim, cgltf_attribute_type type,
                                            i32 set)
{
    for (cgltf_size i = 0; i < prim->attributes_count; ++i) {
        const cgltf_attribute *a = &prim->attributes[i];
        if (a->type == type && a->index == set) {
            return a->data;
        }
    }
    return NULL;
}

// The accessor's elements as floats (sparse substitutions applied and
// integer components converted as the specification says), `components`
// per element. NULL when the accessor is not of that shape or cannot be
// read (an undecoded compressed buffer view, say).
static f32 *unpack_floats(NphAllocator *alloc, const cgltf_accessor *a, u32 components)
{
    if (a == NULL || cgltf_num_components(a->type) != components || a->count == 0U ||
        a->count > GLTF_MAX_VERTICES) {
        return NULL;
    }
    const cgltf_size n = (cgltf_size)a->count * components;
    f32 *out = NPH_ALLOC_ARRAY(alloc, f32, (u32)n);
    if (out == NULL) {
        return NULL;
    }
    if (cgltf_accessor_unpack_floats(a, out, n) != n) {
        NPH_FREE_ARRAY(alloc, f32, out, (u32)n);
        return NULL;
    }
    return out;
}

static void free_floats(NphAllocator *alloc, f32 *p, const cgltf_accessor *a, u32 components)
{
    if (p != NULL) {
        NPH_FREE_ARRAY(alloc, f32, p, (u32)(a->count * components));
    }
}

// The primitive's triangle list: its indices (or 0..n-1) expanded from
// lists, strips, and fans, into *out (NULL when there are none) with the
// count in *out_count. NPH_RESULT_UNSUPPORTED for sparse indices,
// NPH_RESULT_PARSE_ERROR for unreadable or out-of-range ones.
static NphResult primitive_triangles(NphAllocator *alloc, const cgltf_primitive *prim,
                                     u32 vertex_count, u32 **out, u32 *out_count)
{
    *out = NULL;
    *out_count = 0U;
    u32 n = vertex_count;
    u32 *src = NULL;
    if (prim->indices != NULL) {
        const cgltf_accessor *ia = prim->indices;
        if (ia->is_sparse) {
            return NPH_RESULT_UNSUPPORTED;
        }
        if (ia->count > (cgltf_size)NPH_U32_MAX / 3U) {
            return NPH_RESULT_PARSE_ERROR;
        }
        n = (u32)ia->count;
        if (n == 0U) {
            return NPH_RESULT_OK;
        }
        src = NPH_ALLOC_ARRAY(alloc, u32, n);
        if (src == NULL) {
            return NPH_RESULT_OUT_OF_MEMORY;
        }
        if (cgltf_accessor_unpack_indices(ia, src, sizeof(u32), n) != n) {
            NPH_FREE_ARRAY(alloc, u32, src, n);
            return NPH_RESULT_PARSE_ERROR;
        }
    }
    u32 count = 0U;
    if (prim->type == cgltf_primitive_type_triangles) {
        count = n - n % 3U;
    } else if (n >= 3U) {
        count = (n - 2U) * 3U; // strips and fans: one triangle per vertex past the second
    }
    NphResult r = NPH_RESULT_OK;
    u32 *tris = NULL;
    if (count != 0U) {
        tris = NPH_ALLOC_ARRAY(alloc, u32, count);
        if (tris == NULL) {
            r = NPH_RESULT_OUT_OF_MEMORY;
        }
    }
    if (tris != NULL) {
#define INDEX(i) (src != NULL ? src[(i)] : (i))
        u32 w = 0U;
        if (prim->type == cgltf_primitive_type_triangles) {
            for (u32 i = 0; i < count; ++i) {
                tris[w++] = INDEX(i);
            }
        } else if (prim->type == cgltf_primitive_type_triangle_strip) {
            for (u32 i = 0; i + 2U < n; ++i) {
                // Odd triangles swap their first two corners to keep the winding.
                tris[w++] = INDEX((i & 1U) == 0U ? i : i + 1U);
                tris[w++] = INDEX((i & 1U) == 0U ? i + 1U : i);
                tris[w++] = INDEX(i + 2U);
            }
        } else {
            for (u32 i = 1; i + 1U < n; ++i) {
                tris[w++] = INDEX(0U);
                tris[w++] = INDEX(i);
                tris[w++] = INDEX(i + 1U);
            }
        }
#undef INDEX
        for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
            if (tris[i] >= vertex_count) {
                r = NPH_RESULT_PARSE_ERROR;
            }
        }
    }
    if (src != NULL) {
        NPH_FREE_ARRAY(alloc, u32, src, n);
    }
    if (r != NPH_RESULT_OK) {
        if (tris != NULL) {
            NPH_FREE_ARRAY(alloc, u32, tris, count);
        }
        return r;
    }
    *out = tris;
    *out_count = count;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

typedef struct PrimitiveAttribs {
    const cgltf_accessor *pos;
    const cgltf_accessor *nrm;
    const cgltf_accessor *tan; // NULL when absent or ignored (no normals)
    const cgltf_accessor *uv;
    const cgltf_accessor *col;
    u32 col_components;
} PrimitiveAttribs;

static bool usable_primitive(const cgltf_primitive *prim)
{
    return prim->type == cgltf_primitive_type_triangles ||
           prim->type == cgltf_primitive_type_triangle_strip ||
           prim->type == cgltf_primitive_type_triangle_fan;
}

static void primitive_attribs(const cgltf_primitive *prim, PrimitiveAttribs *out)
{
    memset(out, 0, sizeof *out);
    out->pos = find_attribute(prim, cgltf_attribute_type_position, 0);
    out->nrm = find_attribute(prim, cgltf_attribute_type_normal, 0);
    out->tan = find_attribute(prim, cgltf_attribute_type_tangent, 0);
    out->uv = find_attribute(prim, cgltf_attribute_type_texcoord, 0);
    out->col = find_attribute(prim, cgltf_attribute_type_color, 0);
    if (out->nrm == NULL) {
        out->tan = NULL; // glTF 2.0 §3.7.2.1: without normals, tangents MUST be ignored
    }
    if (out->col != NULL) {
        out->col_components = (u32)cgltf_num_components(out->col->type);
    }
}

// One primitive as a stand-alone CookMesh with every attribute it has;
// normals made flat when absent, tangents made when `want_tangents` and
// the primitive has texture coordinates but none of its own.
static NphResult convert_primitive(NphAllocator *alloc, const cgltf_primitive *prim,
                                   const PrimitiveAttribs *at, bool want_tangents, CookMesh *out)
{
    const u32 count = (u32)at->pos->count;
    f32 *pos = unpack_floats(alloc, at->pos, 3U);
    f32 *nrm = at->nrm != NULL ? unpack_floats(alloc, at->nrm, 3U) : NULL;
    f32 *tan = at->tan != NULL ? unpack_floats(alloc, at->tan, 4U) : NULL;
    f32 *uv = at->uv != NULL ? unpack_floats(alloc, at->uv, 2U) : NULL;
    f32 *col = at->col != NULL ? unpack_floats(alloc, at->col, at->col_components) : NULL;
    CookVertex *verts = NPH_ALLOC_ARRAY(alloc, CookVertex, count);
    u32 *tris = NULL;
    u32 tri_indices = 0U;
    NphResult r = primitive_triangles(alloc, prim, count, &tris, &tri_indices);
    if (r == NPH_RESULT_OK && verts == NULL) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    } else if (r == NPH_RESULT_OK &&
               (pos == NULL || (at->nrm != NULL && nrm == NULL) ||
                (at->tan != NULL && tan == NULL) || (at->uv != NULL && uv == NULL) ||
                (at->col != NULL && col == NULL) ||
                (at->col != NULL && at->col_components != 3U && at->col_components != 4U))) {
        r = NPH_RESULT_PARSE_ERROR; // an attribute of the wrong shape or unreadable
    }
    if (r == NPH_RESULT_OK) {
        for (u32 i = 0; i < count; ++i) {
            CookVertex *v = &verts[i];
            const u64 i2 = (u64)i * 2U;
            const u64 i3 = (u64)i * 3U;
            const u64 i4 = (u64)i * 4U;
            v->position = nph_vec3(pos[i3], pos[i3 + 1U], pos[i3 + 2U]);
            v->normal =
                nrm != NULL ? nph_vec3(nrm[i3], nrm[i3 + 1U], nrm[i3 + 2U]) : nph_vec3_unit_z();
            v->tangent = tan != NULL ? nph_vec4(tan[i4], tan[i4 + 1U], tan[i4 + 2U],
                                                tan[i4 + 3U] < 0.0F ? -1.0F : 1.0F)
                                     : nph_vec4(1.0F, 0.0F, 0.0F, 1.0F);
            v->uv0 = uv != NULL ? nph_vec2(uv[i2], uv[i2 + 1U]) : nph_vec2_zero();
            v->color = nph_vec4_one();
            if (col != NULL) {
                const f32 *c = &col[(u64)i * at->col_components];
                v->color = nph_vec4(c[0], c[1], c[2], at->col_components == 4U ? c[3] : 1.0F);
            }
        }
    }
    if (r == NPH_RESULT_OK) {
        r = cook_mesh_add_submesh(out, verts, count, tris, tri_indices, 0U);
        if (r == NPH_RESULT_INVALID_ARGUMENT) {
            r = NPH_RESULT_PARSE_ERROR;
        }
    }
    if (r == NPH_RESULT_OK) {
        out->flags = (u32)COOK_MESH_HAS_NORMALS | (at->uv != NULL ? (u32)COOK_MESH_HAS_UV0 : 0U) |
                     (at->col != NULL ? (u32)COOK_MESH_HAS_COLORS : 0U) |
                     (tan != NULL ? (u32)COOK_MESH_HAS_TANGENTS : 0U);
        // Non-finite values (or anything else the mesh pipeline cannot
        // take) make the document invalid.
        if (cook_mesh_validate(out) != NPH_RESULT_OK) {
            r = NPH_RESULT_PARSE_ERROR;
        }
    }
    if (r == NPH_RESULT_OK && nrm == NULL) {
        r = cook_mesh_generate_normals(out, COOK_NORMALS_FLAT);
    }
    if (r == NPH_RESULT_OK && want_tangents && uv != NULL && tan == NULL) {
        r = cook_mesh_weld(out);
        if (r == NPH_RESULT_OK) {
            r = cook_mesh_generate_tangents(out);
        }
    }
    if (r == NPH_RESULT_OK && want_tangents && uv == NULL && tan == NULL) {
        // Part of a mesh with tangents but no texture coordinates or
        // tangents of its own: any frame perpendicular to the normal.
        NPH_ARRAY_FOREACH (CookVertex, v, &out->vertices) {
            v->tangent = nph_vec4_from_vec3(
                nph_vec3_perpendicular(nph_vec3_normalize_or(v->normal, nph_vec3_unit_z())), 1.0F);
        }
        out->flags |= (u32)COOK_MESH_HAS_TANGENTS;
    }
    free_floats(alloc, pos, at->pos, 3U);
    free_floats(alloc, nrm, at->nrm, 3U);
    free_floats(alloc, tan, at->tan, 4U);
    free_floats(alloc, uv, at->uv, 2U);
    free_floats(alloc, col, at->col, at->col_components);
    if (verts != NULL) {
        NPH_FREE_ARRAY(alloc, CookVertex, verts, count);
    }
    if (tris != NULL) {
        NPH_FREE_ARRAY(alloc, u32, tris, tri_indices);
    }
    return r;
}

static NphResult convert_mesh(CookGltfScene *scene, const cgltf_data *data, const cgltf_mesh *src,
                              CookGltfMesh *out)
{
    NphAllocator *alloc = scene->alloc;
    cook_mesh_init(&out->mesh, alloc);
    copy_name(out->mesh.name, sizeof out->mesh.name, src->name, "mesh",
              (u64)cgltf_mesh_index(data, src));
    out->material_capacity = nph_max_u32((u32)src->primitives_count, 1U);
    out->materials = NPH_ALLOC_ARRAY(alloc, i32, out->material_capacity);
    if (out->materials == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    out->material_count = 0U;
    if (src->primitives_count != 0U && src->primitives[0].targets_count != 0U) {
        NPH_LOG_WARN(&NPH_LOG_COOK, "%s: morph targets are not supported; ignored", out->mesh.name);
        scene->warnings += 1U;
    }
    // First pass: what the primitives carry. Tangents are left to the mesh
    // pipeline (one MikkTSpace pass over the whole mesh, as a DCC tool
    // computes them) unless some primitives bring their own, in which case
    // the others make theirs at import.
    bool any_uv = false;
    bool uv_with_tangents = false;
    bool uv_without_tangents = false;
    bool any_color = false;
    for (cgltf_size p = 0; p < src->primitives_count; ++p) {
        const cgltf_primitive *prim = &src->primitives[p];
        if (!usable_primitive(prim)) {
            continue;
        }
        PrimitiveAttribs at;
        primitive_attribs(prim, &at);
        if (at.pos == NULL) {
            continue;
        }
        any_uv = any_uv || at.uv != NULL;
        any_color = any_color || at.col != NULL;
        uv_with_tangents = uv_with_tangents || (at.uv != NULL && at.tan != NULL);
        uv_without_tangents = uv_without_tangents || (at.uv != NULL && at.tan == NULL);
    }
    const bool want_tangents = uv_with_tangents;
    if (uv_with_tangents && uv_without_tangents) {
        NPH_LOG_INFO(&NPH_LOG_COOK, "%s: tangents generated for the primitives without them",
                     out->mesh.name);
    }
    u32 flags = (u32)COOK_MESH_HAS_NORMALS | (any_uv ? (u32)COOK_MESH_HAS_UV0 : 0U) |
                (any_color ? (u32)COOK_MESH_HAS_COLORS : 0U) |
                (want_tangents ? (u32)COOK_MESH_HAS_TANGENTS : 0U);
    for (cgltf_size p = 0; p < src->primitives_count; ++p) {
        const cgltf_primitive *prim = &src->primitives[p];
        if (!usable_primitive(prim)) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "%s: primitive %llu is not triangles; skipped",
                         out->mesh.name, (unsigned long long)p);
            scene->warnings += 1U;
            continue;
        }
        PrimitiveAttribs at;
        primitive_attribs(prim, &at);
        if (at.pos == NULL) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "%s: primitive %llu has no positions; skipped",
                         out->mesh.name, (unsigned long long)p);
            scene->warnings += 1U;
            continue;
        }
        if (at.pos->count > GLTF_MAX_VERTICES) {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: primitive %llu has %llu vertices (at most %u)",
                          out->mesh.name, (unsigned long long)p, (unsigned long long)at.pos->count,
                          GLTF_MAX_VERTICES);
            return NPH_RESULT_UNSUPPORTED;
        }
        if (find_attribute(prim, cgltf_attribute_type_texcoord, 1) != NULL) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "%s: TEXCOORD_1 is not supported; ignored", out->mesh.name);
            scene->warnings += 1U;
        }
        CookMesh piece;
        cook_mesh_init(&piece, alloc);
        NphResult r = convert_primitive(alloc, prim, &at, want_tangents, &piece);
        // The material slot: one per distinct material in this mesh.
        const i32 material =
            prim->material != NULL ? (i32)cgltf_material_index(data, prim->material) : -1;
        u32 slot = out->material_count;
        for (u32 s = 0; s < out->material_count; ++s) {
            if (out->materials[s] == material) {
                slot = s;
                break;
            }
        }
        if (slot == out->material_count) {
            out->materials[out->material_count++] = material;
        }
        if (r == NPH_RESULT_OK) {
            r = cook_mesh_add_submesh(&out->mesh, piece.vertices.data, piece.vertices.len,
                                      piece.indices.data, piece.indices.len, slot);
        }
        cook_mesh_free(&piece);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: primitive %llu: %s", out->mesh.name,
                          (unsigned long long)p, nph_result_string(r));
            return r;
        }
    }
    out->mesh.flags = out->mesh.submeshes.len != 0U ? flags : 0U;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

static u8 wrap_mode(cgltf_int gl)
{
    switch (gl) {
    case 33071: // GL_CLAMP_TO_EDGE
        return (u8)NPH_MATERIAL_WRAP_CLAMP;
    case 33648: // GL_MIRRORED_REPEAT
        return (u8)NPH_MATERIAL_WRAP_MIRROR;
    default:
        return (u8)NPH_MATERIAL_WRAP_REPEAT;
    }
}

static void convert_texture_ref(CookGltfScene *scene, const cgltf_data *data, const char *material,
                                const cgltf_texture_view *view, NphMaterialTextureRef *ref,
                                i32 *image)
{
    *image = -1;
    if (view->texture == NULL || view->texture->image == NULL) {
        return;
    }
    *image = (i32)cgltf_image_index(data, view->texture->image);
    ref->texcoord = 0U;
    ref->scale = view->scale;
    if (view->texcoord != 0) {
        NPH_LOG_WARN(&NPH_LOG_COOK, "%s: a texture uses TEXCOORD_%d; TEXCOORD_0 is used instead",
                     material, (int)view->texcoord);
        scene->warnings += 1U;
    }
    if (view->has_transform) {
        NPH_LOG_WARN(&NPH_LOG_COOK, "%s: KHR_texture_transform is not supported; ignored",
                     material);
        scene->warnings += 1U;
    }
    if (view->texture->sampler != NULL) {
        const cgltf_sampler *s = view->texture->sampler;
        ref->wrap_u = wrap_mode(s->wrap_s);
        ref->wrap_v = wrap_mode(s->wrap_t);
        ref->nearest = s->mag_filter == 9728 ? 1U : 0U; // GL_NEAREST
    }
}

static void convert_material(CookGltfScene *scene, const cgltf_data *data,
                             const cgltf_material *src, CookGltfMaterial *out)
{
    memset(out, 0, sizeof *out);
    copy_name(out->name, sizeof out->name, src->name, "material",
              (u64)cgltf_material_index(data, src));
    NphMaterialHeader *h = &out->header;
    nph_material_header_default(h);
    for (u32 i = 0; i < NPH_MATERIAL_TEXTURE_COUNT; ++i) {
        out->image[i] = -1;
    }
    if (src->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness *pbr = &src->pbr_metallic_roughness;
        h->base_color_factor = nph_vec4(pbr->base_color_factor[0], pbr->base_color_factor[1],
                                        pbr->base_color_factor[2], pbr->base_color_factor[3]);
        h->metallic_factor = pbr->metallic_factor;
        h->roughness_factor = pbr->roughness_factor;
        convert_texture_ref(scene, data, out->name, &pbr->base_color_texture,
                            &h->textures[NPH_MATERIAL_TEXTURE_BASE_COLOR],
                            &out->image[NPH_MATERIAL_TEXTURE_BASE_COLOR]);
        convert_texture_ref(scene, data, out->name, &pbr->metallic_roughness_texture,
                            &h->textures[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS],
                            &out->image[NPH_MATERIAL_TEXTURE_METALLIC_ROUGHNESS]);
    }
    convert_texture_ref(scene, data, out->name, &src->normal_texture,
                        &h->textures[NPH_MATERIAL_TEXTURE_NORMAL],
                        &out->image[NPH_MATERIAL_TEXTURE_NORMAL]);
    convert_texture_ref(scene, data, out->name, &src->occlusion_texture,
                        &h->textures[NPH_MATERIAL_TEXTURE_OCCLUSION],
                        &out->image[NPH_MATERIAL_TEXTURE_OCCLUSION]);
    convert_texture_ref(scene, data, out->name, &src->emissive_texture,
                        &h->textures[NPH_MATERIAL_TEXTURE_EMISSIVE],
                        &out->image[NPH_MATERIAL_TEXTURE_EMISSIVE]);
    h->emissive_factor =
        nph_vec3(src->emissive_factor[0], src->emissive_factor[1], src->emissive_factor[2]);
    h->emissive_strength =
        src->has_emissive_strength ? src->emissive_strength.emissive_strength : 1.0F;
    h->alpha_mode = src->alpha_mode == cgltf_alpha_mode_mask    ? (u32)NPH_MATERIAL_ALPHA_MASK
                    : src->alpha_mode == cgltf_alpha_mode_blend ? (u32)NPH_MATERIAL_ALPHA_BLEND
                                                                : (u32)NPH_MATERIAL_ALPHA_OPAQUE;
    h->alpha_cutoff = src->alpha_cutoff;
    h->flags = (src->double_sided ? (u32)NPH_MATERIAL_FLAG_DOUBLE_SIDED : 0U) |
               (src->unlit ? (u32)NPH_MATERIAL_FLAG_UNLIT : 0U);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// The decoded size of `n` base64 characters (RFC 4648, padded or not), or
// 0 when no valid encoding has that length.
static u64 base64_decoded_size(const char *s, u64 n)
{
    const u64 rem = n % 4U;
    if (n == 0U || rem == 1U) {
        return 0U;
    }
    if (rem != 0U) {
        return n / 4U * 3U + rem - 1U; // unpadded: two characters give a byte, three give two
    }
    const u64 pad = (s[n - 1U] == '=' ? 1U : 0U) + (s[n - 2U] == '=' ? 1U : 0U);
    return n / 4U * 3U - pad;
}

static NphResult convert_image(CookGltfScene *scene, const cgltf_options *options,
                               const cgltf_data *data, const cgltf_image *src, const char *base_dir,
                               CookGltfImage *out)
{
    memset(out, 0, sizeof *out);
    copy_name(out->name, sizeof out->name, src->name, "image", (u64)cgltf_image_index(data, src));
    if (src->buffer_view != NULL) {
        const cgltf_buffer_view *bv = src->buffer_view;
        if (bv->buffer == NULL || bv->buffer->data == NULL || bv->offset > bv->buffer->size ||
            bv->size > bv->buffer->size - bv->offset) {
            return NPH_RESULT_PARSE_ERROR;
        }
        out->data = (const u8 *)bv->buffer->data + bv->offset;
        out->size = (u64)bv->size;
        return NPH_RESULT_OK;
    }
    if (src->uri == NULL) {
        return NPH_RESULT_PARSE_ERROR;
    }
    CookGltfOwned *owned = &scene->owned[scene->owned_count];
    if (strncmp(src->uri, "data:", 5U) == 0) {
        const char *marker = strstr(src->uri, ";base64,");
        if (marker == NULL) {
            return NPH_RESULT_PARSE_ERROR;
        }
        const char *b64 = marker + 8;
        const u64 size = base64_decoded_size(b64, strlen(b64));
        void *decoded = NULL;
        if (size == 0U || cgltf_load_buffer_base64(options, (cgltf_size)size, b64, &decoded) !=
                              cgltf_result_success) {
            return NPH_RESULT_PARSE_ERROR;
        }
        owned->data = decoded;
        owned->size = size;
        owned->from_file = 0;
        scene->owned_count += 1U;
        out->data = (const u8 *)decoded;
        out->size = size;
        return NPH_RESULT_OK;
    }
    // A file next to the document.
    char uri[COOK_GLTF_URI_MAX];
    const u64 n = strlen(src->uri);
    if (n >= sizeof uri) {
        return NPH_RESULT_PARSE_ERROR;
    }
    memcpy(uri, src->uri, (size_t)n + 1U);
    cgltf_decode_uri(uri);
    if (base_dir == NULL || nph_path_join(out->uri, sizeof out->uri, nph_str_from_cstr(base_dir),
                                          nph_str_from_cstr(uri), NULL) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    void *bytes = NULL;
    u64 size = 0U;
    const NphResult r = nph_file_read_all(out->uri, scene->alloc, &bytes, &size);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "image %s: cannot read %s (%s)", out->name, out->uri,
                      nph_result_string(r));
        return r == NPH_RESULT_NOT_FOUND ? NPH_RESULT_IO_ERROR : r;
    }
    owned->data = bytes;
    owned->size = size;
    owned->from_file = 1;
    scene->owned_count += 1U;
    out->data = (const u8 *)bytes;
    out->size = size;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Nodes, cameras, lights
// ---------------------------------------------------------------------------

static void node_transform(CookGltfScene *scene, const cgltf_node *n, CookGltfNode *out)
{
    out->translation = nph_vec3_zero();
    out->rotation = nph_quat_identity();
    out->scale = nph_vec3_one();
    if (n->has_matrix) {
        NphMat4 m;
        memcpy(&m, n->matrix, sizeof m); // column-major on both sides (ADR-0008)
        NphVec3 t;
        NphQuat r;
        NphVec3 s;
        bool finite = true;
        for (u32 i = 0; i < 16U; ++i) {
            finite = finite && nph_f32_is_finite(n->matrix[i]);
        }
        if (finite && nph_affine_decompose_trs(nph_affine_from_mat4(m), &t, &r, &s)) {
            out->translation = t;
            out->rotation = r;
            out->scale = s;
        } else {
            NPH_LOG_WARN(&NPH_LOG_COOK, "node %s: matrix is not decomposable; identity used",
                         out->name);
            scene->warnings += 1U;
        }
        return;
    }
    if (n->has_translation) {
        out->translation = nph_vec3(n->translation[0], n->translation[1], n->translation[2]);
    }
    if (n->has_rotation) {
        out->rotation = nph_quat_normalize_or_identity(
            nph_quat(n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]));
    }
    if (n->has_scale) {
        out->scale = nph_vec3(n->scale[0], n->scale[1], n->scale[2]);
    }
    const f32 values[10] = {out->translation.x, out->translation.y, out->translation.z,
                            out->rotation.x,    out->rotation.y,    out->rotation.z,
                            out->rotation.w,    out->scale.x,       out->scale.y,
                            out->scale.z};
    for (u32 i = 0; i < 10U; ++i) {
        if (!nph_f32_is_finite(values[i])) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "node %s: non-finite transform; identity used", out->name);
            scene->warnings += 1U;
            out->translation = nph_vec3_zero();
            out->rotation = nph_quat_identity();
            out->scale = nph_vec3_one();
            return;
        }
    }
}

static void add_camera(CookGltfScene *scene, const cgltf_camera *c, u32 node, CookGltfNode *out)
{
    NphSceneCamera *cam = &scene->cameras[scene->camera_count];
    memset(cam, 0, sizeof *cam);
    cam->node = node;
    if (c->type == cgltf_camera_type_orthographic) {
        cam->type = (u32)NPH_SCENE_CAMERA_ORTHOGRAPHIC;
        cam->xmag = c->data.orthographic.xmag;
        cam->ymag = c->data.orthographic.ymag;
        cam->znear = c->data.orthographic.znear;
        cam->zfar = c->data.orthographic.zfar;
    } else {
        cam->type = (u32)NPH_SCENE_CAMERA_PERSPECTIVE;
        cam->yfov = c->data.perspective.yfov;
        cam->aspect =
            c->data.perspective.has_aspect_ratio ? c->data.perspective.aspect_ratio : 0.0F;
        cam->znear = c->data.perspective.znear;
        cam->zfar = c->data.perspective.has_zfar ? c->data.perspective.zfar : 0.0F;
    }
    out->camera = (i32)scene->camera_count++;
}

static void add_light(CookGltfScene *scene, const cgltf_light *l, u32 node, CookGltfNode *out)
{
    NphSceneLight *light = &scene->lights[scene->light_count];
    memset(light, 0, sizeof *light);
    light->node = node;
    light->type = l->type == cgltf_light_type_directional ? (u32)NPH_SCENE_LIGHT_DIRECTIONAL
                  : l->type == cgltf_light_type_spot      ? (u32)NPH_SCENE_LIGHT_SPOT
                                                          : (u32)NPH_SCENE_LIGHT_POINT;
    light->color = nph_vec3(l->color[0], l->color[1], l->color[2]);
    light->intensity = l->intensity;
    light->range = l->range;
    light->inner_cone = l->spot_inner_cone_angle;
    light->outer_cone = l->spot_outer_cone_angle;
    out->light = (i32)scene->light_count++;
}

typedef struct NodeVisit {
    const cgltf_node *node;
    u32 parent;
} NodeVisit;

typedef NPH_ARRAY(NodeVisit) NodeVisitArray;

// Appends the subtree of `root` depth first, a parent before its children
// and siblings in document order, without recursion (a hostile document
// can nest nodes as deep as it has nodes). A node met twice (glTF forbids
// it; cgltf rejects cycles but not every repetition) is taken once.
static NphResult append_tree(CookGltfScene *scene, const cgltf_data *data, const cgltf_node *root,
                             u8 *visited, NodeVisitArray *stack)
{
    NPH_ARRAY_CLEAR(stack);
    const NodeVisit first = {root, NPH_SCENE_NO_NODE};
    NPH_TRY(NPH_ARRAY_PUSH(stack, first));
    while (stack->len != 0U) {
        const NodeVisit visit = NPH_ARRAY_POP(stack);
        const cgltf_node *n = visit.node;
        const cgltf_size gltf_index = cgltf_node_index(data, n);
        if (visited[gltf_index] != 0U) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "node %llu appears twice in the scene; taken once",
                         (unsigned long long)gltf_index);
            scene->warnings += 1U;
            continue;
        }
        visited[gltf_index] = 1U;
        const u32 index = scene->node_count++;
        CookGltfNode *out = &scene->nodes[index];
        memset(out, 0, sizeof *out);
        copy_name(out->name, sizeof out->name, n->name, "node", (u64)gltf_index);
        out->parent = visit.parent;
        node_transform(scene, n, out);
        out->mesh = n->mesh != NULL ? (i32)cgltf_mesh_index(data, n->mesh) : -1;
        out->camera = -1;
        out->light = -1;
        if (n->skin != NULL) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "node %s: skinning is not supported; the mesh is static",
                         out->name);
            scene->warnings += 1U;
        }
        if (n->has_mesh_gpu_instancing) {
            NPH_LOG_WARN(&NPH_LOG_COOK, "node %s: GPU instancing is not supported; one instance",
                         out->name);
            scene->warnings += 1U;
        }
        // Each node holds at most one camera and one light, so arrays of
        // a node count's capacity hold every instance.
        if (n->camera != NULL) {
            add_camera(scene, n->camera, index, out);
        }
        if (n->light != NULL) {
            add_light(scene, n->light, index, out);
        }
        // Children in reverse so the first is taken next.
        for (cgltf_size c = n->children_count; c > 0U; --c) {
            const NodeVisit child = {n->children[c - 1U], index};
            NPH_TRY(NPH_ARRAY_PUSH(stack, child));
        }
    }
    return NPH_RESULT_OK;
}

static NphResult convert_nodes(CookGltfScene *scene, const cgltf_data *data)
{
    if (data->nodes_count == 0U) {
        return NPH_RESULT_OK;
    }
    NphAllocator *alloc = scene->alloc;
    u8 *visited = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, (u32)data->nodes_count);
    NodeVisitArray stack;
    NPH_ARRAY_INIT(&stack, alloc);
    NphResult r = visited != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
    const cgltf_scene *s = data->scene != NULL        ? data->scene
                           : data->scenes_count != 0U ? &data->scenes[0]
                                                      : NULL;
    if (s != NULL) {
        for (cgltf_size i = 0; i < s->nodes_count && r == NPH_RESULT_OK; ++i) {
            r = append_tree(scene, data, s->nodes[i], visited, &stack);
        }
    } else {
        // No scene: every root node.
        for (cgltf_size i = 0; i < data->nodes_count && r == NPH_RESULT_OK; ++i) {
            if (data->nodes[i].parent == NULL) {
                r = append_tree(scene, data, &data->nodes[i], visited, &stack);
            }
        }
    }
    NPH_ARRAY_FREE(&stack);
    if (visited != NULL) {
        NPH_FREE_ARRAY(alloc, u8, visited, (u32)data->nodes_count);
    }
    return r;
}

// ---------------------------------------------------------------------------
// The document
// ---------------------------------------------------------------------------

static NphResult convert(CookGltfScene *scene, const cgltf_options *options, cgltf_data *data,
                         const char *base_dir, const void *document, u64 document_size)
{
    NphAllocator *alloc = scene->alloc;
    if (data->meshes_count > NPH_U32_MAX / 2U || data->materials_count > NPH_U32_MAX / 2U ||
        data->images_count > NPH_U32_MAX / 2U || data->nodes_count > NPH_U32_MAX / 2U) {
        return NPH_RESULT_PARSE_ERROR;
    }
    NPH_TRY(check_extensions(scene, data));
    scene->capacity_meshes = nph_max_u32((u32)data->meshes_count, 1U);
    scene->capacity_materials = nph_max_u32((u32)data->materials_count, 1U);
    scene->capacity_images = nph_max_u32((u32)data->images_count, 1U);
    scene->capacity_nodes = nph_max_u32((u32)data->nodes_count, 1U);
    scene->meshes = NPH_ALLOC_ARRAY_ZEROED(alloc, CookGltfMesh, scene->capacity_meshes);
    scene->materials = NPH_ALLOC_ARRAY_ZEROED(alloc, CookGltfMaterial, scene->capacity_materials);
    scene->images = NPH_ALLOC_ARRAY_ZEROED(alloc, CookGltfImage, scene->capacity_images);
    scene->nodes = NPH_ALLOC_ARRAY_ZEROED(alloc, CookGltfNode, scene->capacity_nodes);
    scene->cameras = NPH_ALLOC_ARRAY_ZEROED(alloc, NphSceneCamera, scene->capacity_nodes);
    scene->lights = NPH_ALLOC_ARRAY_ZEROED(alloc, NphSceneLight, scene->capacity_nodes);
    scene->owned = NPH_ALLOC_ARRAY_ZEROED(alloc, CookGltfOwned, scene->capacity_images);
    if (scene->meshes == NULL || scene->materials == NULL || scene->images == NULL ||
        scene->nodes == NULL || scene->cameras == NULL || scene->lights == NULL ||
        scene->owned == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        scene->mesh_count += 1U; // counted first: a failed mesh is still released
        NPH_TRY(convert_mesh(scene, data, &data->meshes[i], &scene->meshes[i]));
    }
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        convert_material(scene, data, &data->materials[i], &scene->materials[i]);
        scene->material_count += 1U;
    }
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        NPH_TRY(convert_image(scene, options, data, &data->images[i], base_dir, &scene->images[i]));
        scene->images[i].hash =
            nph_hash64(scene->images[i].data, scene->images[i].size, NPH_HASH_DEFAULT_SEED);
        scene->image_count += 1U;
    }
    if (data->animations_count != 0U) {
        NPH_LOG_WARN(&NPH_LOG_COOK, "%llu animations are not supported; ignored",
                     (unsigned long long)data->animations_count);
        scene->warnings += 1U;
    }
    NPH_TRY(convert_nodes(scene, data));
    // What the cooked assets depend on: the document, every buffer it
    // loaded (external .bin files included), and every image.
    u64 h =
        nph_hash_combine(nph_hash64(document, document_size, NPH_HASH_DEFAULT_SEED), document_size);
    for (cgltf_size i = 0; i < data->buffers_count; ++i) {
        const cgltf_buffer *b = &data->buffers[i];
        if (b->data != NULL) {
            h = nph_hash_combine(h, nph_hash64(b->data, (u64)b->size, NPH_HASH_DEFAULT_SEED));
        }
    }
    for (u32 i = 0; i < scene->image_count; ++i) {
        h = nph_hash_combine(h, scene->images[i].hash);
    }
    scene->content_hash = h;
    return NPH_RESULT_OK;
}

// Parses the document in `bytes` (a cook block the scene adopts: a GLB's
// binary chunk is used in place), loads its buffers, validates, converts.
static NphResult load(NphAllocator *alloc, u8 *bytes, u64 size, const char *path_for_uris,
                      const char *base_dir, CookGltfScene *out)
{
    memset(out, 0, sizeof *out);
    out->alloc = alloc;
    out->document.data = bytes;
    out->document.size = size;
    cgltf_options options;
    gltf_options(alloc, &options);
    cgltf_data *data = NULL;
    cgltf_result cr = cgltf_parse(&options, bytes, (cgltf_size)size, &data);
    NphResult r = NPH_RESULT_OK;
    if (cr == cgltf_result_success) {
        out->cgltf = data;
        if (base_dir == NULL) {
            // Without a directory only embedded buffers can be loaded.
            for (cgltf_size i = 0; i < data->buffers_count; ++i) {
                const char *uri = data->buffers[i].uri;
                if (uri != NULL && strncmp(uri, "data:", 5U) != 0) {
                    NPH_LOG_ERROR(&NPH_LOG_COOK,
                                  "%s: buffer %s is a file, and there is no "
                                  "directory to find it in",
                                  path_for_uris, uri);
                    r = NPH_RESULT_PARSE_ERROR;
                    break;
                }
            }
        }
        if (r == NPH_RESULT_OK) {
            cr = cgltf_load_buffers(&options, data, path_for_uris);
        }
    }
    if (r == NPH_RESULT_OK && cr == cgltf_result_success) {
        cr = cgltf_validate(data);
    }
    if (r == NPH_RESULT_OK && cr != cgltf_result_success) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", path_for_uris, gltf_result_name(cr));
        r = gltf_result_to_nph(cr);
    }
    if (r == NPH_RESULT_OK) {
        r = convert(out, &options, data, base_dir, bytes, size);
    }
    if (r != NPH_RESULT_OK) {
        cook_gltf_free(out);
    }
    return r;
}

NphResult cook_gltf_load_memory(NphAllocator *alloc, const void *data_bytes, u64 size,
                                const char *base_dir, CookGltfScene *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (data_bytes == NULL || size == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    u8 *copy = (u8 *)cook_block_alloc(alloc, size);
    if (copy == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    memcpy(copy, data_bytes, (size_t)size);
    // cgltf resolves relative URIs against the directory of this path.
    char pseudo[COOK_GLTF_URI_MAX + 16U];
    snprintf(pseudo, sizeof pseudo, "%s/scene.gltf", base_dir != NULL ? base_dir : ".");
    return load(alloc, copy, size, pseudo, base_dir, out);
}

NphResult cook_gltf_load(NphAllocator *alloc, const char *path, CookGltfScene *out)
{
    NPH_ASSERT(alloc != NULL && path != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    cgltf_options options;
    gltf_options(alloc, &options);
    cgltf_size size = 0U;
    void *bytes = NULL;
    const cgltf_result cr = gltf_file_read(&options.memory, &options.file, path, &size, &bytes);
    if (cr != cgltf_result_success) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: %s", path, gltf_result_name(cr));
        return gltf_result_to_nph(cr);
    }
    if (size == 0U) {
        cook_block_free(alloc, bytes);
        NPH_LOG_ERROR(&NPH_LOG_COOK, "%s: empty file", path);
        return NPH_RESULT_PARSE_ERROR;
    }
    char base_dir[COOK_GLTF_URI_MAX];
    const NphStr dir = nph_path_dirname(nph_str_from_cstr(path));
    if (dir.len >= sizeof base_dir) {
        cook_block_free(alloc, bytes);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    memcpy(base_dir, dir.data, (size_t)dir.len);
    base_dir[dir.len] = '\0';
    return load(alloc, (u8 *)bytes, (u64)size, path, dir.len != 0U ? base_dir : ".", out);
}

void cook_gltf_free(CookGltfScene *scene)
{
    if (scene == NULL || scene->alloc == NULL) {
        return;
    }
    NphAllocator *alloc = scene->alloc;
    if (scene->meshes != NULL) {
        for (u32 i = 0; i < scene->mesh_count; ++i) {
            CookGltfMesh *m = &scene->meshes[i];
            cook_mesh_free(&m->mesh);
            if (m->materials != NULL) {
                NPH_FREE_ARRAY(alloc, i32, m->materials, m->material_capacity);
            }
        }
        NPH_FREE_ARRAY(alloc, CookGltfMesh, scene->meshes, scene->capacity_meshes);
    }
    if (scene->materials != NULL) {
        NPH_FREE_ARRAY(alloc, CookGltfMaterial, scene->materials, scene->capacity_materials);
    }
    if (scene->images != NULL) {
        NPH_FREE_ARRAY(alloc, CookGltfImage, scene->images, scene->capacity_images);
    }
    if (scene->nodes != NULL) {
        NPH_FREE_ARRAY(alloc, CookGltfNode, scene->nodes, scene->capacity_nodes);
    }
    if (scene->cameras != NULL) {
        NPH_FREE_ARRAY(alloc, NphSceneCamera, scene->cameras, scene->capacity_nodes);
    }
    if (scene->lights != NULL) {
        NPH_FREE_ARRAY(alloc, NphSceneLight, scene->lights, scene->capacity_nodes);
    }
    if (scene->owned != NULL) {
        for (u32 i = 0; i < scene->owned_count; ++i) {
            const CookGltfOwned *o = &scene->owned[i];
            if (o->from_file) {
                nph_file_free_all(alloc, o->data, o->size);
            } else {
                cook_block_free(alloc, o->data);
            }
        }
        NPH_FREE_ARRAY(alloc, CookGltfOwned, scene->owned, scene->capacity_images);
    }
    if (scene->cgltf != NULL) {
        cgltf_free((cgltf_data *)scene->cgltf);
    }
    // After cgltf: its buffers may point into the document.
    cook_block_free(alloc, scene->document.data);
    memset(scene, 0, sizeof *scene);
}
