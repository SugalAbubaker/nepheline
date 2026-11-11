// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page raster, indirect-draw path for devices
// without mesh shaders, the casters whose material is not alpha tested
// (vsm_raster_vert.glsl).
#version 460
#define VSM_MASKED 0
#include "vsm_raster_vert.glsl"
