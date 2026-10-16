// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor (CLAUDE.md §8.13; docs/editor.md; docs/adr/0025-editor-ui.md):
// a host of the engine (runtime/engine.h) that draws a docked Dear ImGui
// interface over the engine's frame, through the engine's hooks:
//
//   - event: window events go to ImGui; the game sees them only while it
//     has the input (playing, after a click in the viewport; Escape gives
//     it back);
//   - sync: at the sync point, where the world is quiescent, the editor
//     builds its UI (menus, the viewport, the console, the profiler, the
//     statistics) and sets the view: its own camera while editing, the
//     world's while playing;
//   - scene_target: the scene renders into the viewport's image, sized to
//     the viewport panel;
//   - render: the UI pass draws into the backbuffer, sampling that image.
//
// An editor is made in steps, because the engine needs the hooks before
// the editor can use the engine:
//
//     editor_create(&desc, &ed);
//     editor_engine_hooks(ed, &engine_desc.hooks);
//     nph_engine_create(&engine_desc, &engine);
//     editor_attach(ed, engine);
//     ... nph_engine_run(engine) ...
//     editor_detach(ed);           // before the engine goes
//     nph_engine_destroy(engine);
//     editor_destroy(ed);
//
// Until attached and after detaching, the hooks do nothing. An OFFSCREEN
// engine works too (tests): the UI draws into the engine's offscreen
// target, and input arrives through editor_feed_event.
//
// One editor at a time per process: Dear ImGui's allocator hooks are
// process-wide, and the editor installs its own while attached.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY: the engine's thread.

#include <nepheline/runtime/engine.h>

#include "camera.h"
#include "editor_log.h"

typedef struct Editor Editor;

typedef struct EditorDesc {
    NphAllocator *alloc;    // required
    EditorLogSink *log;     // the sink feeding the console panel (NULL: none)
    const char *shader_dir; // the editor's shaders (NULL: this build's)
} EditorDesc;

// NPH_RESULT_OUT_OF_MEMORY.
NPH_MUST_USE NphResult editor_create(const EditorDesc *desc, Editor **out);

// The hooks to create the engine with (user is the editor).
void editor_engine_hooks(Editor *ed, NphEngineHooks *out);

// Brings the editor up on `engine` (WINDOWED or OFFSCREEN): its cvars,
// Dear ImGui and its backends, and the viewport. NPH_RESULT_UNSUPPORTED for
// a HEADLESS engine; the shaders' (NOT_FOUND, IO_ERROR) and the device's
// errors. Nothing is left attached on failure.
NPH_MUST_USE NphResult editor_attach(Editor *ed, NphEngine *engine);

// Waits for the GPU, saves the layout (editor.layout), and releases what
// attach made. Safe when not attached.
void editor_detach(Editor *ed);

// Safe on NULL. Detaches first.
void editor_destroy(Editor *ed);

// Feeds a window event as the engine's event hook does; for an offscreen
// editor, which has no window. Returns whether the editor consumed it.
bool editor_feed_event(Editor *ed, const NphWindowEvent *ev);

// The display size of an offscreen editor (the engine's offscreen target
// size by default), in pixels.
void editor_set_display_size(Editor *ed, u32 width, u32 height);

// The viewport's scene image (null before the first frame lays it out).
NphGpuImage editor_viewport_image(const Editor *ed);

// Where the last frame showed the viewport's image, in display units, for
// tests that drive the editor with events. False before it was shown.
bool editor_viewport_rect(const Editor *ed, f32 *out_x, f32 *out_y, f32 *out_w, f32 *out_h);

// The editor's camera, which frames render from while not playing.
EditorCamera *editor_camera(Editor *ed);
