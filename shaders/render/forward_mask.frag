// SPDX-License-Identifier: Apache-2.0
// Forward shading of alpha-tested materials (glTF alphaMode MASK): a
// separate variant so the discard stays out of the opaque shader, where it
// would disable early depth testing (forward_frag.glsl).
#version 460
#define NPH_FORWARD_ALPHA_MODE 1
#include "forward_frag.glsl"
