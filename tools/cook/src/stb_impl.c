// SPDX-License-Identifier: Apache-2.0
// The stb_image implementation, in one translation unit compiled with
// the library's own warning policy (tools/cook/CMakeLists.txt). Decoding
// is restricted to the formats the cooker accepts, reading is from memory
// only (the cooker reads files through platform/file.h), and memory goes
// through the cooker's wrapper (cook.h) so the engine's allocation policy
// holds. The wrapper is declared here rather than through cook.h, which
// pulls in the engine's headers this unit is kept away from.
#include <stddef.h>
#include <stdint.h>

void *cook_thirdparty_alloc(uint64_t size);
void *cook_thirdparty_realloc(void *ptr, uint64_t new_size);
void cook_thirdparty_free(void *ptr);

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#define STBI_MAX_DIMENSIONS     16384
#define STBI_MALLOC(size)       cook_thirdparty_alloc((uint64_t)(size))
#define STBI_REALLOC(ptr, size) cook_thirdparty_realloc((ptr), (uint64_t)(size))
#define STBI_FREE(ptr)          cook_thirdparty_free(ptr)
#include <stb_image.h>
