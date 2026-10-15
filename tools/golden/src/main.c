// SPDX-License-Identifier: Apache-2.0
// nph_flip: compares two PNG images with LDR-FLIP (golden/flip.h) and
// prints the mean error and percentiles; optionally writes the error map
// coloured with magma, or the raw per-pixel errors.
//
//     nph_flip REFERENCE.png TEST.png [--ppd=N] [--map=OUT.png] [--raw=OUT.f32]
//
// --raw writes width x height little-endian 32-bit floats (the tests that
// check this implementation against the authors' read them).
//
// Exit status: 0 on success, 1 when an image cannot be read or the sizes
// differ, 2 for a usage error.

#include <nepheline/platform/file.h>

#include <golden/flip.h>
#include <golden/png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
            "usage: nph_flip REFERENCE.png TEST.png [--ppd=N] [--map=OUT.png] [--raw=OUT.f32]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *paths[2] = {NULL, NULL};
    const char *map_path = NULL;
    const char *raw_path = NULL;
    f32 ppd = 0.0F;
    u32 positional = 0U;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--ppd=", 6U) == 0) {
            ppd = strtof(argv[i] + 6, NULL);
        } else if (strncmp(argv[i], "--map=", 6U) == 0) {
            map_path = argv[i] + 6;
        } else if (strncmp(argv[i], "--raw=", 6U) == 0) {
            raw_path = argv[i] + 6;
        } else if (argv[i][0] != '-' && positional < 2U) {
            paths[positional++] = argv[i];
        } else {
            return usage();
        }
    }
    if (positional != 2U || !(ppd >= 0.0F)) {
        return usage();
    }
    NphAllocator *alloc = nph_allocator_system();
    GoldenImage img[2];
    memset(img, 0, sizeof img);
    for (u32 i = 0; i < 2U; ++i) {
        const NphResult r = golden_png_read(paths[i], alloc, &img[i]);
        if (r != NPH_RESULT_OK) {
            fprintf(stderr, "nph_flip: %s: %s\n", paths[i], nph_result_string(r));
            golden_image_free(&img[0]);
            return 1;
        }
    }
    int status = 0;
    f32 *map = NULL;
    const u64 n = (u64)img[0].width * img[0].height;
    if (img[0].width != img[1].width || img[0].height != img[1].height) {
        fprintf(stderr, "nph_flip: the images differ in size (%ux%u against %ux%u)\n", img[0].width,
                img[0].height, img[1].width, img[1].height);
        status = 1;
    } else {
        map = NPH_ALLOC_ARRAY(alloc, f32, n);
        GoldenFlipStats s;
        memset(&s, 0, sizeof s);
        NphResult r = map != NULL ? golden_flip(img[0].rgba, img[1].rgba, img[0].width,
                                                img[0].height, ppd, alloc, map, &s)
                                  : NPH_RESULT_OUT_OF_MEMORY;
        if (r == NPH_RESULT_OK) {
            printf("mean %.6f  max %.6f  p50 %.6f  p95 %.6f  p99 %.6f  (%ux%u)\n", s.mean,
                   (f64)s.max, (f64)s.p50, (f64)s.p95, (f64)s.p99, img[0].width, img[0].height);
        }
        if (r == NPH_RESULT_OK && map_path != NULL) {
            u8 *rgba = NPH_ALLOC_ARRAY(alloc, u8, n * 4U);
            r = rgba != NULL ? NPH_RESULT_OK : NPH_RESULT_OUT_OF_MEMORY;
            if (r == NPH_RESULT_OK) {
                golden_flip_magma(map, img[0].width, img[0].height, rgba);
                r = golden_png_write(map_path, rgba, img[0].width, img[0].height, 3U);
                NPH_FREE_ARRAY(alloc, u8, rgba, n * 4U);
            }
        }
        if (r == NPH_RESULT_OK && raw_path != NULL) {
            r = nph_file_write_all(raw_path, map, n * sizeof(f32));
        }
        if (r != NPH_RESULT_OK) {
            fprintf(stderr, "nph_flip: %s\n", nph_result_string(r));
            status = 1;
        }
    }
    if (map != NULL) {
        NPH_FREE_ARRAY(alloc, f32, map, n);
    }
    golden_image_free(&img[0]);
    golden_image_free(&img[1]);
    return status;
}
