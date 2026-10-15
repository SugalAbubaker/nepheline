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
