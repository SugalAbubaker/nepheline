// SPDX-License-Identifier: Apache-2.0
#pragma once

// DDS sources (dds.c), for cook_image_info and cook_image_decode in
// image.c: the top level of a 2D texture.

#include <cook/image.h>

// The extent and channel count of a DDS file, from its headers alone.
// NPH_RESULT_PARSE_ERROR for malformed headers, NPH_RESULT_UNSUPPORTED for
// a well-formed file the cooker does not read (a cube map, an array, a
// volume, a format outside docs/cook.md's list, an extent past
// COOK_IMAGE_MAX_EXTENT).
NPH_MUST_USE NphResult cook_dds_info(const void *data, u64 size, u32 *out_width, u32 *out_height,
                                     u32 *out_channels);

// True when the file's headers parse and name BC6H (whose texels are
// HDR, as a Radiance file's).
bool cook_dds_is_hdr(const void *data, u64 size);

// Decodes the top level: RGBA8 for every format but BC6H, which gives
// RGBA32F. Errors as cook_dds_info's, and NPH_RESULT_PARSE_ERROR when the
// file is shorter than its top level.
NPH_MUST_USE NphResult cook_dds_decode(NphAllocator *alloc, const void *data, u64 size,
                                       CookImage *out);
