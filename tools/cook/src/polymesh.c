// SPDX-License-Identifier: Apache-2.0
// Editable meshes (polymesh.h): the polygon mesh, its `.nmesh` file, its
// triangulation and normals, extrusion, and the primitives.
#include <nepheline/core/assert.h>
#include <nepheline/core/hashmap.h>
#include <nepheline/core/json.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>

#include <cook/polymesh.h>
#include <stdio.h>
#include <string.h>

// UV coordinates beyond this are refused: far past any tiling a material
// wants.
#define MAX_UV 1e6F

// The longest path cook_polymesh_save takes (its ".part" included).
#define SAVE_PATH_MAX 4096U

#define NONE NPH_U32_MAX

// ---------------------------------------------------------------------------
// The mesh
// ---------------------------------------------------------------------------

void cook_polymesh_init(CookPolyMesh *m, NphAllocator *alloc)
{
    NPH_ASSERT(m != NULL && alloc != NULL);
    memset(m, 0, sizeof *m);
    m->alloc = alloc;
    NPH_ARRAY_INIT(&m->positions, alloc);
    NPH_ARRAY_INIT(&m->faces, alloc);
    NPH_ARRAY_INIT(&m->corners, alloc);
    NPH_ARRAY_INIT(&m->uvs, alloc);
    m->smoothing = COOK_POLYMESH_SMOOTHING;
}

void cook_polymesh_free(CookPolyMesh *m)
{
    if (m == NULL || m->alloc == NULL) {
        return;
    }
    NPH_ARRAY_FREE(&m->positions);
    NPH_ARRAY_FREE(&m->faces);
    NPH_ARRAY_FREE(&m->corners);
    NPH_ARRAY_FREE(&m->uvs);
}

void cook_polymesh_clear(CookPolyMesh *m)
{
    NPH_ASSERT(m != NULL);
    NPH_ARRAY_CLEAR(&m->positions);
    NPH_ARRAY_CLEAR(&m->faces);
    NPH_ARRAY_CLEAR(&m->corners);
    NPH_ARRAY_CLEAR(&m->uvs);
}

NphResult cook_polymesh_copy(const CookPolyMesh *src, NphAllocator *alloc, CookPolyMesh *out)
{
    NPH_ASSERT(src != NULL && out != NULL);
    cook_polymesh_init(out, alloc);
    out->smoothing = src->smoothing;
    NphResult r = NPH_RESULT_OK;
    if (src->positions.len != 0U) {
        r = NPH_ARRAY_PUSH_N(&out->positions, src->positions.data, src->positions.len);
    }
    if (r == NPH_RESULT_OK && src->faces.len != 0U) {
        r = NPH_ARRAY_PUSH_N(&out->faces, src->faces.data, src->faces.len);
    }
    if (r == NPH_RESULT_OK && src->corners.len != 0U) {
        r = NPH_ARRAY_PUSH_N(&out->corners, src->corners.data, src->corners.len);
    }
    if (r == NPH_RESULT_OK && src->uvs.len != 0U) {
        r = NPH_ARRAY_PUSH_N(&out->uvs, src->uvs.data, src->uvs.len);
    }
    if (r != NPH_RESULT_OK) {
        cook_polymesh_free(out);
        cook_polymesh_init(out, alloc);
    }
    return r;
}

NphResult cook_polymesh_add_position(CookPolyMesh *m, NphVec3 position, u32 *out_index)
{
    NPH_ASSERT(m != NULL);
    if (m->positions.len >= COOK_POLYMESH_MAX_POSITIONS) {
        return NPH_RESULT_FULL;
    }
    const u32 index = m->positions.len;
    NPH_TRY(NPH_ARRAY_PUSH(&m->positions, position));
    if (out_index != NULL) {
        *out_index = index;
    }
    return NPH_RESULT_OK;
}

NphResult cook_polymesh_add_face(CookPolyMesh *m, const u32 *positions, const NphVec2 *uvs,
                                 u32 count, u32 material, u32 *out_face)
{
    NPH_ASSERT(m != NULL && positions != NULL);
    if (count < 3U || count > COOK_POLYMESH_MAX_FACE_CORNERS ||
        material >= COOK_POLYMESH_MATERIALS || m->faces.len >= COOK_POLYMESH_MAX_FACES ||
        m->corners.len > COOK_POLYMESH_MAX_CORNERS - count) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < count; ++i) {
        if (positions[i] >= m->positions.len) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    // Room first, so a failure leaves the mesh as it was.
    NPH_TRY(NPH_ARRAY_RESERVE(&m->faces, m->faces.len + 1U));
    NPH_TRY(NPH_ARRAY_RESERVE(&m->corners, m->corners.len + count));
    NPH_TRY(NPH_ARRAY_RESERVE(&m->uvs, m->uvs.len + count));
    const CookPolyFace face = {m->corners.len, count, material};
    const u32 index = m->faces.len;
    NPH_VERIFY(NPH_ARRAY_PUSH(&m->faces, face) == NPH_RESULT_OK);
    for (u32 i = 0; i < count; ++i) {
        NPH_VERIFY(NPH_ARRAY_PUSH(&m->corners, positions[i]) == NPH_RESULT_OK);
        NPH_VERIFY(NPH_ARRAY_PUSH(&m->uvs, uvs != NULL ? uvs[i] : nph_vec2_zero()) ==
                   NPH_RESULT_OK);
    }
    if (out_face != NULL) {
        *out_face = index;
    }
    return NPH_RESULT_OK;
}

static bool coordinate_ok(f32 x, f32 limit)
{
    return x >= -limit && x <= limit; // also false for NaN
}

NphResult cook_polymesh_validate(const CookPolyMesh *m)
{
    NPH_ASSERT(m != NULL);
    if (m->positions.len > COOK_POLYMESH_MAX_POSITIONS || m->faces.len > COOK_POLYMESH_MAX_FACES ||
        m->corners.len > COOK_POLYMESH_MAX_CORNERS || m->uvs.len != m->corners.len ||
        !(m->smoothing >= 0.0F && m->smoothing <= 180.0F)) {
        return NPH_RESULT_CORRUPT;
    }
    for (u32 i = 0; i < m->positions.len; ++i) {
        const NphVec3 p = m->positions.data[i];
        if (!coordinate_ok(p.x, COOK_MESH_MAX_COORDINATE) ||
            !coordinate_ok(p.y, COOK_MESH_MAX_COORDINATE) ||
            !coordinate_ok(p.z, COOK_MESH_MAX_COORDINATE)) {
            return NPH_RESULT_CORRUPT;
        }
    }
    // The faces tile the corners in order.
    u32 next = 0U;
    for (u32 f = 0; f < m->faces.len; ++f) {
        const CookPolyFace *face = &m->faces.data[f];
        if (face->first != next || face->count < 3U ||
            face->count > COOK_POLYMESH_MAX_FACE_CORNERS ||
            face->material >= COOK_POLYMESH_MATERIALS || face->count > m->corners.len - next) {
            return NPH_RESULT_CORRUPT;
        }
        next += face->count;
    }
    if (next != m->corners.len) {
        return NPH_RESULT_CORRUPT;
    }
    for (u32 c = 0; c < m->corners.len; ++c) {
        const NphVec2 uv = m->uvs.data[c];
        if (m->corners.data[c] >= m->positions.len || !coordinate_ok(uv.x, MAX_UV) ||
            !coordinate_ok(uv.y, MAX_UV)) {
            return NPH_RESULT_CORRUPT;
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// The `.nmesh` file
// ---------------------------------------------------------------------------

// `n` numbers of an array, each within `limit`.
static NphResult read_numbers(const NphJsonValue *v, u32 n, f32 limit, f32 *out)
{
    if (v == NULL || v->type != NPH_JSON_ARRAY || v->count != n) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u32 i = 0U;
    for (const NphJsonValue *e = v->first_child; e != NULL; e = e->next) {
        if (e->type != NPH_JSON_NUMBER || !(e->number >= (f64)-limit && e->number <= (f64)limit)) {
            return NPH_RESULT_PARSE_ERROR;
        }
        out[i++] = (f32)e->number;
    }
    return NPH_RESULT_OK;
}

static NphResult read_face(CookPolyMesh *m, const NphJsonValue *face)
{
    if (face == NULL || face->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const NphJsonValue *v = nph_json_object_get(face, NPH_STR("v"));
    if (v == NULL || v->type != NPH_JSON_ARRAY || v->count < 3U ||
        v->count > COOK_POLYMESH_MAX_FACE_CORNERS) {
        return NPH_RESULT_PARSE_ERROR;
    }
    u32 positions[COOK_POLYMESH_MAX_FACE_CORNERS];
    NphVec2 uvs[COOK_POLYMESH_MAX_FACE_CORNERS];
    const u32 count = v->count;
    u32 i = 0U;
    for (const NphJsonValue *e = v->first_child; e != NULL; e = e->next) {
        i64 index = 0;
        if (nph_json_as_i64(e, &index) != NPH_RESULT_OK || index < 0 ||
            index >= (i64)m->positions.len) {
            return NPH_RESULT_PARSE_ERROR;
        }
        positions[i++] = (u32)index;
    }
    memset(uvs, 0, sizeof(NphVec2) * (u64)count);
    const NphJsonValue *uv = nph_json_object_get(face, NPH_STR("uv"));
    if (uv != NULL) {
        if (uv->type != NPH_JSON_ARRAY || uv->count != count) {
            return NPH_RESULT_PARSE_ERROR;
        }
        i = 0U;
        for (const NphJsonValue *e = uv->first_child; e != NULL; e = e->next) {
            f32 pair[2] = {0.0F, 0.0F};
            NPH_TRY(read_numbers(e, 2U, MAX_UV, pair));
            uvs[i++] = nph_vec2(pair[0], pair[1]);
        }
    }
    i64 material = 0;
    const NphJsonValue *mv = nph_json_object_get(face, NPH_STR("m"));
    if (mv != NULL && (nph_json_as_i64(mv, &material) != NPH_RESULT_OK || material < 0 ||
                       material >= (i64)COOK_POLYMESH_MATERIALS)) {
        return NPH_RESULT_PARSE_ERROR;
    }
    const NphResult r = cook_polymesh_add_face(m, positions, uvs, count, (u32)material, NULL);
    return r == NPH_RESULT_INVALID_ARGUMENT ? NPH_RESULT_PARSE_ERROR : r;
}

static NphResult read_document(CookPolyMesh *m, const NphJsonValue *root)
{
    if (root == NULL || root->type != NPH_JSON_OBJECT) {
        return NPH_RESULT_PARSE_ERROR;
    }
    i64 version = 0;
    if (nph_json_as_i64(nph_json_object_get(root, NPH_STR("nmesh")), &version) != NPH_RESULT_OK) {
        return NPH_RESULT_PARSE_ERROR;
    }
    if (version != (i64)COOK_POLYMESH_VERSION) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    const NphJsonValue *smoothing = nph_json_object_get(root, NPH_STR("smoothing"));
    if (smoothing != NULL) {
        if (smoothing->type != NPH_JSON_NUMBER || !(smoothing->number >= 0.0) ||
            !(smoothing->number <= 180.0)) {
            return NPH_RESULT_PARSE_ERROR;
        }
        m->smoothing = (f32)smoothing->number;
    }
    const NphJsonValue *positions = nph_json_object_get(root, NPH_STR("positions"));
    const NphJsonValue *faces = nph_json_object_get(root, NPH_STR("faces"));
    if (positions == NULL || positions->type != NPH_JSON_ARRAY ||
        positions->count > COOK_POLYMESH_MAX_POSITIONS || faces == NULL ||
        faces->type != NPH_JSON_ARRAY || faces->count > COOK_POLYMESH_MAX_FACES) {
        return NPH_RESULT_PARSE_ERROR;
    }
    NPH_TRY(NPH_ARRAY_RESERVE(&m->positions, positions->count));
    for (const NphJsonValue *e = positions->first_child; e != NULL; e = e->next) {
        f32 p[3] = {0.0F, 0.0F, 0.0F};
        NPH_TRY(read_numbers(e, 3U, COOK_MESH_MAX_COORDINATE, p));
        NPH_TRY(cook_polymesh_add_position(m, nph_vec3(p[0], p[1], p[2]), NULL));
    }
    NPH_TRY(NPH_ARRAY_RESERVE(&m->faces, faces->count));
    for (const NphJsonValue *e = faces->first_child; e != NULL; e = e->next) {
        NPH_TRY(read_face(m, e));
    }
    return NPH_RESULT_OK;
}

NphResult cook_polymesh_read(NphAllocator *alloc, NphStr text, CookPolyMesh *out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    cook_polymesh_init(out, alloc);
    NphJsonDoc doc;
    NphResult r = nph_json_parse(text, alloc, &doc);
    if (r == NPH_RESULT_OK) {
        r = read_document(out, doc.root);
    } else if (r != NPH_RESULT_OUT_OF_MEMORY) {
        r = NPH_RESULT_PARSE_ERROR;
    }
    nph_json_doc_free(&doc);
    if (r != NPH_RESULT_OK) {
        cook_polymesh_free(out);
        cook_polymesh_init(out, alloc);
    }
    return r;
}

NphResult cook_polymesh_load(NphAllocator *alloc, const char *path, CookPolyMesh *out)
{
    NPH_ASSERT(alloc != NULL && path != NULL && out != NULL);
    cook_polymesh_init(out, alloc);
    void *data = NULL;
    u64 size = 0U;
    NPH_TRY(nph_file_read_all(path, alloc, &data, &size));
    const NphResult r = cook_polymesh_read(alloc, nph_str((const char *)data, size), out);
    nph_file_free_all(alloc, data, size);
    return r;
}

static NphResult write_vec(NphJsonWriter *w, const f32 *v, u32 n)
{
    NPH_TRY(nph_json_writer_begin_array_inline(w));
    for (u32 i = 0; i < n; ++i) {
        // -0 reads back as 0: written as 0, so the file is the same either way.
        NPH_TRY(nph_json_writer_f32_shortest(w, v[i] == 0.0F ? 0.0F : v[i]));
    }
    return nph_json_writer_end_array(w);
}

static NphResult write_document(const CookPolyMesh *m, NphJsonWriter *w)
{
    NPH_TRY(nph_json_writer_begin_object(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("nmesh")));
    NPH_TRY(nph_json_writer_i64(w, (i64)COOK_POLYMESH_VERSION));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("smoothing")));
    NPH_TRY(nph_json_writer_f32_shortest(w, m->smoothing));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("positions")));
    NPH_TRY(nph_json_writer_begin_array(w));
    for (u32 i = 0; i < m->positions.len; ++i) {
        const NphVec3 p = m->positions.data[i];
        const f32 v[3] = {p.x, p.y, p.z};
        NPH_TRY(write_vec(w, v, 3U));
    }
    NPH_TRY(nph_json_writer_end_array(w));
    NPH_TRY(nph_json_writer_key(w, NPH_STR("faces")));
    NPH_TRY(nph_json_writer_begin_array(w));
    for (u32 f = 0; f < m->faces.len; ++f) {
        const CookPolyFace *face = &m->faces.data[f];
        NPH_TRY(nph_json_writer_begin_object(w));
        NPH_TRY(nph_json_writer_key(w, NPH_STR("v")));
        NPH_TRY(nph_json_writer_begin_array_inline(w));
        for (u32 i = 0; i < face->count; ++i) {
            NPH_TRY(nph_json_writer_i64(w, (i64)m->corners.data[face->first + i]));
        }
        NPH_TRY(nph_json_writer_end_array(w));
        NPH_TRY(nph_json_writer_key(w, NPH_STR("uv")));
        NPH_TRY(nph_json_writer_begin_array_inline(w));
        for (u32 i = 0; i < face->count; ++i) {
            const NphVec2 uv = m->uvs.data[face->first + i];
            const f32 v[2] = {uv.x, uv.y};
            NPH_TRY(write_vec(w, v, 2U));
        }
        NPH_TRY(nph_json_writer_end_array(w));
        if (face->material != 0U) {
            NPH_TRY(nph_json_writer_key(w, NPH_STR("m")));
            NPH_TRY(nph_json_writer_i64(w, (i64)face->material));
        }
        NPH_TRY(nph_json_writer_end_object(w));
    }
    NPH_TRY(nph_json_writer_end_array(w));
    NPH_TRY(nph_json_writer_end_object(w));
    return nph_sb_append_cstr(w->out, "\n");
}

NphResult cook_polymesh_write(const CookPolyMesh *m, NphStrBuilder *out)
{
    NPH_ASSERT(m != NULL && out != NULL);
    NPH_TRY(cook_polymesh_validate(m));
    NphJsonWriter w;
    nph_json_writer_init(&w, out, true);
    return write_document(m, &w);
}

NphResult cook_polymesh_save(const CookPolyMesh *m, const char *path)
{
    NPH_ASSERT(m != NULL && path != NULL);
    char temp[SAVE_PATH_MAX];
    const int n = snprintf(temp, sizeof temp, "%s.part", path);
    if (n < 0 || (u64)n >= sizeof temp) {
        return NPH_RESULT_OVERFLOW;
    }
    NphStrBuilder sb;
    nph_sb_init(&sb, m->alloc);
    NphResult r = cook_polymesh_write(m, &sb);
    if (r == NPH_RESULT_OK) {
        r = nph_file_write_all(temp, sb.data, sb.len);
    }
    nph_sb_free(&sb);
    if (r == NPH_RESULT_OK) {
        r = nph_fs_rename(temp, path);
    }
    if (r != NPH_RESULT_OK) {
        (void)nph_fs_remove_file(temp);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

static NphVec3 corner_position(const CookPolyMesh *m, u32 corner)
{
    return m->positions.data[m->corners.data[corner]];
}

NphVec3 cook_polymesh_face_normal(const CookPolyMesh *m, u32 face)
{
    NPH_ASSERT(m != NULL && face < m->faces.len);
    const CookPolyFace *f = &m->faces.data[face];
    // Relative to the first corner, so faces far from the origin keep
    // their precision.
    const NphVec3 o = corner_position(m, f->first);
    NphVec3 n = nph_vec3_zero();
    for (u32 i = 0; i < f->count; ++i) {
        const NphVec3 a = nph_vec3_sub(corner_position(m, f->first + i), o);
        const NphVec3 b = nph_vec3_sub(corner_position(m, f->first + (i + 1U) % f->count), o);
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return nph_vec3_normalize_or(n, nph_vec3_zero());
}

NphVec3 cook_polymesh_face_center(const CookPolyMesh *m, u32 face)
{
    NPH_ASSERT(m != NULL && face < m->faces.len);
    const CookPolyFace *f = &m->faces.data[face];
    NphVec3 sum = nph_vec3_zero();
    for (u32 i = 0; i < f->count; ++i) {
        sum = nph_vec3_add(sum, corner_position(m, f->first + i));
    }
    return nph_vec3_scale(sum, 1.0F / (f32)f->count);
}

// Twice the signed area of (a, b, c): positive when they turn left.
static f32 turn(NphVec2 a, NphVec2 b, NphVec2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Inside or on the edge of the counter-clockwise triangle (a, b, c).
static bool in_triangle(NphVec2 p, NphVec2 a, NphVec2 b, NphVec2 c)
{
    return turn(a, b, p) >= 0.0F && turn(b, c, p) >= 0.0F && turn(c, a, p) >= 0.0F;
}

u32 cook_polymesh_triangulate_face(const CookPolyMesh *m, u32 face, u32 *out)
{
    NPH_ASSERT(m != NULL && face < m->faces.len && out != NULL);
    const CookPolyFace *f = &m->faces.data[face];
    const u32 n = f->count;
    NPH_ASSERT(n >= 3U && n <= COOK_POLYMESH_MAX_FACE_CORNERS);
    u32 tri = 0U;
    const NphVec3 normal = cook_polymesh_face_normal(m, face);
    // The corners in the plane the face faces, counter-clockwise as seen
    // from its front: (u, v, normal) is right-handed.
    NphVec2 p[COOK_POLYMESH_MAX_FACE_CORNERS];
    bool convex = !nph_vec3_eq(normal, nph_vec3_zero());
    if (convex) {
        NphVec3 u;
        NphVec3 v;
        nph_vec3_orthonormal_basis(normal, &u, &v);
        const NphVec3 o = corner_position(m, f->first);
        for (u32 i = 0; i < n; ++i) {
            const NphVec3 d = nph_vec3_sub(corner_position(m, f->first + i), o);
            p[i] = nph_vec2(nph_vec3_dot(d, u), nph_vec3_dot(d, v));
        }
        for (u32 i = 0; i < n && convex; ++i) {
            convex = turn(p[(i + n - 1U) % n], p[i], p[(i + 1U) % n]) >= 0.0F;
        }
    }
    u32 ring[COOK_POLYMESH_MAX_FACE_CORNERS];
    u32 left = n;
    for (u32 i = 0; i < n; ++i) {
        ring[i] = i;
    }
    if (!convex && !nph_vec3_eq(normal, nph_vec3_zero())) {
        // Ear clipping: cut a corner that turns left and holds no other
        // corner, until a triangle is left.
        while (left > 3U) {
            bool cut = false;
            for (u32 k = 0; k < left && !cut; ++k) {
                const u32 a = ring[(k + left - 1U) % left];
                const u32 b = ring[k];
                const u32 c = ring[(k + 1U) % left];
                if (turn(p[a], p[b], p[c]) <= 0.0F) {
                    continue; // reflex, or a straight corner
                }
                bool empty = true;
                for (u32 j = 0; j < left && empty; ++j) {
                    const u32 q = ring[j];
                    empty = q == a || q == b || q == c || !in_triangle(p[q], p[a], p[b], p[c]);
                }
                if (!empty) {
                    continue;
                }
                out[(u64)tri * 3U] = a;
                out[(u64)tri * 3U + 1U] = b;
                out[(u64)tri * 3U + 2U] = c;
                ++tri;
                memmove(&ring[k], &ring[k + 1U], sizeof(u32) * (u64)(left - k - 1U));
                --left;
                cut = true;
            }
            if (!cut) {
                break; // degenerate or crossing itself: the fan below
            }
        }
    }
    // A convex face, the last triangle, or what clipping could not cut.
    for (u32 k = 1; k + 1U < left; ++k) {
        out[(u64)tri * 3U] = ring[0];
        out[(u64)tri * 3U + 1U] = ring[k];
        out[(u64)tri * 3U + 2U] = ring[k + 1U];
        ++tri;
    }
    return tri;
}

// The angle at `corner` of its face: how much of the space around its
// position the face covers, the weight of its normal there.
static f32 corner_angle(const CookPolyMesh *m, const CookPolyFace *f, u32 i)
{
    const NphVec3 at = corner_position(m, f->first + i);
    const NphVec3 prev =
        nph_vec3_sub(corner_position(m, f->first + (i + f->count - 1U) % f->count), at);
    const NphVec3 next = nph_vec3_sub(corner_position(m, f->first + (i + 1U) % f->count), at);
    if (nph_vec3_length_sq(prev) == 0.0F || nph_vec3_length_sq(next) == 0.0F) {
        return 0.0F;
    }
    return nph_vec3_angle(prev, next);
}

// The corners around each position, and what the normals need of them.
// The arrays have one slot more than they need, so an empty mesh never asks
// for zero bytes.
typedef struct Adjacency {
    NphAllocator *alloc;
    u32 positions;
    u32 corners;
    u32 *start;       // positions + 1: corners around position p are list[start[p] .. start[p + 1])
    u32 *list;        // corner indices
    u32 *corner_face; // per corner
    NphVec3 *face_normal;
    f32 *angle; // per corner
} Adjacency;

static void adjacency_free(Adjacency *a)
{
    const u64 corners = (u64)a->corners + 1U;
    if (a->start != NULL) {
        NPH_FREE_ARRAY(a->alloc, u32, a->start, (u64)a->positions + 1U);
    }
    if (a->list != NULL) {
        NPH_FREE_ARRAY(a->alloc, u32, a->list, corners);
    }
    if (a->corner_face != NULL) {
        NPH_FREE_ARRAY(a->alloc, u32, a->corner_face, corners);
    }
    if (a->angle != NULL) {
        NPH_FREE_ARRAY(a->alloc, f32, a->angle, corners);
    }
    memset(a, 0, sizeof *a);
}

static NphResult adjacency_build(const CookPolyMesh *m, NphVec3 *face_normal, Adjacency *a)
{
    memset(a, 0, sizeof *a);
    a->alloc = m->alloc;
    a->positions = m->positions.len;
    a->corners = m->corners.len;
    a->face_normal = face_normal;
    const u64 corners = (u64)a->corners + 1U;
    a->start = NPH_ALLOC_ARRAY_ZEROED(a->alloc, u32, (u64)a->positions + 1U);
    a->list = NPH_ALLOC_ARRAY(a->alloc, u32, corners);
    a->corner_face = NPH_ALLOC_ARRAY(a->alloc, u32, corners);
    a->angle = NPH_ALLOC_ARRAY(a->alloc, f32, corners);
    if (a->start == NULL || a->list == NULL || a->corner_face == NULL || a->angle == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 f = 0; f < m->faces.len; ++f) {
        const CookPolyFace *face = &m->faces.data[f];
        for (u32 i = 0; i < face->count; ++i) {
            const u32 c = face->first + i;
            a->corner_face[c] = f;
            a->angle[c] = corner_angle(m, face, i);
            a->start[m->corners.data[c] + 1U] += 1U;
        }
    }
    for (u32 p = 0; p < a->positions; ++p) {
        a->start[p + 1U] += a->start[p];
    }
    for (u32 c = 0; c < a->corners; ++c) {
        // start[p] is used as the fill cursor, then restored below.
        const u32 p = m->corners.data[c];
        a->list[a->start[p]++] = c;
    }
    for (u32 p = a->positions; p > 0U; --p) {
        a->start[p] = a->start[p - 1U];
    }
    a->start[0] = 0U;
    return NPH_RESULT_OK;
}

// The normal of corner `c`: its face's, averaged with the faces around its
// position within the smoothing angle, weighted by their corner angles.
static NphVec3 corner_normal(const CookPolyMesh *m, const Adjacency *a, f32 cos_limit, u32 c)
{
    const NphVec3 own = a->face_normal[a->corner_face[c]];
    if (m->smoothing <= 0.0F || nph_vec3_eq(own, nph_vec3_zero())) {
        return own;
    }
    const u32 p = m->corners.data[c];
    NphVec3 sum = nph_vec3_zero();
    for (u32 k = a->start[p]; k < a->start[p + 1U]; ++k) {
        const u32 other = a->list[k];
        const NphVec3 n = a->face_normal[a->corner_face[other]];
        if (nph_vec3_dot(n, own) >= cos_limit) {
            sum = nph_vec3_mul_add(sum, n, a->angle[other]);
        }
    }
    return nph_vec3_normalize_or(sum, own);
}

NphResult cook_polymesh_to_mesh(const CookPolyMesh *m, const char *name, CookMesh *out)
{
    NPH_ASSERT(m != NULL && out != NULL);
    cook_mesh_init(out, m->alloc);
    (void)snprintf(out->name, sizeof out->name, "%s", name != NULL ? name : "mesh");
    NPH_TRY(cook_polymesh_validate(m));
    NphAllocator *alloc = m->alloc;
    NphVec3 *face_normal = NPH_ALLOC_ARRAY(alloc, NphVec3, (u64)m->faces.len + 1U);
    if (face_normal == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 f = 0; f < m->faces.len; ++f) {
        face_normal[f] = cook_polymesh_face_normal(m, f);
    }
    Adjacency adj;
    NphResult r = adjacency_build(m, face_normal, &adj);
    const f32 cos_limit = nph_f32_cos(nph_f32_radians(m->smoothing));
    // A submesh per material slot used: its faces' corners as vertices, and
    // their triangles.
    CookVertexArray vertices;
    NphArrayU32 indices;
    NPH_ARRAY_INIT(&vertices, alloc);
    NPH_ARRAY_INIT(&indices, alloc);
    for (u32 slot = 0; slot < COOK_POLYMESH_MATERIALS && r == NPH_RESULT_OK; ++slot) {
        NPH_ARRAY_CLEAR(&vertices);
        NPH_ARRAY_CLEAR(&indices);
        for (u32 f = 0; f < m->faces.len && r == NPH_RESULT_OK; ++f) {
            const CookPolyFace *face = &m->faces.data[f];
            if (face->material != slot) {
                continue;
            }
            const u32 base = vertices.len;
            for (u32 i = 0; i < face->count && r == NPH_RESULT_OK; ++i) {
                const u32 c = face->first + i;
                CookVertex vx;
                memset(&vx, 0, sizeof vx);
                vx.position = corner_position(m, c);
                vx.normal = corner_normal(m, &adj, cos_limit, c);
                vx.uv0 = m->uvs.data[c];
                r = NPH_ARRAY_PUSH(&vertices, vx);
            }
            u32 local[(COOK_POLYMESH_MAX_FACE_CORNERS - 2U) * 3U];
            const u32 triangles =
                r == NPH_RESULT_OK ? cook_polymesh_triangulate_face(m, f, local) : 0U;
            for (u32 t = 0; t < triangles * 3U && r == NPH_RESULT_OK; ++t) {
                r = NPH_ARRAY_PUSH(&indices, base + local[t]);
            }
        }
        if (r == NPH_RESULT_OK && indices.len != 0U) {
            r = cook_mesh_add_submesh(out, vertices.data, vertices.len, indices.data, indices.len,
                                      slot);
        }
    }
    out->flags = (u32)COOK_MESH_HAS_NORMALS | (u32)COOK_MESH_HAS_UV0;
    NPH_ARRAY_FREE(&vertices);
    NPH_ARRAY_FREE(&indices);
    adjacency_free(&adj);
    NPH_FREE_ARRAY(alloc, NphVec3, face_normal, (u64)m->faces.len + 1U);
    return r;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

static u64 edge_key(u32 a, u32 b)
{
    return ((u64)a << 32U) | (u64)b;
}

typedef struct BoundaryEdge {
    u32 a;
    u32 b;
    u32 material;
} BoundaryEdge;

typedef NPH_ARRAY(BoundaryEdge) BoundaryArray;

NphResult cook_polymesh_extrude(CookPolyMesh *m, const u32 *faces, u32 count, f32 distance,
                                u32 *out_sides)
{
    NPH_ASSERT(m != NULL && (faces != NULL || count == 0U));
    if (out_sides != NULL) {
        *out_sides = 0U;
    }
    for (u32 i = 0; i < count; ++i) {
        if (faces[i] >= m->faces.len) {
            return NPH_RESULT_INVALID_ARGUMENT;
        }
    }
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    NphAllocator *alloc = m->alloc;
    const u32 np = m->positions.len;
    const u32 nf = m->faces.len;
    u8 *selected = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, nf);
    u32 *moved = NPH_ALLOC_ARRAY(alloc, u32, np);
    NphVec3 *normal = NPH_ALLOC_ARRAY_ZEROED(alloc, NphVec3, np);
    NphHashMap edges;
    const NphHashMapDesc ed = nph_hashmap_desc_u64(0U);
    NphResult r = nph_hashmap_init(&edges, alloc, &ed);
    const bool edges_ready = r == NPH_RESULT_OK;
    BoundaryArray boundary;
    NPH_ARRAY_INIT(&boundary, alloc);
    if (selected == NULL || moved == NULL || normal == NULL) {
        r = NPH_RESULT_OUT_OF_MEMORY;
    }
    // The region: its faces once each, its directed edges, and the mean
    // normal of its faces around each of their positions.
    for (u32 i = 0; i < count && r == NPH_RESULT_OK; ++i) {
        if (selected[faces[i]] != 0U) {
            continue;
        }
        selected[faces[i]] = 1U;
        const CookPolyFace f = m->faces.data[faces[i]];
        const NphVec3 n = cook_polymesh_face_normal(m, faces[i]);
        for (u32 k = 0; k < f.count && r == NPH_RESULT_OK; ++k) {
            const u32 a = m->corners.data[f.first + k];
            const u32 b = m->corners.data[f.first + (k + 1U) % f.count];
            normal[a] = nph_vec3_add(normal[a], n);
            if (!nph_hashmap_contains_u64(&edges, edge_key(a, b))) {
                r = nph_hashmap_insert_u64(&edges, edge_key(a, b), NULL);
            }
        }
    }
    // Its boundary: edges no other face of the region runs the other way.
    for (u32 face = 0; face < nf && r == NPH_RESULT_OK; ++face) {
        if (selected[face] == 0U) {
            continue;
        }
        const CookPolyFace f = m->faces.data[face];
        for (u32 k = 0; k < f.count && r == NPH_RESULT_OK; ++k) {
            const u32 a = m->corners.data[f.first + k];
            const u32 b = m->corners.data[f.first + (k + 1U) % f.count];
            if (!nph_hashmap_contains_u64(&edges, edge_key(b, a))) {
                const BoundaryEdge e = {a, b, f.material};
                r = NPH_ARRAY_PUSH(&boundary, e);
            }
        }
    }
    // The region's positions, copied and moved along their normals.
    for (u32 p = 0; p < np && moved != NULL; ++p) {
        moved[p] = NONE;
    }
    for (u32 face = 0; face < nf && r == NPH_RESULT_OK; ++face) {
        if (selected[face] == 0U) {
            continue;
        }
        const CookPolyFace f = m->faces.data[face];
        const NphVec3 own = cook_polymesh_face_normal(m, face);
        for (u32 k = 0; k < f.count && r == NPH_RESULT_OK; ++k) {
            const u32 p = m->corners.data[f.first + k];
            if (moved[p] != NONE) {
                continue;
            }
            const NphVec3 dir = nph_vec3_normalize_or(normal[p], own);
            const NphVec3 to = nph_vec3_mul_add(m->positions.data[p], dir, distance);
            r = cook_polymesh_add_position(m, to, &moved[p]);
        }
    }
    // A quad over each boundary edge, from the old edge to its copy: it
    // runs a -> b as the face outside the region does in reverse, and
    // b' -> a' against the moved face.
    u32 sides = 0U;
    for (u32 i = 0; i < boundary.len && r == NPH_RESULT_OK; ++i) {
        const BoundaryEdge e = boundary.data[i];
        const u32 quad[4] = {e.a, e.b, moved[e.b], moved[e.a]};
        static const NphVec2 uvs[4] = {{0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F}};
        r = cook_polymesh_add_face(m, quad, uvs, 4U, e.material, NULL);
        sides += r == NPH_RESULT_OK ? 1U : 0U;
    }
    // The region's faces onto the copies.
    if (r == NPH_RESULT_OK) {
        for (u32 face = 0; face < nf; ++face) {
            if (selected[face] == 0U) {
                continue;
            }
            const CookPolyFace f = m->faces.data[face];
            for (u32 k = 0; k < f.count; ++k) {
                u32 *corner = &m->corners.data[f.first + k];
                *corner = moved[*corner];
            }
        }
        if (out_sides != NULL) {
            *out_sides = sides;
        }
    }
    NPH_ARRAY_FREE(&boundary);
    if (edges_ready) {
        nph_hashmap_free(&edges);
    }
    if (normal != NULL) {
        NPH_FREE_ARRAY(alloc, NphVec3, normal, np);
    }
    if (moved != NULL) {
        NPH_FREE_ARRAY(alloc, u32, moved, np);
    }
    if (selected != NULL) {
        NPH_FREE_ARRAY(alloc, u8, selected, nf);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

static void begin_primitive(CookPolyMesh *m)
{
    cook_polymesh_clear(m);
    m->smoothing = COOK_POLYMESH_SMOOTHING;
}

NphResult cook_polymesh_cube(CookPolyMesh *m, f32 size)
{
    NPH_ASSERT(m != NULL);
    begin_primitive(m);
    const f32 h = size * 0.5F;
    // Position i has x, y, z from its bits 0, 1, 2 (set: +h).
    for (u32 i = 0; i < 8U; ++i) {
        const NphVec3 p =
            nph_vec3((i & 1U) != 0U ? h : -h, (i & 2U) != 0U ? h : -h, (i & 4U) != 0U ? h : -h);
        NPH_TRY(cook_polymesh_add_position(m, p, NULL));
    }
    // Each face from its bottom left as seen from outside (+Y faces seen
    // with -Z up, -Y faces with +Z up), counter-clockwise, with the
    // texture's bottom left there.
    static const u32 quads[6][4] = {
        {4, 5, 7, 6}, // +Z
        {1, 0, 2, 3}, // -Z
        {5, 1, 3, 7}, // +X
        {0, 4, 6, 2}, // -X
        {6, 7, 3, 2}, // +Y
        {0, 1, 5, 4}, // -Y
    };
    static const NphVec2 uvs[4] = {{0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F}};
    for (u32 f = 0; f < 6U; ++f) {
        NPH_TRY(cook_polymesh_add_face(m, quads[f], uvs, 4U, 0U, NULL));
    }
    return NPH_RESULT_OK;
}

NphResult cook_polymesh_plane(CookPolyMesh *m, f32 size, u32 subdivisions)
{
    NPH_ASSERT(m != NULL);
    if (subdivisions == 0U || subdivisions > 1024U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    begin_primitive(m);
    const u32 n = subdivisions + 1U;
    const f32 step = size / (f32)subdivisions;
    const f32 h = size * 0.5F;
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            NPH_TRY(cook_polymesh_add_position(
                m, nph_vec3(-h + step * (f32)i, 0.0F, -h + step * (f32)j), NULL));
        }
    }
    // (x0, z0), (x0, z1), (x1, z1), (x1, z0) turns counter-clockwise seen
    // from +Y; u follows x and v follows z.
    for (u32 j = 0; j < subdivisions; ++j) {
        for (u32 i = 0; i < subdivisions; ++i) {
            const u32 quad[4] = {j * n + i, (j + 1U) * n + i, (j + 1U) * n + i + 1U,
                                 j * n + i + 1U};
            const f32 u0 = (f32)i / (f32)subdivisions;
            const f32 u1 = (f32)(i + 1U) / (f32)subdivisions;
            const f32 v0 = (f32)j / (f32)subdivisions;
            const f32 v1 = (f32)(j + 1U) / (f32)subdivisions;
            const NphVec2 uvs[4] = {nph_vec2(u0, v0), nph_vec2(u0, v1), nph_vec2(u1, v1),
                                    nph_vec2(u1, v0)};
            NPH_TRY(cook_polymesh_add_face(m, quad, uvs, 4U, 0U, NULL));
        }
    }
    return NPH_RESULT_OK;
}

// The point at angle `a` around +Y, counter-clockwise seen from above.
static NphVec3 around(f32 radius, f32 a, f32 y)
{
    return nph_vec3(radius * nph_f32_cos(a), y, -radius * nph_f32_sin(a));
}

NphResult cook_polymesh_cylinder(CookPolyMesh *m, f32 radius, f32 height, u32 segments)
{
    NPH_ASSERT(m != NULL);
    if (segments < 3U || segments > COOK_POLYMESH_MAX_FACE_CORNERS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    begin_primitive(m);
    const f32 h = height * 0.5F;
    // Bottom ring 0 .. segments - 1, top ring segments .. 2 * segments - 1.
    for (u32 ring = 0; ring < 2U; ++ring) {
        for (u32 i = 0; i < segments; ++i) {
            const f32 a = 2.0F * NPH_PI * (f32)i / (f32)segments;
            NPH_TRY(cook_polymesh_add_position(m, around(radius, a, ring == 0U ? -h : h), NULL));
        }
    }
    for (u32 i = 0; i < segments; ++i) {
        const u32 next = (i + 1U) % segments;
        const u32 quad[4] = {i, next, segments + next, segments + i};
        const f32 u0 = (f32)i / (f32)segments;
        const f32 u1 = (f32)(i + 1U) / (f32)segments; // 1 at the seam, not 0
        const NphVec2 uvs[4] = {nph_vec2(u0, 1.0F), nph_vec2(u1, 1.0F), nph_vec2(u1, 0.0F),
                                nph_vec2(u0, 0.0F)};
        NPH_TRY(cook_polymesh_add_face(m, quad, uvs, 4U, 0U, NULL));
    }
    // The caps: the top ring counter-clockwise from above, the bottom one
    // reversed, mapped as a disc onto the texture.
    u32 cap[COOK_POLYMESH_MAX_FACE_CORNERS];
    NphVec2 uvs[COOK_POLYMESH_MAX_FACE_CORNERS];
    for (u32 i = 0; i < segments; ++i) {
        const f32 a = 2.0F * NPH_PI * (f32)i / (f32)segments;
        cap[i] = segments + i;
        uvs[i] = nph_vec2(0.5F + 0.5F * nph_f32_cos(a), 0.5F + 0.5F * nph_f32_sin(a));
    }
    NPH_TRY(cook_polymesh_add_face(m, cap, uvs, segments, 0U, NULL));
    for (u32 i = 0; i < segments; ++i) {
        const u32 k = segments - 1U - i;
        const f32 a = 2.0F * NPH_PI * (f32)k / (f32)segments;
        cap[i] = k;
        uvs[i] = nph_vec2(0.5F + 0.5F * nph_f32_cos(a), 0.5F - 0.5F * nph_f32_sin(a));
    }
    return cook_polymesh_add_face(m, cap, uvs, segments, 0U, NULL);
}

NphResult cook_polymesh_sphere(CookPolyMesh *m, f32 radius, u32 segments, u32 rings)
{
    NPH_ASSERT(m != NULL);
    if (segments < 3U || rings < 2U || segments > 4096U || rings > 4096U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    begin_primitive(m);
    // The poles, then the rings between them from north to south.
    NPH_TRY(cook_polymesh_add_position(m, nph_vec3(0.0F, radius, 0.0F), NULL));
    NPH_TRY(cook_polymesh_add_position(m, nph_vec3(0.0F, -radius, 0.0F), NULL));
    for (u32 k = 1; k < rings; ++k) {
        const f32 polar = NPH_PI * (f32)k / (f32)rings;
        const f32 ring_radius = radius * nph_f32_sin(polar);
        const f32 y = radius * nph_f32_cos(polar);
        for (u32 i = 0; i < segments; ++i) {
            const f32 a = 2.0F * NPH_PI * (f32)i / (f32)segments;
            NPH_TRY(cook_polymesh_add_position(m, around(ring_radius, a, y), NULL));
        }
    }
    // Ring k (1 .. rings - 1) starts at position 2 + (k - 1) * segments; the
    // poles stand in for rings 0 and `rings`. Band k lies between rings k and
    // k + 1.
    for (u32 k = 0; k < rings; ++k) {
        const f32 v0 = (f32)k / (f32)rings;
        const f32 v1 = (f32)(k + 1U) / (f32)rings;
        const u32 upper = k != 0U ? 2U + (k - 1U) * segments : 0U;
        const u32 lower = k + 1U != rings ? 2U + k * segments : 1U;
        for (u32 i = 0; i < segments; ++i) {
            const u32 next = (i + 1U) % segments;
            const f32 u0 = (f32)i / (f32)segments;
            const f32 u1 = (f32)(i + 1U) / (f32)segments;
            if (k == 0U) {
                const u32 tri[3] = {upper, lower + i, lower + next};
                const NphVec2 uvs[3] = {nph_vec2((u0 + u1) * 0.5F, v0), nph_vec2(u0, v1),
                                        nph_vec2(u1, v1)};
                NPH_TRY(cook_polymesh_add_face(m, tri, uvs, 3U, 0U, NULL));
            } else if (k == rings - 1U) {
                const u32 tri[3] = {upper + i, lower, upper + next};
                const NphVec2 uvs[3] = {nph_vec2(u0, v0), nph_vec2((u0 + u1) * 0.5F, v1),
                                        nph_vec2(u1, v0)};
                NPH_TRY(cook_polymesh_add_face(m, tri, uvs, 3U, 0U, NULL));
            } else {
                const u32 quad[4] = {upper + i, lower + i, lower + next, upper + next};
                const NphVec2 uvs[4] = {nph_vec2(u0, v0), nph_vec2(u0, v1), nph_vec2(u1, v1),
                                        nph_vec2(u1, v0)};
                NPH_TRY(cook_polymesh_add_face(m, quad, uvs, 4U, 0U, NULL));
            }
        }
    }
    return NPH_RESULT_OK;
}
