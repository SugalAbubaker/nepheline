// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page raster, alpha-tested casters' fragment stage
// (vsm_raster_mesh.glsl, vsm_raster_vert.glsl): the material's alpha test,
// the base colour's alpha (factor, vertex colour, texture) against the
// cutoff as the visibility buffer's masked raster runs it
// (vis_raster_frag.glsl), so that a texel of the map holds what a pixel of
// the view would show there: a leaf, not its card. No outputs: a fragment
// that passes leaves its depth, written after the test (a discard keeps the
// early test off).
#version 460
#include "vis_common.glsl"

layout(location = 0) in vec3 in_uv_alpha;
layout(location = 1) flat in uvec2 in_alpha_texture;
layout(location = 2) flat in vec2 in_alpha_test;

void main()
{
    // Sampled before the discard so the quad's derivatives stay defined.
    float alpha = in_alpha_test.x * in_uv_alpha.z;
    if (in_alpha_texture.x != NPH_RENDER_NO_TEXTURE) {
        alpha *= nph_sample_2d(in_alpha_texture.x, in_alpha_texture.y, in_uv_alpha.xy).a;
    }
    if (alpha < in_alpha_test.y) {
        discard;
    }
}
