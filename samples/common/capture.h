// SPDX-License-Identifier: Apache-2.0
#pragma once

// Writing 8-bit RGBA images to disk for the samples' --capture option:
// a 24-bit BMP, which every image viewer opens and which needs no codec.

#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

// Writes `width` x `height` texels of 8-bit RGBA (or BGRA when `bgra`),
// rows top-down, as a 24-bit bottom-up BMP. NPH_RESULT_OVERFLOW for an
// image over 4 GiB; NPH_RESULT_IO_ERROR from the file system.
NPH_MUST_USE NphResult capture_write_bmp(const char *path, const u8 *texels, u32 width, u32 height,
                                         bool bgra);
