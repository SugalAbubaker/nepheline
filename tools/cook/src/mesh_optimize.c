// SPDX-License-Identifier: Apache-2.0
// Triangle and vertex reordering (mesh.h): vertex cache optimisation
// after Forsyth, "Linear-Speed Vertex Cache Optimisation" (2006), overdraw
// reduction by sorting runs of triangles by their outward facing (the
// cluster pass of Sander, Nehab & Barczak, SIGGRAPH 2007, as simplified
// in meshoptimizer), and vertex fetch optimisation by first use.
#include <nepheline/core/assert.h>
#include <nepheline/math/geometry.h>

#include <cook/mesh.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The cache model used to score and to measure
// ---------------------------------------------------------------------------

f32 cook_mesh_acmr(const u32 *indices, u32 index_count, u32 cache_size)
{
    if (index_count < 3U || cache_size == 0U) {
        return 0.0F;
    }
    // A FIFO of vertex indices; a miss shifts everything along.
    u32 cache[64];
    cache_size = nph_min_u32(cache_size, NPH_ARRAY_COUNT(cache));
    u32 filled = 0U;
    u32 misses = 0U;
    for (u32 i = 0; i < index_count; ++i) {
        const u32 v = indices[i];
        bool hit = false;
        for (u32 c = 0; c < filled; ++c) {
            if (cache[c] == v) {
                hit = true;
                break;
            }
        }
        if (hit) {
            continue;
        }
        ++misses;
        const u32 keep = nph_min_u32(filled, cache_size - 1U);
        memmove(cache + 1, cache, keep * sizeof(u32));
        cache[0] = v;
        filled = keep + 1U;
    }
    const u32 triangles = index_count / 3U;
    return (f32)misses / (f32)triangles;
}

// ---------------------------------------------------------------------------
// Forsyth's algorithm
// ---------------------------------------------------------------------------

#define FORSYTH_CACHE          32U
#define FORSYTH_CACHE_DECAY    1.5F
#define FORSYTH_LAST_TRI_SCORE 0.75F
#define FORSYTH_VALENCE_SCALE  2.0F
#define FORSYTH_VALENCE_POWER  0.5F
#define FORSYTH_MAX_VALENCE    15U

typedef struct ForsythVertex {
    u32 tri_offset; // into tri_lists
    u32 tri_count;  // triangles not yet emitted
    i32 cache_pos;  // -1 when not in the cache
    f32 score;
} ForsythVertex;

// The two terms of Forsyth's vertex score, tabulated once per mesh.
typedef struct ForsythTables {
    f32 cache[FORSYTH_CACHE];              // by position in the cache
    f32 valence[FORSYTH_MAX_VALENCE + 1U]; // by triangles still to come (entry 0 unused)
} ForsythTables;

static void forsyth_tables(ForsythTables *t)
{
    for (u32 pos = 0; pos < FORSYTH_CACHE; ++pos) {
        if (pos < 3U) {
            t->cache[pos] =
                FORSYTH_LAST_TRI_SCORE; // the last triangle's vertices: no bonus for churn
        } else {
            const f32 scaled = (f32)(pos - 3U) / (f32)(FORSYTH_CACHE - 3U);
            t->cache[pos] = nph_f32_pow(1.0F - scaled, FORSYTH_CACHE_DECAY);
        }
    }
    t->valence[0] = 0.0F;
    for (u32 n = 1; n <= FORSYTH_MAX_VALENCE; ++n) {
        // Favour vertices with few triangles left so they leave the cache soon.
        t->valence[n] = FORSYTH_VALENCE_SCALE * nph_f32_pow((f32)n, -FORSYTH_VALENCE_POWER);
    }
}

static f32 forsyth_score(const ForsythTables *t, const ForsythVertex *v)
{
    if (v->tri_count == 0U) {
        return -1.0F; // nothing left to gain from this vertex
    }
    const f32 cached = v->cache_pos >= 0 ? t->cache[v->cache_pos] : 0.0F;
    return cached + t->valence[nph_min_u32(v->tri_count, FORSYTH_MAX_VALENCE)];
}

// Reorders the triangles of [indices, indices + count) in place.
// `verts` is scratch for every vertex of the mesh; only the vertices this
// range references are initialised and used, so one allocation serves
// every submesh.
static NphResult forsyth_optimize(NphAllocator *alloc, const ForsythTables *tables,
                                  ForsythVertex *verts, u32 *indices, u32 index_count)
{
    const u32 tri_count = index_count / 3U;
    if (tri_count == 0U) {
        return NPH_RESULT_OK;
    }
    u32 *tri_lists = NPH_ALLOC_ARRAY(alloc, u32, index_count);
    f32 *tri_score = NPH_ALLOC_ARRAY(alloc, f32, tri_count);
    u8 *tri_done = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, tri_count);
    u32 *out = NPH_ALLOC_ARRAY(alloc, u32, index_count);
    NphResult r = NPH_RESULT_OK;
    if (tri_lists == NULL || tri_score == NULL || tri_done == NULL || out == NULL) {
        r = NPH_RESULT_OUT_OF_MEMORY;
        goto done;
    }
    // Per-vertex triangle lists (a counting sort over the referenced
    // vertices). After the reset, cache_pos 0 marks a vertex whose run of
    // the lists is not claimed yet; claiming sets it to -1 (not cached).
    for (u32 i = 0; i < index_count; ++i) {
        memset(&verts[indices[i]], 0, sizeof *verts);
    }
    for (u32 i = 0; i < index_count; ++i) {
        verts[indices[i]].tri_count += 1U;
    }
    u32 offset = 0U;
    for (u32 i = 0; i < index_count; ++i) {
        ForsythVertex *v = &verts[indices[i]];
        if (v->cache_pos == 0) {
            v->tri_offset = offset;
            offset += v->tri_count;
            v->tri_count = 0U;
            v->cache_pos = -1;
        }
    }
    for (u32 t = 0; t < tri_count; ++t) {
        for (u32 k = 0; k < 3U; ++k) {
            ForsythVertex *v = &verts[indices[t * 3U + k]];
            tri_lists[v->tri_offset + v->tri_count] = t;
            v->tri_count += 1U;
        }
    }
    for (u32 i = 0; i < index_count; ++i) {
        ForsythVertex *v = &verts[indices[i]];
        v->score = forsyth_score(tables, v);
    }
    for (u32 t = 0; t < tri_count; ++t) {
        tri_score[t] = verts[indices[(u64)t * 3U]].score + verts[indices[(u64)t * 3U + 1U]].score +
                       verts[indices[(u64)t * 3U + 2U]].score;
    }

    u32 cache[FORSYTH_CACHE + 3U];
    u32 cache_len = 0U;
    u32 emitted = 0U;
    u32 cursor = 0U; // for the fallback scan over untouched triangles
    // The best candidate among the triangles of cached vertices.
    i32 best = -1;
    f32 best_score = -1.0F;
    for (u32 t = 0; t < tri_count; ++t) {
        if (tri_score[t] > best_score) {
            best_score = tri_score[t];
            best = (i32)t;
        }
    }
    while (emitted < tri_count) {
        if (best < 0) {
            // No candidate near the cache: take the next untouched triangle.
            while (tri_done[cursor]) {
                ++cursor;
            }
            best = (i32)cursor;
        }
        const u32 t = (u32)best;
        tri_done[t] = 1U;
        for (u32 k = 0; k < 3U; ++k) {
            out[emitted * 3U + k] = indices[t * 3U + k];
        }
        ++emitted;

        // Move the triangle's vertices to the front of the cache, remove
        // the triangle from their lists.
        for (u32 k = 0; k < 3U; ++k) {
            const u32 vi = indices[t * 3U + k];
            ForsythVertex *v = &verts[vi];
            // Remove t from v's list by swapping with the last.
            for (u32 j = 0; j < v->tri_count; ++j) {
                if (tri_lists[v->tri_offset + j] == t) {
                    tri_lists[v->tri_offset + j] = tri_lists[v->tri_offset + v->tri_count - 1U];
                    break;
                }
            }
            v->tri_count -= 1U;
            // Pull vi to the front of the cache.
            u32 at = cache_len;
            for (u32 c = 0; c < cache_len; ++c) {
                if (cache[c] == vi) {
                    at = c;
                    break;
                }
            }
            if (at == cache_len) {
                cache_len = nph_min_u32(cache_len + 1U, NPH_ARRAY_COUNT(cache));
                at = cache_len - 1U;
            }
            memmove(cache + 1, cache, at * sizeof(u32));
            cache[0] = vi;
        }
        // Rescore the cache's vertices and their triangles; vertices pushed
        // past the cache size leave it.
        for (u32 c = 0; c < cache_len; ++c) {
            ForsythVertex *v = &verts[cache[c]];
            v->cache_pos = c < FORSYTH_CACHE ? (i32)c : -1;
            v->score = forsyth_score(tables, v);
        }
        best = -1;
        best_score = -1.0F;
        for (u32 c = 0; c < cache_len; ++c) {
            const ForsythVertex *v = &verts[cache[c]];
            for (u32 j = 0; j < v->tri_count; ++j) {
                const u32 tj = tri_lists[v->tri_offset + j];
                tri_score[tj] = verts[indices[(u64)tj * 3U]].score +
                                verts[indices[(u64)tj * 3U + 1U]].score +
                                verts[indices[(u64)tj * 3U + 2U]].score;
                if (tri_score[tj] > best_score) {
                    best_score = tri_score[tj];
                    best = (i32)tj;
                }
            }
        }
        cache_len = nph_min_u32(cache_len, FORSYTH_CACHE);
    }
    memcpy(indices, out, (size_t)index_count * sizeof(u32));

done:
    if (tri_lists != NULL) {
        NPH_FREE_ARRAY(alloc, u32, tri_lists, index_count);
    }
    if (tri_score != NULL) {
        NPH_FREE_ARRAY(alloc, f32, tri_score, tri_count);
    }
    if (tri_done != NULL) {
        NPH_FREE_ARRAY(alloc, u8, tri_done, tri_count);
    }
    if (out != NULL) {
        NPH_FREE_ARRAY(alloc, u32, out, index_count);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Overdraw: runs of cache-friendly triangles sorted by how much they face
// away from the mesh's centre, so the parts facing any outside viewer come
// first and occlude what is behind them.
// ---------------------------------------------------------------------------

#define OVERDRAW_CLUSTER_TRIANGLES 64U

typedef struct Cluster {
    u32 first; // triangle
    u32 count;
    f32 key;
} Cluster;

static NphResult overdraw_sort(NphAllocator *alloc, const CookMesh *mesh, u32 *indices,
                               u32 index_count)
{
    const u32 tri_count = index_count / 3U;
    const u32 cluster_count =
        (tri_count + OVERDRAW_CLUSTER_TRIANGLES - 1U) / OVERDRAW_CLUSTER_TRIANGLES;
    if (cluster_count < 2U) {
        return NPH_RESULT_OK;
    }
    NphVec3 center = nph_vec3_zero();
    NPH_ARRAY_FOREACH (CookVertex, v, &mesh->vertices) {
        center = nph_vec3_add(center, v->position);
    }
    center = nph_vec3_scale(center, 1.0F / (f32)nph_max_u32(mesh->vertices.len, 1U));

    Cluster *clusters = NPH_ALLOC_ARRAY(alloc, Cluster, cluster_count);
    u32 *out = NPH_ALLOC_ARRAY(alloc, u32, index_count);
    if (clusters == NULL || out == NULL) {
        if (clusters != NULL) {
            NPH_FREE_ARRAY(alloc, Cluster, clusters, cluster_count);
        }
        if (out != NULL) {
            NPH_FREE_ARRAY(alloc, u32, out, index_count);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 c = 0; c < cluster_count; ++c) {
        Cluster *cl = &clusters[c];
        cl->first = c * OVERDRAW_CLUSTER_TRIANGLES;
        cl->count = nph_min_u32(OVERDRAW_CLUSTER_TRIANGLES, tri_count - cl->first);
        NphVec3 normal = nph_vec3_zero();
        NphVec3 centroid = nph_vec3_zero();
        for (u32 t = cl->first; t < cl->first + cl->count; ++t) {
            const NphVec3 a = mesh->vertices.data[indices[(u64)t * 3U]].position;
            const NphVec3 b = mesh->vertices.data[indices[(u64)t * 3U + 1U]].position;
            const NphVec3 p = mesh->vertices.data[indices[(u64)t * 3U + 2U]].position;
            normal = nph_vec3_add(normal, nph_vec3_cross(nph_vec3_sub(b, a), nph_vec3_sub(p, a)));
            centroid = nph_vec3_add(centroid, nph_vec3_add(nph_vec3_add(a, b), p));
        }
        centroid = nph_vec3_scale(centroid, 1.0F / (3.0F * (f32)cl->count));
        // Outward-facing clusters (normal along the direction from the
        // centre) are drawn first: a negative key sorts them to the front.
        cl->key = -nph_vec3_dot(nph_vec3_normalize_or_zero(normal),
                                nph_vec3_normalize_or_zero(nph_vec3_sub(centroid, center)));
    }
    // Stable insertion sort by key (clusters are few; ties keep cache order).
    for (u32 i = 1; i < cluster_count; ++i) {
        const Cluster key = clusters[i];
        u32 j = i;
        while (j > 0U && clusters[j - 1U].key > key.key) {
            clusters[j] = clusters[j - 1U];
            --j;
        }
        clusters[j] = key;
    }
    u32 write = 0U;
    for (u32 c = 0; c < cluster_count; ++c) {
        memcpy(out + write, indices + (u64)clusters[c].first * 3U,
               (size_t)clusters[c].count * 3U * sizeof(u32));
        write += clusters[c].count * 3U;
    }
    memcpy(indices, out, (size_t)index_count * sizeof(u32));
    NPH_FREE_ARRAY(alloc, Cluster, clusters, cluster_count);
    NPH_FREE_ARRAY(alloc, u32, out, index_count);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Vertex fetch: renumber vertices in the order the index stream first
// touches them, so the vertex fetch walks memory forward.
// ---------------------------------------------------------------------------

static NphResult fetch_optimize(CookMesh *mesh)
{
    const u32 n = mesh->vertices.len;
    if (n == 0U) {
        return NPH_RESULT_OK;
    }
    u32 *remap = NPH_ALLOC_ARRAY(mesh->alloc, u32, n);
    CookVertex *sorted = NPH_ALLOC_ARRAY(mesh->alloc, CookVertex, n);
    if (remap == NULL || sorted == NULL) {
        if (remap != NULL) {
            NPH_FREE_ARRAY(mesh->alloc, u32, remap, n);
        }
        if (sorted != NULL) {
            NPH_FREE_ARRAY(mesh->alloc, CookVertex, sorted, n);
        }
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < n; ++i) {
        remap[i] = NPH_U32_MAX;
    }
    u32 next = 0U;
    for (u32 i = 0; i < mesh->indices.len; ++i) {
        const u32 v = mesh->indices.data[i];
        if (remap[v] == NPH_U32_MAX) {
            remap[v] = next++;
        }
    }
    // Vertices no triangle references keep their relative order at the end.
    for (u32 v = 0; v < n; ++v) {
        if (remap[v] == NPH_U32_MAX) {
            remap[v] = next++;
        }
    }
    for (u32 v = 0; v < n; ++v) {
        sorted[remap[v]] = mesh->vertices.data[v];
    }
    memcpy(mesh->vertices.data, sorted, (size_t)n * sizeof(CookVertex));
    for (u32 i = 0; i < mesh->indices.len; ++i) {
        mesh->indices.data[i] = remap[mesh->indices.data[i]];
    }
    NPH_FREE_ARRAY(mesh->alloc, u32, remap, n);
    NPH_FREE_ARRAY(mesh->alloc, CookVertex, sorted, n);
    return NPH_RESULT_OK;
}

NphResult cook_mesh_optimize(CookMesh *mesh)
{
    NPH_ASSERT(mesh != NULL);
    NPH_TRY(cook_mesh_validate(mesh));
    if (mesh->vertices.len == 0U) {
        return NPH_RESULT_OK;
    }
    ForsythTables tables;
    forsyth_tables(&tables);
    ForsythVertex *verts = NPH_ALLOC_ARRAY(mesh->alloc, ForsythVertex, mesh->vertices.len);
    if (verts == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphResult r = NPH_RESULT_OK;
    for (u32 i = 0; i < mesh->submeshes.len && r == NPH_RESULT_OK; ++i) {
        const CookSubmesh *s = &mesh->submeshes.data[i];
        u32 *indices = mesh->indices.data + s->index_offset;
        r = forsyth_optimize(mesh->alloc, &tables, verts, indices, s->index_count);
        if (r == NPH_RESULT_OK) {
            r = overdraw_sort(mesh->alloc, mesh, indices, s->index_count);
        }
    }
    NPH_FREE_ARRAY(mesh->alloc, ForsythVertex, verts, mesh->vertices.len);
    return r == NPH_RESULT_OK ? fetch_optimize(mesh) : r;
}
