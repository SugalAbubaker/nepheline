# Editor (`editor/`)

The editor is the engine's last host (CLAUDE.md §8.13). It is a separate
executable, `nph_editor`, linked against the engine's shared library like
the player and the game modules. It draws a docked Dear ImGui interface
over the engine's frame. Decision: [ADR-0025](adr/0025-editor-ui.md) covers
the library pin, the backends, the host model, and input.

This is editor v1's first part: the shell, the viewport and its camera,
play-in-editor controls, the console, the profiler, and the statistics.
Scene editing follows: the outliner, the reflection-driven inspector,
gizmos, undo and redo, import, and primitives.

| File | What |
|---|---|
| `src/editor.h` | the editor: creation, the engine's hooks, attach and detach, test hooks |
| `src/camera.h` | the editor camera: orbit, look, pan, dolly, fly, focus |
| `src/editor_log.h` | the editor's log category, the console panel's lines, and the log sink |
| `src/ui_platform.h` | Dear ImGui's platform backend on `platform/window.h` |
| `src/ui_renderer.h` | Dear ImGui's renderer backend on the RHI |
| `src/panel_*.c` | the panels: viewport, console, profiler, statistics |
| `src/main.c` | `nph_editor` |

## Running

```sh
nph_editor --pack=build/win-msvc/samples/asset_viewer/Debug/showcase.npk
nph_editor --game=build/win-msvc/bin/Debug/game_sandbox.dll --scene=level.nscene
```

The engine's options and cvars apply (`--help`, and `cvars` in the
console). The edited world never simulates. **Play** (F5) plays a copy of
it, keeping every entity's handle. **Stop** (F5 again) discards the copy
and shows the edited world again. **Pause** is F6 and **Step** is F7.
File > Open Scene and Save Scene read and write scene files (Ctrl+S
saves).

The editor adds one cvar:

| Cvar | Default | What |
|---|---|---|
| `editor.layout` | `nph_editor_layout.ini` | the file the window layout is kept in; empty: none (tests) |

Exit status: 0; 1 on a failure (validation errors included); 2 for a bad
command line; 77 without a window system or a Vulkan device.

## The panels

The default layout puts the viewport in the centre, the statistics on the
right, and the console and the profiler as tabs below it. View > Reset
Layout restores it, and the View menu shows or hides each panel.

**Viewport.** It shows the scene, rendered into an image the size of the
panel, with a toolbar holding Play or Stop, Pause or Resume, and Step.
While editing, the camera is the editor's:

| Input | Does |
|---|---|
| right drag | look around; W A S D move, Q E go down and up, Shift is faster, the wheel changes the speed |
| Alt + left drag | orbit the pivot |
| middle drag | pan |
| wheel | move toward or away from the pivot |

The view keeps the world's exposure and tonemapper, from its active
Camera, and replaces only the camera. While playing, the view is the
world's own camera. A click on the scene gives the game the input, and
Escape takes it back.

**Console.**
- It shows the log, coloured by level, and can be filtered by level and
  by text.
- The command line feeds the engine's console (`runtime/console.h`):
  commands and cvars, Up and Down for the history, and Tab to complete
  the name being typed (one match completes, several extend to the
  common prefix and are listed).
- A sink on the logger thread fills a buffer under a mutex, and the
  editor moves the lines into the panel once per frame. If the editor
  falls behind, a warning line counts the dropped lines.

**Profiler.** It shows the profile collector's history
(`profile/profiler.h`):
- a frame-time graph;
- a **timeline** of the newest complete frame, with one band per thread
  and one row per zone depth. Hovering a zone shows its name, its
  duration, and whether it was cut at a frame or fiber boundary;
- the **CPU zones** and the render graph's **GPU passes** (time and
  declared traffic) over the history window;
- the tracked allocators' **memory**. Dear ImGui's own memory appears as
  `editor.imgui`.

Pause keeps the frame on screen. Save trace writes the Chrome trace
(chrome://tracing, Perfetto) of the whole history.

**Statistics.**
- The last frame's times: the frame, the sync point with its wait for the
  simulation, and rendering.
- The steps, the entity count, and the renderer's and the scene mirror's
  counts.
- Every cvar, edited in place through the registry, with its type, range,
  read-only, and cheat rules. Hovering a cvar's name shows its
  description.

## How it works

The editor is built in steps, because the engine needs the hooks before
the editor can use the engine:

```c
editor_create(&desc, &ed);                    // the shell
editor_engine_hooks(ed, &engine_desc.hooks);
nph_engine_create(&engine_desc, &engine);
editor_attach(ed, engine);                    // Dear ImGui, its backends, the cvars
nph_engine_run(engine);
editor_detach(ed);                            // before the engine goes
nph_engine_destroy(engine);
editor_destroy(ed);
```

Each frame, the engine calls the editor's hooks
([runtime.md](runtime.md) "The frame"):

1. **event**: each window event goes to Dear ImGui and is consumed, unless
   the game has the input.
2. **sync**, at the sync point, where the world is quiescent:
   1. the log lines are drained;
   2. a Dear ImGui frame is built: the dock space, the menus, and the
      panels;
   3. the view is set (the editor's camera while editing, the world's
      while playing);
   4. the frame's draw data is finished.

   Panels may read and edit the world here.
3. **scene_target**: the renderer draws into the viewport's image. The
   viewport panel recreates the image when the panel changes size.
4. **render**: the UI renderer copies the draw data into this frame
   slot's buffers, applies Dear ImGui's texture requests, and adds one
   pass. That pass clears the backbuffer and draws the interface,
   sampling the viewport's image, which the graph orders after the
   scene's passes. The next simulation step runs meanwhile.

**The renderer backend** (`ui_renderer.c`):
- `ImTextureID` is the bindless sampled index plus one, so 0 stays "no
  texture". Bit 32 marks opaque images: the viewport's alpha is ignored.
- The vertex shader pulls `ImDrawVert`s through a buffer device address,
  so the pipeline has no vertex input.
- The font atlas is a texture Dear ImGui manages (1.92's
  `RendererHasTextures`). The backend creates it, uploads only the
  rectangles Dear ImGui marks as changed (`nph_gpu_upload_image_region`),
  and destroys it on request. Alpha8 textures become white RGBA.
- The swapchain and the viewport image are UNORM. The renderer writes
  sRGB-encoded values, and the interface's colours are written as they
  are.

**The platform backend** (`ui_platform.c`):
- It maps `NphKey` to `ImGuiKey`, with the ISO world keys as Oem102.
- It tracks both sides of each modifier from the key events, so releasing
  one side keeps the modifier while the other is held. All of them are
  forgotten when the window loses focus.
- It reads and writes the clipboard through the window system, and takes
  the display size and framebuffer scale from the window.

**Memory.**
- The editor allocates from its allocator.
- Dear ImGui allocates through a tracked allocator over it, installed
  while the editor is attached. The previous hooks are restored on
  detach, and any blocks left behind are reported.
- Dear ImGui's allocator hooks and current context are process-wide, so
  there is one editor per process.

## Threading model

Everything runs on the engine's thread (`NPH_MAIN_THREAD_ONLY`), except
the log sink, which runs on the logger thread and shares a mutex with the
per-frame drain. The UI is built at the sync point and recorded while the
next simulation step runs on the job system. It reads nothing the
simulation writes.

## Costs (§1.4)

- **Attach** allocates the Dear ImGui context, the console's 4096 lines
  (1.1 MB), and the profiler panel's segment buffer (32768 segments,
  768 KB).
- **A frame** allocates nothing in the editor's own code. Dear ImGui's
  buffers grow to their steady size and then stop.
- **The UI renderer** grows its buffers by doubling, and allocates a
  staging copy for each texture upload.
- **Reading the clipboard** grows a buffer to the text's size.

## Testing

`editor_tests` (25 tests):
- The camera's invariants, including a 64-bit pivot 10,000 km from the
  origin.
- The log lines and the sink. One test fills the sink from another thread
  while draining it, and checks every line arrives once, in order.
- The platform backend with a real Dear ImGui context and no GPU: the key
  map, modifiers, text, focus loss, the mouse, and when Dear ImGui wants
  the mouse.
- The editor on an OFFSCREEN engine under the validation gate, with the
  input fed as window events:
  - The interface and the cooked showcase in the viewport are drawn
    (checked by reading pixels back).
  - Drags and keys move the camera, and the engine renders from it.
  - Play hands the input to the game and Escape takes it back.
  - The console panel receives the log.
  - A headless engine is refused.

`editor.smoke` (label `sample`) runs `nph_editor` windowed for 60 frames
with no layout file, and is skipped (77) without a window system or a
device.

## Limitations and future work

- Scene editing comes next: the outliner, the inspector driven by
  component reflection, translate/rotate/scale gizmos, picking, undo and
  redo as a command stack, the asset browser with drag-and-drop import
  through the cooker (which needs file-drop events from the window
  system), and test scenes.
- Model creation comes after that: primitives and basic mesh editing,
  saved as cooked meshes.
- **Open Scene** clears the edited world before loading, so a scene that
  fails to load leaves it empty. The scene-editing work loads into a
  separate world and swaps it in.
- Scene paths are typed. There is no native file dialog: the window
  system has none, and the asset browser will cover content.
- The window system has no cursor shapes, so splitters and text fields
  keep the arrow pointer.
- Dear ImGui draw callbacks are not supported (skipped with a warning).
