// SPDX-License-Identifier: Apache-2.0
// The statistics panel (docs/editor.md "Statistics"; CLAUDE.md §8.11 "a
// debug overlay with frame stats, cvars, and a console"): the last frame's
// times and counts, physics and audio, and every cvar, editable in place
// through the registry
// (core/cvar.h), with its type checks, ranges, and read-only and cheat
// rules.
#include <nepheline/core/assert.h>
#include <nepheline/core/cvar.h>

#include "editor_internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void frame_stats(Editor *ed)
{
    NphEngine *e = ed->engine;
    const NphEngineFrameStats *s = nph_engine_frame_stats(e);
    if (!igCollapsingHeader_TreeNodeFlags("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    igText("frame %llu   %.3f ms (%.0f fps)", (unsigned long long)s->frame, s->frame_ms,
           s->frame_ms > 0.0 ? 1000.0 / s->frame_ms : 0.0);
    igText("sync %.3f ms (simulation wait %.3f)   render %.3f ms", s->sync_ms, s->sim_wait_ms,
           s->render_ms);
    igText("%s%s   steps %u   total %llu   dropped %.3f ms",
           nph_engine_is_playing(e) ? "playing" : "editing",
           nph_engine_is_paused(e) ? " (paused)" : "", s->steps, (unsigned long long)s->total_steps,
           (f64)s->dropped_ns / 1.0e6);
    igText("entities %u", nph_world_entity_count(nph_engine_world(e)));
    if (igCollapsingHeader_TreeNodeFlags("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
        const NphRenderFrameStats *r = &s->render;
        const bool production = r->pipeline == NPH_RENDER_PIPELINE_PRODUCTION;
        igText("%s pipeline (r.pipeline)", production ? "production" : "reference");
        igText("instances %u (culled %u)   draws %u (blended %u, dropped %u)", r->instances,
               r->culled, r->draws, r->blended, r->dropped);
        igText("triangles %u   lights %u   debug lines %u", r->triangles, r->lights,
               r->debug_lines);
        if (production) {
            // The GPU's counters, from frame vis_frame (a couple behind).
            igText("GPU: clusters %u (%u in software)   triangles %u   lights in view %u%s",
                   r->vis_clusters, r->vis_clusters_sw, r->vis_triangles, r->vis_lights,
                   r->vis_overflow != 0U ? "   (lists overflowed)" : "");
            igText("culled instances: frustum %u, occlusion %u", r->vis_instances_culled_frustum,
                   r->vis_instances_culled_occlusion);
            igText("culled clusters: frustum %u, cone %u, occlusion %u",
                   r->vis_meshlets_culled_frustum, r->vis_meshlets_culled_cone,
                   r->vis_meshlets_culled_occlusion);
        }
        if (r->vsm) {
            // The sun's virtual shadow map (r.virtual_shadows).
            igText("sun: virtual shadow map, pages needed %u, drawn %u, held %u   bias %u%s",
                   r->vsm_requested, r->vsm_drawn, r->vsm_resident, r->vsm_lod_bias,
                   r->vsm_overflow != 0U ? "   (overflowed)" : "");
        }
        if (r->shadow_rays) {
            igText("shadow rays%s", r->shadow_rebuild ? "   (top level rebuilt)" : "");
        }
        if (r->taa) {
            // The temporal resolve (r.taa): a history restarts after a cut.
            igText("temporal anti-aliasing%s", r->taa_history ? "" : "   (history restarted)");
        }
        if (r->ao) {
            // The ambient occlusion (r.ao).
            igText("ambient occlusion");
        }
        if (r->gi) {
            // The radiance cache's global illumination (r.gi).
            igText("global illumination");
        }
        // The exposure (r.auto_exposure adapts it toward the metering) and
        // the bloom (r.bloom).
        if (r->metered_ev100 != 0.0F) {
            igText("exposure EV100 %.2f (metered %.2f)%s", (f64)r->ev100, (f64)r->metered_ev100,
                   r->bloom ? "   bloom" : "");
        } else {
            igText("exposure EV100 %.2f%s", (f64)r->ev100, r->bloom ? "   bloom" : "");
        }
        igText("viewport %ux%u", ed->viewport.width, ed->viewport.height);
    }
    if (igCollapsingHeader_TreeNodeFlags("Scene mirror", ImGuiTreeNodeFlags_None)) {
        const NphSceneRenderStats *m = &s->scene;
        igText("entities %u   instances %u   lights %u", m->entities, m->instances, m->lights);
        igText("waiting %u   failed %u", m->pending, m->failed);
        igText("assets: loading %u, ready %u, failed %u", m->assets_loading, m->assets_ready,
               m->assets_failed);
        igText("last sync: created %u, destroyed %u, moved %u, between steps %u", m->created,
               m->destroyed, m->moved, m->interpolating);
    }
}

static void report(NphResult r, const NphCvar *cv)
{
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "%s: %s", cv->name, nph_result_string(r));
    }
}

static void cvar_value(NphCvarRegistry *reg, NphCvar *cv)
{
    switch (cv->type) {
    case NPH_CVAR_TYPE_BOOL: {
        bool v = cv->value_bool != 0;
        if (igCheckbox("##value", &v)) {
            report(nph_cvar_set_bool(reg, cv, v ? 1 : 0), cv);
        }
        break;
    }
    case NPH_CVAR_TYPE_INT: {
        i64 v = cv->value_int;
        if (igDragScalar("##value", ImGuiDataType_S64, &v, 0.25F, &cv->min_int, &cv->max_int, NULL,
                         ImGuiSliderFlags_AlwaysClamp)) {
            report(nph_cvar_set_int(reg, cv, v), cv);
        }
        break;
    }
    case NPH_CVAR_TYPE_FLOAT: {
        f64 v = cv->value_float;
        const f64 span = cv->max_float - cv->min_float;
        const f32 speed = span > 0.0 && span < 1.0e6 ? (f32)(span * 0.001) : 0.01F;
        if (igDragScalar("##value", ImGuiDataType_Double, &v, speed, &cv->min_float, &cv->max_float,
                         "%.4g", ImGuiSliderFlags_AlwaysClamp)) {
            report(nph_cvar_set_float(reg, cv, v), cv);
        }
        break;
    }
    case NPH_CVAR_TYPE_STRING: {
        char buf[256];
        (void)snprintf(buf, sizeof buf, "%s", cv->value_string != NULL ? cv->value_string : "");
        if (igInputText("##value", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue, NULL,
                        NULL)) {
            report(nph_cvar_set_string(reg, cv, nph_str_from_cstr(buf)), cv);
        }
        break;
    }
    default:
        igTextDisabled("?");
        break;
    }
}

static void cvars(Editor *ed)
{
    char *filter = ed->cvar_filter;
    if (!igCollapsingHeader_TreeNodeFlags("Cvars", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    igSetNextItemWidth(-FLT_MIN);
    (void)igInputText("##filter", filter, sizeof ed->cvar_filter, ImGuiInputTextFlags_None, NULL,
                      NULL);
    NphCvarRegistry *reg = nph_engine_cvars(ed->engine);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!igBeginTable("##cvars", 2, flags, ui_vec2(0.0F, 0.0F), 0.0F)) {
        return;
    }
    igTableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0F, 0U);
    igTableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.2F, 0U);
    igTableHeadersRow();
    const u32 n = nph_cvar_registry_count(reg);
    for (u32 i = 0; i < n; ++i) {
        NphCvar *cv = nph_cvar_registry_at(reg, i);
        if (filter[0] != '\0' && strstr(cv->name, filter) == NULL) {
            continue;
        }
        igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
        (void)igTableNextColumn();
        igAlignTextToFramePadding();
        igTextUnformatted(cv->name, NULL);
        if (cv->description != NULL) {
            igSetItemTooltip("%s%s", cv->description,
                             (cv->flags & (u32)NPH_CVAR_FLAG_LATCHED) != 0U
                                 ? "\n(takes effect after a restart)"
                                 : "");
        }
        (void)igTableNextColumn();
        const bool locked = (cv->flags & (u32)NPH_CVAR_FLAG_READ_ONLY) != 0U ||
                            ((cv->flags & (u32)NPH_CVAR_FLAG_CHEAT) != 0U && !reg->cheats_enabled);
        igPushID_Str(cv->name);
        igBeginDisabled(locked);
        igSetNextItemWidth(-FLT_MIN);
        cvar_value(reg, cv);
        igEndDisabled();
        igPopID();
    }
    igEndTable();
}

// A cvar's bit as a checkbox (phys.debug_draw's).
static void cvar_bit(NphCvarRegistry *reg, NphCvar *cv, const char *label, i64 bit)
{
    const i64 value = nph_cvar_get_int(cv);
    bool on = (value & bit) != 0;
    if (igCheckbox(label, &on)) {
        report(nph_cvar_set_int(reg, cv, on ? (value | bit) : (value & ~bit)), cv);
    }
}

static void cvar_checkbox(NphCvarRegistry *reg, NphCvar *cv, const char *label)
{
    bool on = nph_cvar_get_bool(cv);
    if (igCheckbox(label, &on)) {
        report(nph_cvar_set_bool(reg, cv, on ? 1 : 0), cv);
    }
}

// The physics world while playing (docs/runtime.md "Physics"), and the
// physics debug toggles (CLAUDE.md §8.13): what phys.debug_draw draws and
// whether the world is drawn between its steps (sim.interpolate).
static void physics(Editor *ed)
{
    if (!igCollapsingHeader_TreeNodeFlags("Physics", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    NphEngine *e = ed->engine;
    if (nph_engine_physics(e) != NULL) {
        const NphPhysicsStats *p = nph_engine_physics_stats(e);
        igText("bodies %u (awake %u)   shapes %u   joints %u", p->bodies, p->awake_bodies,
               p->shapes, p->joints);
        igText("contacts %u (touching %u, points %u)   islands %u (awake %u)", p->contacts,
               p->touching_contacts, p->manifold_points, p->islands, p->awake_islands);
        igText("step %.3f ms: pairs %.3f, collide %.3f, solve %.3f, finalize %.3f   threads %u",
               p->step_ms, p->pairs_ms, p->collide_ms, p->solve_ms, p->finalize_ms, p->workers);
        igText("swept %u (set back %u)", p->swept_bodies, p->impact_bodies);
    } else {
        igTextDisabled("not playing: the bodies are made when play begins");
    }
    NphCvarRegistry *reg = nph_engine_cvars(e);
    NphCvar *draw = nph_cvar_find(reg, NPH_STR("phys.debug_draw"));
    NphCvar *interp = nph_cvar_find(reg, NPH_STR("sim.interpolate"));
    if (draw != NULL) {
        cvar_bit(reg, draw, "shapes", NPH_PHYSICS_DRAW_SHAPES);
        igSameLine(0.0F, -1.0F);
        cvar_bit(reg, draw, "boxes", NPH_PHYSICS_DRAW_AABBS);
        igSameLine(0.0F, -1.0F);
        cvar_bit(reg, draw, "contacts", NPH_PHYSICS_DRAW_CONTACTS);
        cvar_bit(reg, draw, "centres of mass", NPH_PHYSICS_DRAW_MASS);
        igSameLine(0.0F, -1.0F);
        cvar_bit(reg, draw, "sleep", NPH_PHYSICS_DRAW_SLEEP);
        igSameLine(0.0F, -1.0F);
        cvar_bit(reg, draw, "joints", NPH_PHYSICS_DRAW_JOINTS);
    }
    if (interp != NULL) {
        cvar_checkbox(reg, interp, "draw between steps (sim.interpolate)");
    }
    (void)igCheckbox("show colliders in the viewport", &ed->show_colliders);
}

// A level as dB for the meters (silence as -inf).
static f64 meter_db(f32 level)
{
    return level > 0.0F ? 20.0 * log10((f64)level) : -(f64)INFINITY;
}

// The engine's sound (docs/editor.md "Audio"): the output and the
// hardware's format, the mixer's voices and blocks, sounds and streams, and
// each bus's meter with its volume (its cvar, edited in place).
static void audio(Editor *ed)
{
    if (!igCollapsingHeader_TreeNodeFlags("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    NphEngine *e = ed->engine;
    NphEngineAudioStats st;
    nph_engine_audio_stats(e, &st);
    if (st.mode == NPH_ENGINE_AUDIO_OFF) {
        igTextDisabled("off (audio.device)");
        return;
    }
    static const char *const modes[] = {"off", "the system's device", "the null device",
                                        "rendered offline"};
    igText("%s, %u Hz, %u channels", modes[st.mode], st.sample_rate, st.channels);
    if (st.mode == NPH_ENGINE_AUDIO_SYSTEM || st.mode == NPH_ENGINE_AUDIO_NULL) {
        const NphAudioDeviceStats *d = &st.device;
        const bool converted =
            d->device_sample_rate != st.sample_rate || d->device_channels != st.channels;
        igText("device %u Hz, %u channels, %u-frame periods%s", d->device_sample_rate,
               d->device_channels, d->period_frames, converted ? "   (converted at the edge)" : "");
        igText("callbacks %llu, slowest %.3f ms", (unsigned long long)d->callbacks,
               (f64)d->longest_ns / 1.0e6);
    }
    const NphAudioMixerStats *m = &st.mixer;
    igText("voices %u (mixed %u, virtual %u)   blocks %llu: slowest %.3f ms, average %.3f ms",
           m->voices_playing, m->voices_real, m->voices_virtual, (unsigned long long)m->blocks,
           (f64)m->block_ns_max / 1.0e6,
           m->blocks > 0U ? (f64)m->block_ns_total / 1.0e6 / (f64)m->blocks : 0.0);
    if (m->commands_dropped != 0U || m->corrupt_reads != 0U || m->stream_starved != 0U) {
        igText("commands dropped %u   corrupt reads %u   frames starved %llu", m->commands_dropped,
               m->corrupt_reads, (unsigned long long)m->stream_starved);
    }
    igText("sounds %u (loading %u, failed %u, streamed %u)   sources playing %u (waiting %u)",
           st.sounds, st.sounds_loading, st.sounds_failed, st.sounds_streamed, st.voices,
           st.voices_waiting);
    const NphAudioStreamerStats *s = &st.streamer;
    igText("streams %u: decoded %llu frames, seeks %llu, read errors %u, corrupt blocks %u",
           s->streams, (unsigned long long)s->frames_decoded, (unsigned long long)s->seeks,
           s->read_errors, s->corrupt_blocks);
    igText("occlusion rays %u   one-shots dropped %u", st.occlusion_rays, st.oneshots_dropped);
    NphAudioMixer *mixer = nph_engine_audio_mixer(e);
    NphCvarRegistry *reg = nph_engine_cvars(e);
    static const char *const names[NPH_AUDIO_SOURCE_BUS_COUNT + 1U] = {
        "effects", "music", "dialogue", "ambience", "ui", "master"};
    static const char *const vars[NPH_AUDIO_SOURCE_BUS_COUNT + 1U] = {
        "audio.volume_effects",  "audio.volume_music", "audio.volume_dialogue",
        "audio.volume_ambience", "audio.volume_ui",    "audio.volume"};
    if (mixer == NULL ||
        !igBeginTable("##buses", 3, ImGuiTableFlags_SizingStretchProp, ui_vec2(0.0F, 0.0F), 0.0F)) {
        return;
    }
    for (u32 i = 0U; i <= NPH_AUDIO_SOURCE_BUS_COUNT; ++i) {
        const NphAudioBus bus = i < NPH_AUDIO_SOURCE_BUS_COUNT
                                    ? nph_engine_audio_bus(e, (NphAudioSourceBus)i)
                                    : NPH_AUDIO_MASTER_BUS;
        f32 peak = 0.0F;
        f32 rms = 0.0F;
        nph_audio_bus_meter(mixer, bus, &peak, &rms);
        igTableNextRow(ImGuiTableRowFlags_None, 0.0F);
        igTableNextColumn();
        igTextUnformatted(names[i], NULL);
        igTableNextColumn();
        igText("peak %6.1f dB  rms %6.1f dB", meter_db(peak), meter_db(rms));
        igTableNextColumn();
        NphCvar *cv = nph_cvar_find(reg, nph_str_from_cstr(vars[i]));
        if (cv != NULL) {
            igPushID_Int((int)i);
            cvar_value(reg, cv);
            igPopID();
        }
    }
    igEndTable();
}

void editor_panel_stats(Editor *ed)
{
    if (!igBegin(EDITOR_WINDOW_STATS, &ed->show.stats, ImGuiWindowFlags_None)) {
        igEnd();
        return;
    }
    frame_stats(ed);
    physics(ed);
    audio(ed);
    cvars(ed);
    igEnd();
}
