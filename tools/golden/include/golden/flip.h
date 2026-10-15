// SPDX-License-Identifier: Apache-2.0
#pragma once

// LDR-FLIP, the image difference evaluator of the golden-image tests
// (CLAUDE.md §9 "compare with NVIDIA FLIP (Andersson et al., HPG 2020)";
// docs/golden.md): Andersson, Nilsson, Akenine-Möller, Oskarsson, Åström,
// and Fairchild, "FLIP: A Difference Evaluator for Alternating Images",
// Proc. ACM Comput. Graph. Interact. Tech. 3(2), 2020. Our own
// implementation of the paper's LDR pipeline; tools/golden/tests checks it
// against the published values and the authors' reference implementation
// agrees with it to 1e-4 per pixel on the images in docs/golden.md.
//
// Per pixel, FLIP estimates how noticeable the difference between a
// reference and a test image is when a viewer flips between them, from 0
// (none) to 1 (as different as possible):
//
//   colour   both images move to YyCxCz, are filtered with the contrast
//            sensitivity of each opponent channel at the viewing distance
//            (pixels per degree), clamped to the sRGB gamut, moved to
//            Hunt-adjusted L*a*b*, and compared with the HyAB distance,
//            which a power and a two-segment remap bring to [0, 1];
//   features edges and points of the achromatic channel are detected with
//            first and second derivatives of a Gaussian of the human
//            edge-detection width, and their difference raises the colour
//            error: error = colour ^ (1 - features).
//
// Images are 8-bit sRGB RGBA (alpha ignored). Thread-safety:
// NPH_THREADSAFE (no shared state); allocates its working planes from the
// allocator passed in.

#include <nepheline/core/alloc.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// The default viewing condition of the paper: a 0.7 m wide 3840-pixel
// display seen from 0.7 m, 67 pixels per degree.
#define GOLDEN_FLIP_DEFAULT_PPD 67.0F

typedef struct GoldenFlipStats {
    f64 mean; // the mean FLIP error, the paper's summary statistic
    f32 max;
    f32 p50; // percentiles of the per-pixel errors (nearest rank)
    f32 p95;
    f32 p99;
} GoldenFlipStats;

// Computes the FLIP error of `test` against `reference`, both `width` x
// `height` RGBA8 sRGB. `ppd` is the viewing condition (0: the default).
// `out_map` (may be NULL) receives width x height errors in [0, 1].
// NPH_RESULT_INVALID_ARGUMENT for an empty image or a non-positive ppd,
// NPH_RESULT_OUT_OF_MEMORY when the working planes cannot be allocated.
NPH_MUST_USE NphResult golden_flip(const u8 *reference, const u8 *test, u32 width, u32 height,
                                   f32 ppd, NphAllocator *alloc, f32 *out_map,
                                   GoldenFlipStats *out_stats);

// Colours an error map with the magma colour map the paper's figures use
// (black = no error, through purple and orange, to pale yellow = 1), as
// RGBA8.
void golden_flip_magma(const f32 *map, u32 width, u32 height, u8 *out_rgba);

#ifdef __cplusplus
}
#endif
