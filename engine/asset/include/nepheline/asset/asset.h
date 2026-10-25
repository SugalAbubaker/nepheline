// SPDX-License-Identifier: Apache-2.0
#pragma once

// The asset module (CLAUDE.md §8.8): the cooked runtime format (`.nph`),
// pack archives (`.npk`), the virtual file system, and the loader. Design:
// docs/asset.md, docs/adr/0016-asset-format.md.
//
// This header holds what every other asset header shares: the log
// category, the asset type identifiers, and the four-character codes used
// as tags in files. The module depends on jobs (asynchronous loading and
// parallel decompression) and through it on platform (files, memory
// mapping), math (bounds in the typed headers), and core.
//
// Thread-safety is stated per header; the type and tag helpers here are
// pure and NPH_THREADSAFE.

#include <nepheline/core/log.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_ASSET);

// A four-character code packed little-endian, so the bytes read in order
// in a hex dump of the file ("MESH" appears as M E S H).
#define NPH_FOURCC(a, b, c, d)                                                                     \
    ((u32)(u8)(a) | ((u32)(u8)(b) << 8U) | ((u32)(u8)(c) << 16U) | ((u32)(u8)(d) << 24U))

// Asset types, as stored in NphAssetHeader.type. Values are four-character
// codes; the fourth character is always below 0x80 so every value fits a
// signed enum on every compiler.
typedef enum NphAssetType {
    NPH_ASSET_TYPE_UNKNOWN = 0,
    NPH_ASSET_TYPE_MESH = NPH_FOURCC('M', 'E', 'S', 'H'),
    NPH_ASSET_TYPE_TEXTURE = NPH_FOURCC('T', 'E', 'X', ' '),
    NPH_ASSET_TYPE_MATERIAL = NPH_FOURCC('M', 'A', 'T', 'L'),
    NPH_ASSET_TYPE_SCENE = NPH_FOURCC('S', 'C', 'E', 'N'),
    NPH_ASSET_TYPE_SOUND = NPH_FOURCC('S', 'N', 'D', ' '),
    NPH_ASSET_TYPE_BLOB = NPH_FOURCC('B', 'L', 'O', 'B'), // untyped bytes (tests, tools)
} NphAssetType;

// A static name for an asset type ("mesh", "texture", ...; "unknown" for
// anything else).
const char *nph_asset_type_name(u32 type);

// Writes a four-character code as text into `out` (5 bytes, NUL
// terminated); non-printable bytes become '.'. For logs.
void nph_fourcc_to_string(u32 code, char out[5]);

#ifdef __cplusplus
}
#endif
