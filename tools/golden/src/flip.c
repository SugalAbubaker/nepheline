// SPDX-License-Identifier: Apache-2.0
// LDR-FLIP (golden/flip.h): Andersson et al., "FLIP: A Difference
// Evaluator for Alternating Images", HPG 2020. Section numbers below are
// the paper's. Constants are the paper's (Table 1 and §4); colour
// matrices are those of IEC 61966-2-1 (sRGB, D65).
//
// The two-dimensional filters of the paper are all sums of separable
// terms, so each is applied as row and column passes; image borders
// replicate the edge texel.
#include <nepheline/core/assert.h>

#include <golden/flip.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FLIP_PI 3.14159265358979323846

// §4.1: the qc, pc, pt of the colour pipeline and the qf of the features.
#define FLIP_QC 0.7F
#define FLIP_PC 0.4F
#define FLIP_PT 0.95F
#define FLIP_QF 0.5F
// §4.2: the human edge-detection filter's peak-to-trough width, degrees.
#define FLIP_FEATURE_WIDTH 0.082

// ---------------------------------------------------------------------------
// Colour spaces (§4.1.1)
// ---------------------------------------------------------------------------

typedef struct Vec3f {
    f32 x, y, z;
} Vec3f;

static const f32 g_rgb_to_xyz[3][3] = {
    {0.41239079926595F, 0.35758433938387F, 0.18048078840183F},
    {0.21263900587151F, 0.71516867876775F, 0.07219231536073F},
    {0.01933081871559F, 0.11919477979462F, 0.95053215224966F},
};
static const f32 g_xyz_to_rgb[3][3] = {
    {3.24096994190452F, -1.53738317757009F, -0.49861076029300F},
    {-0.96924363628087F, 1.87596750150772F, 0.04155505740718F},
    {0.05563007969699F, -0.20397695888897F, 1.05697151424288F},
};

static Vec3f mul3(const f32 m[3][3], Vec3f v)
{
    Vec3f r;
    r.x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z;
    r.y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z;
    r.z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z;
    return r;
}

// The reference white: linear RGB (1, 1, 1) in XYZ (D65).
static Vec3f white_xyz(void)
{
    const Vec3f one = {1.0F, 1.0F, 1.0F};
    return mul3(g_rgb_to_xyz, one);
}

static f32 srgb_to_linear(f32 c)
{
    return c <= 0.04045F ? c / 12.92F : powf((c + 0.055F) / 1.055F, 2.4F);
}

// XYZ to YyCxCz, the linearised L*a*b* of §4.1.1 (no cube root, so that
// the contrast sensitivity filters act on a linear space).
static Vec3f xyz_to_ycxcz(Vec3f xyz, Vec3f w)
{
    const f32 xn = xyz.x / w.x;
    const f32 yn = xyz.y / w.y;
    const f32 zn = xyz.z / w.z;
    Vec3f r;
    r.x = 116.0F * yn - 16.0F;
    r.y = 500.0F * (xn - yn);
    r.z = 200.0F * (yn - zn);
    return r;
}

static Vec3f ycxcz_to_xyz(Vec3f c, Vec3f w)
{
    const f32 yn = (c.x + 16.0F) / 116.0F;
    const f32 xn = c.y / 500.0F + yn;
    const f32 zn = yn - c.z / 200.0F;
    Vec3f r;
    r.x = xn * w.x;
    r.y = yn * w.y;
    r.z = zn * w.z;
    return r;
}

static f32 lab_f(f32 t)
{
    const f32 delta = 6.0F / 29.0F;
    return t > delta * delta * delta ? cbrtf(t) : t / (3.0F * delta * delta) + 4.0F / 29.0F;
}

// Linear RGB to CIELAB, then the Hunt adjustment of §4.1.2: chroma scales
// with lightness, a* and b* by 0.01 L*.
static Vec3f linear_to_hunt_lab(Vec3f rgb, Vec3f w)
{
    const Vec3f xyz = mul3(g_rgb_to_xyz, rgb);
    const f32 fx = lab_f(xyz.x / w.x);
    const f32 fy = lab_f(xyz.y / w.y);
    const f32 fz = lab_f(xyz.z / w.z);
    Vec3f lab;
    lab.x = 116.0F * fy - 16.0F;
    lab.y = 500.0F * (fx - fy) * 0.01F * lab.x;
    lab.z = 200.0F * (fy - fz) * 0.01F * lab.x;
    return lab;
}

// The HyAB distance of §4.1.2: city block in lightness, Euclidean in chroma.
static f32 hyab(Vec3f a, Vec3f b)
{
    const f32 da = a.y - b.y;
    const f32 db = a.z - b.z;
    return fabsf(a.x - b.x) + sqrtf(da * da + db * db);
}

// ---------------------------------------------------------------------------
// Separable filtering with replicated borders
// ---------------------------------------------------------------------------

static void filter_rows(const f32 *src, f32 *dst, u32 w, u32 h, const f32 *k, i32 r)
{
    for (u32 y = 0; y < h; ++y) {
        const f32 *row = src + (u64)y * w;
        f32 *out = dst + (u64)y * w;
        for (u32 x = 0; x < w; ++x) {
            f32 sum = 0.0F;
            for (i32 t = -r; t <= r; ++t) {
                i32 xi = (i32)x + t;
                xi = xi < 0 ? 0 : (xi >= (i32)w ? (i32)w - 1 : xi);
                sum += k[t + r] * row[xi];
            }
            out[x] = sum;
        }
    }
}

static void filter_cols(const f32 *src, f32 *dst, u32 w, u32 h, const f32 *k, i32 r)
{
    for (u32 y = 0; y < h; ++y) {
        f32 *out = dst + (u64)y * w;
        for (u32 x = 0; x < w; ++x) {
            f32 sum = 0.0F;
            for (i32 t = -r; t <= r; ++t) {
                i32 yi = (i32)y + t;
                yi = yi < 0 ? 0 : (yi >= (i32)h ? (i32)h - 1 : yi);
                sum += k[t + r] * src[(u64)yi * w + x];
            }
            out[x] = sum;
        }
    }
}

// ---------------------------------------------------------------------------
// The working state
// ---------------------------------------------------------------------------

#define FLIP_MAX_RADIUS 512

typedef struct Flip {
    u32 w;
    u32 h;
    u64 n;
    f32 ppd;
    NphAllocator *alloc;
    f32 *planes; // one allocation, sliced below
    u64 plane_count;
    f32 *ycc[2][3]; // [reference, test][Yy, Cx, Cz]
    f32 *filtered[3];
    f32 *tmp[2];
    f32 *lab[2][3];     // [image] Hunt-adjusted L*a*b* of the filtered image
    f32 *color;         // the colour error per pixel
    f32 *feature[2][2]; // [image][edges, points] magnitudes
    f32 *kernel;        // scratch for 1D kernels (several, 2 * FLIP_MAX_RADIUS + 1 each)
} Flip;

// The contrast sensitivity filters of §4.1.1 (Table 1): per channel, one
// or two Gaussians a sqrt(pi / b) exp(-pi^2 x^2 / b) with x in degrees.
typedef struct Csf {
    f32 a1, b1, a2, b2;
} Csf;

static const Csf g_csf[3] = {
    {1.0F, 0.0047F, 0.0F, 1e-5F},  // achromatic
    {1.0F, 0.0053F, 0.0F, 1e-5F},  // red-green
    {34.1F, 0.04F, 13.5F, 0.025F}, // blue-yellow
};

// Filters channel `c` of one image's YyCxCz planes into `out`: each
// Gaussian term separably, summed, normalised so the 2D kernel sums to 1.
static void csf_filter(Flip *f, const f32 *src, u32 c, i32 r, f32 *out)
{
    const Csf *p = &g_csf[c];
    const f64 dx = 1.0 / (f64)f->ppd;
    f32 *k1 = f->kernel;
    f32 *k2 = f->kernel + (2 * FLIP_MAX_RADIUS + 1);
    f64 s1 = 0.0;
    f64 s2 = 0.0;
    for (i32 t = -r; t <= r; ++t) {
        const f64 z = ((f64)t * dx) * ((f64)t * dx);
        k1[t + r] = (f32)exp(-FLIP_PI * FLIP_PI * z / (f64)p->b1);
        k2[t + r] = (f32)exp(-FLIP_PI * FLIP_PI * z / (f64)p->b2);
        s1 += (f64)k1[t + r];
        s2 += (f64)k2[t + r];
    }
    const f64 w1 = (f64)p->a1 * sqrt(FLIP_PI / (f64)p->b1);
    const f64 w2 = (f64)p->a2 * sqrt(FLIP_PI / (f64)p->b2);
    const f64 total = w1 * s1 * s1 + w2 * s2 * s2;
    filter_rows(src, f->tmp[0], f->w, f->h, k1, r);
    filter_cols(f->tmp[0], out, f->w, f->h, k1, r);
    const f32 c1 = (f32)(w1 / total);
    if (w2 == 0.0) {
        for (u64 i = 0; i < f->n; ++i) {
            out[i] *= c1;
        }
        return;
    }
    filter_rows(src, f->tmp[0], f->w, f->h, k2, r);
    filter_cols(f->tmp[0], f->tmp[1], f->w, f->h, k2, r);
    const f32 c2 = (f32)(w2 / total);
    for (u64 i = 0; i < f->n; ++i) {
        out[i] = out[i] * c1 + f->tmp[1][i] * c2;
    }
}

// Filters image `img` and leaves its Hunt-adjusted L*a*b* in `lab`: the
// filtered YyCxCz back to linear RGB, clamped to the gamut (§4.1.1).
static void filtered_lab(Flip *f, u32 img, f32 *lab[3], Vec3f w)
{
    const f64 max_b = 0.04; // the largest b of Table 1: the shared radius
    i32 r = (i32)ceil(3.0 * sqrt(max_b / (2.0 * FLIP_PI * FLIP_PI)) * (f64)f->ppd);
    r = r > FLIP_MAX_RADIUS ? FLIP_MAX_RADIUS : r;
    for (u32 c = 0; c < 3U; ++c) {
        csf_filter(f, f->ycc[img][c], c, r, f->filtered[c]);
    }
    for (u64 i = 0; i < f->n; ++i) {
        const Vec3f ycc = {f->filtered[0][i], f->filtered[1][i], f->filtered[2][i]};
        Vec3f rgb = mul3(g_xyz_to_rgb, ycxcz_to_xyz(ycc, w));
        rgb.x = rgb.x < 0.0F ? 0.0F : (rgb.x > 1.0F ? 1.0F : rgb.x);
        rgb.y = rgb.y < 0.0F ? 0.0F : (rgb.y > 1.0F ? 1.0F : rgb.y);
        rgb.z = rgb.z < 0.0F ? 0.0F : (rgb.z > 1.0F ? 1.0F : rgb.z);
        const Vec3f l = linear_to_hunt_lab(rgb, w);
        lab[0][i] = l.x;
        lab[1][i] = l.y;
        lab[2][i] = l.z;
    }
}

// The edge and point magnitudes of §4.2 for image `img`, from its
// normalised achromatic channel (Yy + 16) / 116.
static void features(Flip *f, u32 img)
{
    const f64 sd = 0.5 * FLIP_FEATURE_WIDTH * (f64)f->ppd;
    i32 r = (i32)ceil(3.0 * sd);
    r = r > FLIP_MAX_RADIUS ? FLIP_MAX_RADIUS : r;
    const i32 len = 2 * r + 1;
    f32 *g = f->kernel;          // Gaussian, normalised to sum 1
    f32 *edge = f->kernel + len; // first derivative, positive and negative lobes to +-1
    f32 *point = edge + len;     // second derivative, likewise
    f64 gs = 0.0;
    f64 ep = 0.0;
    f64 en = 0.0;
    f64 pp = 0.0;
    f64 pn = 0.0;
    for (i32 t = -r; t <= r; ++t) {
        const f64 x = (f64)t;
        const f64 gv = exp(-(x * x) / (2.0 * sd * sd));
        const f64 ev = -x * gv;
        const f64 pv = (x * x / (sd * sd) - 1.0) * gv;
        g[t + r] = (f32)gv;
        edge[t + r] = (f32)ev;
        point[t + r] = (f32)pv;
        gs += gv;
        ep += ev > 0.0 ? ev : 0.0;
        en += ev < 0.0 ? -ev : 0.0;
        pp += pv > 0.0 ? pv : 0.0;
        pn += pv < 0.0 ? -pv : 0.0;
    }
    // The 2D kernels h(x) g(y) are normalised so their positive weights sum
    // to 1 and their negative ones to -1: per lobe of h, times g's sum.
    for (i32 i = 0; i < len; ++i) {
        g[i] = (f32)((f64)g[i] / gs);
        edge[i] = (f32)((f64)edge[i] / (edge[i] > 0.0F ? ep : en));
        point[i] = (f32)((f64)point[i] / (point[i] > 0.0F ? pp : pn));
    }
    // The achromatic channel, normalised to [0, 1].
    f32 *y = f->filtered[0];
    for (u64 i = 0; i < f->n; ++i) {
        y[i] = (f->ycc[img][0][i] + 16.0F) / 116.0F;
    }
    const f32 *k1d[2] = {edge, point};
    for (u32 kind = 0; kind < 2U; ++kind) {
        f32 *gx = f->filtered[1];
        f32 *gy = f->filtered[2];
        // d/dx: the derivative along rows, the Gaussian along columns;
        // d/dy: the other way round.
        filter_rows(y, f->tmp[0], f->w, f->h, k1d[kind], r);
        filter_cols(f->tmp[0], gx, f->w, f->h, g, r);
        filter_rows(y, f->tmp[0], f->w, f->h, g, r);
        filter_cols(f->tmp[0], gy, f->w, f->h, k1d[kind], r);
        f32 *mag = f->feature[img][kind];
        for (u64 i = 0; i < f->n; ++i) {
            mag[i] = sqrtf(gx[i] * gx[i] + gy[i] * gy[i]);
        }
    }
}

static int compare_f32(const void *a, const void *b)
{
    const f32 x = *(const f32 *)a;
    const f32 y = *(const f32 *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static f32 nearest_rank(const f32 *sorted, u64 n, u32 percent)
{
    u64 rank = (n * percent + 99U) / 100U; // ceil(n p / 100)
    rank = rank == 0U ? 1U : rank;
    return sorted[rank - 1U];
}

NphResult golden_flip(const u8 *reference, const u8 *test, u32 width, u32 height, f32 ppd,
                      NphAllocator *alloc, f32 *out_map, GoldenFlipStats *out_stats)
{
    NPH_ASSERT(alloc != NULL);
    if (reference == NULL || test == NULL || width == 0U || height == 0U || !(ppd >= 0.0F) ||
        (u64)width * height > (1ULL << 28U)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    Flip f;
    memset(&f, 0, sizeof f);
    f.w = width;
    f.h = height;
    f.n = (u64)width * height;
    f.ppd = ppd > 0.0F ? ppd : GOLDEN_FLIP_DEFAULT_PPD;
    f.alloc = alloc;
    f.plane_count = 6U + 3U + 2U + 6U + 1U + 4U;
    const u64 kernel_floats = 3ULL * (2ULL * FLIP_MAX_RADIUS + 1ULL);
    f.planes = NPH_ALLOC_ARRAY(alloc, f32, f.plane_count * f.n + kernel_floats);
    if (f.planes == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    f32 *at = f.planes;
    for (u32 img = 0; img < 2U; ++img) {
        for (u32 c = 0; c < 3U; ++c) {
            f.ycc[img][c] = at;
            at += f.n;
        }
    }
    for (u32 c = 0; c < 3U; ++c) {
        f.filtered[c] = at;
        at += f.n;
    }
    f.tmp[0] = at;
    at += f.n;
    f.tmp[1] = at;
    at += f.n;
    for (u32 img = 0; img < 2U; ++img) {
        for (u32 c = 0; c < 3U; ++c) {
            f.lab[img][c] = at;
            at += f.n;
        }
    }
    f.color = at;
    at += f.n;
    for (u32 img = 0; img < 2U; ++img) {
        f.feature[img][0] = at;
        at += f.n;
        f.feature[img][1] = at;
        at += f.n;
    }
    f.kernel = at;

    // Both images to YyCxCz.
    const Vec3f w = white_xyz();
    f32 lut[256];
    for (u32 i = 0; i < 256U; ++i) {
        lut[i] = srgb_to_linear((f32)i / 255.0F);
    }
    const u8 *images[2] = {reference, test};
    for (u32 img = 0; img < 2U; ++img) {
        for (u64 i = 0; i < f.n; ++i) {
            const u8 *p = images[img] + i * 4U;
            const Vec3f rgb = {lut[p[0]], lut[p[1]], lut[p[2]]};
            const Vec3f ycc = xyz_to_ycxcz(mul3(g_rgb_to_xyz, rgb), w);
            f.ycc[img][0][i] = ycc.x;
            f.ycc[img][1][i] = ycc.y;
            f.ycc[img][2][i] = ycc.z;
        }
    }

    // The colour pipeline (§4.1): filtered, gamut-clamped, Hunt-adjusted
    // L*a*b*; the HyAB distance raised to qc; the two-segment remap that
    // sends pc of the largest distance (green against blue) to pt.
    filtered_lab(&f, 0U, f.lab[0], w);
    filtered_lab(&f, 1U, f.lab[1], w);
    const Vec3f green = linear_to_hunt_lab((Vec3f){0.0F, 1.0F, 0.0F}, w);
    const Vec3f blue = linear_to_hunt_lab((Vec3f){0.0F, 0.0F, 1.0F}, w);
    const f32 cmax = powf(hyab(green, blue), FLIP_QC);
    const f32 pccmax = FLIP_PC * cmax;
    for (u64 i = 0; i < f.n; ++i) {
        const Vec3f a = {f.lab[0][0][i], f.lab[0][1][i], f.lab[0][2][i]};
        const Vec3f b = {f.lab[1][0][i], f.lab[1][1][i], f.lab[1][2][i]};
        const f32 d = powf(hyab(a, b), FLIP_QC);
        f.color[i] = d < pccmax ? (FLIP_PT / pccmax) * d
                                : FLIP_PT + ((d - pccmax) / (cmax - pccmax)) * (1.0F - FLIP_PT);
    }

    // The feature pipeline (§4.2).
    features(&f, 0U);
    features(&f, 1U);

    // §4.3: the final error, colour raised to (1 - features).
    f32 *map = out_map != NULL ? out_map : f.tmp[1];
    f64 sum = 0.0;
    f32 max = 0.0F;
    const f32 inv_sqrt2 = 0.70710678118654752F;
    for (u64 i = 0; i < f.n; ++i) {
        const f32 de = fabsf(f.feature[0][0][i] - f.feature[1][0][i]);
        const f32 dp = fabsf(f.feature[0][1][i] - f.feature[1][1][i]);
        const f32 df = powf(inv_sqrt2 * (de > dp ? de : dp), FLIP_QF);
        const f32 e = powf(f.color[i], 1.0F - df);
        map[i] = e;
        sum += (f64)e;
        max = e > max ? e : max;
    }
    if (out_stats != NULL) {
        memset(out_stats, 0, sizeof *out_stats);
        out_stats->mean = sum / (f64)f.n;
        out_stats->max = max;
        f32 *sorted = f.tmp[0];
        memcpy(sorted, map, sizeof(f32) * f.n);
        qsort(sorted, f.n, sizeof(f32), compare_f32);
        out_stats->p50 = nearest_rank(sorted, f.n, 50U);
        out_stats->p95 = nearest_rank(sorted, f.n, 95U);
        out_stats->p99 = nearest_rank(sorted, f.n, 99U);
    }
    NPH_FREE_ARRAY(alloc, f32, f.planes, f.plane_count * f.n + kernel_floats);
    return NPH_RESULT_OK;
}

// Matt Zucker's sixth-degree polynomial fit of matplotlib's magma colour
// map ("Polynomial approximations of matplotlib colormaps", 2019, CC0),
// in sRGB-encoded values.
void golden_flip_magma(const f32 *map, u32 width, u32 height, u8 *out_rgba)
{
    static const f32 c[7][3] = {
        {-0.002136485053939582F, -0.000749655052795221F, -0.005386127855323933F},
        {0.2516605407371642F, 0.6775232436837668F, 2.494026599312351F},
        {8.353717279216625F, -3.577719514958484F, 0.3144679030132573F},
        {-27.66873308576866F, 14.26473078096533F, -13.64921318813922F},
        {52.17613981234068F, -27.94360607168351F, 12.94416944238394F},
        {-50.76852536473588F, 29.04658282127291F, 4.23415299384598F},
        {18.65570506591883F, -11.48977351997711F, -5.601961508734096F},
    };
    const u64 n = (u64)width * height;
    for (u64 i = 0; i < n; ++i) {
        f32 t = map[i];
        t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
        for (u32 ch = 0; ch < 3U; ++ch) {
            f32 v = c[6][ch];
            for (i32 k = 5; k >= 0; --k) {
                v = v * t + c[k][ch];
            }
            v = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
            out_rgba[i * 4U + ch] = (u8)(v * 255.0F + 0.5F);
        }
        out_rgba[i * 4U + 3U] = 255U;
    }
}
