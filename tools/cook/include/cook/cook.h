// SPDX-License-Identifier: Apache-2.0
#pragma once

// The asset cooker (CLAUDE.md §8.8 "Offline cooker"): converts source
// assets (glTF scenes, images) into the engine's cooked runtime files
// (engine/asset) and packs them. Design: docs/cook.md,
// docs/adr/0017-cooker.md.
//
// This header holds what every cooker header shares: the log category,
// the version that is folded into every cooked file's settings hash (so a
// cooker change recooks everything), and the memory blocks the two
// third-party libraries allocate through.
//
// Thread-safety: the cooker's library functions are NPH_THREADSAFE as far
// as their arguments are distinct; the texture encoders use the job
// system when one is given.

#include <nepheline/asset/asset.h>
#include <nepheline/core/alloc.h>
#include <nepheline/core/log.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_CATEGORY_DECLARE(NPH_LOG_COOK);

// Bumped whenever the cooker's output for an unchanged source changes in a
// way the settings hash does not already see (a new encoder, say). Part of
// every settings hash, so the database recooks everything on the next run.
// A type's own payload version and settings are in its hash too, so a new
// version of one type recooks that type alone (meshes at their version 2,
// the levels of detail).
//   1: the first cooker.
//   2: BC7 mode 4/5 index layout per the specification, flat normals,
//      MikkTSpace tangents with glTF handedness, the adjacency meshlet
//      builder, bounds from dequantised positions, exact-area mips.
#define COOK_VERSION 2U

// The same for the mesh pipeline alone (the meshlet and level-of-detail
// builders), part of meshes' settings hash: bumped, it recooks the meshes
// and leaves the textures, whose encoding is most of a cook's time.
//   1: levels of detail (mesh type version 2), root groups.
#define COOK_MESH_VERSION 1U

// Sized blocks for libraries that free by pointer alone (cgltf, stb_image):
// a 16-byte header in front of each block records its size, so the
// allocator's sized-free contract holds. NULL for a size that cannot be
// represented with the header, or when the allocator fails.
void *cook_block_alloc(NphAllocator *alloc, u64 size);
void *cook_block_realloc(NphAllocator *alloc, void *ptr, u64 new_size);
void cook_block_free(NphAllocator *alloc, void *ptr);

// stb_image's allocation hooks are macros without a context argument, so
// its blocks come from the system allocator (nph_allocator_system); they
// live only inside one decode call (image.c).
void *cook_thirdparty_alloc(u64 size);
void *cook_thirdparty_realloc(void *ptr, u64 new_size);
void cook_thirdparty_free(void *ptr);

#ifdef __cplusplus
}
#endif
