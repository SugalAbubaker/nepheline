// SPDX-License-Identifier: Apache-2.0
#pragma once

// Window internals shared by window_glfw.c and the white-box tests, with no
// OS header (platform_internal.h brings <windows.h>).

#include <nepheline/platform/window.h>

// Queues an NPH_WINDOW_EVENT_DROP for `count` paths, copying them, as the
// window system's drop callback does. The tests call it because no OS lets
// a program drop files on its own window.
void nph_window_push_drop(NphWindow *w, u32 count, const char *const *paths);
