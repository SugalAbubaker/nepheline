// SPDX-License-Identifier: Apache-2.0
// Procedural meshes in the renderer's streams (render/primitives.h).
//
// Every primitive is built from quads laid out the same way: a grid of
// vertices with u growing to the right and v growing downwards as seen
// from outside, split into two counter-clockwise triangles each. The
// float vertices are then quantised exactly as the cooker quantises
// (tools/cook mesh.c): positions to unorm16 across the bounds, normals to
// octahedral snorm16, tangents to snorm8 with the handedness in w, UVs to
// half floats, colours to RGBA8.
#include <nepheline/math/pack.h>
#include <nepheline/math/scalar.h>
#include <nepheline/render/primitives.h>

#include <string.h>

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// Per-vertex tangents from the UV gradients of the triangles around it
// (Lengyel 2001): the accumulated dp/du, made orthogonal to the normal,
// with w chosen so that cross(n, t) * w follows decreasing v (glTF 2.0
// §3.7.2.1).
static void compute_tangents(const NphRenderVertexInput *v, u32 vertex_count, const u32 *indices,
                             u32 index_count, NphVec3 *sdir, NphVec3 *tdir)
{
    memset(sdir, 0, sizeof(NphVec3) * vertex_count);
    memset(tdir, 0, sizeof(NphVec3) * vertex_count);
    for (u32 i = 0; i + 2U < index_count; i += 3U) {
        const u32 a = indices[i];
        const u32 b = indices[i + 1U];
        const u32 c = indices[i + 2U];
        const NphVec3 e1 = nph_vec3_sub(v[b].position, v[a].position);
        const NphVec3 e2 = nph_vec3_sub(v[c].position, v[a].position);
        const f32 du1 = v[b].uv.x - v[a].uv.x;
        const f32 dv1 = v[b].uv.y - v[a].uv.y;
        const f32 du2 = v[c].uv.x - v[a].uv.x;
        const f32 dv2 = v[c].uv.y - v[a].uv.y;
        const f32 det = du1 * dv2 - du2 * dv1;
        if (nph_f32_abs(det) < 1e-12F) {
            continue; // no UV area: the triangle says nothing about the frame
        }
        const f32 inv = 1.0F / det;
        const NphVec3 s =
            nph_vec3_scale(nph_vec3_sub(nph_vec3_scale(e1, dv2), nph_vec3_scale(e2, dv1)), inv);
        const NphVec3 t =
            nph_vec3_scale(nph_vec3_sub(nph_vec3_scale(e2, du1), nph_vec3_scale(e1, du2)), inv);
        sdir[a] = nph_vec3_add(sdir[a], s);
        sdir[b] = nph_vec3_add(sdir[b], s);
        sdir[c] = nph_vec3_add(sdir[c], s);
        tdir[a] = nph_vec3_add(tdir[a], t);
        tdir[b] = nph_vec3_add(tdir[b], t);
        tdir[c] = nph_vec3_add(tdir[c], t);
    }
}

NphResult nph_render_mesh_data_build(NphAllocator *alloc, const NphRenderVertexInput *vertices,
                                     u32 vertex_count, const u32 *indices, u32 index_count,
                                     NphRenderMeshData *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (vertices == NULL || vertex_count == 0U || indices == NULL || index_count == 0U ||
        index_count % 3U != 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NphAABB bounds = nph_aabb_empty();
    for (u32 i = 0; i < vertex_count; ++i) {
        if (!nph_vec3_is_finite(vertices[i].position)) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
        bounds = nph_aabb_expand(bounds, vertices[i].position);
    }
    for (u32 i = 0; i < index_count; ++i) {
        if (indices[i] >= vertex_count) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    out->alloc = alloc;
    out->positions = NPH_ALLOC_ARRAY(alloc, NphRenderVertexPosition, vertex_count);
    out->attribs = NPH_ALLOC_ARRAY(alloc, NphRenderVertexAttrib, vertex_count);
    out->indices = NPH_ALLOC_ARRAY(alloc, u32, index_count);
    NphVec3 *sdir = NPH_ALLOC_ARRAY(alloc, NphVec3, vertex_count);
    NphVec3 *tdir = NPH_ALLOC_ARRAY(alloc, NphVec3, vertex_count);
    if (out->positions == NULL || out->attribs == NULL || out->indices == NULL || sdir == NULL ||
        tdir == NULL) {
        if (sdir != NULL) {
            NPH_FREE_ARRAY(alloc, NphVec3, sdir, vertex_count);
        }
        if (tdir != NULL) {
            NPH_FREE_ARRAY(alloc, NphVec3, tdir, vertex_count);
        }
        out->vertex_count = vertex_count;
        out->index_count = index_count;
        nph_render_mesh_data_free(out);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    out->vertex_count = vertex_count;
    out->index_count = index_count;
    memcpy(out->indices, indices, sizeof(u32) * index_count);
    compute_tangents(vertices, vertex_count, indices, index_count, sdir, tdir);

    const NphVec3 size = nph_aabb_size(bounds);
    out->position_offset = bounds.min;
    out->position_scale = size;
    out->bounds = bounds;
    for (u32 i = 0; i < vertex_count; ++i) {
        const NphRenderVertexInput *v = &vertices[i];
        const NphVec3 p = v->position;
        const f32 rel[3] = {size.x > 0.0F ? (p.x - bounds.min.x) / size.x : 0.0F,
                            size.y > 0.0F ? (p.y - bounds.min.y) / size.y : 0.0F,
                            size.z > 0.0F ? (p.z - bounds.min.z) / size.z : 0.0F};
        out->positions[i].x = nph_f32_to_unorm16(rel[0]);
        out->positions[i].y = nph_f32_to_unorm16(rel[1]);
        out->positions[i].z = nph_f32_to_unorm16(rel[2]);
        out->positions[i].reserved = 0U;

        const NphVec3 n = nph_vec3_normalize_or(v->normal, nph_vec3_unit_y());
        NphVec3 t = nph_vec3_sub(sdir[i], nph_vec3_scale(n, nph_vec3_dot(n, sdir[i])));
        f32 w = 1.0F;
        if (nph_vec3_length_sq(t) < 1e-20F) {
            t = nph_vec3_normalize(nph_vec3_perpendicular(n));
        } else {
            t = nph_vec3_normalize(t);
            w = nph_vec3_dot(nph_vec3_cross(n, t), tdir[i]) < 0.0F ? 1.0F : -1.0F;
        }
        NphRenderVertexAttrib *a = &out->attribs[i];
        a->normal = nph_oct_encode_snorm16(n);
        a->tangent = nph_pack_snorm8x4(nph_vec4_from_vec3(t, w));
        a->uv0 = nph_pack_f16x2(v->uv);
        a->color = nph_pack_unorm8x4(nph_vec4_clamp(v->color, nph_vec4_zero(), nph_vec4_one()));
    }
    NPH_FREE_ARRAY(alloc, NphVec3, sdir, vertex_count);
    NPH_FREE_ARRAY(alloc, NphVec3, tdir, vertex_count);
    return NPH_RESULT_OK;
}

void nph_render_mesh_data_free(NphRenderMeshData *data)
{
    if (data == NULL || data->alloc == NULL) {
        return;
    }
    if (data->positions != NULL) {
        NPH_FREE_ARRAY(data->alloc, NphRenderVertexPosition, data->positions, data->vertex_count);
    }
    if (data->attribs != NULL) {
        NPH_FREE_ARRAY(data->alloc, NphRenderVertexAttrib, data->attribs, data->vertex_count);
    }
    if (data->indices != NULL) {
        NPH_FREE_ARRAY(data->alloc, u32, data->indices, data->index_count);
    }
    memset(data, 0, sizeof *data);
}

void nph_render_mesh_data_desc(const NphRenderMeshData *data, const char *name,
                               NphRenderSubmesh *submesh, NphRenderMeshDesc *out)
{
    NPH_ASSERT(data != NULL && submesh != NULL && out != NULL);
    submesh->index_offset = 0U;
    submesh->index_count = data->index_count;
    submesh->material_slot = 0U;
    memset(out, 0, sizeof *out);
    out->vertex_count = data->vertex_count;
    out->index_count = data->index_count;
    out->positions = data->positions;
    out->attribs = data->attribs;
    out->indices = data->indices;
    out->submeshes = submesh;
    out->submesh_count = 1U;
    out->position_offset = data->position_offset;
    out->position_scale = data->position_scale;
    out->bounds = data->bounds;
    out->name = name;
}

// ---------------------------------------------------------------------------
// Grids
// ---------------------------------------------------------------------------

typedef struct Builder {
    NphRenderVertexInput *vertices;
    u32 *indices;
    u32 vertex_count;
    u32 index_count;
    u32 vertex_cap;
    u32 index_cap;
} Builder;

static NphResult builder_init(Builder *b, NphAllocator *alloc, u32 vertices, u32 indices)
{
    memset(b, 0, sizeof *b);
    b->vertices = NPH_ALLOC_ARRAY(alloc, NphRenderVertexInput, vertices);
    b->indices = NPH_ALLOC_ARRAY(alloc, u32, indices);
    b->vertex_cap = vertices;
    b->index_cap = indices;
    return b->vertices != NULL && b->indices != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
}

static void builder_free(Builder *b, NphAllocator *alloc)
{
    if (b->vertices != NULL) {
        NPH_FREE_ARRAY(alloc, NphRenderVertexInput, b->vertices, b->vertex_cap);
    }
    if (b->indices != NULL) {
        NPH_FREE_ARRAY(alloc, u32, b->indices, b->index_cap);
    }
}

static u32 builder_vertex(Builder *b, NphVec3 p, NphVec3 n, NphVec2 uv)
{
    NPH_ASSERT(b->vertex_count < b->vertex_cap);
    NphRenderVertexInput *v = &b->vertices[b->vertex_count];
    v->position = p;
    v->normal = n;
    v->uv = uv;
    v->color = nph_vec4_one();
    return b->vertex_count++;
}

static void builder_triangle(Builder *b, u32 i0, u32 i1, u32 i2)
{
    NPH_ASSERT(b->index_count + 3U <= b->index_cap);
    b->indices[b->index_count++] = i0;
    b->indices[b->index_count++] = i1;
    b->indices[b->index_count++] = i2;
}

// The two counter-clockwise triangles of the quad whose top-left vertex is
// `i00` in a grid `stride` vertices wide (u right, v down from outside).
static void builder_quad(Builder *b, u32 i00, u32 stride)
{
    const u32 i10 = i00 + 1U;
    const u32 i01 = i00 + stride;
    const u32 i11 = i01 + 1U;
    builder_triangle(b, i00, i01, i11);
    builder_triangle(b, i00, i11, i10);
}

// A flat face: centre `c`, unit axes `right` and `up` with
// cross(right, up) the outward normal, half extents `hr` and `hu`, split
// into n x n quads, UVs spanning [0, uv_scale].
static void face_grid(Builder *b, NphVec3 c, NphVec3 right, NphVec3 up, f32 hr, f32 hu, u32 n,
                      f32 uv_scale)
{
    const NphVec3 normal = nph_vec3_normalize(nph_vec3_cross(right, up));
    const u32 base = b->vertex_count;
    for (u32 j = 0; j <= n; ++j) {
        for (u32 i = 0; i <= n; ++i) {
            const f32 s = (f32)i / (f32)n;
            const f32 t = (f32)j / (f32)n;
            const NphVec3 p =
                nph_vec3_add(c, nph_vec3_add(nph_vec3_scale(right, hr * (2.0F * s - 1.0F)),
                                             nph_vec3_scale(up, hu * (1.0F - 2.0F * t))));
            NPH_IGNORE_RESULT(builder_vertex(b, p, normal, nph_vec2(s * uv_scale, t * uv_scale)));
        }
    }
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            builder_quad(b, base + j * (n + 1U) + i, n + 1U);
        }
    }
}

static NphResult finish(NphAllocator *alloc, Builder *b, NphRenderMeshData *out)
{
    const NphResult r = nph_render_mesh_data_build(alloc, b->vertices, b->vertex_count, b->indices,
                                                   b->index_count, out);
    builder_free(b, alloc);
    return r;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

NphResult nph_render_primitive_cube(NphAllocator *alloc, f32 size, NphRenderMeshData *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (!(size > 0.0F) || !nph_f32_is_finite(size)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Builder b;
    NphResult r = builder_init(&b, alloc, 24U, 36U);
    if (r != NPH_RESULT_OK) {
        builder_free(&b, alloc);
        return r;
    }
    const f32 h = size * 0.5F;
    // (normal, right, up) per face with cross(right, up) = normal.
    static const f32 faces[6][3][3] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (u32 f = 0; f < 6U; ++f) {
        const NphVec3 n = nph_vec3(faces[f][0][0], faces[f][0][1], faces[f][0][2]);
        const NphVec3 right = nph_vec3(faces[f][1][0], faces[f][1][1], faces[f][1][2]);
        const NphVec3 up = nph_vec3(faces[f][2][0], faces[f][2][1], faces[f][2][2]);
        face_grid(&b, nph_vec3_scale(n, h), right, up, h, h, 1U, 1.0F);
    }
    return finish(alloc, &b, out);
}

NphResult nph_render_primitive_plane(NphAllocator *alloc, f32 size, u32 subdivisions, f32 uv_scale,
                                     NphRenderMeshData *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (!(size > 0.0F) || !nph_f32_is_finite(size) || subdivisions == 0U || subdivisions > 1024U ||
        !nph_f32_is_finite(uv_scale)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 n = subdivisions;
    Builder b;
    NphResult r = builder_init(&b, alloc, (n + 1U) * (n + 1U), n * n * 6U);
    if (r != NPH_RESULT_OK) {
        builder_free(&b, alloc);
        return r;
    }
    const f32 h = size * 0.5F;
    face_grid(&b, nph_vec3_zero(), nph_vec3_unit_x(), nph_vec3(0.0F, 0.0F, -1.0F), h, h, n,
              uv_scale);
    return finish(alloc, &b, out);
}

// The point of a unit sphere at polar angle theta (0 at +Y) and azimuth
// phi, oriented so that phi grows to the right seen from outside.
static NphVec3 sphere_point(f32 theta, f32 phi)
{
    const f32 st = nph_f32_sin(theta);
    return nph_vec3(st * nph_f32_cos(phi), nph_f32_cos(theta), -st * nph_f32_sin(phi));
}

NphResult nph_render_primitive_sphere(NphAllocator *alloc, f32 radius, u32 segments, u32 rings,
                                      NphRenderMeshData *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (!(radius > 0.0F) || !nph_f32_is_finite(radius) || segments < 3U || rings < 2U ||
        segments > 4096U || rings > 4096U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Builder b;
    NphResult r = builder_init(&b, alloc, (segments + 1U) * (rings + 1U), segments * rings * 6U);
    if (r != NPH_RESULT_OK) {
        builder_free(&b, alloc);
        return r;
    }
    for (u32 j = 0; j <= rings; ++j) {
        const f32 v = (f32)j / (f32)rings;
        for (u32 i = 0; i <= segments; ++i) {
            const f32 u = (f32)i / (f32)segments;
            const NphVec3 n = sphere_point(v * NPH_PI, u * 2.0F * NPH_PI);
            NPH_IGNORE_RESULT(builder_vertex(&b, nph_vec3_scale(n, radius), n, nph_vec2(u, v)));
        }
    }
    const u32 stride = segments + 1U;
    for (u32 j = 0; j < rings; ++j) {
        for (u32 i = 0; i < segments; ++i) {
            const u32 i00 = j * stride + i;
            const u32 i10 = i00 + 1U;
            const u32 i01 = i00 + stride;
            const u32 i11 = i01 + 1U;
            // The pole rows collapse one triangle of each quad to a line.
            if (j + 1U < rings) {
                builder_triangle(&b, i00, i01, i11);
            }
            if (j > 0U) {
                builder_triangle(&b, i00, i11, i10);
            }
        }
    }
    return finish(alloc, &b, out);
}

NphResult nph_render_primitive_cylinder(NphAllocator *alloc, f32 radius, f32 height, u32 segments,
                                        NphRenderMeshData *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    memset(out, 0, sizeof *out);
    if (!(radius > 0.0F) || !(height > 0.0F) || !nph_f32_is_finite(radius) ||
        !nph_f32_is_finite(height) || segments < 3U || segments > 4096U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const u32 ring = segments + 1U;
    Builder b;
    NphResult r = builder_init(&b, alloc, ring * 2U + 2U * (ring + 1U), segments * 12U);
    if (r != NPH_RESULT_OK) {
        builder_free(&b, alloc);
        return r;
    }
    const f32 hh = height * 0.5F;
    // The side: two rows, top then bottom, normals radial.
    for (u32 row = 0; row < 2U; ++row) {
        const f32 y = row == 0U ? hh : -hh;
        for (u32 i = 0; i <= segments; ++i) {
            const f32 u = (f32)i / (f32)segments;
            const NphVec3 n = sphere_point(0.5F * NPH_PI, u * 2.0F * NPH_PI);
            NPH_IGNORE_RESULT(builder_vertex(&b, nph_vec3(n.x * radius, y, n.z * radius), n,
                                             nph_vec2(u, (f32)row)));
        }
    }
    for (u32 i = 0; i < segments; ++i) {
        builder_quad(&b, i, ring);
    }
    // The caps: a centre and a ring each, UVs projected from above (top)
    // and below (bottom).
    for (u32 cap = 0; cap < 2U; ++cap) {
        const bool top = cap == 0U;
        const f32 y = top ? hh : -hh;
        const NphVec3 n = top ? nph_vec3_unit_y() : nph_vec3(0.0F, -1.0F, 0.0F);
        const u32 center = builder_vertex(&b, nph_vec3(0.0F, y, 0.0F), n, nph_vec2(0.5F, 0.5F));
        const u32 first = b.vertex_count;
        for (u32 i = 0; i <= segments; ++i) {
            const NphVec3 d = sphere_point(0.5F * NPH_PI, (f32)i / (f32)segments * 2.0F * NPH_PI);
            const NphVec2 uv =
                nph_vec2(0.5F + 0.5F * d.x, top ? 0.5F + 0.5F * d.z : 0.5F - 0.5F * d.z);
            NPH_IGNORE_RESULT(builder_vertex(&b, nph_vec3(d.x * radius, y, d.z * radius), n, uv));
        }
        for (u32 i = 0; i < segments; ++i) {
            if (top) {
                builder_triangle(&b, center, first + i, first + i + 1U);
            } else {
                builder_triangle(&b, center, first + i + 1U, first + i);
            }
        }
    }
    return finish(alloc, &b, out);
}
