// SPDX-License-Identifier: Apache-2.0
// The viewport's camera as the screen sees it. See view.h.
#include "view.h"

#include <nepheline/math/quat.h>
#include <nepheline/math/scalar.h>

void editor_view_init(EditorView *v, const NphRenderCamera *camera, f32 x, f32 y, f32 w, f32 h)
{
    v->eye = camera->position;
    v->orientation = nph_quat_normalize_or_identity(camera->orientation);
    v->tan_half_fov = nph_f32_tan(camera->yfov * 0.5F);
    v->aspect = h > 0.0F ? w / h : 1.0F;
    v->x = x;
    v->y = y;
    v->w = w;
    v->h = h;
}

f32 editor_view_depth(const EditorView *v, NphDVec3 p)
{
    const NphVec3 c = nph_quat_rotate_inverse(v->orientation, nph_dvec3_relative(p, v->eye));
    return -c.z;
}

bool editor_view_project(const EditorView *v, NphDVec3 p, f32 *out_sx, f32 *out_sy)
{
    // Camera space: the camera looks down -Z with +Y up.
    const NphVec3 c = nph_quat_rotate_inverse(v->orientation, nph_dvec3_relative(p, v->eye));
    const f32 depth = -c.z;
    if (depth <= 1e-6F) {
        return false;
    }
    const f32 ndc_x = c.x / (depth * v->tan_half_fov * v->aspect);
    const f32 ndc_y = -c.y / (depth * v->tan_half_fov); // the screen's Y points down
    *out_sx = v->x + (ndc_x * 0.5F + 0.5F) * v->w;
    *out_sy = v->y + (ndc_y * 0.5F + 0.5F) * v->h;
    return true;
}

NphVec3 editor_view_ray(const EditorView *v, f32 sx, f32 sy)
{
    const f32 ndc_x = v->w > 0.0F ? (sx - v->x) / v->w * 2.0F - 1.0F : 0.0F;
    const f32 ndc_y = v->h > 0.0F ? (sy - v->y) / v->h * 2.0F - 1.0F : 0.0F;
    const NphVec3 c =
        nph_vec3(ndc_x * v->tan_half_fov * v->aspect, -ndc_y * v->tan_half_fov, -1.0F);
    return nph_vec3_normalize(nph_quat_rotate(v->orientation, c));
}

f32 editor_view_world_per_unit(const EditorView *v, f32 distance)
{
    return v->h > 0.0F ? 2.0F * distance * v->tan_half_fov / v->h : 0.0F;
}
