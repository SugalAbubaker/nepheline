// SPDX-License-Identifier: Apache-2.0
#pragma once

// LZ77 byte-oriented compression (CLAUDE.md §8.8 "LZ4-class compression
// we implement ourselves").
//
// The compressed byte stream is the LZ4 block format (Collet, "LZ4 Block
// Format Description", 2011): a sequence of tokens, each a run of literal
// bytes followed by a back-reference of at least four bytes within a
// 64 KiB window. The format is chosen because it is the right design for
// the job (decoding is a memcpy loop with no entropy stage, which is what
// keeps streaming decompression off the critical path, §10.5) and because
// it is documented and testable against other implementations. The
// compressor and the decompressor here are the engine's own.
//
// Two compression levels: FAST (a hash table, one candidate per position,
// greedy; for tools that run often) and HIGH (hash chains over the whole
// window with lazy matching; for the cooker, where ratio matters more than
// cook time). Both produce streams the one decoder reads. Decompression
// speed does not depend on the level.
//
// Memory (§1.4): the compressor takes its work table from the caller
// (nph_lz_workspace_size); the decompressor needs nothing. Neither
// allocates.
//
// Safety: the decoder checks every read against the input and every write
// against the output, so a corrupt or hostile stream yields
// NPH_RESULT_CORRUPT and never a memory fault (fuzzed, tests/fuzz).
//
// Thread-safety: NPH_THREADSAFE; each call uses only its arguments.

#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphLzLevel {
    NPH_LZ_LEVEL_FAST = 0,
    NPH_LZ_LEVEL_HIGH = 1,
} NphLzLevel;

// Largest input the block format can hold (the match-length encoding and
// the 32-bit positions the compressor uses bound it).
#define NPH_LZ_MAX_INPUT_SIZE 0x7E000000ULL

// The window: the farthest back a match may reach.
#define NPH_LZ_MAX_DISTANCE 65535U

// Worst-case compressed size of `size` input bytes: incompressible data
// costs one byte per 255 plus a few bytes of framing. 0 when `size`
// exceeds NPH_LZ_MAX_INPUT_SIZE.
u64 nph_lz_compress_bound(u64 size);

// Bytes of workspace nph_lz_compress needs for `level`, 16-byte aligned.
u64 nph_lz_workspace_size(NphLzLevel level);

// Compresses `src_size` bytes into `dst`. *out_size receives the compressed
// size. `workspace` is nph_lz_workspace_size(level) bytes the call may
// scribble on. NPH_RESULT_INVALID_ARGUMENT when src_size exceeds the
// maximum; NPH_RESULT_OVERFLOW when `dst_cap` is too small (size the
// buffer with nph_lz_compress_bound to rule that out). An empty input
// produces a one-byte stream.
NPH_MUST_USE NphResult nph_lz_compress(const void *src, u64 src_size, void *dst, u64 dst_cap,
                                       NphLzLevel level, void *workspace, u64 *out_size);

// Decompresses `src_size` bytes into exactly `dst_size` bytes at `dst`.
// NPH_RESULT_CORRUPT when the stream is malformed, truncated, or does not
// decode to precisely `dst_size` bytes; `dst` then holds garbage.
NPH_MUST_USE NphResult nph_lz_decompress(const void *src, u64 src_size, void *dst, u64 dst_size);

#ifdef __cplusplus
}
#endif
