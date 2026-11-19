// SPDX-License-Identifier: Apache-2.0
// The Import Settings window (docs/editor.md "Import settings"; ADR-0107 decision 1): a scene's,
// an image's, or a sound's settings file, <source>.import.json, as widgets. A setting the file
// leaves out is shown as "Default", the cooker's. Save writes the file and cooks the source again;
// what the cook writes is reloaded where it is drawn, as an import's is.
#include <nepheline/core/assert.h>
#include <nepheline/platform/file.h>

#include "editor_internal.h"

#include <cook/cooker.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_PATH_MAX (EDITOR_PROJECT_PATH_MAX * 2U + 16U)

static bool has_settings(u32 kind)
{
    return kind == (u32)COOKER_SOURCE_SCENE || kind == (u32)COOKER_SOURCE_IMAGE ||
           kind == (u32)COOKER_SOURCE_SOUND;
}

// The window's source's settings file; false when the path does not fit.
static bool settings_path(const Editor *ed, char *out, u64 cap)
{
    const char *assets = editor_project_assets_dir(ed->project);
    return (u64)snprintf(out, (size_t)cap, "%s/%s.import.json", assets, ed->import_settings.key) <
           cap;
}

// What the settings file holds, read into the saved settings and shown; the cooker's, and why,
// when it does not read.
static void read_file(Editor *ed)
{
    EditorImportSettings *im = &ed->import_settings;
    cook_scene_import_default(&im->scene_saved);
    cook_texture_import_default(&im->texture_saved);
    cook_sound_settings_default(&im->sound_saved);
    im->error[0] = '\0';
    im->on_disk = false;
    char path[SETTINGS_PATH_MAX];
    NphResult r = settings_path(ed, path, sizeof path) ? NPH_RESULT_OK : NPH_RESULT_OVERFLOW;
    if (r == NPH_RESULT_OK && nph_fs_exists(path)) {
        im->on_disk = true;
        void *text = NULL;
        u64 size = 0U;
        r = nph_file_read_all(path, ed->alloc, &text, &size);
        if (r == NPH_RESULT_OK) {
            const NphStr json = nph_str((const char *)text, size);
            if (im->kind == (u32)COOKER_SOURCE_SCENE) {
                r = cook_scene_import_read(ed->alloc, json, &im->scene_saved);
            } else if (im->kind == (u32)COOKER_SOURCE_IMAGE) {
                r = cook_texture_import_read(ed->alloc, json, &im->texture_saved);
            } else {
                r = cook_sound_settings_parse(ed->alloc, text, size, &im->sound_saved);
            }
            nph_file_free_all(ed->alloc, text, size);
        }
    }
    if (r != NPH_RESULT_OK) {
        (void)snprintf(im->error, sizeof im->error,
                       "Its settings file does not read (%s): the cooker's settings are shown, "
                       "and Save replaces it.",
                       nph_result_string(r));
        cook_scene_import_default(&im->scene_saved);
        cook_texture_import_default(&im->texture_saved);
        cook_sound_settings_default(&im->sound_saved);
    }
    im->scene = im->scene_saved;
    im->texture = im->texture_saved;
    im->sound = im->sound_saved;
}

// Queues the source's cook by the settings now on disk (its result is reloaded with the rest).
static void cook_again(Editor *ed)
{
    const NphResult r = editor_project_cook(ed->project, ed->import_settings.key, false, 0U, NULL);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_EDITOR, "cooking %s: %s", ed->import_settings.key, nph_result_string(r));
    }
}

NphResult editor_import_settings_open(Editor *ed, const char *key)
{
    NPH_ASSERT(ed != NULL && key != NULL);
    if (ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    const u32 kind = (u32)cooker_source_kind(key);
    char source[SETTINGS_PATH_MAX];
    if (!has_settings(kind) || strlen(key) >= EDITOR_PROJECT_PATH_MAX ||
        (u64)snprintf(source, sizeof source, "%s/%s", editor_project_assets_dir(ed->project),
                      key) >= sizeof source) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    if (!nph_fs_exists(source)) {
        return NPH_RESULT_NOT_FOUND;
    }
    EditorImportSettings *im = &ed->import_settings;
    memset(im, 0, sizeof *im);
    (void)snprintf(im->key, sizeof im->key, "%s", key);
    im->kind = kind;
    im->open = true;
    im->reveal = true;
    read_file(ed);
    ed->show.import_settings = true;
    return NPH_RESULT_OK;
}

const char *editor_import_settings_key(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->import_settings.open ? ed->import_settings.key : "";
}

const char *editor_import_settings_error(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return ed->import_settings.open ? ed->import_settings.error : "";
}

bool editor_import_settings_changed(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    const EditorImportSettings *im = &ed->import_settings;
    if (!im->open) {
        return false;
    }
    if (im->kind == (u32)COOKER_SOURCE_SCENE) {
        return !cook_scene_import_equal(&im->scene, &im->scene_saved);
    }
    if (im->kind == (u32)COOKER_SOURCE_IMAGE) {
        return !cook_texture_import_equal(&im->texture, &im->texture_saved);
    }
    return !cook_sound_settings_equal(&im->sound, &im->sound_saved);
}

static bool showing(const Editor *ed, CookerSourceKind kind)
{
    return ed->import_settings.open && ed->import_settings.kind == (u32)kind;
}

const CookSceneImport *editor_import_settings_scene(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return showing(ed, COOKER_SOURCE_SCENE) ? &ed->import_settings.scene : NULL;
}

const CookTextureImport *editor_import_settings_texture(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return showing(ed, COOKER_SOURCE_IMAGE) ? &ed->import_settings.texture : NULL;
}

const CookSoundSettings *editor_import_settings_sound(const Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    return showing(ed, COOKER_SOURCE_SOUND) ? &ed->import_settings.sound : NULL;
}

// Whether settings are in range: what their writer takes (NPH_RESULT_CORRUPT otherwise).
static NphResult in_range(Editor *ed, u32 kind, const void *s)
{
    NphStrBuilder sb;
    nph_sb_init(&sb, ed->alloc);
    NphResult r = NPH_RESULT_OK;
    if (kind == (u32)COOKER_SOURCE_SCENE) {
        r = cook_scene_import_write((const CookSceneImport *)s, &sb);
    } else if (kind == (u32)COOKER_SOURCE_IMAGE) {
        r = cook_texture_import_write((const CookTextureImport *)s, &sb);
    } else {
        r = cook_sound_settings_write((const CookSoundSettings *)s, &sb);
    }
    nph_sb_free(&sb);
    return r == NPH_RESULT_CORRUPT ? NPH_RESULT_INVALID_ARGUMENT : r;
}

NphResult editor_import_settings_change_scene(Editor *ed, const CookSceneImport *s)
{
    NPH_ASSERT(ed != NULL && s != NULL);
    if (!showing(ed, COOKER_SOURCE_SCENE)) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(in_range(ed, (u32)COOKER_SOURCE_SCENE, s));
    ed->import_settings.scene = *s;
    return NPH_RESULT_OK;
}

NphResult editor_import_settings_change_texture(Editor *ed, const CookTextureImport *s)
{
    NPH_ASSERT(ed != NULL && s != NULL);
    if (!showing(ed, COOKER_SOURCE_IMAGE)) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(in_range(ed, (u32)COOKER_SOURCE_IMAGE, s));
    ed->import_settings.texture = *s;
    return NPH_RESULT_OK;
}

NphResult editor_import_settings_change_sound(Editor *ed, const CookSoundSettings *s)
{
    NPH_ASSERT(ed != NULL && s != NULL);
    if (!showing(ed, COOKER_SOURCE_SOUND)) {
        return NPH_RESULT_INVALID_STATE;
    }
    NPH_TRY(in_range(ed, (u32)COOKER_SOURCE_SOUND, s));
    ed->import_settings.sound = *s;
    return NPH_RESULT_OK;
}

NphResult editor_import_settings_save(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorImportSettings *im = &ed->import_settings;
    if (!im->open || ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    char path[SETTINGS_PATH_MAX];
    NphResult r = settings_path(ed, path, sizeof path) ? NPH_RESULT_OK : NPH_RESULT_OVERFLOW;
    if (r == NPH_RESULT_OK && im->kind == (u32)COOKER_SOURCE_SCENE) {
        r = cook_scene_import_save(&im->scene, ed->alloc, path);
    } else if (r == NPH_RESULT_OK && im->kind == (u32)COOKER_SOURCE_IMAGE) {
        r = cook_texture_import_save(&im->texture, ed->alloc, path);
    } else if (r == NPH_RESULT_OK) {
        r = cook_sound_settings_save(&im->sound, ed->alloc, path);
    }
    if (r != NPH_RESULT_OK) {
        (void)snprintf(im->error, sizeof im->error, "Its settings file was not written: %s",
                       nph_result_string(r));
        NPH_LOG_ERROR(&LOG_EDITOR, "%s.import.json: %s", im->key, nph_result_string(r));
        return r;
    }
    im->error[0] = '\0';
    im->on_disk = true;
    im->scene_saved = im->scene;
    im->texture_saved = im->texture;
    im->sound_saved = im->sound;
    NPH_LOG_INFO(&LOG_EDITOR, "%s: import settings saved; cooking it again", im->key);
    cook_again(ed);
    return NPH_RESULT_OK;
}

NphResult editor_import_settings_use_cooker(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    EditorImportSettings *im = &ed->import_settings;
    if (!im->open || ed->project == NULL) {
        return NPH_RESULT_INVALID_STATE;
    }
    char path[SETTINGS_PATH_MAX];
    NphResult r = settings_path(ed, path, sizeof path) ? NPH_RESULT_OK : NPH_RESULT_OVERFLOW;
    if (r == NPH_RESULT_OK && nph_fs_exists(path)) {
        r = nph_fs_remove_file(path);
    }
    if (r != NPH_RESULT_OK) {
        (void)snprintf(im->error, sizeof im->error, "Its settings file was not removed: %s",
                       nph_result_string(r));
        NPH_LOG_ERROR(&LOG_EDITOR, "%s.import.json: %s", im->key, nph_result_string(r));
        return r;
    }
    read_file(ed); // the cooker's, with no file
    NPH_LOG_INFO(&LOG_EDITOR, "%s: the cooker's settings; cooking it again", im->key);
    cook_again(ed);
    return NPH_RESULT_OK;
}

void editor_import_settings_revert(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    if (ed->import_settings.open && ed->project != NULL) {
        read_file(ed);
    }
}

void editor_import_settings_close(Editor *ed)
{
    NPH_ASSERT(ed != NULL);
    memset(&ed->import_settings, 0, sizeof ed->import_settings);
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

static void tooltip(const char *text)
{
    if (igIsItemHovered(ImGuiHoveredFlags_None)) {
        igSetTooltip("%s", text);
    }
}

// A setting the file may leave out: "Default" (the cooker's, `cooker` said beside it), or one of
// `names`. True when it was changed.
static bool optional_choice(const char *label, b32 *has, u32 *value, const char *const *names,
                            u32 count, const char *cooker)
{
    char preview[96];
    if (*has != 0 && *value < count) {
        (void)snprintf(preview, sizeof preview, "%s", names[*value]);
    } else {
        (void)snprintf(preview, sizeof preview, "Default (%s)", cooker);
    }
    bool changed = false;
    if (igBeginCombo(label, preview, ImGuiComboFlags_None)) {
        char item[96];
        (void)snprintf(item, sizeof item, "Default (%s)", cooker);
        if (igSelectable_Bool(item, *has == 0, ImGuiSelectableFlags_None, ui_vec2(0.0F, 0.0F))) {
            changed = *has != 0;
            *has = 0;
        }
        for (u32 i = 0; i < count; ++i) {
            if (igSelectable_Bool(names[i], *has != 0 && *value == i, ImGuiSelectableFlags_None,
                                  ui_vec2(0.0F, 0.0F))) {
                changed = changed || *has == 0 || *value != i;
                *has = 1;
                *value = i;
            }
        }
        igEndCombo();
    }
    return changed;
}

// An on-or-off setting the file may leave out.
static bool optional_switch(const char *label, b32 *has, b32 *on, const char *cooker)
{
    static const char *const names[2] = {"Off", "On"};
    u32 value = *on != 0 ? 1U : 0U;
    const bool changed = optional_choice(label, has, &value, names, 2U, cooker);
    *on = value != 0U ? 1 : 0;
    return changed;
}

// The largest side an image is cut to, which the file may leave out (0: no cap).
static bool optional_extent(const char *label, b32 *has, u32 *extent)
{
    static const char *const names[] = {"No cap", "64",   "128",  "256",  "512",
                                        "1024",   "2048", "4096", "8192", "16384"};
    u32 index = 0U;
    for (u32 i = 1; i < NPH_ARRAY_COUNT(names); ++i) {
        index = *extent == (32U << i) ? i : index;
    }
    if (*has != 0 && *extent != 0U && index == 0U) {
        // A value of the file's own that is not in the list: shown as it is until another is
        // chosen.
        igText("%s: %u", label, *extent);
        igSameLine(0.0F, -1.0F);
        if (igSmallButton("Change")) {
            *extent = 2048U;
            return true;
        }
        return false;
    }
    const bool changed =
        optional_choice(label, has, &index, names, NPH_ARRAY_COUNT(names), "no cap");
    if (changed) {
        *extent = index == 0U ? 0U : 32U << index;
    }
    return changed;
}

static void scene_widgets(CookSceneImport *s)
{
    static const char *const kinds[COOK_COLLISION_KIND_COUNT] = {"None", "Mesh (triangles)",
                                                                 "Hull (convex)"};
    static const CookCollisionKind kind_of[COOK_COLLISION_KIND_COUNT] = {
        COOK_COLLISION_NONE, COOK_COLLISION_MESH, COOK_COLLISION_HULL};
    u32 kind = (u32)s->collision;
    (void)optional_choice("Collision", &s->has_collision, &kind, kinds, COOK_COLLISION_KIND_COUNT,
                          "none");
    s->collision = kind < COOK_COLLISION_KIND_COUNT ? kind_of[kind] : COOK_COLLISION_NONE;
    tooltip("What the scene's meshes collide as where neither a UCX_ proxy nor an object's "
            "collision property says: their triangles (static world) or a convex hull (props)");
    bool set = s->has_hull_vertices != 0;
    if (igCheckbox("##hull_set", &set)) {
        s->has_hull_vertices = set ? 1 : 0;
        s->hull_vertices =
            set && s->hull_vertices == 0U ? COOK_SCENE_HULL_VERTICES_DEFAULT : s->hull_vertices;
    }
    tooltip("Set: the hulls' most points; clear: the default, 32");
    igSameLine(0.0F, -1.0F);
    igBeginDisabled(!set);
    int vertices = (int)(set ? s->hull_vertices : COOK_SCENE_HULL_VERTICES_DEFAULT);
    if (igSliderInt("Hull points", &vertices, (int)COOK_SCENE_HULL_VERTICES_MIN,
                    (int)COOK_SCENE_HULL_VERTICES_MAX, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        s->hull_vertices = (u32)vertices;
    }
    igEndDisabled();
    igSeparator();
    (void)optional_switch("Levels of detail", &s->has_lods, &s->lods, "the cooker's");
    tooltip("The meshes' levels of detail (cook/mesh.h)");
    (void)optional_switch("Voxels", &s->has_voxels, &s->voxels, "the cooker's");
    tooltip("Voxel levels for aggregate geometry such as foliage (ADR-0061)");
    igSeparator();
    igTextDisabled("The scene's images");
    (void)optional_extent("Largest side", &s->has_texture_max_extent, &s->texture_max_extent);
    static const char *const compression[2] = {"Compressed (BC)", "Uncompressed"};
    u32 raw = s->texture_uncompressed != 0 ? 1U : 0U;
    (void)optional_choice("Compression", &s->has_texture_uncompressed, &raw, compression, 2U,
                          "compressed");
    s->texture_uncompressed = raw != 0U ? 1 : 0;
    (void)optional_switch("Mip levels", &s->has_texture_mips, &s->texture_mips, "on");
}

static void texture_widgets(CookTextureImport *s)
{
    static const char *const usages[] = {"Colour", "Linear", "Normal map", "Mask", "HDR"};
    static const CookTextureUsage usage_of[] = {COOK_TEXTURE_COLOR, COOK_TEXTURE_LINEAR,
                                                COOK_TEXTURE_NORMAL, COOK_TEXTURE_MASK,
                                                COOK_TEXTURE_HDR};
    u32 usage = (u32)s->usage;
    (void)optional_choice("Usage", &s->has_usage, &usage, usages, NPH_ARRAY_COUNT(usages),
                          "colour");
    s->usage = usage < NPH_ARRAY_COUNT(usage_of) ? usage_of[usage] : COOK_TEXTURE_COLOR;
    tooltip("What the image holds when it is cooked alone; a material's slot says it otherwise");
    (void)optional_extent("Largest side", &s->has_max_extent, &s->max_extent);
    static const char *const compression[2] = {"Compressed (BC)", "Uncompressed"};
    u32 raw = s->uncompressed != 0 ? 1U : 0U;
    (void)optional_choice("Compression", &s->has_uncompressed, &raw, compression, 2U, "compressed");
    s->uncompressed = raw != 0U ? 1 : 0;
    (void)optional_switch("Mip levels", &s->has_mips, &s->mips, "on");
    static const char *const greens[2] = {"Up (OpenGL, glTF)", "Down (DirectX)"};
    int green = s->green_down != 0 ? 1 : 0;
    if (igCombo_Str_arr("Normal map green", &green, greens, 2, -1)) {
        s->green_down = green != 0 ? 1 : 0;
    }
    tooltip("Which way a normal map's green points; the cooker turns DirectX's to OpenGL's");
}

static void sound_widgets(CookSoundSettings *s)
{
    static const char *const codecs[] = {"Auto", "PCM", "Lossless", "ADPCM"};
    static const char *const streams[] = {"Auto (long sounds)", "Always", "Never"};
    static const CookSoundCodec codec_of[] = {COOK_SOUND_CODEC_AUTO, COOK_SOUND_CODEC_PCM,
                                              COOK_SOUND_CODEC_LOSSLESS, COOK_SOUND_CODEC_ADPCM};
    static const CookSoundStream stream_of[] = {COOK_SOUND_STREAM_AUTO, COOK_SOUND_STREAM_ALWAYS,
                                                COOK_SOUND_STREAM_NEVER};
    int codec = (int)s->codec;
    if (igCombo_Str_arr("Codec", &codec, codecs, (int)NPH_ARRAY_COUNT(codecs), -1) && codec >= 0 &&
        (u32)codec < NPH_ARRAY_COUNT(codec_of)) {
        s->codec = codec_of[codec];
    }
    tooltip("Auto: lossless where it holds the samples exactly, else PCM");
    int stream = (int)s->stream;
    if (igCombo_Str_arr("Streaming", &stream, streams, (int)NPH_ARRAY_COUNT(streams), -1) &&
        stream >= 0 && (u32)stream < NPH_ARRAY_COUNT(stream_of)) {
        s->stream = stream_of[stream];
    }
    igBeginDisabled(s->stream != COOK_SOUND_STREAM_AUTO);
    f32 seconds = s->stream_seconds != 0.0F ? s->stream_seconds : COOK_SOUND_STREAM_SECONDS;
    if (igInputFloat("Stream from (s)", &seconds, 0.5F, 5.0F, "%.2f", ImGuiInputTextFlags_None)) {
        s->stream_seconds = seconds < 0.1F ? 0.1F : (seconds > 3600.0F ? 3600.0F : seconds);
    }
    igEndDisabled();
    bool exhaustive = s->exhaustive != 0;
    if (igCheckbox("Exhaustive search (lossless)", &exhaustive)) {
        s->exhaustive = exhaustive ? 1 : 0;
    }
    tooltip("Every prediction order: a slower cook, a smaller file");
}

void editor_panel_import_settings(Editor *ed)
{
    EditorImportSettings *im = &ed->import_settings;
    if (im->reveal) {
        im->reveal = false;
        igSetNextWindowFocus();
    }
    bool show = ed->show.import_settings;
    if (igBegin(EDITOR_WINDOW_IMPORT, &show, ImGuiWindowFlags_None)) {
        if (!im->open || ed->project == NULL) {
            igTextDisabled("No source is open.");
            igTextWrapped("Right-click a scene, an image, or a sound in the Assets panel and "
                          "choose Import Settings.");
        } else {
            igTextUnformatted(im->key, NULL);
            igTextDisabled(im->on_disk ? "from its settings file" : "the cooker's settings");
            if (im->error[0] != '\0') {
                igTextColored(ui_vec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", im->error);
            }
            igSeparator();
            if (im->kind == (u32)COOKER_SOURCE_SCENE) {
                scene_widgets(&im->scene);
            } else if (im->kind == (u32)COOKER_SOURCE_IMAGE) {
                texture_widgets(&im->texture);
            } else {
                sound_widgets(&im->sound);
            }
            igSeparator();
            const bool changed = editor_import_settings_changed(ed);
            igBeginDisabled(!changed && im->on_disk && im->error[0] == '\0');
            if (igButton("Save", ui_vec2(0.0F, 0.0F))) {
                NPH_IGNORE_RESULT(editor_import_settings_save(ed)); // logged and shown
            }
            igEndDisabled();
            tooltip("Writes the settings file and cooks the source again");
            igSameLine(0.0F, -1.0F);
            igBeginDisabled(!changed);
            if (igButton("Revert", ui_vec2(0.0F, 0.0F))) {
                editor_import_settings_revert(ed);
            }
            igEndDisabled();
            igSameLine(0.0F, -1.0F);
            igBeginDisabled(!im->on_disk);
            if (igButton("Use the Cooker's", ui_vec2(0.0F, 0.0F))) {
                NPH_IGNORE_RESULT(editor_import_settings_use_cooker(ed)); // logged and shown
            }
            igEndDisabled();
            tooltip("Removes the settings file: the source cooks again by the cooker's settings");
        }
    }
    igEnd();
    if (!show && ed->show.import_settings) {
        editor_import_settings_close(ed); // the window closed: changes not saved are dropped
    }
    ed->show.import_settings = show;
}
