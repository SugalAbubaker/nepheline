// SPDX-License-Identifier: Apache-2.0
#pragma once

// CPU reference implementations of the renderer's shader math
// (shaders/render/render_common.glsl, tonemap.frag, brdf_lut.comp,
// ibl_sh.comp, and the culling of vis_common.glsl). The render tests run the GPU kernels and
// compare their output with these, so a shader that drifts from the formulas it cites fails a test
// rather than a golden image. They are written for clarity and double-checked arithmetic, not
// speed.
//
// Thread-safety: pure functions, NPH_THREADSAFE.

#include <nepheline/render/renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

// The GGX distribution (`a` = GGX alpha = perceptual roughness squared).
f32 nph_render_ref_d_ggx(f32 noh, f32 a);
// Smith's height-correlated visibility, G2 / (4 NoV NoL).
f32 nph_render_ref_v_smith(f32 nov, f32 nol, f32 a);
// Schlick's Fresnel for one channel.
f32 nph_render_ref_f_schlick(f32 voh, f32 f0);

// The Hammersley point i of n.
NphVec2 nph_render_ref_hammersley(u32 i, u32 n);
// A GGX-distributed half vector around +Z.
NphVec3 nph_render_ref_importance_sample_ggx(NphVec2 xi, f32 a);

// The split-sum DFG terms (scale, bias) for NoV and perceptual roughness,
// integrated with `samples` Hammersley samples as brdf_lut.comp does.
NphVec2 nph_render_ref_dfg(f32 nov, f32 roughness, u32 samples);

// The tonemap curves of tonemap.frag (linear in, linear out).
NphVec3 nph_render_ref_tonemap(NphVec3 color, NphRenderTonemapper tonemapper);

// The sRGB transfer functions (IEC 61966-2-1).
f32 nph_render_ref_linear_to_srgb(f32 c);
f32 nph_render_ref_srgb_to_linear(f32 c);

// The direction through point (s, t) in [0, 1]^2 of cube face `face`
// (+X -X +Y -Y +Z -Z), normalised.
NphVec3 nph_render_ref_cube_direction(u32 face, f32 s, f32 t);
// The solid angle of the cube-face texel centred on (u, v) in [-1, 1]^2
// with half-size h.
f32 nph_render_ref_cube_texel_solid_angle(f32 u, f32 v, f32 h);

// The procedural sky's radiance in direction `d` (unit).
NphVec3 nph_render_ref_sky(const NphRenderSkyDesc *sky, NphVec3 d);

// The real SH basis to band 2 in direction `d`.
void nph_render_ref_sh9_basis(NphVec3 d, f32 out[9]);
// The cosine-convolved irradiance coefficients of the procedural sky,
// projected from a cube of `size` texels per face with the same
// quadrature as ibl_sh.comp, in double precision.
void nph_render_ref_sh9_sky(const NphRenderSkyDesc *sky, u32 size, NphVec3 out[9]);
// Irradiance in direction `n` from cosine-convolved coefficients.
NphVec3 nph_render_ref_sh9_irradiance(const NphVec3 sh[9], NphVec3 n);

// ---------------------------------------------------------------------------
// The production path's culling (shaders/render/vis_common.glsl), in double
// precision. Where the shader decides by a sign or a rounding, the twin also
// gives the margin to that boundary, so a comparison can skip the cases
// single precision cannot resolve.
// ---------------------------------------------------------------------------

// vis_sphere_in_frustum: the least of dot(n, c) + d + r over the five
// inward planes (xyz normal, w distance; the far plane is at infinity). The
// sphere is kept when it is >= 0.
f64 nph_render_ref_vis_frustum_margin(const NphVec4 planes[5], NphVec3 c, f32 r);

// vis_cone_culled with the camera at the origin: dot(apex, axis) minus
// cutoff * |apex|. Every triangle of the meshlet faces away (culled) when it
// is >= 0 and the apex is not at the camera.
f64 nph_render_ref_vis_cone_margin(NphVec3 apex, NphVec3 axis, f32 cutoff);

// vis_project_sphere (Mara & McGuire, JCGT 2013): the screen rectangle (UV,
// y down) and nearest reverse-Z depth of a view-space sphere (the camera
// looks down -Z). `p00` and `p11` are the projection's scales (p11 is
// negative: the Y flip), `znear` its near plane. False, with zeros, when the
// sphere reaches the near plane.
bool nph_render_ref_vis_project_sphere(NphVec3 c, f32 r, f32 p00, f32 p11, f32 znear, NphVec4 *rect,
                                       f32 *depth);

// vis_front_facing's determinant of the homogeneous (x, y, w) clip-space
// vertices: the triangle is front facing when it is negative, unless the
// transform mirrors.
f64 nph_render_ref_vis_facing_det(NphVec4 c0, NphVec4 c1, NphVec4 c2);

// vis_cull_triangle with back faces culled: true for a back face or a
// triangle whose screen bounding box (grown by 1/256 pixel) holds no pixel
// centre; false when a vertex is behind the camera. How near the decision
// came to flipping: `facing_margin` receives the facing determinant
// relative to its terms (infinite when a vertex is behind the camera, a
// test single precision makes exactly), `box_margin` the distance in
// pixels from a box edge to the pixel-centre boundary (infinite when the
// box was not tested).
bool nph_render_ref_vis_cull_triangle(NphVec4 c0, NphVec4 c1, NphVec4 c2, bool mirrored,
                                      NphVec2 viewport, f64 *facing_margin, f64 *box_margin);

// A hierarchical depth pyramid in CPU memory for vis_hzb_occluded: mip 0 is
// width x height, each later mip the one above halved and rounded down (as
// Vulkan sizes mips, at least 1), each row major.
typedef struct NphRenderRefHzb {
    const f32 *const *mips; // mip_count pointers
    u32 width;
    u32 height;
    u32 mip_count;
    NphVec2 viewport; // the target the pyramid was built from (twice mip 0)
} NphRenderRefHzb;

// vis_hzb_occluded: whether a rectangle (UV) at nearest depth `depth` lies
// behind everything the pyramid records there. `margin` receives the
// distance to the nearest boundary of the decision: the mip choice, the
// texel rounding (in texels), or the depth comparison.
bool nph_render_ref_vis_hzb_occluded(const NphRenderRefHzb *hzb, NphVec4 rect, f32 depth,
                                     f64 *margin);

#ifdef __cplusplus
}
#endif
