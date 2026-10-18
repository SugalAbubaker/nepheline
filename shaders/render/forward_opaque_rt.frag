// SPDX-License-Identifier: Apache-2.0
// forward_opaque.frag with ray-traced shadows (forward_frag.glsl, rt_common.glsl).
#version 460
#extension GL_EXT_ray_query : require
#define NPH_RAY_QUERY
#define NPH_FORWARD_ALPHA_MODE 0
#include "forward_frag.glsl"
