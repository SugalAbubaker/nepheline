// SPDX-License-Identifier: Apache-2.0
// Render module and golden-toolkit benchmarks (docs/render.md, docs/golden.md):
// the CPU work around the reference renderer that hosted runners can time.
// The renderer's GPU passes are timed by its GPU profiler on a device
// (pbr_viewer --profile; the PR records them, CLAUDE.md §10.7).
//
//   primitive_sphere_64x32   build a 64 x 32 UV sphere's streams (2145 vertices: tangents,
//                            octahedral normals, quantisation)
//   primitive_cube           build the 24-vertex cube
//   flip_256                 LDR-FLIP of a 256 x 256 pair: one golden-image comparison
//   png_encode_256           encode a 256 x 256 RGB golden image
//   png_decode_256           decode it again (inflate + unfilter)
//   ref_dfg_256_samples      one DFG texel of the CPU reference (render/reference.h)
//   cluster_lod_torus_262k   the cluster levels of detail of a 262,144-triangle torus
//                            (render/cluster_lod.h): its meshlets made beforehand

#include <nepheline/math/scalar.h>
#include <nepheline/render/cluster_lod.h>
#include <nepheline/render/primitives.h>
#include <nepheline/render/reference.h>

#include <nbench/nbench.h>

#include <golden/flip.h>
#include <golden/png.h>
#include <string.h>

#define IMAGE 256U

static u8 g_ref[IMAGE * IMAGE * 4U];
static u8 g_test[IMAGE * IMAGE * 4U];
static bool g_ready;

// A smooth gradient with a pattern, and the same image a little brighter
// with a shifted feature: a realistic golden comparison.
static void prepare(void)
{
    if (g_ready) {
        return;
    }
    for (u32 y = 0; y < IMAGE; ++y) {
        for (u32 x = 0; x < IMAGE; ++x) {
            u8 *r = &g_ref[((u64)y * IMAGE + x) * 4U];
            u8 *t = &g_test[((u64)y * IMAGE + x) * 4U];
            const u32 checker = (((x / 16U) + (y / 16U)) & 1U) * 40U;
            r[0] = (u8)(x / 2U + checker);
            r[1] = (u8)(y / 2U + checker);
            r[2] = (u8)((x + y) / 4U);
            r[3] = 255U;
            const u32 shifted = ((((x + 1U) / 16U) + (y / 16U)) & 1U) * 40U;
            t[0] = (u8)(x / 2U + shifted + 3U);
            t[1] = (u8)(y / 2U + shifted + 3U);
            t[2] = (u8)((x + y) / 4U + 3U);
            t[3] = 255U;
        }
    }
    g_ready = true;
}

NBENCH(primitive_sphere_64x32)
{
    while (nbench_keep_running(state)) {
        NphRenderMeshData d;
        if (nph_render_primitive_sphere(nph_allocator_system(), 1.0F, 64U, 32U, &d) ==
            NPH_RESULT_OK) {
            nbench_do_not_optimize(d.attribs);
            nph_render_mesh_data_free(&d);
        }
    }
}

NBENCH(primitive_cube)
{
    while (nbench_keep_running(state)) {
        NphRenderMeshData d;
        if (nph_render_primitive_cube(nph_allocator_system(), 1.0F, &d) == NPH_RESULT_OK) {
            nbench_do_not_optimize(d.attribs);
            nph_render_mesh_data_free(&d);
        }
    }
}

NBENCH(flip_256)
{
    prepare();
    GoldenFlipStats s;
    while (nbench_keep_running(state)) {
        NPH_IGNORE_RESULT(golden_flip(g_ref, g_test, IMAGE, IMAGE, GOLDEN_FLIP_DEFAULT_PPD,
                                      nph_allocator_system(), NULL, &s));
        nbench_do_not_optimize(&s);
    }
}

NBENCH(png_encode_256)
{
    prepare();
    while (nbench_keep_running(state)) {
        u8 *file = NULL;
        u64 size = 0U;
        if (golden_png_encode(g_ref, IMAGE, IMAGE, 3U, nph_allocator_system(), &file, &size) ==
            NPH_RESULT_OK) {
            nbench_do_not_optimize(file);
            NPH_FREE_ARRAY(nph_allocator_system(), u8, file, size);
        }
    }
}

NBENCH(png_decode_256)
{
    prepare();
    u8 *file = NULL;
    u64 size = 0U;
    if (golden_png_encode(g_ref, IMAGE, IMAGE, 3U, nph_allocator_system(), &file, &size) !=
        NPH_RESULT_OK) {
        return;
    }
    while (nbench_keep_running(state)) {
        GoldenImage img;
        if (golden_png_decode(file, size, nph_allocator_system(), &img) == NPH_RESULT_OK) {
            nbench_do_not_optimize(img.rgba);
            golden_image_free(&img);
        }
    }
    NPH_FREE_ARRAY(nph_allocator_system(), u8, file, size);
}

NBENCH(ref_dfg_256_samples)
{
    f32 nov = 0.25F;
    while (nbench_keep_running(state)) {
        const NphVec2 v = nph_render_ref_dfg(nov, 0.5F, 256U);
        nbench_do_not_optimize(&v);
        nov = nov > 0.9F ? 0.25F : nov + 0.01F;
    }
}

// A torus of rings x sides quads (closed, each position one vertex), and
// its meshlets.
typedef struct LodBenchMesh {
    NphVec3 *positions;
    u32 *indices;
    u32 vertex_count;
    u32 index_count;
    NphRenderMeshlets meshlets;
    bool ready;
} LodBenchMesh;

static LodBenchMesh g_torus;

static void prepare_torus(void)
{
    if (g_torus.ready) {
        return;
    }
    NphAllocator *a = nph_allocator_system();
    const u32 rings = 512U;
    const u32 sides = 256U;
    g_torus.vertex_count = rings * sides;
    g_torus.index_count = rings * sides * 6U;
    g_torus.positions = NPH_ALLOC_ARRAY(a, NphVec3, g_torus.vertex_count);
    g_torus.indices = NPH_ALLOC_ARRAY(a, u32, g_torus.index_count);
    if (g_torus.positions == NULL || g_torus.indices == NULL) {
        return;
    }
    for (u32 i = 0; i < rings; ++i) {
        const f32 u = 2.0F * NPH_PI * (f32)i / (f32)rings;
        for (u32 j = 0; j < sides; ++j) {
            const f32 v = 2.0F * NPH_PI * (f32)j / (f32)sides;
            const f32 rr = 1.0F + 0.35F * nph_f32_cos(v);
            g_torus.positions[i * sides + j] =
                nph_vec3(rr * nph_f32_cos(u), 0.35F * nph_f32_sin(v), rr * nph_f32_sin(u));
        }
    }
    u32 k = 0U;
    for (u32 i = 0; i < rings; ++i) {
        for (u32 j = 0; j < sides; ++j) {
            const u32 v00 = i * sides + j;
            const u32 v10 = ((i + 1U) % rings) * sides + j;
            const u32 v01 = i * sides + (j + 1U) % sides;
            const u32 v11 = ((i + 1U) % rings) * sides + (j + 1U) % sides;
            g_torus.indices[k++] = v00;
            g_torus.indices[k++] = v01;
            g_torus.indices[k++] = v11;
            g_torus.indices[k++] = v00;
            g_torus.indices[k++] = v11;
            g_torus.indices[k++] = v10;
        }
    }
    nph_render_meshlets_init(&g_torus.meshlets, a);
    const NphRenderIndexRange range = {0U, g_torus.index_count};
    NphRenderMeshletInput mi;
    memset(&mi, 0, sizeof mi);
    mi.positions = g_torus.positions;
    mi.vertex_count = g_torus.vertex_count;
    mi.indices = g_torus.indices;
    mi.index_count = g_torus.index_count;
    mi.ranges = &range;
    mi.range_count = 1U;
    mi.cone_weight = 0.25F;
    mi.scratch = a;
    g_torus.ready = nph_render_meshlets_build(&mi, &g_torus.meshlets) == NPH_RESULT_OK;
}

NBENCH(cluster_lod_torus_262k)
{
    prepare_torus();
    if (!g_torus.ready) {
        return;
    }
    NphAllocator *a = nph_allocator_system();
    const NphRenderIndexRange range = {0U, g_torus.index_count};
    NphRenderClusterLodInput in;
    memset(&in, 0, sizeof in);
    in.positions = g_torus.positions;
    in.vertex_count = g_torus.vertex_count;
    in.indices = g_torus.indices;
    in.index_count = g_torus.index_count;
    in.ranges = &range;
    in.range_count = 1U;
    in.meshlets = &g_torus.meshlets;
    in.cone_weight = 0.25F;
    in.scratch = a;
    nbench_set_items_per_iteration(state, g_torus.index_count / 3U);
    while (nbench_keep_running(state)) {
        NphRenderClusterLods l;
        nph_render_cluster_lods_init(&l, a);
        if (nph_render_cluster_lods_build(&in, &l) == NPH_RESULT_OK) {
            nbench_do_not_optimize(l.groups.data);
        }
        nph_render_cluster_lods_free(&l);
    }
}
