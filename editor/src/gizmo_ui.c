// SPDX-License-Identifier: Apache-2.0
// The transform gizmo on screen (docs/editor.md "Gizmos"): drawn over the
// viewport with Dear ImGui's draw list at a constant size, its handles
// picked by the mouse in screen space, and a drag turned into one undoable
// step on the entity's LocalTransform. The math is gizmo.c.
//
//   W, E, R   translate, rotate, scale
//   X         world or local axes (scale is always local)
//   Ctrl      snap: 0.5 m, 15 degrees, a tenth of the scale
#include <nepheline/core/assert.h>
#include <nepheline/math/scalar.h>

#include "editor_internal.h"

#include <float.h>
#include <string.h>

#define HIT_PIXELS    7.0F // how near the mouse must be to a handle
#define RING_SEGMENTS 64
#define PLANE_NEAR    0.2F // the plane squares, as fractions of the axes
#define PLANE_FAR     0.45F

typedef struct Screen {
    f32 x[2]; // the origin on screen
    f32 tip[3][2];
    bool tip_ok[3];
    NphVec3 axes[3];
    f32 size; // world length of the axes
    NphDVec3 origin;
} Screen;

// A quadrilateral on screen, its corners in order. A struct, so a const
// pointer to it takes a mutable one without a cast in C17.
typedef struct Quad {
    f32 p[4][2];
} Quad;

static ImU32 color(u32 axis, bool lit)
{
    if (lit) {
        return igColorConvertFloat4ToU32(ui_vec4(1.0F, 0.85F, 0.2F, 1.0F));
    }
    static const f32 rgb[3][3] = {{0.92F, 0.25F, 0.25F}, {0.35F, 0.85F, 0.3F}, {0.3F, 0.5F, 1.0F}};
    return igColorConvertFloat4ToU32(ui_vec4(rgb[axis][0], rgb[axis][1], rgb[axis][2], 1.0F));
}

static f32 segment_distance(f32 px, f32 py, const f32 a[2], const f32 b[2])
{
    const f32 dx = b[0] - a[0];
    const f32 dy = b[1] - a[1];
    const f32 len2 = dx * dx + dy * dy;
    f32 t = len2 > 0.0F ? ((px - a[0]) * dx + (py - a[1]) * dy) / len2 : 0.0F;
    t = nph_f32_clamp(t, 0.0F, 1.0F);
    const f32 ex = a[0] + t * dx - px;
    const f32 ey = a[1] + t * dy - py;
    return nph_f32_sqrt(ex * ex + ey * ey);
}

// A point inside a convex quadrilateral given in either winding.
static bool in_quad(f32 px, f32 py, const Quad *q)
{
    f32 sign = 0.0F;
    for (u32 i = 0; i < 4U; ++i) {
        const f32 *a = q->p[i];
        const f32 *b = q->p[(i + 1U) % 4U];
        const f32 cross = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
        if (cross != 0.0F) {
            if (sign != 0.0F && (cross > 0.0F) != (sign > 0.0F)) {
                return false;
            }
            sign = cross;
        }
    }
    return true;
}

static bool project_offset(const EditorView *v, const Screen *s, NphVec3 offset, f32 out[2])
{
    return editor_view_project(v, nph_dvec3_add_vec3(s->origin, offset), &out[0], &out[1]);
}

// The plane square facing axis `i` on screen; false when part of it is
// behind the eye.
static bool plane_quad(const EditorView *v, const Screen *s, u32 i, Quad *q)
{
    const NphVec3 a = s->axes[(i + 1U) % 3U];
    const NphVec3 b = s->axes[(i + 2U) % 3U];
    const f32 n = PLANE_NEAR * s->size;
    const f32 f = PLANE_FAR * s->size;
    const NphVec3 corners[4] = {
        nph_vec3_add(nph_vec3_scale(a, n), nph_vec3_scale(b, n)),
        nph_vec3_add(nph_vec3_scale(a, f), nph_vec3_scale(b, n)),
        nph_vec3_add(nph_vec3_scale(a, f), nph_vec3_scale(b, f)),
        nph_vec3_add(nph_vec3_scale(a, n), nph_vec3_scale(b, f)),
    };
    for (u32 c = 0; c < 4U; ++c) {
        if (!project_offset(v, s, corners[c], q->p[c])) {
            return false;
        }
    }
    return true;
}

// The ring around axis `i` on screen, RING_SEGMENTS + 1 points (closed);
// false when part of it is behind the eye.
static bool ring_points(const EditorView *v, const Screen *s, u32 i, ImVec2_c *out)
{
    const NphVec3 a = s->axes[(i + 1U) % 3U];
    const NphVec3 b = s->axes[(i + 2U) % 3U];
    for (u32 k = 0; k <= (u32)RING_SEGMENTS; ++k) {
        const f32 angle = (f32)k * (2.0F * NPH_PI / (f32)RING_SEGMENTS);
        const NphVec3 p = nph_vec3_scale(nph_vec3_add(nph_vec3_scale(a, nph_f32_cos(angle)),
                                                      nph_vec3_scale(b, nph_f32_sin(angle))),
                                         s->size);
        f32 xy[2];
        if (!project_offset(v, s, p, xy)) {
            return false;
        }
        out[k] = ui_vec2(xy[0], xy[1]);
    }
    return true;
}

static EditorGizmoHandle hit_test(const Editor *ed, const EditorView *v, const Screen *s, f32 mx,
                                  f32 my)
{
    const EditorGizmoMode mode = ed->gizmo_mode;
    EditorGizmoHandle best = EDITOR_GIZMO_NONE;
    f32 best_d = HIT_PIXELS;
    if (mode == EDITOR_GIZMO_SCALE) {
        const f32 dx = mx - s->x[0];
        const f32 dy = my - s->x[1];
        if (dx * dx + dy * dy <= 81.0F) {
            return EDITOR_GIZMO_ALL;
        }
    }
    if (mode == EDITOR_GIZMO_TRANSLATE) {
        for (u32 i = 0; i < 3U; ++i) {
            Quad q;
            if (plane_quad(v, s, i, &q) && in_quad(mx, my, &q)) {
                return (EditorGizmoHandle)((u32)EDITOR_GIZMO_PLANE_X + i);
            }
        }
    }
    for (u32 i = 0; i < 3U; ++i) {
        f32 d = FLT_MAX;
        if (mode == EDITOR_GIZMO_ROTATE) {
            ImVec2_c pts[RING_SEGMENTS + 1];
            if (ring_points(v, s, i, pts)) {
                for (u32 k = 0; k < (u32)RING_SEGMENTS; ++k) {
                    const f32 a[2] = {pts[k].x, pts[k].y};
                    const f32 b[2] = {pts[k + 1U].x, pts[k + 1U].y};
                    d = nph_f32_min(d, segment_distance(mx, my, a, b));
                }
            }
        } else if (s->tip_ok[i]) {
            d = segment_distance(mx, my, s->x, s->tip[i]);
        }
        if (d < best_d) {
            best_d = d;
            best = (EditorGizmoHandle)((u32)EDITOR_GIZMO_X + i);
        }
    }
    return best;
}

static void draw(const Editor *ed, const EditorView *v, const Screen *s, ImDrawList *dl)
{
    const EditorGizmoHandle lit = ed->gizmo_dragging ? ed->gizmo_drag.handle : ed->gizmo_hover;
    const ImVec2_c o = ui_vec2(s->x[0], s->x[1]);
    for (u32 i = 0; i < 3U; ++i) {
        const bool axis_lit = lit == (EditorGizmoHandle)((u32)EDITOR_GIZMO_X + i);
        const ImU32 c = color(i, axis_lit);
        if (ed->gizmo_mode == EDITOR_GIZMO_ROTATE) {
            ImVec2_c pts[RING_SEGMENTS + 1];
            if (ring_points(v, s, i, pts)) {
                ImDrawList_AddPolyline(dl, pts, RING_SEGMENTS + 1, c, axis_lit ? 3.5F : 2.5F,
                                       ImDrawFlags_None);
            }
            continue;
        }
        if (!s->tip_ok[i]) {
            continue;
        }
        const ImVec2_c tip = ui_vec2(s->tip[i][0], s->tip[i][1]);
        ImDrawList_AddLine(dl, o, tip, c, axis_lit ? 4.0F : 3.0F);
        if (ed->gizmo_mode == EDITOR_GIZMO_SCALE) {
            ImDrawList_AddRectFilled(dl, ui_vec2(tip.x - 5.0F, tip.y - 5.0F),
                                     ui_vec2(tip.x + 5.0F, tip.y + 5.0F), c, 0.0F, 0);
        } else {
            // An arrowhead along the axis as it runs on screen.
            f32 dx = tip.x - o.x;
            f32 dy = tip.y - o.y;
            const f32 len = nph_f32_sqrt(dx * dx + dy * dy);
            if (len > 1.0F) {
                dx /= len;
                dy /= len;
                const ImVec2_c point = ui_vec2(tip.x + dx * 12.0F, tip.y + dy * 12.0F);
                const ImVec2_c left = ui_vec2(tip.x - dy * 5.0F, tip.y + dx * 5.0F);
                const ImVec2_c right = ui_vec2(tip.x + dy * 5.0F, tip.y - dx * 5.0F);
                ImDrawList_AddTriangleFilled(dl, left, point, right, c);
            }
        }
    }
    if (ed->gizmo_mode == EDITOR_GIZMO_TRANSLATE) {
        for (u32 i = 0; i < 3U; ++i) {
            Quad q;
            if (!plane_quad(v, s, i, &q)) {
                continue;
            }
            const bool plane_lit = lit == (EditorGizmoHandle)((u32)EDITOR_GIZMO_PLANE_X + i);
            ImVec4_c c = igColorConvertU32ToFloat4(color(i, plane_lit));
            c.w = plane_lit ? 0.8F : 0.35F;
            ImDrawList_AddQuadFilled(dl, ui_vec2(q.p[0][0], q.p[0][1]),
                                     ui_vec2(q.p[1][0], q.p[1][1]), ui_vec2(q.p[2][0], q.p[2][1]),
                                     ui_vec2(q.p[3][0], q.p[3][1]), igColorConvertFloat4ToU32(c));
        }
    }
    if (ed->gizmo_mode == EDITOR_GIZMO_SCALE) {
        const ImU32 c = lit == EDITOR_GIZMO_ALL
                            ? color(0U, true)
                            : igColorConvertFloat4ToU32(ui_vec4(0.9F, 0.9F, 0.9F, 1.0F));
        ImDrawList_AddRectFilled(dl, ui_vec2(o.x - 6.0F, o.y - 6.0F),
                                 ui_vec2(o.x + 6.0F, o.y + 6.0F), c, 0.0F, 0);
    }
}

static void end_drag(Editor *ed)
{
    if (ed->gizmo_dragging) {
        ed->gizmo_dragging = false;
        editor_history_end_merge(ed->history);
    }
}

static void apply_drag(Editor *ed, NphWorld *w, NphEntity e, const EditorView *v, f32 mx, f32 my)
{
    const ImGuiIO *io = igGetIO_Nil();
    const NphVec3 dir = editor_view_ray(v, mx, my);
    const NphVec3 o = nph_dvec3_relative(v->eye, ed->gizmo_drag.start.translation);
    const NphWorldTransform moved = editor_gizmo_drag(&ed->gizmo_drag, o, dir, mx, io->KeyCtrl);
    const NphEntity parent = nph_world_parent(w, e);
    NphWorldTransform parent_world = nph_world_transform_identity();
    if (!nph_entity_is_null(parent)) {
        parent_world = editor_world_transform_of(w, parent);
    }
    const NphLocalTransform fitted =
        editor_local_from_world(&moved, nph_entity_is_null(parent) ? NULL : &parent_world);
    NphLocalTransform local = ed->gizmo_start_local;
    if (ed->gizmo_mode == EDITOR_GIZMO_TRANSLATE) {
        local.translation = fitted.translation; // the rotation and the scale stay exactly
    } else {
        local = fitted;
        local.translation = ed->gizmo_start_local.translation;
    }
    static const char *const labels[3] = {"Move", "Rotate", "Scale"};
    const NphResult r = editor_history_set_merged(ed->history, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM,
                                                  &local, ((u64)0x6A1DU << 48U) | ed->gizmo_drag_id,
                                                  labels[ed->gizmo_mode]);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "gizmo: %s", nph_result_string(r));
        end_drag(ed);
    }
}

bool editor_gizmo_update(Editor *ed, const EditorView *v, bool image_hovered, ImDrawList *dl)
{
    NphEngine *engine = ed->engine;
    NphWorld *w = nph_engine_edit_world(engine);
    const NphEntity e = ed->selection.primary;
    if (nph_engine_is_playing(engine) || !nph_world_is_alive(w, e) ||
        !nph_world_has(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM)) {
        end_drag(ed);
        ed->gizmo_hover = EDITOR_GIZMO_NONE;
        return false;
    }
    const NphWorldTransform t = editor_world_transform_of(w, e);
    const f32 depth = editor_view_depth(v, t.translation);
    if (depth <= ed->camera.znear) {
        end_drag(ed);
        ed->gizmo_hover = EDITOR_GIZMO_NONE;
        return false; // behind the eye: no gizmo to show
    }
    Screen s;
    memset(&s, 0, sizeof s);
    s.origin = t.translation;
    s.size = EDITOR_GIZMO_PIXELS * editor_view_world_per_unit(v, depth);
    editor_gizmo_axes(&t, ed->gizmo_mode == EDITOR_GIZMO_SCALE || ed->gizmo_local, s.axes);
    if (!editor_view_project(v, s.origin, &s.x[0], &s.x[1])) {
        return false;
    }
    for (u32 i = 0; i < 3U; ++i) {
        s.tip_ok[i] = project_offset(v, &s, nph_vec3_scale(s.axes[i], s.size), s.tip[i]);
    }
    const ImGuiIO *io = igGetIO_Nil();
    const f32 mx = io->MousePos.x;
    const f32 my = io->MousePos.y;
    if (!ed->gizmo_dragging) {
        ed->gizmo_hover =
            image_hovered && !io->KeyAlt ? hit_test(ed, v, &s, mx, my) : EDITOR_GIZMO_NONE;
        if (ed->gizmo_hover != EDITOR_GIZMO_NONE &&
            igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
            const NphVec3 dir = editor_view_ray(v, mx, my);
            const NphVec3 o = nph_dvec3_relative(v->eye, t.translation);
            if (editor_gizmo_begin(&ed->gizmo_drag, ed->gizmo_mode, ed->gizmo_hover, &t,
                                   ed->gizmo_local, o, dir, mx)) {
                ed->gizmo_dragging = true;
                ed->gizmo_entity = e;
                ed->gizmo_start_local =
                    *NPH_WORLD_GET(w, e, NPH_COMPONENT_ID_LOCAL_TRANSFORM, NphLocalTransform);
                ++ed->gizmo_drag_id;
            }
        }
    }
    if (ed->gizmo_dragging) {
        if (!igIsMouseDown_Nil(ImGuiMouseButton_Left) || !nph_entity_equals(e, ed->gizmo_entity)) {
            end_drag(ed);
        } else {
            apply_drag(ed, w, e, v, mx, my);
        }
    }
    ImDrawList_PushClipRect(dl, ui_vec2(v->x, v->y), ui_vec2(v->x + v->w, v->y + v->h), true);
    draw(ed, v, &s, dl);
    ImDrawList_PopClipRect(dl);
    return ed->gizmo_dragging || ed->gizmo_hover != EDITOR_GIZMO_NONE;
}
