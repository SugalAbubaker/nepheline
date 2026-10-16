# 0025. The editor's interface: Dear ImGui through cimgui, on our own backends, as an engine host

- **Date:** 2026-10-16
- **Status:** Accepted

## Context

CLAUDE.md §8.13 says the editor is "a separate executable that links the engine. UI
is built with cimgui and uses our Vulkan RHI as its backend". §3 allows
cimgui (with Dear ImGui) for the editor only: "C++ compiled only for the
editor target. Never in the runtime". §1.8 keeps every third-party header
inside the one module that owns its library, so GLFW stays in `platform`
and Vulkan stays in `gpu`.

Three facts about the library shaped the choices:

- cimgui stopped tagging releases at v1.66.2. Its `docking_inter` branch
  follows Dear ImGui's docking branch, and the editor's panels need docking.
- Dear ImGui 1.92 moved texture management to the backend
  (`ImGuiBackendFlags_RendererHasTextures`). The font atlas now grows glyph
  by glyph, and the backend creates, updates, and destroys textures by
  rectangle.
- Dear ImGui's allocator hooks and its "current context" are process-wide.

The engine already gives hosts hooks (ADR-0023): `event`, `sync`,
`scene_target`, `render`, and `world_replaced`. It also already chooses a
UNORM swapchain, because the renderer encodes sRGB itself.

## Decision

1. **The pin.** cimgui is pinned at `docking_inter` 125f397e0982, and its
   own submodule at Dear ImGui 1.92.9b, docking branch (b48d1afb). Both are
   built as one static library, `nph_cimgui`, only when `NPH_BUILD_EDITOR`
   is on, with warnings off. ImGui's own configuration keeps its asserts,
   and its Win32 and shell helpers are disabled. C code sees cimgui's C
   declarations (`CIMGUI_DEFINE_ENUMS_AND_STRUCTS`) through one header,
   `editor/src/imgui_c.h`.
2. **Our own platform backend.** It is built on `platform/window.h`
   events:
   - `NphKey` maps to `ImGuiKey`.
   - Modifier state is tracked from the key events per side, and
     forgotten when the window loses focus.
   - The clipboard goes through `nph_window_get/set_clipboard`.
   - The display size and framebuffer scale come from the window.
   - Without a window (an offscreen editor in tests), the caller gives the
     display size, and events arrive the same way.
3. **Our own renderer backend on the RHI.**
   - Draw commands name textures by `ImTextureID`: the bindless sampled
     index plus one, with bit 32 marking images whose alpha means nothing
     (the scene viewport).
   - Vertices are pulled through a buffer device address, so the pipeline
     has no vertex input. Each frame slot has host-visible vertex and index
     buffers, grown by doubling.
   - There is one alpha-blended pipeline for the target's format.
   - Textures follow Dear ImGui 1.92's protocol. Updates upload only the
     changed rectangles, through a new GPU call,
     `nph_gpu_upload_image_region`.
   - One render-graph pass clears the backbuffer and draws the UI. It
     declares the viewport image as a sampled read, so the graph orders
     the scene's passes before it.
4. **UNORM everywhere.** The viewport image is R8G8B8A8_UNORM like the
   swapchain. The renderer writes sRGB-encoded values into it, and Dear
   ImGui's colours, which are authored in sRGB, are written unchanged.
5. **The editor is an engine host.**
   - `editor_create` makes a shell that fills `NphEngineDesc.hooks`.
   - `editor_attach` brings up Dear ImGui and the backends on the created
     engine.
   - `editor_detach` releases them before the engine goes.
   - The UI is built in the `sync` hook, at the sync point, where the
     world is quiescent. Panels may therefore read and edit the world
     there, which the scene-editing panels will do.
   - The UI is drawn in the `render` hook with the rest of the frame,
     while the next simulation step runs.
   - The scene renders into the viewport's image (`scene_target`), which
     is recreated when the viewport panel changes size.
6. **Input belongs to the editor.**
   - The `event` hook feeds Dear ImGui and consumes the event.
   - While playing, a click on the viewport gives the game the input:
     events then pass to the engine untouched.
   - Escape takes the input back. The engine's input gets a synthetic
     focus loss at that point, so keys held during play do not stay down.
   - The editor camera reads Dear ImGui's input state while the viewport
     is hovered or a drag that began there is held. The camera orbits,
     looks, pans, dollies, and flies, with a 64-bit pivot (§8.2).
7. **Dear ImGui's memory.** It goes through a tracked allocator
   (`editor.imgui` in the profiler's memory panel), installed while the
   editor is attached. The previous hooks are restored on detach, and
   leaked blocks are reported. Because the hooks are process-wide, there
   is one editor per process.
8. **The layout.** It lives in the file the `editor.layout` cvar names (an
   empty value means none, as in tests). When no layout exists, a default
   docked one is built: the viewport in the centre, statistics on the
   right, and the console and profiler below.

## Alternatives considered

- **Dear ImGui's own GLFW and Vulkan backends.** They are C++, they include
  GLFW and Vulkan headers directly (§1.8), and they manage their own
  descriptor pools and pipelines beside our RHI's bindless model and
  render graph. Our two backends are about 900 lines of C and keep the
  validation, synchronisation, and leak tooling.
- **Our own immediate-mode UI, or a C library (Nuklear, microui).** The
  first is a large project outside the engine's goals. The second lacks
  docking and is not the library §3 names.
- **An sRGB swapchain.** Dear ImGui's colours would need linearising in
  the shader, and the viewport image would need an sRGB view to keep the
  renderer's encoding. UNORM end to end has no conversions.
- **The scene in the backbuffer under the UI.** That cannot be docked,
  resized, or placed in a panel. The viewport image costs one extra
  sampled read of a panel-sized image per frame.

## Consequences

- C++ is compiled for the editor and its tests only, and the runtime stays
  C. The editor needs `git submodule update --init --recursive`, because
  cimgui has its own submodule. CMake checks for it.
- ImGui draw callbacks are not supported. One is skipped with a warning,
  logged once.
- The platform layer has no cursor shapes yet, so resize handles do not
  change the pointer. The editor uses only `NPH_CURSOR_MODE_DISABLED` while
  looking around. Cursor shapes are a follow-up in `platform/window.h`.
- Resizing the viewport recreates its image every frame the size changes,
  and the renderer reallocates its targets with it. This is acceptable
  while a splitter is dragged.
- The editor's tests run an OFFSCREEN engine. The UI draws into the
  engine's offscreen target, and events are fed directly, so CI covers the
  editor on lavapipe.
