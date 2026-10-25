// SPDX-License-Identifier: Apache-2.0
// miniaudio's implementation, in one translation unit compiled with the
// library's own warning policy (engine/audio/CMakeLists.txt). Its
// configuration (no decoding, mixing, or engine; the backends we use) is
// the miniaudio interface target's (third_party/CMakeLists.txt), so this
// unit and device.c, the only code that includes miniaudio.h, see the same
// structures. Memory comes from the device's allocator through the
// allocation callbacks device.c passes in.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
