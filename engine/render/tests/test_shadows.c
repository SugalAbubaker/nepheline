// SPDX-License-Identifier: Apache-2.0
// Ray-traced shadows (render/renderer.h NphRenderLightDesc.shadows;
// docs/render.md "Shadows"): on both pipelines, what an occluder hides from
// a light is dark exactly where the geometry says (every pixel classified
// analytically, away from the shadow's edges) and nothing else changes; a
// shadow ray stops at its light; the structures follow the scene (instances
// that move, hide, and go; meshes created later); and a renderer without
// ray tracing draws the same lights unshadowed. Skipped where the device
// has no ray queries; the fixture's teardown asserts zero validation and
// synchronisation-validation errors and nothing leaked.
#include <nepheline/math/scalar.h>

#include "test_render_support.h"

typedef RenderFixture Shadows;

NTEST_SETUP(Shadows)
{
    test_render_setup(f, NULL);
}

NTEST_TEARDOWN(Shadows)
{
    test_render_teardown(f);
}

#define REQUIRE_RAY_TRACING(f)                                                                     \
    do {                                                                                           \
        REQUIRE_RENDERER(f);                                                                       \
        if (!nph_renderer_ray_tracing_supported((f)->renderer)) {                                  \
            NTEST_SKIP("no ray queries on this device");                                           \
        }                                                                                          \
    } while (0)

#define PIXELS ((u64)TEST_TARGET_SIZE * TEST_TARGET_SIZE * 4U)

// Levels a shadowed pixel may differ from black, and an unshadowed one
// from the image without shadows: the production path's stochastic
// rounding moves a level either way.
#define DARK_MAX 2U
#define SAME_TOL 2U

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

static NphRenderMesh mesh_of(RenderFixture *f, NphResult (*make)(NphRenderMeshData *))
{
    NphRenderMeshData data;
    NTEST_ASSERT_EQ_INT(make(&data), NPH_RESULT_OK);
    const NphRenderMesh mesh = test_upload_mesh(f->renderer, &data);
    nph_render_mesh_data_free(&data);
    return mesh;
}

static NphResult make_cube(NphRenderMeshData *out)
{
    return nph_render_primitive_cube(nph_allocator_system(), 1.0F, out);
}

static NphResult make_plane(NphRenderMeshData *out)
{
    return nph_render_primitive_plane(nph_allocator_system(), 1.0F, 4U, 1.0F, out);
}

static NphRenderMaterial material(RenderFixture *f, NphVec4 color)
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

static NphRenderInstance instance(RenderFixture *f, NphRenderMesh mesh, NphRenderMaterial mat,
                                  NphRenderTransform t)
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

static NphRenderTransform at(f64 x, f64 y, f64 z, f32 scale)
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

static void scene_build(RenderFixture *f, Scene *s, NphRenderTransform occluder)
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
static NphRenderLightDesc sun_desc(bool shadows)
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

static NphRenderLightDesc point_desc(NphDVec3 p, bool shadows)
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

static NphRenderLight make_light(RenderFixture *f, const NphRenderLightDesc *ld)
{
    NphRenderLight l = NPH_RENDER_LIGHT_NULL;
    NTEST_ASSERT_EQ_INT(nph_render_light_create(f->renderer, ld, &l), NPH_RESULT_OK);
    return l;
}

static void set_shadows(RenderFixture *f, NphRenderLight l, NphRenderLightDesc *ld, bool shadows)
{
    ld->shadows = shadows ? 1 : 0;
    NTEST_ASSERT_EQ_INT(nph_render_light_update(f->renderer, l, ld), NPH_RESULT_OK);
}

// Straight down at the floor from 6 m: +X to the right, +Z down the image.
static NphRenderCamera top_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(0.0, 6.0, 0.0), nph_dvec3(0.0, 0.0, 0.0),
                                     nph_vec3(0.0F, 0.0F, -1.0F), 0.9F, 0.05F);
}

// From the side and above, at the floor under the point light.
static NphRenderCamera side_camera(void)
{
    return nph_render_camera_look_at(nph_dvec3(-3.0, 4.0, 0.0), nph_dvec3(1.0, 0.0, 0.0),
                                     nph_vec3(0.0F, 1.0F, 0.0F), 0.9F, 0.05F);
}

// No environment and a black background: what no light reaches is black.
static void shadow_view(NphRenderView *v, NphRenderCamera c, f32 ev100, NphRenderPipeline pipeline)
{
    nph_render_view_default(v, c);
    v->ev100 = ev100;
    v->pipeline = pipeline;
    v->disable_ibl = 1;
    v->background = NPH_RENDER_BACKGROUND_SOLID;
    v->background_color = nph_vec3_zero();
}

// Renders two frames (the production path's culling settles on the
// first) and keeps the last image in `out`; returns the last frame's stats.
static NphRenderFrameStats render_into(RenderFixture *f, const NphRenderView *v, u8 *out)
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
static u32 pipeline_count(const RenderFixture *f)
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

static Box box_at(NphVec3 centre, f32 size)
{
    const NphVec3 h = nph_vec3(size * 0.5F, size * 0.5F, size * 0.5F);
    return (Box){nph_vec3_sub(centre, h), nph_vec3_add(centre, h)};
}

// The slab test (Kay & Kajiya 1986; Ericson, Real-Time Collision Detection
// 5.3.3): whether o + t d meets the box for some t in (0, tmax).
static bool ray_hits_box(NphVec3 o, NphVec3 d, f32 tmax, const Box *b)
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
static bool shadowed(const Light *l, NphVec3 p, const Box *occluder)
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

// What the pixel sees, from rays through its footprint grown by half a
// pixel each side (the shadow and the footprint are convex, so agreement
// at the corners and the centre holds over the footprint).
static PixelClass classify(const NphRenderCamera *cam, u32 px, u32 py, const Light *l,
                           const Box *occluder)
{
    const NphMat3 rot = nph_mat3_from_quat(nph_quat_normalize_or_identity(cam->orientation));
    const NphMat4 view = nph_mat4_from_mat3(nph_mat3_transpose(rot));
    const NphMat4 proj = nph_mat4_perspective_infinite(cam->yfov, 1.0F, cam->znear);
    NphMat4 inv = nph_mat4_identity();
    NTEST_ASSERT_TRUE(nph_mat4_inverse(nph_mat4_mul(proj, view), &inv));
    const NphVec3 eye = nph_vec3((f32)cam->position.x, (f32)cam->position.y, (f32)cam->position.z);
    static const f32 k_offsets[5][2] = {
        {-0.5F, -0.5F}, {1.5F, -0.5F}, {-0.5F, 1.5F}, {1.5F, 1.5F}, {0.5F, 0.5F}};
    PixelClass seen = PIXEL_UNSURE;
    for (u32 i = 0; i < 5U; ++i) {
        const f32 sx = ((f32)px + k_offsets[i][0]) / (f32)TEST_TARGET_SIZE * 2.0F - 1.0F;
        const f32 sy = ((f32)py + k_offsets[i][1]) / (f32)TEST_TARGET_SIZE * 2.0F - 1.0F;
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
            if (nph_f32_abs(p.x) > 2.9F || nph_f32_abs(p.z) > 2.9F) {
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

static u32 channel_diff(const u8 *a, const u8 *b)
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
static u32 image_max_diff(const u8 *a, const u8 *b)
{
    u32 worst = 0U;
    for (u64 i = 0; i < PIXELS; i += 4U) {
        worst = nph_max_u32(worst, channel_diff(a + i, b + i));
    }
    return worst;
}

static u32 channel_max(const u8 *a)
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
// everything else as in `unshadowed_img`.
static ShadowCheck check_shadow(const u8 *shadowed_img, const u8 *unshadowed_img,
                                const NphRenderCamera *cam, const Light *l, const Box *occluder)
{
    ShadowCheck c;
    memset(&c, 0, sizeof c);
    for (u32 y = 0; y < TEST_TARGET_SIZE; ++y) {
        for (u32 x = 0; x < TEST_TARGET_SIZE; ++x) {
            const u64 at_px = ((u64)y * TEST_TARGET_SIZE + x) * 4U;
            const u8 *s = shadowed_img + at_px;
            const u8 *u = unshadowed_img + at_px;
            switch (classify(cam, x, y, l, occluder)) {
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

static void log_check(const char *what, const char *pipeline, const ShadowCheck *c)
{
    NTEST_LOG("%s (%s): %u lit, %u shadowed, %u occluder pixels checked; %u wrong", what, pipeline,
              c->lit, c->shadow, c->occluder, c->wrong);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The sun behind a cube: on both pipelines, the floor the cube hides from
// it is black, and every other pixel is as it was without shadows.
NTEST_F(Shadows, an_occluder_darkens_exactly_what_it_hides)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, cam, 13.0F, k_pipelines[k]);
        set_shadows(f, sun, &ld, false);
        NphRenderFrameStats stats = render_into(f, &v, off);
        NTEST_EXPECT_FALSE(stats.shadow_rays);
        set_shadows(f, sun, &ld, true);
        stats = render_into(f, &v, on);
        NTEST_EXPECT_TRUE(stats.shadow_rays);
        NTEST_EXPECT_EQ_INT((int)stats.pipeline, (int)k_pipelines[k]);
        const ShadowCheck c = check_shadow(on, off, &cam, &l, &occluder);
        log_check("sun behind a cube", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);
        NTEST_EXPECT_GT_UINT(c.lit, 2000U);
        NTEST_EXPECT_GT_UINT(c.occluder, 100U);
    }
}

// A point light under a cube: the cube shadows the floor below it, but not
// once it is above the light, where an unbounded ray would still meet it.
NTEST_F(Shadows, a_shadow_ray_stops_at_its_light)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(1.0, 0.5, 0.0, 0.4F));
    NphRenderLightDesc ld = point_desc(nph_dvec3(1.0, 1.0, 0.0), false);
    const NphRenderLight lamp = make_light(f, &ld);
    const NphRenderCamera cam = side_camera();
    const Light l = {false, nph_vec3_zero(), nph_vec3(1.0F, 1.0F, 0.0F)};
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, cam, 5.0F, k_pipelines[k]);
        // Between the light and the floor.
        const NphRenderTransform below = at(1.0, 0.5, 0.0, 0.4F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &below),
                            NPH_RESULT_OK);
        set_shadows(f, lamp, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, lamp, &ld, true);
        render_into(f, &v, on);
        const Box between = box_at(nph_vec3(1.0F, 0.5F, 0.0F), 0.4F);
        ShadowCheck c = check_shadow(on, off, &cam, &l, &between);
        log_check("cube under a point light", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 30U);
        // Above the light: nothing changes.
        const NphRenderTransform above = at(1.0, 2.0, 0.0, 0.5F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &above),
                            NPH_RESULT_OK);
        set_shadows(f, lamp, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, lamp, &ld, true);
        render_into(f, &v, on);
        const Box beyond = box_at(nph_vec3(1.0F, 2.0F, 0.0F), 0.5F);
        c = check_shadow(on, off, &cam, &l, &beyond);
        log_check("cube over a point light", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_EQ_UINT(c.shadow, 0U);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, off), SAME_TOL);
    }
}

// The structures follow the scene: a mesh's bottom level is built once, in
// the first frame that traces; the top level each frame, so the shadow
// moves, hides, and goes with its instance, and a mesh created later casts
// one from its first frame.
NTEST_F(Shadows, the_structures_follow_the_scene)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const NphRenderCamera cam = top_camera();
    const Light l = {true, ld.direction, nph_vec3_zero()};
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    NphRenderView v;
    shadow_view(&v, cam, 13.0F, NPH_RENDER_PIPELINE_REFERENCE);

    // Frames without shadows build nothing; the first with builds both
    // meshes, and the next nothing.
    NphRenderFrameStats stats = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);
    set_shadows(f, sun, &ld, true);
    stats = test_render_read(f, &v);
    NTEST_EXPECT_TRUE(stats.shadow_rays);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 2U);
    stats = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);
    // A mesh created and destroyed between frames is never built.
    const NphRenderMesh gone = mesh_of(f, make_cube);
    nph_render_mesh_destroy(f->renderer, gone);
    stats = test_render_read(f, &v);
    NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);

    for (u32 k = 0; k < pipeline_count(f); ++k) {
        shadow_view(&v, cam, 13.0F, k_pipelines[k]);
        NphRenderTransform t = at(0.0, 1.0, 1.2, 1.0F);
        NTEST_ASSERT_EQ_INT(nph_render_instance_set_transform(f->renderer, s.occluder, &t),
                            NPH_RESULT_OK);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        render_into(f, &v, on);
        const Box moved = box_at(nph_vec3(0.0F, 1.0F, 1.2F), 1.0F);
        ShadowCheck c = check_shadow(on, off, &cam, &l, &moved);
        log_check("moved", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);

        // Hidden: no shadow, no occluder; shown again: as before.
        nph_render_instance_set_hidden(f->renderer, s.occluder, true);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        render_into(f, &v, on);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, off), SAME_TOL);
        nph_render_instance_set_hidden(f->renderer, s.occluder, false);

        // Gone, and a new mesh's instance in its place: built in the frame
        // that first traces after it, and casting from that frame.
        nph_render_instance_destroy(f->renderer, s.occluder);
        nph_render_mesh_destroy(f->renderer, s.cube);
        s.cube = mesh_of(f, make_cube);
        s.occluder = instance(f, s.cube, s.grey, at(0.0, 1.0, 0.0, 1.0F));
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        stats = test_render_read(f, &v);
        NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 1U);
        memcpy(on, f->pixels, PIXELS);
        const Box again = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
        c = check_shadow(on, off, &cam, &l, &again);
        log_check("a new mesh", k_pipeline_names[k], &c);
        NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
        NTEST_EXPECT_GT_UINT(c.shadow, 100U);
    }
}

// The production path's top level stays as it was built while the camera
// moves within 16 m of where it was (the shadows right from the new
// viewpoint all the same), and is rebuilt around the camera beyond that,
// when an instance changes, after a frame that traced nothing, and after
// the reference drew a frame.
NTEST_F(Shadows, the_top_level_is_rebuilt_only_when_it_must_be)
{
    REQUIRE_RAY_TRACING(f);
    if (!nph_renderer_production_supported(f->renderer)) {
        NTEST_SKIP("no production path on this device");
    }
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    const Light l = {true, ld.direction, nph_vec3_zero()};
    const Box occluder = box_at(nph_vec3(0.0F, 1.0F, 0.0F), 1.0F);
    // Straight down from 6, 14 (8 m on), and 30 m (24 m on), the field of
    // view narrowed to keep the floor in frame.
    const f64 heights[3] = {6.0, 14.0, 30.0};
    const f32 fovs[3] = {0.9F, 0.45F, 0.2F};
    NphRenderView views[3];
    static u8 off[3][PIXELS];
    static u8 on[PIXELS];
    for (u32 i = 0; i < 3U; ++i) {
        const NphRenderCamera c =
            nph_render_camera_look_at(nph_dvec3(0.0, heights[i], 0.0), nph_dvec3(0.0, 0.0, 0.0),
                                      nph_vec3(0.0F, 0.0F, -1.0F), fovs[i], 0.05F);
        shadow_view(&views[i], c, 13.0F, NPH_RENDER_PIPELINE_PRODUCTION);
        render_into(f, &views[i], off[i]);
    }
    set_shadows(f, sun, &ld, true);
    // Built once, then kept.
    NphRenderFrameStats stats = test_render_read(f, &views[0]);
    NTEST_EXPECT_TRUE(stats.shadow_rebuild);
    stats = test_render_read(f, &views[0]);
    NTEST_EXPECT_FALSE(stats.shadow_rebuild);
    ShadowCheck c = check_shadow(f->pixels, off[0], &views[0].camera, &l, &occluder);
    log_check("built at 6 m", "production", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 100U);
    // 8 m on: the same structure, from a camera 8 m from its anchor.
    stats = test_render_read(f, &views[1]);
    NTEST_EXPECT_FALSE(stats.shadow_rebuild);
    memcpy(on, f->pixels, PIXELS);
    c = check_shadow(on, off[1], &views[1].camera, &l, &occluder);
    log_check("8 m from the anchor", "production", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 50U);
    // 24 m on: rebuilt around the camera, then kept.
    stats = test_render_read(f, &views[2]);
    NTEST_EXPECT_TRUE(stats.shadow_rebuild);
    stats = test_render_read(f, &views[2]);
    NTEST_EXPECT_FALSE(stats.shadow_rebuild);
    c = check_shadow(f->pixels, off[2], &views[2].camera, &l, &occluder);
    log_check("rebuilt at 30 m", "production", &c);
    NTEST_EXPECT_EQ_UINT(c.wrong, 0U);
    NTEST_EXPECT_GT_UINT(c.shadow, 50U);
    // An instance changed: rebuilt.
    nph_render_instance_set_hidden(f->renderer, s.occluder, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    NTEST_EXPECT_LE_UINT(image_max_diff(f->pixels, off[2]), SAME_TOL);
    nph_render_instance_set_hidden(f->renderer, s.occluder, false);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    NTEST_EXPECT_FALSE(test_render_read(f, &views[2]).shadow_rebuild);
    // A frame without shadows, then one with: rebuilt (the instances may
    // have changed unseen).
    set_shadows(f, sun, &ld, false);
    NTEST_EXPECT_FALSE(test_render_read(f, &views[2]).shadow_rebuild);
    set_shadows(f, sun, &ld, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    // The reference rebuilds every frame; the production path after it.
    NphRenderView ref = views[2];
    ref.pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    NTEST_EXPECT_TRUE(test_render_read(f, &ref).shadow_rebuild);
    NTEST_EXPECT_TRUE(test_render_read(f, &ref).shadow_rebuild);
    NTEST_EXPECT_TRUE(test_render_read(f, &views[2]).shadow_rebuild);
    NTEST_EXPECT_FALSE(test_render_read(f, &views[2]).shadow_rebuild);
}

// Shadow rays are traced while some light casts shadows, and only then.
NTEST_F(Shadows, rays_are_traced_while_a_light_casts_shadows)
{
    REQUIRE_RAY_TRACING(f);
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderView v;
    shadow_view(&v, top_camera(), 13.0F, NPH_RENDER_PIPELINE_REFERENCE);
    NphRenderLightDesc a_desc = sun_desc(true);
    NphRenderLightDesc b_desc = point_desc(nph_dvec3(0.0, 3.0, 0.0), true);
    const NphRenderLight a = make_light(f, &a_desc);
    const NphRenderLight b = make_light(f, &b_desc);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rays);
    nph_render_light_destroy(f->renderer, a);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rays);
    set_shadows(f, b, &b_desc, false);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rays);
    // An update that keeps the shadows on counts the light once.
    set_shadows(f, b, &b_desc, true);
    set_shadows(f, b, &b_desc, true);
    NTEST_EXPECT_TRUE(test_render_read(f, &v).shadow_rays);
    nph_render_light_destroy(f->renderer, b);
    NTEST_EXPECT_FALSE(test_render_read(f, &v).shadow_rays);
}

// Without ray tracing (turned off here; absent on some devices) a light
// that casts shadows draws exactly as one that does not.
NTEST(shadows, a_renderer_without_ray_tracing_draws_them_unshadowed)
{
    NphRendererDesc d;
    test_renderer_desc(&d, NULL);
    d.disable_ray_tracing = 1;
    RenderFixture fx;
    RenderFixture *f = &fx;
    test_render_setup(f, &d);
    if (f->dev == NULL) {
        test_render_teardown(f);
        NTEST_SKIP("no Vulkan device available");
    }
    NTEST_EXPECT_FALSE(nph_renderer_ray_tracing_supported(f->renderer));
    Scene s;
    scene_build(f, &s, at(0.0, 1.0, 0.0, 1.0F));
    NphRenderLightDesc ld = sun_desc(false);
    const NphRenderLight sun = make_light(f, &ld);
    static u8 off[PIXELS];
    static u8 on[PIXELS];
    for (u32 k = 0; k < pipeline_count(f); ++k) {
        NphRenderView v;
        shadow_view(&v, top_camera(), 13.0F, k_pipelines[k]);
        set_shadows(f, sun, &ld, false);
        render_into(f, &v, off);
        set_shadows(f, sun, &ld, true);
        const NphRenderFrameStats stats = render_into(f, &v, on);
        NTEST_EXPECT_FALSE(stats.shadow_rays);
        NTEST_EXPECT_EQ_UINT(stats.shadow_builds, 0U);
        NTEST_EXPECT_LE_UINT(image_max_diff(on, off), SAME_TOL);
    }
    test_render_teardown(f);
}
