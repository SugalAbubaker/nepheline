# 0012. Window and input: GLFW behind `nph_window_*`, an event queue, and the action layer

- **Date:** 2026-10-12
- **Status:** Accepted

## Context

CLAUDE.md §3 allows GLFW for "window, input, monitors", vendored as a
pinned submodule and wrapped by `platform` (`nph_window_*`, `nph_input_*`);
§1.8 forbids any other engine code from including a third-party header.
§8.3 asks for "a GLFW wrapper exposing `nph_window_*` and an event queue.
Input is mapped to an action layer (keyboard, mouse, gamepad)". §12's
Phase 3 exit criterion is "a window opens and input is logged on Windows
and Linux", and the renderer (Phase 5) needs a Vulkan surface from the
window without GLFW leaking into `gpu`.

## Decision

1. **GLFW 3.4 is a git submodule in `third_party/glfw`, built from source
   as part of the tree** (`third_party/CMakeLists.txt`: examples, tests,
   docs, and install off; static). It is added with CMake's `SYSTEM`
   keyword so the engine's warning policy never applies to it, and it is
   a *private* dependency of `nph_platform`: `src/window_glfw.c` is the
   only translation unit in the engine that includes `GLFW/glfw3.h`.
   `git submodule update --init` is a build prerequisite; Linux needs the
   X11 and Wayland development packages (both backends are built; GLFW
   picks one at run time).
2. **The window API is a thin, typed façade.** `NphWindow` is opaque and
   owns a fixed ring of 1024 `NphWindowEvent`s (32 bytes each, no
   pointers) filled by GLFW's callbacks during `nph_window_poll_events`
   and drained by the owner; a full queue drops the oldest events and
   counts them. Keys are the engine's own `NphKey` enum (US-layout
   identities; text arrives separately as code points), mapped from GLFW's
   codes by a table; modifiers, actions, mouse buttons, and the gamepad
   layout are engine enums with their own values. Monitors, fullscreen
   switching (restoring the windowed placement), cursor modes, raw mouse
   motion, the clipboard, and window state are exposed as plain functions.
   The window system is initialised once per process on the main thread,
   which is also the only thread that creates windows and pumps events
   (an OS rule); `NPH_RESULT_UNSUPPORTED` from `nph_window_system_init`
   means a headless machine.
3. **Gamepads are polled, not event driven.** `nph_window_get_gamepad_state`
   returns a snapshot in the standard (SDL/GLFW) layout; the input layer
   samples every pad once per frame, and a connection change is simply the
   `connected` flag changing. This avoids a global window list for
   routing joystick callbacks (GLFW's joystick callback is not per window)
   and matches how gameplay reads sticks anyway.
4. **Vulkan surfaces come from the window, without Vulkan headers.**
   `nph_window_vulkan_extensions` and `nph_window_vulkan_create_surface`
   wrap `glfwGetRequiredInstanceExtensions` and `glfwCreateWindowSurface`;
   handles cross the boundary as `void *` and `u64`. GLFW declares its
   Vulkan entry points only when the Vulkan headers precede it, and the
   engine takes no Vulkan header dependency before Phase 5, so
   `window_glfw.c` declares the two functions it needs with ABI-identical
   types (pointer-sized handles, an int-sized result). Phase 5, which adds
   the SDK as a build dependency, may switch to `GLFW_INCLUDE_VULKAN`.
5. **The action layer is independent of the window system.** `NphInput`
   consumes `NphWindowEvent`s and gamepad snapshots and keeps per-frame
   state (held, pressed, released; cursor deltas; scroll; typed text). Named
   actions have any number of bindings, each a small record: a key with
   modifiers (exact, or `NPH_MOD_ANY`), a mouse button, a key pair as an
   axis, a mouse axis with a scale, a gamepad button, or a gamepad axis
   with a dead zone (rescaled so the value starts at 0 at the edge) on one
   pad or any. Digital bindings contribute ±1 and analog ones their value
   to a sum clamped to [-1, 1]; mouse deltas pass through scaled and
   unclamped. Losing focus releases every held key and button, since the
   release events will never arrive. The layer allocates its tables once
   from the allocator it is given and is tested entirely with synthetic
   events, with no display.
6. **CI opens real windows.** The window tests create a hidden window and
   the `platform_window` sample a visible one for 30 frames, on Windows
   runners directly and on Linux runners under Xvfb (`xvfb-run`); a
   machine without a display skips the window tests rather than failing,
   and the sample fails, so CI cannot pass vacuously.

## Alternatives considered

- **SDL3 instead of GLFW.** Rejected by CLAUDE.md §3, and SDL brings
  audio, rendering, and much else the engine writes itself.
- **A per-event callback API instead of a queue.** Rejected: callbacks run
  inside the OS message loop where gameplay must not; a queue drained at
  a known point per frame keeps input deterministic and recordable
  (replay for the determinism tests is a queue dump).
- **Using GLFW's key codes directly as `NphKey`.** Rejected: §1.8; the
  engine's enums are contiguous (table lookups), and the wrapper is the
  one place that changes if the library does.
- **Allocating event strings (file drops).** Not supported in this
  version; the editor adds a callback-based drop API in Phase 9.

## Consequences

- `third_party/` exists from this phase with its first submodule; every
  later library (volk, VMA, cgltf, stb_image, miniaudio, cimgui) follows
  the same pattern: pinned, SYSTEM, wrapped by one module.
- Phase 5 creates the swapchain surface through `nph_window_vulkan_*`
  and sizes it from `NPH_WINDOW_EVENT_FRAMEBUFFER_RESIZE`.
- Phase 9's editor builds its shortcuts and gizmo interaction on the
  action layer and the raw queries; text fields use the typed code points.
- Linux CI installs `libwayland-dev libxkbcommon-dev wayland-protocols
  xorg-dev xvfb`, and every workflow checks out submodules.
