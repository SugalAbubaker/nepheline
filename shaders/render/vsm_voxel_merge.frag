// SPDX-License-Identifier: Apache-2.0
// The voxel casters' depths into the virtual shadow map's pool
// (vsm_voxel_splat.comp): over each page drawn this frame (vsm_clear.vert's
// quads), a texel that a voxel cast on takes its depth, which the depth test
// keeps where it is nearer than the triangles'; every texel of the page is
// reset to none for the next frame that draws it.
//
// arg0: the voxel casters' depths' storage index.
#version 460
#include "vis_common.glsl"

// Storage images with a format (binding 1), as vsm_voxel_splat.comp has them.
layout(set = 0, binding = 1, r32ui) uniform uimage2D vsm_voxel_depth_images[];

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const uint depth = imageLoad(vsm_voxel_depth_images[nonuniformEXT(pc.arg0)], p).x;
    imageStore(vsm_voxel_depth_images[nonuniformEXT(pc.arg0)], p, uvec4(0xFFFFFFFFu));
    if (depth == 0xFFFFFFFFu) {
        discard;
    }
    gl_FragDepth = uintBitsToFloat(depth);
}
