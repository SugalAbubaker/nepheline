// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the shadow tests (test_shadows.c, ray traced; test_vsm.c, the
// virtual shadow map): a floor with an occluder under a sun or a point
// light, views of it, and the analytic shadow every pixel is checked
// against: rays from the camera through the pixel's footprint, against the
// floor and the occluder's box, then toward the light.

#include <nepheline/math/scalar.h>

#include "test_render_support.h"

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)

// Levels a shadowed pixel may differ from black, and an unshadowed one
// from the image without shadows: the production path's stochastic
// rounding moves a level either way.
#define DARK_MAX 2U
#define SAME_TOL 2U

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

static inline NphRenderMesh mesh_of(RenderFixture *f, NphResult (*make)(NphRenderMeshData *))
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(make(&data), NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

static inline NphResult make_cube(NphRenderMeshData *out)
{
    return nph_render_primitive_cube(nph_allocator_system(), 1.0F, out);
}

static inline NphResult make_plane(NphRenderMeshData *out)
{
    return nph_render_primitive_plane(nph_allocator_system(), 1.0F, 4U, 1.0F, out);
}

static inline NphRenderMaterial material(RenderFixture *f, NphVec4 color)
{
    NphRenderMaterialDesc md;
    nph_render_material_desc_default(&md);
    md.base_color = color;
    md.metallic = 0.0F;
    md.roughness = 0.9F;
    NphRenderMaterial m = NPH_RENDER_MATERIAL_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_material_create(f->renderer, &md, &m), NPH_RESULT_OK);
    return m;
}

static inline NphRenderInstance instance(RenderFixture *f, NphRenderMesh mesh,
                                         NphRenderMaterial mat, NphRenderTransform t)
{
    NphRenderInstanceDesc id;
    memset(&id, 0, sizeof id);
    id.mesh = mesh;
    id.materials = &mat;
    id.material_count = 1U;
    id.transform = t;
    NphRenderInstance inst = NPH_RENDER_INSTANCE_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_instance_create(f->renderer, &id, &inst), NPH_RESULT_OK);
    return inst;
}

static inline NphRenderTransform at(f64 x, f64 y, f64 z, f32 scale)
{
    return nph_render_transform_from_trs(nph_dvec3(x, y, z), nph_quat_identity(),
                                         nph_vec3(scale, scale, scale));
}

// The floor (y = 0, 6 m square) and a unit cube over it.
typedef struct Scene {
    NphRenderMesh cube;
    NphRenderMesh plane;
    NphRenderMaterial grey;
    NphRenderInstance floor;
    NphRenderInstance occluder;
} Scene;

static inline void scene_build(RenderFixture *f, Scene *s, NphRenderTransform occluder)
{
    memset(s, 0, sizeof *s);
    s->cube = mesh_of(f, make_cube);
    s->plane = mesh_of(f, make_plane);
    s->grey = material(f, nph_vec4(0.5F, 0.5F, 0.5F, 1.0F));
    s->floor = instance(f, s->plane, s->grey, at(0.0, 0.0, 0.0, 6.0F));
    s->occluder = instance(f, s->cube, s->grey, occluder);
}

// The sun, 45 degrees up, travelling along +X: a point at height h casts
// its shadow h further along X.
static inline NphRenderLightDesc sun_desc(bool shadows)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_DIRECTIONAL;
    ld.color = nph_vec3_one();
    ld.intensity = 30000.0F;
    ld.direction = nph_vec3(1.0F, -1.0F, 0.0F);
    ld.shadows = shadows ? 1 : 0;
    return ld;
}

static inline NphRenderLightDesc point_desc(NphDVec3 p, bool shadows)
{
    NphRenderLightDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.type = NPH_RENDER_LIGHT_TYPE_POINT;
    ld.color = nph_vec3_one();
    ld.intensity = 100.0F;
    ld.position = p;
    ld.shadows = shadows ? 1 : 0;
    return ld;
}

static inline NphRenderLight make_light(RenderFixture *f, const NphRenderLightDesc *ld)
{
    NphRenderLight l = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, ld, &l), NPH_RESULT_OK);
    return l;
}

static inline void set_shadows(RenderFixture *f, NphRenderLight l, NphRenderLightDesc *ld,
                               bool shadows)
{
    ld->shadows = shadows ? 1 : 0;
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, l, ld), NPH_RESULT_OK);
}

// Straight down at the floor from 6 m: +X to the right, +Z down the image.
static inline NphRenderCamera top_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(0.0, 6.0, 0.0), nph_dvec3(0.0, 0.0, 0.0),
                                     nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.05F);
}

// From the side and above, at the floor under the point light.
static inline NphRenderCamera side_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(-3.0, 4.0, 0.0), nph_dvec3(1.0, 0.0, 0.0),
                                     nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F);
}

// No environment and a black background: what no light reaches is black.
// `vis_flags` is the production path's (NPH_RENDER_VIS_RT_SHADOWS: the sun
// ray traced rather than from its virtual shadow map).
static inline void shadow_view(NphRenderView *v, NphRenderCamera c, f32 ev100,
                               NphRenderPipeline pipeline, u32 vis_flags)
{
    nph_render_view_default(v, c);
    v->ev100 = ev100;
    v->pipeline = pipeline;
    v->vis_flags = vis_flags;
    v->disable_ibl = 1;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
}

// Renders two frames (the production path's culling settles on the
// first) and keeps the last image in `out`; returns the last frame's stats.
static inline NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *v, u8 *out)
{
    (void)test_render_read(f, v);
    const NphRenderFrameStats stats = test_render_read(f, v);
    memcpy(out, f->pixels, PIXELS);
    return stats;
}

static const NphRenderPipeline k_pipelines[2] = {NPH_RENDER_PIPELINE_REFERENCE,
                                                 NPH_RENDER_PIPELINE_PRODUCTION};
static const char *const k_pipeline_names[2] = {"reference", "production"};

// The pipelines the renderer has.
static inline u32 pipeline_count(const RenderFixture *f)
{
    return nph_renderer_production_supported(f->renderer) ? 2U : 1U;
}

// ---------------------------------------------------------------------------
// The analytic shadow
// ---------------------------------------------------------------------------

typedef struct Box {
    NphVec3 lo;
    NphVec3 hi;
} Box;

static inline Box box_at(NphVec3 centre, f32 size)
{
    const NphVec3 h = nph_vec3(size * 0.5F, size * 0.5F, size * 0.5F);
    return (Box){nph_vec3_sub(centre, h), nph_vec3_add(centre, h)};
}

// The slab test (Kay & Kajiya 1986; Ericson, Real-Time Collision Detection
// 5.3.3): whether o + t d meets the box for some t in (0, tmax).
static inline bool ray_hits_box(NphVec3 o, NphVec3 d, f32 tmax, const Box *b)
{
    const f32 oo[3] = {o.x, o.y, o.z};
    const f32 dd[3] = {d.x, d.y, d.z};
    const f32 lo[3] = {b->lo.x, b->lo.y, b->lo.z};
    const f32 hi[3] = {b->hi.x, b->hi.y, b->hi.z};
    f32 t0 = 0.0F;
    f32 t1 = tmax;
    for (u32 k = 0; k < 3U; ++k) {
        if (nph_f32_abs(dd[k]) < 1e-12F) {
            if (oo[k] < lo[k] || oo[k] > hi[k]) {
                return false;
            }
            continue;
        }
        f32 a = (lo[k] - oo[k]) / dd[k];
        f32 c = (hi[k] - oo[k]) / dd[k];
        if (a > c) {
            const f32 t = a;
            a = c;
            c = t;
        }
        t0 = nph_f32_max(t0, a);
        t1 = nph_f32_min(t1, c);
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}

typedef struct Light {
    bool directional;
    NphVec3 direction; // the way a directional light travels
    NphVec3 position;  // a point light's
} Light;

// Whether the box hides floor point p from the light.
static inline bool shadowed(const Light *l, NphVec3 p, const Box *occluder)
{
    if (l->directional) {
        return ray_hits_box(p, nph_vec3_neg(nph_vec3_normalize(l->direction)), 1e9F, occluder);
    }
    const NphVec3 to = nph_vec3_sub(l->position, p);
    return ray_hits_box(p, to, 1.0F, occluder); // t in (0, 1): up to the light
}

typedef enum PixelClass {
    PIXEL_UNSURE = 0, // near an edge: not checked
    PIXEL_LIT,        // the floor, in the light: as without shadows
    PIXEL_SHADOW,     // the floor, hidden from the light: black
    PIXEL_OCCLUDER,   // the occluder itself: as without shadows
} PixelClass;

// What the pixel sees, from rays through a grid over its footprint grown by
// `grow` pixels each side, half a pixel apart: a pixel near a shadow's edge
// (or the occluder's) is unsure and not checked. Ray-traced shadows are
// exact, so half a pixel covers the rasteriser's sampling; a shadow map
// blurs its edges over its texels, which the caller's margin covers. The
// floor ends `edge` metres from the origin along X and Z (a little inside
// its own edge).
static inline PixelClass classify(const NphRenderCamera *cam, u32 px, u32 py, const Light *l,
                                  const Box *occluder, f32 grow, f32 edge)
{
    const NphMat3 rot = nph_mat3_from_quat(nph_quat_normalize_or_identity(cam->orientation));
    const NphMat4 view = nph_mat4_from_mat3(nph_mat3_transpose(rot));
    const NphMat4 proj = nph_mat4_perspective_infinite(cam->yfov, 1.0F, cam->znear);
    NphMat4 inv = nph_mat4_identity();
    NTEST_ASSERT_TRUE(nph_mat4_inverse(nph_mat4_mul(proj, view), &inv));
    const NphVec3 eye = nph_vec3((f32)cam->position.x, (f32)cam->position.y, (f32)cam->position.z);
    const f32 span = 1.0F + 2.0F * grow;
    const u32 n = (u32)nph_f32_ceil(span / 0.5F) + 1U;
    PixelClass seen = PIXEL_UNSURE;
    for (u32 i = 0; i < n * n; ++i) {
        const u32 column = i % n;
        const u32 row = i / n;
        const f32 ox = -grow + span * (f32)column / (f32)(n - 1U);
        const f32 oy = -grow + span * (f32)row / (f32)(n - 1U);
        const f32 sx = ((f32)px + ox) / (f32)TEST_TARGET_SIZE * 2.0F - 1.0F;
        const f32 sy = ((f32)py + oy) / (f32)TEST_TARGET_SIZE * 2.0F - 1.0F;
        // Any depth on the ray will do (reverse-Z: 0.5 is in front).
        const NphVec4 h = nph_mat4_mul_vec4(inv, nph_vec4(sx, sy, 0.5F, 1.0F));
        const NphVec3 d = nph_vec3_normalize(nph_vec3(h.x / h.w, h.y / h.w, h.z / h.w));
        PixelClass c;
        if (d.y >= 0.0F) {
            return PIXEL_UNSURE; // above the horizon
        }
        const f32 t_floor = -eye.y / d.y;
        if (ray_hits_box(eye, d, t_floor, occluder)) {
            c = PIXEL_OCCLUDER;
        } else {
            const NphVec3 p = nph_vec3_add(eye, nph_vec3_scale(d, t_floor));
            if (nph_f32_abs(p.x) > edge || nph_f32_abs(p.z) > edge) {
                return PIXEL_UNSURE; // the floor's edge
            }
            c = shadowed(l, p, occluder) ? PIXEL_SHADOW : PIXEL_LIT;
        }
        if (i == 0U) {
            seen = c;
        } else if (c != seen) {
            return PIXEL_UNSURE;
        }
    }
    return seen;
}

static inline u32 channel_diff(const u8 *a, const u8 *b)
{
    u32 worst = 0U;
    for (u32 c = 0; c < 3U; ++c) {
        const u32 x = a[c];
        const u32 y = b[c];
        worst = nph_max_u32(worst, x > y ? x - y : y - x);
    }
    return worst;
}

// The largest channel difference between two images.
static inline u32 image_max_diff(const u8 *a, const u8 *b)
{
    u32 worst = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        worst = nph_max_u32(worst, channel_diff(a + i, b + i));
    }
    return worst;
}

static inline u32 channel_max(const u8 *a)
{
    return nph_max_u32(nph_max_u32(a[0], a[1]), a[2]);
}

typedef struct ShadowCheck {
    u32 lit;      // floor pixels in the light, as without shadows (however dim)
    u32 shadow;   // floor pixels in the shadow, black
    u32 occluder; // occluder pixels, as without shadows
    u32 wrong;    // pixels that broke their class's rule
} ShadowCheck;

// Every pixel of `shadowed_img` against its class: shadowed floor black,
// everything else as in `unshadowed_img`. The floor ends `edge` metres out.
static inline ShadowCheck check_shadow_floor(const u8 *shadowed_img, const u8 *unshadowed_img,
                                             const NphRenderCamera *cam, const Light *l,
                                             const Box *occluder, f32 grow, f32 edge)
{
    ShadowCheck c;
    memset(&c, 0, sizeof c);
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            const u64 at_px = ((u64)y * TEST_TARGET_SIZE + x) * 4U;
            const u8 *s = shadowed_img + at_px;
            const u8 *u = unshadowed_img + at_px;
            switch (classify(cam, x, y, l, occluder, grow, edge)) {
            case PIXEL_LIT:
                ++c.lit;
                c.wrong += channel_diff(s, u) > SAME_TOL ? 1U : 0U;
                break;
            case PIXEL_SHADOW:
                // Black now, and lit without shadows (or the check says
                // nothing).
                ++c.shadow;
                c.wrong += channel_max(s) > DARK_MAX || channel_max(u) < 20U ? 1U : 0U;
                break;
            case PIXEL_OCCLUDER:
                ++c.occluder;
                c.wrong += channel_diff(s, u) > SAME_TOL ? 1U : 0U;
                break;
            case PIXEL_UNSURE:
            default:
                break;
            }
        }
    }
    return c;
}

// The same over scene_build's floor (6 m square).
static inline ShadowCheck check_shadow(const u8 *shadowed_img, const u8 *unshadowed_img,
                                       const NphRenderCamera *cam, const Light *l,
                                       const Box *occluder, f32 grow)
{
    return check_shadow_floor(shadowed_img, unshadowed_img, cam, l, occluder, grow, 2.9F);
}

static inline void log_check(const char *what, const char *pipeline, const ShadowCheck *c)
{
    NTEST_LOG("%s (%s): %u lit, %u shadowed, %u occluder pixels checked; %u wrong", what, pipeline,
              c->lit, c->shadow, c->occluder, c->wrong);
}
