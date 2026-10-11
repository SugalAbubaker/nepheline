// SPDX-License-Identifier: Apache-2.0
#pragma once

// Engine version. This header is the single source of truth: CMake reads the
// three NPH_VERSION_* macros (cmake/NphVersion.cmake) rather than generating
// them, so the build and the binary can never disagree.
//
// Versioning follows SemVer (CLAUDE.md §7): v0.1.0 is tagged at the end of
// Phase 4, v1.0.0 at the end of Phase 15.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NPH_VERSION_MAJOR 0
#define NPH_VERSION_MINOR 0
#define NPH_VERSION_PATCH 1

#define NPH_VERSION_STRINGIZE_(x) #x
#define NPH_VERSION_STRINGIZE(x)  NPH_VERSION_STRINGIZE_(x)

// "major.minor.patch"
#define NPH_VERSION_STRING                                                                         \
    NPH_VERSION_STRINGIZE(NPH_VERSION_MAJOR)                                                       \
    "." NPH_VERSION_STRINGIZE(NPH_VERSION_MINOR) "." NPH_VERSION_STRINGIZE(NPH_VERSION_PATCH)

// Packs a version into one 32-bit integer using the Vulkan layout
// (10-bit major, 10-bit minor, 12-bit patch) so versions compare with `<`.
#define NPH_MAKE_VERSION(major, minor, patch)                                                      \
    (((uint32_t)(major) << 22U) | ((uint32_t)(minor) << 12U) | (uint32_t)(patch))

#define NPH_VERSION_GET_MAJOR(v) ((uint32_t)(v) >> 22U)
#define NPH_VERSION_GET_MINOR(v) (((uint32_t)(v) >> 12U) & 0x3FFU)
#define NPH_VERSION_GET_PATCH(v) ((uint32_t)(v) & 0xFFFU)

// The version this header was compiled against.
#define NPH_VERSION NPH_MAKE_VERSION(NPH_VERSION_MAJOR, NPH_VERSION_MINOR, NPH_VERSION_PATCH)

// Returns the packed version of the linked nph_core library. Compare against
// NPH_VERSION to detect a header/library mismatch.
// Thread-safety: safe from any thread. Never fails, never allocates.
uint32_t nph_version(void);

// Returns the version of the linked nph_core library as a static
// "major.minor.patch" string. The pointer is valid for the lifetime of the
// process and must not be freed.
// Thread-safety: safe from any thread. Never fails, never allocates.
const char *nph_version_string(void);

#ifdef __cplusplus
}
#endif
