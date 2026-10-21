// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's raster fragment stage: the mesh path, the masked
// clusters at the front of their instance, whose depth is written after the
// alpha test (vis_raster_frag.glsl).
#version 460
#define VIS_MESH 1
#define VIS_MASKED 1
#define VIS_LATE_Z 1
#include "vis_raster_frag.glsl"
