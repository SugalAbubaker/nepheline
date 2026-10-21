// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's raster fragment stage: the mesh path, opaque materials,
// through the colour attachment (phase 0; vis_raster_frag.glsl).
#version 460
#define VIS_MESH 1
#define VIS_MASKED 0
#define VIS_ROP 1
#include "vis_raster_frag.glsl"
