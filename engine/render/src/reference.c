// SPDX-License-Identifier: Apache-2.0
// CPU twins of the renderer's shader math (render/reference.h). Each
// function mirrors a GLSL function in shaders/render/ line for line, in
// double precision where the shader's single precision is not the point.
#include <nepheline/math/scalar.h>
#include <nepheline/render/reference.h>

#include <math.h>
#include <nph_render.h>

#define REF_PI 3.14159265358979323846

f32 nph_render_ref_d_ggx(f32 noh, f32 a)
{
    const f64 a2 = (f64)a * (f64)a;
    const f64 d = ((f64)noh * a2 - (f64)noh) * (f64)noh + 1.0;
    return (f32)(a2 / (REF_PI * d * d));
}

f32 nph_render_ref_v_smith(f32 nov, f32 nol, f32 a)
{
    const f64 a2 = (f64)a * (f64)a;
    const f64 v = (f64)nov;
    const f64 l = (f64)nol;
    const f64 gv = l * sqrt(v * v * (1.0 - a2) + a2);
    const f64 gl = v * sqrt(l * l * (1.0 - a2) + a2);
    const f64 s = gv + gl;
    return (f32)(0.5 / (s > 1e-8 ? s : 1e-8));
}

f32 nph_render_ref_f_schlick(f32 voh, f32 f0)
{
    const f64 f = pow(1.0 - (f64)voh, 5.0);
    return (f32)((f64)f0 + (1.0 - (f64)f0) * f);
}

static u32 reverse_bits(u32 v)
{
    v = ((v >> 1U) & 0x55555555U) | ((v & 0x55555555U) << 1U);
    v = ((v >> 2U) & 0x33333333U) | ((v & 0x33333333U) << 2U);
    v = ((v >> 4U) & 0x0F0F0F0FU) | ((v & 0x0F0F0F0FU) << 4U);
    v = ((v >> 8U) & 0x00FF00FFU) | ((v & 0x00FF00FFU) << 8U);
    return (v >> 16U) | (v << 16U);
}

NphVec2 nph_render_ref_hammersley(u32 i, u32 n)
{
    return nph_vec2((f32)i / (f32)n, (f32)((f64)reverse_bits(i) * 2.3283064365386963e-10));
}

NphVec3 nph_render_ref_importance_sample_ggx(NphVec2 xi, f32 a)
{
    const f64 phi = 2.0 * REF_PI * (f64)xi.x;
    const f64 a2 = (f64)a * (f64)a;
    const f64 c = sqrt((1.0 - (f64)xi.y) / (1.0 + (a2 - 1.0) * (f64)xi.y));
    const f64 cos_theta = c < 1.0 ? c : 1.0;
    const f64 s2 = 1.0 - cos_theta * cos_theta;
    const f64 sin_theta = sqrt(s2 > 0.0 ? s2 : 0.0);
    return nph_vec3((f32)(sin_theta * cos(phi)), (f32)(sin_theta * sin(phi)), (f32)cos_theta);
}

NphVec2 nph_render_ref_dfg(f32 nov, f32 roughness, u32 samples)
{
    const f64 a = (f64)roughness * (f64)roughness;
    const f64 vx = sqrt(1.0 - (f64)nov * (f64)nov);
    const f64 vz = (f64)nov;
    f64 scale = 0.0;
    f64 bias = 0.0;
    for (u32 i = 0; i < samples; ++i) {
        const NphVec3 h =
            nph_render_ref_importance_sample_ggx(nph_render_ref_hammersley(i, samples), (f32)a);
        const f64 voh = vx * (f64)h.x + vz * (f64)h.z;
        const f64 lz = 2.0 * voh * (f64)h.z - vz;
        if (lz > 0.0 && voh > 0.0) {
            const f64 g =
                (f64)nph_render_ref_v_smith(nov, (f32)lz, (f32)a) * 4.0 * lz * voh / (f64)h.z;
            const f64 fc = pow(1.0 - voh, 5.0);
            scale += (1.0 - fc) * g;
            bias += fc * g;
        }
    }
    return nph_vec2((f32)(scale / samples), (f32)(bias / samples));
}

static f64 clamp01(f64 v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static void mul_rows(const f64 m[3][3], const f64 in[3], f64 out[3])
{
    for (u32 r = 0; r < 3U; ++r) {
        out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2];
    }
}

NphVec3 nph_render_ref_tonemap(NphVec3 color, NphRenderTonemapper tonemapper)
{
    const f64 c[3] = {(f64)color.x, (f64)color.y, (f64)color.z};
    f64 out[3];
    if (tonemapper == NPH_RENDER_TONEMAPPER_ACES) {
        static const f64 in_m[3][3] = {
            {0.59719, 0.35458, 0.04823}, {0.07600, 0.90834, 0.01566}, {0.02840, 0.13383, 0.83777}};
        static const f64 out_m[3][3] = {{1.60475, -0.53108, -0.07367},
                                        {-0.10208, 1.10813, -0.00605},
                                        {-0.00327, -0.07276, 1.07602}};
        f64 v[3];
        mul_rows(in_m, c, v);
        f64 f[3];
        for (u32 i = 0; i < 3U; ++i) {
            const f64 a = v[i] * (v[i] + 0.0245786) - 0.000090537;
            const f64 b = v[i] * (0.983729 * v[i] + 0.4329510) + 0.238081;
            f[i] = a / b;
        }
        mul_rows(out_m, f, out);
        for (u32 i = 0; i < 3U; ++i) {
            out[i] = clamp01(out[i]);
        }
    } else if (tonemapper == NPH_RENDER_TONEMAPPER_AGX) {
        static const f64 in_m[3][3] = {{0.842479062253094, 0.0784335999999992, 0.0792237451477643},
                                       {0.0423282422610123, 0.878468636469772, 0.0791661274605434},
                                       {0.0423756549057051, 0.0784336, 0.879142973793104}};
        static const f64 out_m[3][3] = {
            {1.19687900512017, -0.0980208811401368, -0.0990297440797205},
            {-0.0528968517574562, 1.15190312990417, -0.0989611768448433},
            {-0.0529716355144438, -0.0980434501171241, 1.15107367264116}};
        const f64 min_ev = -12.47393;
        const f64 max_ev = 4.026069;
        f64 v[3];
        mul_rows(in_m, c, v);
        for (u32 i = 0; i < 3U; ++i) {
            f64 x = log2(v[i] > 1e-10 ? v[i] : 1e-10);
            x = x < min_ev ? min_ev : (x > max_ev ? max_ev : x);
            x = (x - min_ev) / (max_ev - min_ev);
            const f64 x2 = x * x;
            const f64 x4 = x2 * x2;
            v[i] = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
                   0.1191 * x - 0.00232;
        }
        mul_rows(out_m, v, out);
        for (u32 i = 0; i < 3U; ++i) {
            out[i] = pow(clamp01(out[i]), 2.2);
        }
    } else {
        for (u32 i = 0; i < 3U; ++i) {
            out[i] = clamp01(c[i]);
        }
    }
    return nph_vec3((f32)out[0], (f32)out[1], (f32)out[2]);
}

f32 nph_render_ref_linear_to_srgb(f32 c)
{
    const f64 v = (f64)c;
    return (f32)(v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

f32 nph_render_ref_srgb_to_linear(f32 c)
{
    const f64 v = (f64)c;
    return (f32)(v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
}

NphVec3 nph_render_ref_cube_direction(u32 face, f32 s, f32 t)
{
    const f32 u = 2.0F * s - 1.0F;
    const f32 v = 2.0F * t - 1.0F;
    NphVec3 d;
    switch (face) {
    case 0U:
        d = nph_vec3(1.0F, -v, -u);
        break;
    case 1U:
        d = nph_vec3(-1.0F, -v, u);
        break;
    case 2U:
        d = nph_vec3(u, 1.0F, v);
        break;
    case 3U:
        d = nph_vec3(u, -1.0F, -v);
        break;
    case 4U:
        d = nph_vec3(u, -v, 1.0F);
        break;
    default:
        d = nph_vec3(-u, -v, -1.0F);
        break;
    }
    return nph_vec3_normalize(d);
}

static f64 area_element(f64 x, f64 y)
{
    return atan2(x * y, sqrt(x * x + y * y + 1.0));
}

f32 nph_render_ref_cube_texel_solid_angle(f32 u, f32 v, f32 h)
{
    const f64 x0 = (f64)u - (f64)h;
    const f64 y0 = (f64)v - (f64)h;
    const f64 x1 = (f64)u + (f64)h;
    const f64 y1 = (f64)v + (f64)h;
    return (f32)(area_element(x0, y0) - area_element(x0, y1) - area_element(x1, y0) +
                 area_element(x1, y1));
}

NphVec3 nph_render_ref_sky(const NphRenderSkyDesc *sky, NphVec3 d)
{
    const f32 h = d.y;
    if (h >= 0.0F) {
        const f32 t = nph_f32_pow(h, sky->zenith_exponent);
        return nph_vec3_lerp(sky->horizon, sky->zenith, t);
    }
    const f32 t = nph_f32_saturate(-h * sky->ground_sharpness);
    return nph_vec3_lerp(sky->horizon, sky->ground, t);
}

void nph_render_ref_sh9_basis(NphVec3 d, f32 out[9])
{
    out[0] = 0.282095F;
    out[1] = 0.488603F * d.y;
    out[2] = 0.488603F * d.z;
    out[3] = 0.488603F * d.x;
    out[4] = 1.092548F * d.x * d.y;
    out[5] = 1.092548F * d.y * d.z;
    out[6] = 0.315392F * (3.0F * d.z * d.z - 1.0F);
    out[7] = 1.092548F * d.x * d.z;
    out[8] = 0.546274F * (d.x * d.x - d.y * d.y);
}

void nph_render_ref_sh9_sky(const NphRenderSkyDesc *sky, u32 size, NphVec3 out[9])
{
    f64 sum[9][3] = {{0.0}};
    f64 weight = 0.0;
    const f32 h = 1.0F / (f32)size;
    for (u32 face = 0; face < 6U; ++face) {
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                const f32 s = ((f32)x + 0.5F) / (f32)size;
                const f32 t = ((f32)y + 0.5F) / (f32)size;
                const NphVec3 dir = nph_render_ref_cube_direction(face, s, t);
                const f64 sa =
                    (f64)nph_render_ref_cube_texel_solid_angle(2.0F * s - 1.0F, 2.0F * t - 1.0F, h);
                const NphVec3 l = nph_render_ref_sky(sky, dir);
                f32 basis[9];
                nph_render_ref_sh9_basis(dir, basis);
                for (u32 k = 0; k < 9U; ++k) {
                    sum[k][0] += (f64)l.x * (f64)basis[k] * sa;
                    sum[k][1] += (f64)l.y * (f64)basis[k] * sa;
                    sum[k][2] += (f64)l.z * (f64)basis[k] * sa;
                }
                weight += sa;
            }
        }
    }
    const f64 norm = 4.0 * REF_PI / weight;
    const f64 band[3] = {REF_PI, 2.0 * REF_PI / 3.0, 0.25 * REF_PI};
    for (u32 k = 0; k < 9U; ++k) {
        const u32 l = k == 0U ? 0U : (k < 4U ? 1U : 2U);
        const f64 f = norm * band[l];
        out[k] = nph_vec3((f32)(sum[k][0] * f), (f32)(sum[k][1] * f), (f32)(sum[k][2] * f));
    }
}

NphVec3 nph_render_ref_sh9_irradiance(const NphVec3 sh[9], NphVec3 n)
{
    f32 basis[9];
    nph_render_ref_sh9_basis(n, basis);
    NphVec3 e = nph_vec3_zero();
    for (u32 k = 0; k < 9U; ++k) {
        e = nph_vec3_add(e, nph_vec3_scale(sh[k], basis[k]));
    }
    return nph_vec3_max(e, nph_vec3_zero());
}

// ---------------------------------------------------------------------------
// The production path's culling
// ---------------------------------------------------------------------------

// The distance from x to the nearest integer.
static f64 to_integer(f64 x)
{
    return fabs(x - floor(x + 0.5));
}

typedef struct D3 {
    f64 x;
    f64 y;
    f64 z;
} D3;

static D3 d3(NphVec3 v)
{
    return (D3){(f64)v.x, (f64)v.y, (f64)v.z};
}

static f64 d3_dot(D3 a, D3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

f64 nph_render_ref_vis_frustum_margin(const NphVec4 planes[5], NphVec3 c, f32 r)
{
    const D3 cd = d3(c);
    f64 margin = HUGE_VAL;
    for (u32 i = 0; i < 5U; ++i) {
        const D3 n = {(f64)planes[i].x, (f64)planes[i].y, (f64)planes[i].z};
        const f64 d = d3_dot(n, cd) + (f64)planes[i].w + (f64)r;
        margin = d < margin ? d : margin;
    }
    return margin;
}

f64 nph_render_ref_vis_cone_margin(NphVec3 apex, NphVec3 axis, f32 cutoff)
{
    const D3 a = d3(apex);
    return d3_dot(a, d3(axis)) - (f64)cutoff * sqrt(d3_dot(a, a));
}

bool nph_render_ref_vis_project_sphere(NphVec3 c, f32 r, f32 p00, f32 p11, f32 znear, NphVec4 *rect,
                                       f32 *depth)
{
    *rect = nph_vec4(0.0F, 0.0F, 0.0F, 0.0F);
    *depth = 0.0F;
    const f64 cx = (f64)c.x;
    const f64 cy = (f64)c.y;
    const f64 cz = -(f64)c.z;
    const f64 rr = (f64)r;
    const f64 n = (f64)znear;
    if (cz - rr < n) {
        return false;
    }
    const f64 czr2 = cz * cz - rr * rr;
    const f64 vx = sqrt(cx * cx + czr2);
    const f64 minx = (vx * cx - rr * cz) / (vx * cz + rr * cx);
    const f64 maxx = (vx * cx + rr * cz) / (vx * cz - rr * cx);
    const f64 vy = sqrt(cy * cy + czr2);
    const f64 miny = (vy * cy - rr * cz) / (vy * cz + rr * cy);
    const f64 maxy = (vy * cy + rr * cz) / (vy * cz - rr * cy);
    const f64 x0 = minx * (f64)p00;
    const f64 x1 = maxx * (f64)p00;
    const f64 y0 = miny * (f64)p11;
    const f64 y1 = maxy * (f64)p11;
    rect->x = (f32)(x0 * 0.5 + 0.5);
    rect->y = (f32)((y0 < y1 ? y0 : y1) * 0.5 + 0.5);
    rect->z = (f32)(x1 * 0.5 + 0.5);
    rect->w = (f32)((y0 < y1 ? y1 : y0) * 0.5 + 0.5);
    *depth = (f32)(n / (cz - rr));
    return true;
}

// One axis of the clipped bounds, in the plane of the axis and the view
// direction: `c` is the centre there (axis coordinate, z), `near_z` the near
// plane's z (negative).
static void sphere_axis_bounds(f64 cx, f64 cz, f64 r, f64 near_z, f64 lower[2], f64 upper[2])
{
    const f64 t2 = cx * cx + cz * cz - r * r;
    const bool inside = t2 <= 0.0;
    f64 cos_t = 0.0;
    f64 sin_t = 0.0;
    if (!inside) {
        const f64 len = sqrt(cx * cx + cz * cz);
        cos_t = sqrt(t2) / len;
        sin_t = r / len;
    }
    const bool clipped = cz + r >= near_z;
    const f64 dz = near_z - cz;
    f64 k = sqrt(r * r - dz * dz > 0.0 ? r * r - dz * dz : 0.0);
    f64 b[2][2];
    for (u32 i = 0; i < 2U; ++i) {
        b[i][0] = 0.0;
        b[i][1] = near_z;
        if (!inside) {
            b[i][0] = (cos_t * cx - sin_t * cz) * cos_t;
            b[i][1] = (sin_t * cx + cos_t * cz) * cos_t;
        }
        if (clipped && (inside || b[i][1] > near_z)) {
            b[i][0] = cx + k;
            b[i][1] = near_z;
        }
        sin_t = -sin_t;
        k = -k;
    }
    upper[0] = b[0][0];
    upper[1] = b[0][1];
    lower[0] = b[1][0];
    lower[1] = b[1][1];
}

NphVec4 nph_render_ref_vis_project_sphere_clipped(NphVec3 c, f32 r, f32 p00, f32 p11, f32 znear)
{
    const f64 near_z = -(f64)znear;
    f64 lx[2];
    f64 ux[2];
    f64 ly[2];
    f64 uy[2];
    sphere_axis_bounds((f64)c.x, (f64)c.z, (f64)r, near_z, lx, ux);
    sphere_axis_bounds((f64)c.y, (f64)c.z, (f64)r, near_z, ly, uy);
    const f64 x0 = lx[0] / -lx[1] * (f64)p00;
    const f64 x1 = ux[0] / -ux[1] * (f64)p00;
    const f64 y0 = ly[0] / -ly[1] * (f64)p11;
    const f64 y1 = uy[0] / -uy[1] * (f64)p11;
    return nph_vec4((f32)((x0 < x1 ? x0 : x1) * 0.5 + 0.5), (f32)((y0 < y1 ? y0 : y1) * 0.5 + 0.5),
                    (f32)((x0 < x1 ? x1 : x0) * 0.5 + 0.5), (f32)((y0 < y1 ? y1 : y0) * 0.5 + 0.5));
}

NphVec4 nph_render_ref_vis_light_sphere(NphVec3 position, NphVec3 direction, f32 range, u32 type,
                                        f32 cos_outer, f32 sin_outer)
{
    f64 t = 0.0;
    f64 radius = (f64)range;
    if (type == (u32)NPH_RENDER_LIGHT_TYPE_SPOT) {
        if ((f64)cos_outer >= 0.70710678) {
            t = (f64)range / (2.0 * (f64)cos_outer);
            radius = t;
        } else {
            t = (f64)range * (f64)cos_outer;
            radius = (f64)range * (f64)sin_outer;
        }
    }
    return nph_vec4((f32)((f64)position.x + (f64)direction.x * t),
                    (f32)((f64)position.y + (f64)direction.y * t),
                    (f32)((f64)position.z + (f64)direction.z * t), (f32)radius);
}

u32 nph_render_ref_zbin(f32 z, f32 znear, f32 scale, f64 *margin)
{
    const f64 x = log2((f64)z / (f64)znear) * (f64)scale;
    const f64 last = (f64)(NPH_RENDER_ZBINS - 1);
    const f64 clamped = x < 0.0 ? 0.0 : (x > last ? last : x);
    // A clamped value is decided by the clamp, unless it is near the bound.
    const f64 to_bound = x < 0.0 ? -x : (x > last ? x - last : (f64)INFINITY);
    const f64 to_edge = fabs(x - floor(x + 0.5));
    *margin = x < 0.0 || x > last ? to_bound : to_edge;
    return (u32)clamped;
}

f64 nph_render_ref_vis_facing_det(NphVec4 c0, NphVec4 c1, NphVec4 c2)
{
    // The determinant of the matrix whose columns are (x, y, w) of each
    // vertex.
    const D3 a = {(f64)c0.x, (f64)c0.y, (f64)c0.w};
    const D3 b = {(f64)c1.x, (f64)c1.y, (f64)c1.w};
    const D3 c = {(f64)c2.x, (f64)c2.y, (f64)c2.w};
    return a.x * (b.y * c.z - b.z * c.y) - b.x * (a.y * c.z - a.z * c.y) +
           c.x * (a.y * b.z - a.z * b.y);
}

bool nph_render_ref_vis_cull_triangle(NphVec4 c0, NphVec4 c1, NphVec4 c2, bool mirrored,
                                      NphVec2 viewport, f64 *facing_margin, f64 *box_margin)
{
    const NphVec4 c[3] = {c0, c1, c2};
    *facing_margin = HUGE_VAL;
    *box_margin = HUGE_VAL;
    if (c0.w <= 0.0F || c1.w <= 0.0F || c2.w <= 0.0F) {
        return false;
    }
    // The determinant against the size of its terms.
    const f64 det = nph_render_ref_vis_facing_det(c0, c1, c2);
    f64 scale = 1.0;
    for (u32 i = 0; i < 3U; ++i) {
        scale *= fabs((f64)c[i].x) + fabs((f64)c[i].y) + fabs((f64)c[i].w);
    }
    *facing_margin = fabs(det) / scale;
    if ((det < 0.0) == mirrored) {
        return true; // a back face
    }
    const f64 size[2] = {(f64)viewport.x, (f64)viewport.y};
    f64 lo[2] = {HUGE_VAL, HUGE_VAL};
    f64 hi[2] = {-HUGE_VAL, -HUGE_VAL};
    for (u32 i = 0; i < 3U; ++i) {
        const f64 w = (f64)c[i].w;
        const f64 s[2] = {((f64)c[i].x / w * 0.5 + 0.5) * size[0],
                          ((f64)c[i].y / w * 0.5 + 0.5) * size[1]};
        for (u32 a = 0; a < 2U; ++a) {
            lo[a] = s[a] < lo[a] ? s[a] : lo[a];
            hi[a] = s[a] > hi[a] ? s[a] : hi[a];
        }
    }
    bool culled = false;
    for (u32 a = 0; a < 2U; ++a) {
        const f64 l = lo[a] - 1.0 / 256.0 - 0.5;
        const f64 h = hi[a] + 1.0 / 256.0 - 0.5;
        culled = culled || floor(h) < ceil(l);
        *box_margin = nph_f64_min(*box_margin, nph_f64_min(to_integer(l), to_integer(h)));
    }
    return culled;
}

bool nph_render_ref_vis_hzb_occluded(const NphRenderRefHzb *hzb, NphVec4 rect, f32 depth,
                                     f64 *margin)
{
    const f64 r[4] = {nph_f64_clamp((f64)rect.x, 0.0, 1.0), nph_f64_clamp((f64)rect.y, 0.0, 1.0),
                      nph_f64_clamp((f64)rect.z, 0.0, 1.0), nph_f64_clamp((f64)rect.w, 0.0, 1.0)};
    const f64 vw = (f64)hzb->viewport.x;
    const f64 vh = (f64)hzb->viewport.y;
    const f64 sx = (r[2] - r[0]) * vw * 0.5;
    const f64 sy = (r[3] - r[1]) * vh * 0.5;
    const f64 extent = nph_f64_max(nph_f64_max(sx, sy), 1.0);
    const f64 level = log2(extent);
    // The mip only matters while the chain has one to choose.
    f64 m = extent > 1.0 ? to_integer(level) : HUGE_VAL;
    const u32 wanted = (u32)ceil(level);
    const u32 mip = wanted < hzb->mip_count - 1U ? wanted : hzb->mip_count - 1U;
    const f64 scale_x = vw * 0.5 / (f64)(1U << mip);
    const f64 scale_y = vh * 0.5 / (f64)(1U << mip);
    const f64 fx = r[0] * scale_x;
    const f64 fy = r[1] * scale_y;
    // A rectangle clamped to the edge sits on texel 0 exactly, in either
    // precision.
    m = nph_f64_min(m, r[0] > 0.0 ? to_integer(fx) : HUGE_VAL);
    m = nph_f64_min(m, r[1] > 0.0 ? to_integer(fy) : HUGE_VAL);
    const u32 w = nph_max_u32(hzb->width >> mip, 1U);
    const u32 h = nph_max_u32(hzb->height >> mip, 1U);
    const u32 x0 = nph_min_u32((u32)floor(fx), w - 1U);
    const u32 y0 = nph_min_u32((u32)floor(fy), h - 1U);
    const u32 x1 = nph_min_u32(x0 + 1U, w - 1U);
    const u32 y1 = nph_min_u32(y0 + 1U, h - 1U);
    const f32 *texels = hzb->mips[mip];
    const f32 d = nph_f32_min(nph_f32_min(texels[(u64)y0 * w + x0], texels[(u64)y0 * w + x1]),
                              nph_f32_min(texels[(u64)y1 * w + x0], texels[(u64)y1 * w + x1]));
    *margin = nph_f64_min(m, fabs((f64)depth - (f64)d));
    return depth < d;
}
