// SPDX-License-Identifier: Apache-2.0
// Image-based lighting (docs/render.md "Image-based lighting"): the
// environment cube, its prefiltered specular levels, its irradiance as
// spherical harmonics, and the split-sum DFG table, all computed on the
// GPU in render-graph compute passes when they change:
//
//   brdf lut          once, at the first frame (brdf_lut.comp)
//   ibl source        environment level 0 from the sky or a texture
//   ibl downsample    one pass per level: the box-filtered mip chain
//   ibl sh            the irradiance SH from the 32 x 32 level
//   ibl prefilter     one pass per roughness level of the specular cube
//
// Each pass declares exactly the levels it reads and writes, so the graph
// places the barriers between them; the forward pass then samples the
// results every frame with no further synchronisation.
#include <nepheline/core/log.h>

#include "render_internal.h"

#include <string.h>

static u32 log2_u32(u32 v)
{
    u32 n = 0U;
    while (v > 1U) {
        v >>= 1U;
        ++n;
    }
    return n;
}

static u32 groups_of(u32 extent)
{
    return (extent + 7U) / 8U;
}

NphResult render_ibl_create(NphRenderer *r)
{
    const NphRendererDesc *d = &r->desc;
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.type = NPH_GPU_IMAGE_CUBE;
    id.format = RENDER_IBL_FORMAT;
    id.width = d->environment_size;
    id.height = d->environment_size;
    id.layer_count = 6U;
    id.mip_count = 0U; // the full chain
    // TRANSFER_SRC lets the tests and captures read the tables back; these
    // images are storage images, which forgo framebuffer compression anyway.
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_STORAGE |
               (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC;
    id.name = "environment";
    NPH_TRY(nph_gpu_image_create(r->dev, &id, &r->env_cube));
    r->env_mips = log2_u32(d->environment_size) + 1U;

    id.width = d->specular_size;
    id.height = d->specular_size;
    id.mip_count = d->specular_mips;
    id.name = "prefiltered environment";
    NPH_TRY(nph_gpu_image_create(r->dev, &id, &r->specular_cube));

    id.type = NPH_GPU_IMAGE_2D;
    id.width = d->brdf_lut_size;
    id.height = d->brdf_lut_size;
    id.layer_count = 1U;
    id.mip_count = 1U;
    id.name = "brdf lut";
    NPH_TRY(nph_gpu_image_create(r->dev, &id, &r->brdf_lut));

    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = 9U * sizeof(NphVec4);
    bd.usage = (u32)NPH_GPU_BUFFER_USAGE_STORAGE | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_SRC;
    bd.name = "irradiance sh";
    return nph_gpu_buffer_create(r->dev, &bd, &r->sh_buffer);
}

void render_ibl_destroy(NphRenderer *r)
{
    nph_gpu_image_destroy(r->dev, r->env_cube);
    nph_gpu_image_destroy(r->dev, r->specular_cube);
    nph_gpu_image_destroy(r->dev, r->brdf_lut);
    nph_gpu_buffer_destroy(r->dev, r->sh_buffer);
}

static void ibl_execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    const RenderIblPass *p = (const RenderIblPass *)user;
    nph_gpu_cmd_bind_pipeline(cmd, p->pipeline);
    nph_gpu_cmd_push_constants(cmd, &p->push, (u32)sizeof p->push);
    nph_gpu_cmd_dispatch(cmd, p->groups[0], p->groups[1], p->groups[2]);
}

static NphGpuRgPass *add_ibl_pass(NphRenderer *r, NphGpuRenderGraph *graph, const char *name,
                                  RenderCompute kernel, const NphRenderIblPush *push, u32 gx,
                                  u32 gy, u32 gz)
{
    NPH_ASSERT(r->ibl_pass_count < RENDER_IBL_MAX_PASSES);
    RenderIblPass *p = &r->ibl_passes[r->ibl_pass_count++];
    p->pipeline = r->compute[kernel];
    p->push = *push;
    p->groups[0] = gx;
    p->groups[1] = gy;
    p->groups[2] = gz;
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = name;
    pd.type = NPH_GPU_RG_PASS_COMPUTE;
    pd.execute = ibl_execute;
    pd.user = p;
    return nph_gpu_rg_add_pass(graph, &pd);
}

// The source level the environment's level 0 reads from a texture: the
// level whose texels are about as dense as the cube's, so the source is
// not undersampled (an equirectangular image spans four faces across).
static f32 source_lod(u32 src_width, u32 src_mips, u32 texels_per_cube_width)
{
    f32 lod = 0.0F;
    u32 w = src_width;
    while (w > texels_per_cube_width && lod + 1.0F < (f32)src_mips) {
        w >>= 1U;
        lod += 1.0F;
    }
    return lod;
}

static NphResult declare_environment(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage env,
                                     NphGpuRgImage specular, NphGpuRgBuffer sh)
{
    const NphRendererDesc *d = &r->desc;
    NphGpuDevice *dev = r->dev;
    const u32 size = d->environment_size;

    // Level 0 from the source.
    NphRenderIblPush push;
    memset(&push, 0, sizeof push);
    push.dst_index = nph_gpu_image_storage_index(dev, r->env_cube, 0U);
    push.size = size;
    RenderCompute kernel = RENDER_COMPUTE_IBL_SKY;
    const RenderTexture *tex = NULL;
    if (r->env.source == NPH_RENDER_ENV_EQUIRECT || r->env.source == NPH_RENDER_ENV_CUBE) {
        tex = render_texture_get(r, r->env.texture);
        if (tex == NULL) {
            NPH_LOG_ERROR(&NPH_LOG_RENDER,
                          "%s: the environment texture was destroyed before its first frame; "
                          "using a black environment",
                          r->name);
        }
    }
    if (tex != NULL && r->env.source == NPH_RENDER_ENV_EQUIRECT) {
        kernel = RENDER_COMPUTE_IBL_EQUIRECT;
        NphRenderTextureBinding wrap;
        memset(&wrap, 0, sizeof wrap);
        wrap.wrap_u = NPH_RENDER_WRAP_REPEAT; // longitude wraps
        wrap.wrap_v = NPH_RENDER_WRAP_CLAMP;
        push.src_index = nph_gpu_image_sampled_index(dev, tex->image);
        push.sampler_index = render_material_sampler(r, &wrap);
        push.src_size = tex->width;
        push.params.x = source_lod(tex->width, tex->mip_count, 4U * size);
    } else if (tex != NULL && r->env.source == NPH_RENDER_ENV_CUBE) {
        kernel = RENDER_COMPUTE_IBL_CUBE;
        push.src_index = nph_gpu_image_sampled_index(dev, tex->image);
        push.sampler_index = nph_gpu_sampler_index(dev, r->clamp_sampler);
        push.src_size = tex->width;
        push.params.x = source_lod(tex->width, tex->mip_count, size);
    } else {
        // The sky, or black (a sky of zero radiance) for no environment.
        NphRenderSkyData *sky = render_frame_at(r, r->layout.sky_offset);
        memset(sky, 0, sizeof *sky);
        if (r->env.source == NPH_RENDER_ENV_SKY) {
            sky->zenith = nph_vec4_from_vec3(r->env.sky.zenith, 0.0F);
            sky->horizon = nph_vec4_from_vec3(r->env.sky.horizon, 0.0F);
            sky->ground = nph_vec4_from_vec3(r->env.sky.ground, 0.0F);
            sky->params =
                nph_vec4(r->env.sky.zenith_exponent, r->env.sky.ground_sharpness, 0.0F, 0.0F);
        } else {
            sky->params = nph_vec4(1.0F, 1.0F, 0.0F, 0.0F);
        }
        push.address = r->frame_addresses[r->frame_slot] + r->layout.sky_offset;
    }
    NphGpuRgPass *src =
        add_ibl_pass(r, graph, "ibl source", kernel, &push, groups_of(size), groups_of(size), 6U);
    if (src == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_write_image(src, env, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);

    // The mip chain.
    for (u32 m = 1U; m < r->env_mips; ++m) {
        memset(&push, 0, sizeof push);
        push.src_index = nph_gpu_image_storage_index(dev, r->env_cube, m - 1U);
        push.dst_index = nph_gpu_image_storage_index(dev, r->env_cube, m);
        push.size = size >> m;
        NphGpuRgPass *p = add_ibl_pass(r, graph, "ibl downsample", RENDER_COMPUTE_IBL_DOWNSAMPLE,
                                       &push, groups_of(push.size), groups_of(push.size), 6U);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_image(p, env, NPH_GPU_RG_USE_STORAGE_READ, m - 1U, 1U);
        nph_gpu_rg_write_image(p, env, NPH_GPU_RG_USE_STORAGE_WRITE, m, 1U);
    }

    // Irradiance.
    const u32 sh_level = size > RENDER_SH_SOURCE_SIZE ? log2_u32(size / RENDER_SH_SOURCE_SIZE) : 0U;
    memset(&push, 0, sizeof push);
    push.src_index = nph_gpu_image_storage_index(dev, r->env_cube, sh_level);
    push.size = size >> sh_level;
    push.address = nph_gpu_buffer_address(dev, r->sh_buffer);
    NphGpuRgPass *shp = add_ibl_pass(r, graph, "ibl sh", RENDER_COMPUTE_IBL_SH, &push, 1U, 1U, 1U);
    if (shp == NULL) {
        return NPH_RESULT_FULL;
    }
    nph_gpu_rg_read_image(shp, env, NPH_GPU_RG_USE_STORAGE_READ, sh_level, 1U);
    nph_gpu_rg_write_buffer(shp, sh, NPH_GPU_RG_USE_STORAGE_WRITE);

    // The prefiltered levels, roughness from 0 to 1.
    for (u32 m = 0; m < d->specular_mips; ++m) {
        memset(&push, 0, sizeof push);
        push.src_index = nph_gpu_image_sampled_index(dev, r->env_cube);
        push.sampler_index = nph_gpu_sampler_index(dev, r->clamp_sampler);
        push.src_size = size;
        push.dst_index = nph_gpu_image_storage_index(dev, r->specular_cube, m);
        push.size = d->specular_size >> m;
        push.sample_count = m == 0U ? 1U : d->specular_samples;
        push.params.x = d->specular_mips > 1U ? (f32)m / (f32)(d->specular_mips - 1U) : 0.0F;
        push.params.y = (f32)(r->env_mips - 1U);
        NphGpuRgPass *p = add_ibl_pass(r, graph, "ibl prefilter", RENDER_COMPUTE_IBL_PREFILTER,
                                       &push, groups_of(push.size), groups_of(push.size), 6U);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_read_image(p, env, NPH_GPU_RG_USE_SAMPLED, 0U, NPH_GPU_ALL_MIPS);
        nph_gpu_rg_write_image(p, specular, NPH_GPU_RG_USE_STORAGE_WRITE, m, 1U);
    }
    return NPH_RESULT_OK;
}

NphResult render_ibl_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage *out_env,
                             NphGpuRgImage *out_specular, NphGpuRgImage *out_lut,
                             NphGpuRgBuffer *out_sh, b32 *out_updated)
{
    r->ibl_pass_count = 0U;
    *out_updated = 0;
    const NphGpuRgImage env = nph_gpu_rg_import_image(graph, r->env_cube);
    const NphGpuRgImage specular = nph_gpu_rg_import_image(graph, r->specular_cube);
    const NphGpuRgImage lut = nph_gpu_rg_import_image(graph, r->brdf_lut);
    const NphGpuRgBuffer sh = nph_gpu_rg_import_buffer(graph, r->sh_buffer);
    if (nph_gpu_rg_image_is_null(env) || nph_gpu_rg_image_is_null(specular) ||
        nph_gpu_rg_image_is_null(lut) || nph_gpu_rg_buffer_is_null(sh)) {
        return NPH_RESULT_FULL;
    }
    if (!r->lut_ready) {
        NphRenderIblPush push;
        memset(&push, 0, sizeof push);
        push.dst_index = nph_gpu_image_storage_index(r->dev, r->brdf_lut, 0U);
        push.size = r->desc.brdf_lut_size;
        push.sample_count = 1024U;
        NphGpuRgPass *p = add_ibl_pass(r, graph, "brdf lut", RENDER_COMPUTE_BRDF_LUT, &push,
                                       groups_of(push.size), groups_of(push.size), 1U);
        if (p == NULL) {
            return NPH_RESULT_FULL;
        }
        nph_gpu_rg_write_image(p, lut, NPH_GPU_RG_USE_STORAGE_WRITE, 0U, 1U);
        r->lut_ready = 1;
        *out_updated = 1;
    }
    if (r->env_dirty) {
        NPH_TRY(declare_environment(r, graph, env, specular, sh));
        r->env_dirty = 0;
        *out_updated = 1;
    }
    *out_env = env;
    *out_specular = specular;
    *out_lut = lut;
    *out_sh = sh;
    return NPH_RESULT_OK;
}
