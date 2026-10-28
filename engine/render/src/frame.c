// SPDX-License-Identifier: Apache-2.0
// One frame of the renderer (render/renderer.h nph_render_frame;
// docs/render.md "A frame"):
//
//   1. the frame's records go into its slot of the per-frame upload
//      buffer: the view, the materials (when they changed since that slot
//      was last filled), the reference's lights, and the draw records the
//      CPU draws itself, every position made camera relative in double
//      precision (the production path keeps its lights in a GPU table and
//      sends only the changed ones, vis_scene.c);
//   2. the draw calls are sorted: opaque and masked ones by pipeline and
//      mesh, blended ones back to front;
//   3. the passes are declared: the image-based-lighting passes when
//      needed (ibl.c), then either the reference renderer's forward pass
//      into a pre-exposed HDR target or the production path's culling,
//      raster, resolve, and transparent passes (vis.c), bloom and the
//      metering (post.c), and the tonemap pass into the caller's target
//      with the debug lines; or on a production view with temporal
//      anti-aliasing the temporal resolve (taa.c), which tonemaps too, and
//      with motion blur the blur (motion_blur.c), which then tonemaps
//      instead.
//
// The reference draws every visible submesh itself; the production path
// leaves opaque and masked surfaces to the GPU and draws only the blended
// ones, which it finds through the blend list (vis_scene.c), so its CPU
// cost does not grow with the number of objects.
#include <nepheline/core/log.h>
#include <nepheline/core/profile.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>

#include "render_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

NPH_STATIC_ASSERT(sizeof(NphRenderViewData) == 448U, "NphRenderViewData matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderDrawData) == 160U, "NphRenderDrawData matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderMaterialData) == 112U,
                  "NphRenderMaterialData matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderLightData) == 64U, "NphRenderLightData matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphRenderLightGrid) == 64U, "NphRenderLightGrid matches its GLSL layout");
NPH_STATIC_ASSERT(offsetof(NphRenderViewData, light_grid) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(offsetof(NphRenderViewData, tlas) % 8U == 0U, "addresses are aligned");
NPH_STATIC_ASSERT(sizeof(NphRenderPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(sizeof(NphRenderIblPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");
NPH_STATIC_ASSERT(sizeof(NphRenderTonemapPush) <= NPH_GPU_PUSH_CONSTANT_BYTES,
                  "push constants fit");
NPH_STATIC_ASSERT((u32)NPH_RENDER_TONEMAPPER_AGX == NPH_RENDER_TONEMAP_AGX,
                  "the tonemapper enums agree with the shader's");
NPH_STATIC_ASSERT((u32)NPH_RENDER_VIEW_COUNT == NPH_RENDER_DEBUG_COUNT,
                  "the debug views agree with the shader's");
NPH_STATIC_ASSERT((u32)NPH_RENDER_VIEW_CLUSTERS == NPH_RENDER_DEBUG_CLUSTERS,
                  "the production debug views agree with the shader's");
NPH_STATIC_ASSERT((u32)NPH_RENDER_SLOT_COUNT == NPH_RENDER_TEX_COUNT,
                  "the texture slots agree with the shader's");
NPH_STATIC_ASSERT((u32)NPH_RENDER_VIS_OVERFLOW_MESHLETS == NPH_VIS_OVERFLOW_MESHLETS &&
                      (u32)NPH_RENDER_VIS_OVERFLOW_CLUSTERS == NPH_VIS_OVERFLOW_CLUSTERS &&
                      (u32)NPH_RENDER_VIS_OVERFLOW_DEFERRED == NPH_VIS_OVERFLOW_DEFERRED,
                  "the overflow bits agree with the shaders'");

#define FRAME_ALIGN 256U

static u64 align_up(u64 v, u64 a)
{
    return (v + a - 1U) & ~(a - 1U);
}

void render_frame_layout(const NphRendererDesc *desc, bool ray_tracing, RenderFrameLayout *out)
{
    u64 at = 0U;
    out->view_offset = at;
    at = align_up(at + sizeof(NphRenderViewData), FRAME_ALIGN);
    out->sky_offset = at;
    at = align_up(at + sizeof(NphRenderSkyData), FRAME_ALIGN);
    out->lights_offset = at;
    at = align_up(at + (u64)desc->max_lights * sizeof(NphRenderLightData), FRAME_ALIGN);
    out->materials_offset = at;
    at =
        align_up(at + ((u64)desc->max_materials + 1U) * sizeof(NphRenderMaterialData), FRAME_ALIGN);
    out->draws_offset = at;
    at = align_up(at + (u64)desc->max_draws * sizeof(NphRenderDrawData), FRAME_ALIGN);
    out->vis_view_offset = at;
    at = align_up(at + sizeof(NphVisViewData), FRAME_ALIGN);
    out->vis_updates_offset = at;
    at = align_up(at + (u64)desc->max_instance_updates * sizeof(NphVisInstanceUpdate), FRAME_ALIGN);
    out->vis_light_updates_offset = at;
    at = align_up(at + (u64)desc->max_lights * sizeof(NphVisLightUpdate), FRAME_ALIGN);
    out->light_grid_offset = at;
    at = align_up(at + sizeof(NphRenderLightGrid), FRAME_ALIGN);
    out->rt_instances_offset = at;
    if (ray_tracing) {
        at = align_up(at + (u64)desc->max_instances * sizeof(NphGpuAccelInstance), FRAME_ALIGN);
    }
    out->vsm_view_offset = at;
    at = align_up(at + sizeof(NphVsmView), FRAME_ALIGN);
    out->gi_view_offset = at;
    at = align_up(at + sizeof(NphRenderGiView), FRAME_ALIGN);
    out->size = at;
}

// ---------------------------------------------------------------------------
// Views and cameras
// ---------------------------------------------------------------------------

f32 nph_render_exposure_from_ev100(f32 ev100)
{
    // The saturation-based exposure of a camera with ISO 100 and a lens
    // transmittance of 0.65 (Lagarde & de Rousiers 2014, §5.1).
    return 1.0F / (1.2F * nph_f32_pow(2.0F, ev100));
}

NphRenderCamera nph_render_camera_look_at(NphDVec3 eye, NphDVec3 target, NphVec3 up, f32 yfov,
                                          f32 znear)
{
    NphRenderCamera c;
    c.position = eye;
    c.yfov = yfov;
    c.znear = znear;
    const NphVec3 forward =
        nph_vec3_normalize_or(nph_dvec3_to_vec3(nph_dvec3_sub(target, eye)), nph_vec3(0, 0, -1));
    NphVec3 right = nph_vec3_cross(forward, up);
    if (nph_vec3_length_sq(right) < 1e-12F) {
        right = nph_vec3_perpendicular(forward);
    }
    right = nph_vec3_normalize(right);
    const NphVec3 true_up = nph_vec3_cross(right, forward);
    // The camera looks down its -Z: its basis is (right, up, -forward).
    c.orientation = nph_quat_normalize(
        nph_quat_from_mat3(nph_mat3_from_cols(right, true_up, nph_vec3_neg(forward))));
    return c;
}

void nph_render_view_default(NphRenderView *view, NphRenderCamera camera)
{
    NPH_ASSERT(view != NULL);
    memset(view, 0, sizeof *view);
    view->camera = camera;
    view->ev100 = 15.0F;
    view->tonemapper = NPH_RENDER_TONEMAPPER_ACES;
    view->background = NPH_RENDER_BACKGROUND_ENV;
    view->pipeline = NPH_RENDER_PIPELINE_REFERENCE;
    view->lod_error = 1.0F;
}

// ---------------------------------------------------------------------------
// Target pipelines
// ---------------------------------------------------------------------------

static bool target_format_supported(NphGpuFormat f)
{
    return f == NPH_GPU_FORMAT_R8G8B8A8_UNORM || f == NPH_GPU_FORMAT_R8G8B8A8_SRGB ||
           f == NPH_GPU_FORMAT_B8G8R8A8_UNORM || f == NPH_GPU_FORMAT_B8G8R8A8_SRGB ||
           f == NPH_GPU_FORMAT_A2B10G10R10_UNORM;
}

static NphResult create_target_pipelines(NphRenderer *r, RenderTargetPipelines *t,
                                         NphGpuFormat format)
{
    NphGpuGraphicsPipelineDesc d;
    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_FULLSCREEN_VERT];
    d.fragment = r->shaders[RENDER_SHADER_TONEMAP_FRAG];
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.depth_format = RENDER_DEPTH_FORMAT; // the scene depth is attached read-only for the lines
    d.name = "tonemap";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->tonemap));

    memset(&d, 0, sizeof d);
    d.vertex = r->shaders[RENDER_SHADER_DEBUG_VERT];
    d.fragment = r->shaders[RENDER_SHADER_DEBUG_FRAG];
    d.topology = NPH_GPU_TOPOLOGY_LINE_LIST;
    d.color_formats[0] = format;
    d.color_count = 1U;
    d.blend[0].enable = 1;
    d.blend[0].src_color = NPH_GPU_BLEND_SRC_ALPHA;
    d.blend[0].dst_color = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
    d.blend[0].src_alpha = NPH_GPU_BLEND_ONE;
    d.blend[0].dst_alpha = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
    d.depth_format = RENDER_DEPTH_FORMAT;
    d.depth.test = 1;
    d.depth.compare = NPH_GPU_COMPARE_GREATER_OR_EQUAL;
    d.name = "debug lines";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->debug_depth));
    d.depth.test = 0;
    d.name = "debug lines (overlay)";
    NPH_TRY(nph_gpu_pipeline_create_graphics(r->dev, &d, &t->debug_overlay));
    if (r->vis.supported) {
        NPH_TRY(render_vis_target_pipelines(r, t, format));
    }
    t->format = format;
    return NPH_RESULT_OK;
}

// The pipelines that write a target of `format`, created the first time
// the format is seen (pipeline creation takes milliseconds, once).
static NphResult target_pipelines(NphRenderer *r, NphGpuFormat format,
                                  const RenderTargetPipelines **out)
{
    for (u32 i = 0; i < RENDER_TARGET_FORMATS; ++i) {
        if (r->target_pipelines[i].format == format) {
            *out = &r->target_pipelines[i];
            return NPH_RESULT_OK;
        }
    }
    for (u32 i = 0; i < RENDER_TARGET_FORMATS; ++i) {
        RenderTargetPipelines *t = &r->target_pipelines[i];
        if (t->format == NPH_GPU_FORMAT_UNDEFINED) {
            const NphResult res = create_target_pipelines(r, t, format);
            if (res != NPH_RESULT_OK) {
                nph_gpu_pipeline_destroy(r->dev, t->tonemap);
                nph_gpu_pipeline_destroy(r->dev, t->debug_depth);
                nph_gpu_pipeline_destroy(r->dev, t->debug_overlay);
                nph_gpu_pipeline_destroy(r->dev, t->vis_tonemap);
                nph_gpu_pipeline_destroy(r->dev, t->vis_debug_depth);
                nph_gpu_pipeline_destroy(r->dev, t->vis_debug_overlay);
                nph_gpu_pipeline_destroy(r->dev, t->taa);
                nph_gpu_pipeline_destroy(r->dev, t->motion_blur);
                memset(t, 0, sizeof *t);
                return res;
            }
            *out = t;
            return NPH_RESULT_OK;
        }
    }
    return NPH_RESULT_FULL;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

static void fill_material(const NphRenderer *r, const NphRenderMaterialDesc *d, u32 handle,
                          NphRenderMaterialData *out)
{
    memset(out, 0, sizeof *out);
    out->base_color = d->base_color;
    out->emissive = nph_vec4_from_vec3(d->emissive, 0.0F);
    out->metallic = d->metallic;
    out->roughness = d->roughness;
    out->alpha_cutoff = d->alpha_cutoff;
    out->normal_scale = d->normal_scale;
    out->occlusion_strength = d->occlusion_strength;
    out->alpha_mode = (u32)d->alpha_mode;
    out->handle = handle;
    out->flags = (d->double_sided ? (u32)NPH_RENDER_MATERIAL_DOUBLE_SIDED : 0U) |
                 (d->unlit ? (u32)NPH_RENDER_MATERIAL_UNLIT : 0U);
    for (u32 s = 0; s < (u32)NPH_RENDER_SLOT_COUNT; ++s) {
        const NphRenderTextureBinding *b = &d->textures[s];
        const RenderTexture *t = render_texture_get(r, b->texture);
        out->textures[s] = NPH_RENDER_NO_TEXTURE;
        out->samplers[s] = 0U;
        if (t != NULL && t->kind == NPH_RENDER_TEXTURE_2D) {
            out->textures[s] = nph_gpu_image_sampled_index(r->dev, t->image);
            out->samplers[s] = render_material_sampler(r, b);
            if (s == (u32)NPH_RENDER_SLOT_NORMAL &&
                (t->flags & (u32)NPH_RENDER_TEXTURE_FLAG_NORMAL_TWO_CHANNEL) != 0U) {
                out->flags |= (u32)NPH_RENDER_MATERIAL_NORMAL_TWO_CHANNEL;
            }
        }
    }
}

// The material table: one record per pool slot, then the default
// material at index max_materials. Free slots are written as zero: the
// production path checks a record's handle against the material map's,
// so a destroyed material's record must stop matching.
static void write_materials(NphRenderer *r)
{
    if (r->material_uploaded[r->frame_slot] == r->material_version) {
        return;
    }
    NphRenderMaterialData *table = render_frame_at(r, r->layout.materials_offset);
    for (u32 i = 0; i < r->desc.max_materials; ++i) {
        if (r->materials[i].handle != 0U) {
            fill_material(r, &r->materials[i].desc, r->materials[i].handle, &table[i]);
        } else {
            memset(&table[i], 0, sizeof table[i]);
        }
    }
    NphRenderMaterialDesc def;
    nph_render_material_desc_default(&def);
    fill_material(r, &def, 0U, &table[r->desc.max_materials]);
    r->material_uploaded[r->frame_slot] = r->material_version;
}

void render_light_data(const NphRenderLightDesc *d, NphVec3 position, NphRenderLightData *out)
{
    memset(out, 0, sizeof *out);
    const f32 inv_range_sq = d->range > 0.0F ? 1.0F / (d->range * d->range) : 0.0F;
    out->position = nph_vec4_from_vec3(position, inv_range_sq);
    const NphVec3 dir = d->type == NPH_RENDER_LIGHT_TYPE_POINT ? nph_vec3(0.0F, -1.0F, 0.0F)
                                                               : nph_vec3_normalize(d->direction);
    out->direction = nph_vec4_from_vec3(dir, (f32)d->type);
    out->color =
        nph_vec4_from_vec3(nph_vec3_scale(d->color, d->intensity), d->shadows ? 1.0F : 0.0F);
    if (d->type == NPH_RENDER_LIGHT_TYPE_SPOT) {
        // KHR_lights_punctual's angular attenuation as a scale and an
        // offset on cos(angle); the outer angle's cosine and sine bound the
        // cone for the production path's culling (vis_light_sphere).
        const f32 cos_outer = nph_f32_cos(d->outer_cone);
        const f32 cos_inner = nph_f32_cos(d->inner_cone);
        const f32 scale = 1.0F / nph_f32_max(cos_inner - cos_outer, 1e-4F);
        out->spot = nph_vec4(scale, -cos_outer * scale, cos_outer, nph_f32_sin(d->outer_cone));
    }
}

static u32 write_lights(NphRenderer *r, NphDVec3 camera)
{
    NphRenderLightData *out = render_frame_at(r, r->layout.lights_offset);
    u32 n = 0U;
    for (u32 i = 0; i < r->light_pool.bump; ++i) {
        const RenderLight *l = &r->lights[i];
        if (l->handle == 0U) {
            continue;
        }
        render_light_data(&l->desc, nph_dvec3_relative(l->desc.position, camera), &out[n++]);
    }
    return n;
}

static u32 cull_of(bool double_sided, bool mirrored)
{
    if (double_sided) {
        // Nothing is culled, but the front face still decides which side
        // the shading normal takes.
        return mirrored ? (u32)RENDER_CULL_NONE_CW : (u32)RENDER_CULL_NONE;
    }
    return mirrored ? (u32)RENDER_CULL_BACK_CW : (u32)RENDER_CULL_BACK_CCW;
}

static int compare_draws(const void *a, const void *b)
{
    const RenderDrawCmd *x = (const RenderDrawCmd *)a;
    const RenderDrawCmd *y = (const RenderDrawCmd *)b;
    if (x->key != y->key) {
        return x->key < y->key ? -1 : 1;
    }
    // Keys are unique (they end in the record index); kept for qsort's
    // contract.
    return x->record < y->record ? -1 : (x->record > y->record ? 1 : 0);
}

// Blended draws sort far to near, after every opaque draw: bit 63 is set,
// bits 31-62 hold the distance along the view axis made order-preserving
// for unsigned compares and inverted (far first), and the low bits the
// record (below 2^24, renderer.c).
static u64 depth_key(f32 view_depth, u32 record)
{
    u32 bits = 0U;
    const f32 d = -view_depth; // view space looks down -Z: larger is farther
    memcpy(&bits, &d, sizeof bits);
    bits = (bits & 0x80000000U) != 0U ? ~bits : bits | 0x80000000U; // ascending with d
    return (1ULL << 63U) | ((u64)(~bits) << 31U) | record;
}

typedef struct FrameCull {
    NphFrustum frustum;
    NphMat4 view;
    NphDVec3 camera;
} FrameCull;

// What one frame's draw list holds so far.
typedef struct DrawList {
    u32 count;
    u32 blend_count;
} DrawList;

// The draw records and commands of instance `i`'s submeshes: every one for
// the reference, the blended ones only for the production path.
static void add_instance_draws(NphRenderer *r, const FrameCull *fc, u32 i, bool blend_only,
                               DrawList *list, NphRenderFrameStats *stats)
{
    NphRenderDrawData *records = render_frame_at(r, r->layout.draws_offset);
    const u32 cap = r->desc.max_draws;
    const u32 default_material = r->desc.max_materials;
    const RenderInstance *inst = &r->instances[i];
    const NphHandle mh = {inst->mesh.value};
    const RenderMesh *mesh = &r->meshes[nph_handle_pool_index(&r->mesh_pool, mh)];
    // Camera-relative transform: the 64-bit translation minus the camera,
    // rounded to 32 bits only after the subtraction.
    const NphVec3 t = nph_dvec3_relative(inst->transform.translation, fc->camera);
    const NphMat3 lin = inst->transform.linear;
    const NphAffine model = nph_affine_from_mat3_translation(lin, t);
    if (inst->hidden) {
        // The production path counts hidden instances without visiting them.
        stats->culled += blend_only ? 0U : 1U;
        return;
    }
    if (mesh->index_count == 0U) {
        // A mesh of voxels alone (a cell's proxy; ADR-0062): no triangles for
        // these draws. The production path draws its voxels; the reference
        // draws the cell's instances.
        return;
    }
    if (!nph_frustum_intersects_aabb(&fc->frustum, nph_aabb_transform(mesh->bounds, model))) {
        stats->culled += 1U;
        return;
    }
    NphMat3 inv = nph_mat3_identity();
    NPH_IGNORE_RESULT(nph_mat3_inverse(lin, &inv)); // invertible: checked at creation
    const NphMat3 normal = nph_mat3_transpose(inv);
    const bool mirrored = nph_mat3_determinant(lin) < 0.0F;
    const NphVec3 center = nph_aabb_center(nph_aabb_transform(mesh->bounds, model));
    const f32 view_depth = nph_mat4_transform_point(fc->view, center).z;
    for (u32 s = 0; s < mesh->submesh_count; ++s) {
        const NphRenderSubmesh *sm = &mesh->submeshes[s];
        u32 material_index = default_material;
        const NphRenderMaterialDesc *md = NULL;
        if (sm->material_slot < inst->material_count) {
            const NphRenderMaterial mat = inst->materials[sm->material_slot];
            const NphHandle h = {mat.value};
            if (mat.value != 0U && nph_handle_pool_is_valid(&r->material_pool, h)) {
                material_index = nph_handle_pool_index(&r->material_pool, h);
                md = &r->materials[material_index].desc;
            }
        }
        const u32 alpha = md != NULL ? (u32)md->alpha_mode : (u32)NPH_RENDER_ALPHA_OPAQUE;
        if (blend_only && alpha != (u32)NPH_RENDER_ALPHA_BLEND) {
            continue;
        }
        const bool double_sided = md != NULL && md->double_sided;
        if (list->count == cap) {
            stats->dropped += 1U;
            continue;
        }
        const u32 rec = list->count;
        NphRenderDrawData *d = &records[rec];
        d->model0 = model.r[0];
        d->model1 = model.r[1];
        d->model2 = model.r[2];
        d->normal0 = nph_vec4_from_vec3(nph_mat3_row(normal, 0U), 0.0F);
        d->normal1 = nph_vec4_from_vec3(nph_mat3_row(normal, 1U), 0.0F);
        d->normal2 = nph_vec4_from_vec3(nph_mat3_row(normal, 2U), 0.0F);
        d->position_offset = nph_vec4_from_vec3(mesh->position_offset, 0.0F);
        d->position_scale = nph_vec4_from_vec3(mesh->position_scale, mirrored ? -1.0F : 1.0F);
        d->positions_address = mesh->positions_address;
        d->attribs_address = mesh->attribs_address;
        d->material_index = material_index;
        d->instance_index = i;
        d->pad0 = 0U;
        d->pad1 = 0U;
        RenderDrawCmd *c = &r->draw_cmds[rec];
        memset(c, 0, sizeof *c);
        c->record = rec;
        c->mesh = nph_handle_pool_index(&r->mesh_pool, mh);
        c->first_index = sm->index_offset;
        c->index_count = sm->index_count;
        c->alpha_mode = (u8)alpha;
        c->cull = (u8)cull_of(double_sided, mirrored);
        if (alpha == (u32)NPH_RENDER_ALPHA_BLEND) {
            c->key = depth_key(view_depth, rec);
            list->blend_count += 1U;
        } else {
            // Pipeline (alpha mode, culling), then mesh, then record.
            c->key = ((u64)alpha << 61U) | ((u64)c->cull << 59U) | ((u64)c->mesh << 32U) | rec;
        }
        stats->triangles += sm->index_count / 3U;
        list->count += 1U;
    }
}

// The draw list: every instance's submeshes (the reference), or only the
// blended submeshes of the instances on the blend list (the production
// path, which leaves the rest to the GPU).
static void build_draws(NphRenderer *r, const FrameCull *fc, bool production,
                        NphRenderFrameStats *stats)
{
    DrawList list = {0U, 0U};
    if (production) {
        stats->instances = nph_handle_pool_live(&r->instance_pool);
        stats->culled = r->hidden_count;
        u32 blended = 0U;
        const u32 *blend = render_vis_blend_list(r, &blended);
        for (u32 i = 0; i < blended; ++i) {
            add_instance_draws(r, fc, blend[i], true, &list, stats);
        }
    } else {
        for (u32 i = 0; i < r->desc.max_instances; ++i) {
            if (r->instances[i].handle != 0U) {
                stats->instances += 1U;
                add_instance_draws(r, fc, i, false, &list, stats);
            }
        }
    }
    // Blended keys have the top bit set, so one sort puts them last and
    // farthest first.
    qsort(r->draw_cmds, list.count, sizeof(RenderDrawCmd), compare_draws);
    r->opaque_count = list.count - list.blend_count;
    r->blend_count = list.blend_count;
    stats->draws = list.count;
    stats->blended = list.blend_count;
}

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

// Records draw_cmds[first, first + count) with the forward pipelines, or
// with `only` where it is not null. `visbuffer` is the visibility buffer's
// sampled index, whose depth the production path's blended surfaces test
// (render_vis_visbuffer_index; 0 on the reference path).
static void record_draws(NphRenderer *r, NphGpuCmd *cmd, u32 first, u32 count, NphGpuPipeline only,
                         u32 visbuffer, NphGpuPipeline *bound, u32 *bound_mesh)
{
    NphRenderPush push = r->push;
    push.visbuffer_index = visbuffer;
    // The production path's blended surfaces draw into its own HDR format;
    // a frame that traces shadow rays, or has the sun's virtual shadow map,
    // uses the pipelines that do.
    const bool production = r->frame_production != 0;
    const u32 rt = r->rt.frame_shadows != 0 ? 1U : 0U;
    const u32 vsm = r->vsm.frame_active != 0 ? 1U : 0U;
    for (u32 i = first; i < first + count; ++i) {
        const RenderDrawCmd *c = &r->draw_cmds[i];
        const NphGpuPipeline p = only.value != 0U ? only
                                 : production && c->alpha_mode == (u8)NPH_RENDER_ALPHA_BLEND
                                     ? r->vis.blend_pipelines[rt][vsm][c->cull]
                                     : (rt != 0U ? r->forward_rt[c->alpha_mode][c->cull]
                                                 : r->forward[c->alpha_mode][c->cull]);
        if (p.value != bound->value) {
            nph_gpu_cmd_bind_pipeline(cmd, p);
            *bound = p;
        }
        if (c->mesh != *bound_mesh) {
            nph_gpu_cmd_bind_index_buffer(cmd, r->meshes[c->mesh].indices, 0U, NPH_GPU_INDEX_U32);
            *bound_mesh = c->mesh;
        }
        push.draw_index = c->record;
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw_indexed(cmd, c->index_count, 1U, c->first_index, 0, 0U);
    }
}

static void forward_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    NphRenderer *r = (NphRenderer *)user;
    NphGpuPipeline bound = NPH_GPU_PIPELINE_NULL;
    u32 bound_mesh = NPH_U32_MAX;
    record_draws(r, cmd, 0U, r->opaque_count, NPH_GPU_PIPELINE_NULL, 0U, &bound, &bound_mesh);
    // The sky fills what the opaque geometry left uncovered, before the
    // blended surfaces composite over both.
    nph_gpu_cmd_bind_pipeline(cmd, r->sky);
    nph_gpu_cmd_push_constants(cmd, &r->push, (u32)sizeof r->push);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
    bound = r->sky;
    record_draws(r, cmd, r->opaque_count, r->blend_count, NPH_GPU_PIPELINE_NULL, 0U, &bound,
                 &bound_mesh);
}

// The production path's blended surfaces, over the resolved image.
static void transparent_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    NphGpuPipeline bound = NPH_GPU_PIPELINE_NULL;
    u32 bound_mesh = NPH_U32_MAX;
    record_draws(r, cmd, r->opaque_count, r->blend_count, NPH_GPU_PIPELINE_NULL,
                 render_vis_visbuffer_index(r, graph), &bound, &bound_mesh);
}

// The production path's blended surfaces again, asking for the pages of
// the sun's virtual shadow map their shading will read (vsm.c): rendering
// with no attachments, over the frame's extent.
static void blend_requests_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    NphGpuRenderingDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.width = r->vis.frame.width;
    rd.height = r->vis.frame.height;
    nph_gpu_cmd_begin_rendering(cmd, &rd);
    NphGpuPipeline bound = NPH_GPU_PIPELINE_NULL;
    u32 bound_mesh = NPH_U32_MAX;
    record_draws(r, cmd, r->opaque_count, r->blend_count, r->vsm.request,
                 render_vis_visbuffer_index(r, graph), &bound, &bound_mesh);
    nph_gpu_cmd_end_rendering(cmd);
}

static void tonemap_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderTargetPipelines *t = r->frame_target;
    const bool production = r->frame_production != 0;
    NphRenderTonemapPush tp = r->tonemap_push;
    tp.hdr_index = nph_gpu_image_sampled_index(r->dev, nph_gpu_rg_image(graph, r->hdr_rg));
    const RenderPostComposite bloom = render_post_composite(r, graph);
    tp.bloom_index = bloom.bloom_index;
    tp.bloom = bloom.bloom;
    tp.bloom_scale = bloom.bloom_scale;
    tp.sampler_index = bloom.sampler_index;
    tp.bloom_uv = bloom.bloom_uv;
    nph_gpu_cmd_bind_pipeline(cmd, production ? t->vis_tonemap : t->tonemap);
    nph_gpu_cmd_push_constants(cmd, &tp, (u32)sizeof tp);
    nph_gpu_cmd_draw(cmd, 3U, 1U, 0U, 0U);
    NphRenderPush push = r->push;
    push.flags = tp.encode_srgb != 0U ? 0U : (u32)NPH_RENDER_PUSH_LINEAR_OUTPUT;
    if (production && r->debug_draw_counts[0] != 0U) {
        push.visbuffer_index = render_vis_visbuffer_index(r, graph);
    }
    if (r->debug_draw_counts[0] != 0U) {
        nph_gpu_cmd_bind_pipeline(cmd, production ? t->vis_debug_depth : t->debug_depth);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw(cmd, r->debug_draw_counts[0] * 2U, 1U, 0U, 0U);
    }
    if (r->debug_draw_counts[1] != 0U) {
        nph_gpu_cmd_bind_pipeline(cmd, production ? t->vis_debug_overlay : t->debug_overlay);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw(cmd, r->debug_draw_counts[1] * 2U, 1U, r->desc.debug_lines * 2U, 0U);
    }
}

// The production path's debug lines over its final image, when another pass
// than the tonemap's wrote it (render/debug_draw.h): the depth-tested ones
// against the visibility buffer, then the overlay.
static void lines_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NphRenderer *r = (NphRenderer *)user;
    const RenderTargetPipelines *t = r->frame_target;
    NphRenderPush push = r->push;
    push.flags = r->tonemap_push.encode_srgb != 0U ? 0U : (u32)NPH_RENDER_PUSH_LINEAR_OUTPUT;
    if (r->debug_draw_counts[0] != 0U) {
        push.visbuffer_index = render_vis_visbuffer_index(r, graph);
        nph_gpu_cmd_bind_pipeline(cmd, t->vis_debug_depth);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw(cmd, r->debug_draw_counts[0] * 2U, 1U, 0U, 0U);
    }
    if (r->debug_draw_counts[1] != 0U) {
        nph_gpu_cmd_bind_pipeline(cmd, t->vis_debug_overlay);
        nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
        nph_gpu_cmd_draw(cmd, r->debug_draw_counts[1] * 2U, 1U, r->desc.debug_lines * 2U, 0U);
    }
}

static NphResult declare_lines(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage out)
{
    if (r->debug_draw_counts[0] == 0U && r->debug_draw_counts[1] == 0U) {
        return NPH_RESULT_OK;
    }
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = "debug lines";
    pd.type = NPH_GPU_RG_PASS_GRAPHICS;
    pd.execute = lines_execute;
    pd.user = r;
    NphGpuRgPass *p = nph_gpu_rg_add_pass(graph, &pd);
    if (p == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_LOAD;
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_color_attachment(p, out, &a);
    if (r->debug_draw_counts[0] != 0U) {
        nph_gpu_rg_read_image(p, r->vis.frame.visbuffer, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    }
    return NPH_RESULT_OK;
}

static bool format_is_srgb(NphGpuFormat f)
{
    return nph_gpu_format_info(f)->srgb != 0;
}

// The reference renderer's passes: the forward pass into the HDR target
// and its depth, which the tonemap pass's debug lines test against. With
// depth of field the depth is also sampled (dof.c).
static NphResult declare_reference(NphRenderer *r, NphGpuRenderGraph *graph, u32 width, u32 height,
                                   bool dof, NphGpuRgImage env, NphGpuRgImage specular,
                                   NphGpuRgImage lut, NphGpuRgBuffer sh, NphGpuRgImage *out_depth)
{
    NphGpuImageDesc hdr_desc;
    memset(&hdr_desc, 0, sizeof hdr_desc);
    hdr_desc.format = RENDER_HDR_FORMAT;
    hdr_desc.width = width;
    hdr_desc.height = height;
    hdr_desc.mip_count = 1U;
    hdr_desc.usage = (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT | (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    hdr_desc.name = "hdr colour";
    NphGpuImageDesc depth_desc = hdr_desc;
    depth_desc.format = RENDER_DEPTH_FORMAT;
    depth_desc.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
    depth_desc.name = "depth";
    if (dof) {
        depth_desc.usage |= (u32)NPH_GPU_IMAGE_USAGE_SAMPLED;
    }
    const NphGpuRgImage hdr = nph_gpu_rg_create_image(graph, &hdr_desc);
    const NphGpuRgImage depth = nph_gpu_rg_create_image(graph, &depth_desc);
    if (nph_gpu_rg_image_is_null(hdr) || nph_gpu_rg_image_is_null(depth)) {
        return NPH_RESULT_FULL;
    }
    r->hdr_rg = hdr;
    *out_depth = depth;

    // Shadow rays: this frame's builds, from the records
    // render_rt_write_instances wrote, before the pass that traces.
    NphGpuRgBuffer tlas = NPH_GPU_RG_BUFFER_NULL;
    if (r->rt.frame_active) {
        NPH_TRY(render_rt_declare(r, graph, NPH_GPU_RG_BUFFER_NULL, NPH_GPU_RG_BUFFER_NULL,
                                  NPH_GPU_RG_BUFFER_NULL, &tlas));
    }

    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = "forward";
    pd.type = NPH_GPU_RG_PASS_GRAPHICS;
    pd.execute = forward_execute;
    pd.user = r;
    NphGpuRgPass *fwd = nph_gpu_rg_add_pass(graph, &pd);
    if (fwd == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_CLEAR;
    a.store = NPH_GPU_STORE_OP_STORE;
    a.clear = nph_gpu_clear_color(0.0F, 0.0F, 0.0F, 1.0F);
    nph_gpu_rg_color_attachment(fwd, hdr, &a);
    a.clear = nph_gpu_clear_depth(0.0F, 0U); // reverse-Z: the far plane is 0
    nph_gpu_rg_depth_attachment(fwd, depth, &a, 0);
    nph_gpu_rg_read_image(fwd, env, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(fwd, specular, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
    nph_gpu_rg_read_image(fwd, lut, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    nph_gpu_rg_read_buffer(fwd, sh, NPH_GPU_RG_USE_SHADER_READ);
    if (r->rt.frame_active) {
        nph_gpu_rg_read_buffer(fwd, tlas, NPH_GPU_RG_USE_ACCEL_READ);
    }
    return NPH_RESULT_OK;
}

static NphResult render_frame(NphRenderer *r, const NphRenderView *view, NphGpuRenderGraph *graph,
                              NphGpuImage target, NphGpuRgImage *out_target,
                              NphRenderFrameStats *out_stats)
{
    NPH_ASSERT(r != NULL && view != NULL && graph != NULL);
    NphRenderFrameStats stats;
    memset(&stats, 0, sizeof stats);
    if (out_target != NULL) {
        *out_target = NPH_GPU_RG_IMAGE_NULL;
    }
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    NphGpuDevice *dev = r->dev;
    if (!nph_gpu_frame_is_active(dev) || r->last_frame_index == nph_gpu_frame_index(dev)) {
        NPH_LOG_ERROR(&NPH_LOG_RENDER, "%s: nph_render_frame needs an active frame, once", r->name);
        return NPH_RESULT_INVALID_STATE;
    }
    if (!nph_gpu_image_is_valid(dev, target) || !(view->camera.yfov > 0.0F) ||
        !(view->camera.yfov < NPH_PI) || !(view->camera.znear > 0.0F) ||
        (u32)view->tonemapper > (u32)NPH_RENDER_TONEMAPPER_AGX ||
        (u32)view->debug_view >= (u32)NPH_RENDER_VIEW_COUNT ||
        (u32)view->pipeline > (u32)NPH_RENDER_PIPELINE_PRODUCTION ||
        (u32)view->anti_aliasing > (u32)NPH_RENDER_AA_TEMPORAL ||
        (u32)view->exposure > (u32)NPH_RENDER_EXPOSURE_AUTO ||
        !(view->bloom >= 0.0F && view->bloom <= 1.0F) || !(view->exposure_speed >= 0.0F) ||
        !(view->frame_seconds >= 0.0F) || !(view->shutter >= 0.0F) || !(view->lod_error >= 0.0F) ||
        !nph_f32_is_finite(view->lod_error) || !nph_f32_is_finite(view->shutter) ||
        !(view->aperture >= 0.0F) || !nph_f32_is_finite(view->aperture) ||
        !(view->sensor_height >= 0.0F) || !nph_f32_is_finite(view->sensor_height) ||
        !nph_f32_is_finite(view->lens_offset.x) || !nph_f32_is_finite(view->lens_offset.y) ||
        !(view->ao_radius >= 0.0F) || !nph_f32_is_finite(view->ao_radius)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // A lens (an aperture, or a point off its centre) focuses beyond its
    // focal length.
    const bool lens_shift = view->lens_offset.x != 0.0F || view->lens_offset.y != 0.0F;
    if ((view->aperture > 0.0F || lens_shift) &&
        !(nph_f32_is_finite(view->focus_distance) &&
          view->focus_distance > nph_render_view_focal_length(view))) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphGpuImageDesc *td = nph_gpu_image_desc(dev, target);
    if (td->type != NPH_GPU_IMAGE_2D || td->layer_count != 1U ||
        (td->usage & (u32)NPH_GPU_IMAGE_USAGE_COLOR_ATTACHMENT) == 0U) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!target_format_supported(td->format)) {
        return NPH_RESULT_UNSUPPORTED;
    }
    // Seen through a point off the lens's centre (NphRenderView.lens_offset):
    // the camera moves there, and the projection shears below so the focus
    // plane stays put. Everything after takes the moved camera.
    NphRenderView lensed;
    if (lens_shift) {
        lensed = *view;
        const NphVec3 o = nph_quat_rotate(nph_quat_normalize_or_identity(view->camera.orientation),
                                          nph_vec3(view->lens_offset.x, view->lens_offset.y, 0.0F));
        lensed.camera.position = nph_dvec3_add(view->camera.position, nph_dvec3_from_vec3(o));
        view = &lensed;
    }
    bool production = view->pipeline == NPH_RENDER_PIPELINE_PRODUCTION;
    if (production && !r->vis.supported) {
        if (!r->vis.fallback_logged) {
            NPH_LOG_WARN(&NPH_LOG_RENDER,
                         "%s: no production path here; drawing with the reference renderer "
                         "(said once)",
                         r->name);
            r->vis.fallback_logged = 1;
        }
        production = false;
    }
    NPH_TRY(target_pipelines(r, td->format, &r->frame_target));
    r->last_frame_index = nph_gpu_frame_index(dev);
    r->frame_slot = nph_gpu_frame_slot(dev);
    r->frame_production = production ? 1 : 0;
    const u64 address = r->frame_addresses[r->frame_slot];
    const u32 width = td->width;
    const u32 height = td->height;
    stats.pipeline = production ? NPH_RENDER_PIPELINE_PRODUCTION : NPH_RENDER_PIPELINE_REFERENCE;

    // The view: rotation only, the camera at the origin.
    const NphMat3 rot =
        nph_mat3_from_quat(nph_quat_normalize_or_identity(view->camera.orientation));
    const NphMat4 view_m = nph_mat4_from_mat3(nph_mat3_transpose(rot));
    NphMat4 proj = nph_mat4_perspective_infinite(view->camera.yfov, (f32)width / (f32)height,
                                                 view->camera.znear);
    // Temporal anti-aliasing moves the projection by the frame's sub-pixel
    // jitter: x_ndc += j_ndc w, through the column that multiplies view z
    // (w = -z). Every pass that draws or rebuilds positions uses it.
    NphVec2 jitter = nph_vec2(0.0F, 0.0F);
    NPH_TRY(render_taa_frame_begin(r, view, width, height, production, &jitter));
    const NphVec2 jitter_ndc =
        nph_vec2(2.0F * jitter.x / (f32)width, 2.0F * jitter.y / (f32)height);
    proj.c[2].x -= jitter_ndc.x;
    proj.c[2].y -= jitter_ndc.y;
    if (lens_shift) {
        // The moved camera sees a point of the focus plane (view z = -focus)
        // the offset aside: its NDC regains P00 x / focus (and P11 y), by
        // the same column as the jitter.
        proj.c[2].x -= proj.c[0].x * view->lens_offset.x / view->focus_distance;
        proj.c[2].y -= proj.c[1].y * view->lens_offset.y / view->focus_distance;
    }
    FrameCull fc;
    fc.view = view_m;
    fc.camera = view->camera.position;
    const NphMat4 view_proj = nph_mat4_mul(proj, view_m);
    fc.frustum = nph_frustum_from_matrix(&view_proj);
    NphMat4 inv_view_proj = nph_mat4_identity();
    NPH_IGNORE_RESULT(nph_mat4_inverse(view_proj, &inv_view_proj));

    // The sun's virtual shadow map (the production path), then shadow rays
    // for the lights that cast shadows it does not take.
    const bool vsm = render_vsm_frame_begin(r, view, &proj, &inv_view_proj, height, production);
    const bool rt = render_rt_frame_begin(r, production, view->camera.position, vsm ? 1U : 0U,
                                          render_gi_frame_wanted(r, view, production));
    stats.shadow_rays = r->rt.frame_shadows;
    stats.vsm = vsm ? 1 : 0;
    stats.vsm_lod_bias = vsm ? r->vsm.frame_bias : 0U;
    stats.taa = r->taa.frame_active;
    stats.taa_history = r->taa.frame_history;
    // The exposure: the view's, or the automatic one's (post.c).
    const f32 ev100 = render_post_frame_begin(r, view);
    stats.ev100 = ev100;
    stats.metered_ev100 = r->post.metered ? r->post.metered_ev100 : 0.0F;
    stats.bloom = r->post.frame_bloom;
    // Motion blur, measured against the previous frame (motion_blur.c), and
    // depth of field (dof.c).
    const bool motion_blur = render_motion_frame_begin(r, view, width, height, production);
    stats.motion_blur = motion_blur ? 1 : 0;
    const bool dof = render_dof_frame_begin(r, view, height);
    stats.dof = dof ? 1 : 0;
    // Ambient occlusion on the production path (ao.c), through this
    // frame's projection.
    stats.ao = render_ao_frame_begin(r, view, &proj, width, height, production) ? 1 : 0;
    // Global illumination on the production path (gi.c), whose rays leave
    // the ambient occlusion's range to it.
    stats.gi = render_gi_frame_begin(r, view, width, height, production) ? 1 : 0;

    write_materials(r);
    // The production path lights from its GPU table (render_vis_frame).
    const u32 light_count = production ? 0U : write_lights(r, view->camera.position);
    build_draws(r, &fc, production, &stats);
    // The production path writes its instance records on the GPU (vis.c).
    if (rt && !production) {
        render_rt_write_instances(r);
    }
    if (stats.dropped != 0U) {
        NPH_LOG_WARN(&NPH_LOG_RENDER, "%s: %u visible draws past max_draws (%u) were dropped",
                     r->name, stats.dropped, r->desc.max_draws);
    }
    // Lines closed by this frame's nph_render_debug_flip, or everything
    // added so far.
    if (r->debug_flip_frame != nph_gpu_frame_index(r->dev) + 1U) {
        render_debug_begin_frame(r);
    }
    stats.lights = nph_handle_pool_live(&r->light_pool);
    stats.debug_lines = r->debug_draw_counts[0] + r->debug_draw_counts[1];

    NphRenderViewData *vd = render_frame_at(r, r->layout.view_offset);
    memset(vd, 0, sizeof *vd);
    vd->view = view_m;
    vd->proj = proj;
    vd->view_proj = view_proj;
    vd->inv_view_proj = inv_view_proj;
    vd->viewport = nph_vec4((f32)width, (f32)height, 1.0F / (f32)width, 1.0F / (f32)height);
    vd->debug_offset =
        nph_vec4_from_vec3(nph_dvec3_relative(r->debug_frame_origin, view->camera.position), 0.0F);
    vd->background =
        nph_vec4_from_vec3(view->background_color, view->background == NPH_RENDER_BACKGROUND_SOLID
                                                       ? (f32)NPH_RENDER_BACKGROUND_COLOR
                                                       : (f32)NPH_RENDER_BACKGROUND_ENVIRONMENT);
    vd->lights_address = address + r->layout.lights_offset;
    vd->materials_address = address + r->layout.materials_offset;
    vd->draws_address = address + r->layout.draws_offset;
    vd->sh_address = nph_gpu_buffer_address(dev, r->sh_buffer);
    vd->debug_address = r->debug_addresses[r->debug_draw_buffer];
    vd->light_count = light_count;
    vd->exposure = nph_render_exposure_from_ev100(ev100);
    vd->ibl_intensity = r->env.intensity;
    vd->specular_index = nph_gpu_image_sampled_index(dev, r->specular_cube);
    vd->specular_max_lod = (f32)(r->desc.specular_mips - 1U);
    vd->brdf_lut_index = nph_gpu_image_sampled_index(dev, r->brdf_lut);
    vd->env_index = nph_gpu_image_sampled_index(dev, r->env_cube);
    vd->clamp_sampler = nph_gpu_sampler_index(dev, r->clamp_sampler);
    vd->debug_mode = (u32)view->debug_view;
    vd->tlas = rt ? nph_gpu_accel_address(dev, r->rt.tlas) : 0U;
    vd->tlas_offset = nph_vec4_from_vec3(r->rt.tlas_offset, 0.0F);
    vd->vsm = vsm ? r->vsm.view_address : 0U;
    vd->gi = stats.gi ? render_gi_view_address(r) : 0U;
    vd->jitter = nph_vec4(jitter_ndc.x, jitter_ndc.y, 0.0F, 0.0F);
    vd->flags =
        !view->disable_ibl && r->env.source != NPH_RENDER_ENV_NONE ? (u32)NPH_RENDER_VIEW_IBL : 0U;

    memset(&r->push, 0, sizeof r->push);
    r->push.view_address = address + r->layout.view_offset;
    memset(&r->tonemap_push, 0, sizeof r->tonemap_push);
    r->tonemap_push.tonemapper = (u32)view->tonemapper;
    r->tonemap_push.encode_srgb = format_is_srgb(td->format) ? 0U : 1U;

    if (r->pending_uploads) {
        NPH_TRY(nph_gpu_upload_flush(dev, NULL));
        r->pending_uploads = 0;
    }

    // Passes.
    NphGpuRgImage env;
    NphGpuRgImage specular;
    NphGpuRgImage lut;
    NphGpuRgBuffer sh;
    NPH_TRY(render_ibl_declare(r, graph, &env, &specular, &lut, &sh, &stats.environment_updated));

    NphGpuRgImage depth = NPH_GPU_RG_IMAGE_NULL;
    NphGpuRgImage visbuffer = NPH_GPU_RG_IMAGE_NULL;
    if (production) {
        NphGpuRgImage hdr = NPH_GPU_RG_IMAGE_NULL;
        NPH_TRY(render_vis_frame(r, view, graph, width, height, &view_m, &proj, env, specular, lut,
                                 sh, r->blend_count != 0U ? blend_requests_execute : NULL, &hdr,
                                 &stats));
        if (r->blend_count != 0U) {
            NPH_TRY(render_vis_declare_transparent(r, graph, transparent_execute, env, specular,
                                                   lut, sh));
        }
        r->hdr_rg = hdr;
        visbuffer = r->vis.frame.visbuffer;
    } else {
        NPH_TRY(declare_reference(r, graph, width, height, dof, env, specular, lut, sh, &depth));
    }
    stats.shadow_builds = r->rt.frame_builds;
    stats.shadow_rebuild = rt && r->rt.frame_rebuild ? 1 : 0;
    stats.shadow_compactions = r->rt.frame_compactions;
    stats.shadow_blas_bytes = r->rt.blas_bytes;
    // Depth of field over the complete HDR image, whose image with it the
    // rest of the frame reads; then bloom (glowing from the blurred image)
    // and the metering.
    if (dof) {
        NPH_TRY(render_dof_declare(
            r, graph, r->hdr_rg, production ? r->vis.hdr_format : RENDER_HDR_FORMAT,
            production ? visbuffer : depth, production, width, height, &r->hdr_rg));
    }
    NPH_TRY(render_post_declare(r, graph, width, height));

    const NphGpuRgImage out = nph_gpu_rg_import_image(graph, target);
    if (nph_gpu_rg_image_is_null(out)) {
        return NPH_RESULT_FULL;
    }
    if (r->taa.frame_active || motion_blur) {
        // The production path's own final passes: the temporal resolve into
        // the target, or into its history alone for the motion blur to read;
        // the motion blur fused with the tonemap; then the debug lines.
        NphGpuRgImage color = r->hdr_rg;
        if (r->taa.frame_active) {
            NPH_TRY(render_taa_declare(r, graph, motion_blur ? NPH_GPU_RG_IMAGE_NULL : out,
                                       vd->exposure, &color));
        }
        if (motion_blur) {
            NPH_TRY(render_motion_declare(r, graph, out, color, !r->taa.frame_active));
        }
        NPH_TRY(declare_lines(r, graph, out));
        if (out_target != NULL) {
            *out_target = out;
        }
        if (out_stats != NULL) {
            *out_stats = stats;
        }
        return NPH_RESULT_OK;
    }
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = "tonemap";
    pd.type = NPH_GPU_RG_PASS_GRAPHICS;
    pd.execute = tonemap_execute;
    pd.user = r;
    NphGpuRgPass *tm = nph_gpu_rg_add_pass(graph, &pd);
    if (tm == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_DONT_CARE; // every pixel is written
    a.store = NPH_GPU_STORE_OP_STORE;
    nph_gpu_rg_color_attachment(tm, out, &a);
    if (production) {
        // The depth-tested lines read the visibility buffer's depth.
        if (r->debug_draw_counts[0] != 0U) {
            nph_gpu_rg_read_image(tm, visbuffer, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
        }
    } else {
        a.load = NPH_GPU_LOAD_OP_LOAD;
        a.store = NPH_GPU_STORE_OP_DONT_CARE;
        nph_gpu_rg_depth_attachment(tm, depth, &a, 1);
    }
    nph_gpu_rg_read_image(tm, r->hdr_rg, NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    render_post_read(r, tm);

    if (out_target != NULL) {
        *out_target = out;
    }
    if (out_stats != NULL) {
        *out_stats = stats;
    }
    return NPH_RESULT_OK;
}

NphResult nph_render_frame(NphRenderer *r, const NphRenderView *view, NphGpuRenderGraph *graph,
                           NphGpuImage target, NphGpuRgImage *out_target,
                           NphRenderFrameStats *out_stats)
{
    NPH_PROFILE_BEGIN("render.frame");
    const NphResult result = render_frame(r, view, graph, target, out_target, out_stats);
    NPH_PROFILE_END();
    return result;
}
