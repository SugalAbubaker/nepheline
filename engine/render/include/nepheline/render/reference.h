// SPDX-License-Identifier: Apache-2.0
#pragma once

// CPU reference implementations of the renderer's shader math
// (shaders/render/render_common.glsl, tonemap.frag, brdf_lut.comp,
// ibl_sh.comp). The render tests run the GPU kernels and compare their
// output with these, so a shader that drifts from the formulas it cites
// fails a test rather than a golden image. They are written for clarity
// and double-checked arithmetic, not speed.
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

#ifdef __cplusplus
}
#endif
