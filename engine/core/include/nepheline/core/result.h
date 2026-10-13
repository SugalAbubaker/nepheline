// SPDX-License-Identifier: Apache-2.0
#pragma once

// NphResult: the error code returned by every fallible engine function
// (CLAUDE.md §5 "Errors"). Results are out-parameters; the return value says
// whether they are valid. Raw allocation is the one documented exception:
// allocators return NULL for "out of memory" (docs/adr/0004-allocator-interface.md).

#include <nepheline/core/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphResult {
    NPH_RESULT_OK = 0,
    NPH_RESULT_OUT_OF_MEMORY,    // an allocator returned NULL
    NPH_RESULT_INVALID_ARGUMENT, // a precondition on an argument failed
    NPH_RESULT_NOT_FOUND,        // key, name, or path does not exist
    NPH_RESULT_ALREADY_EXISTS,   // key or name is taken
    NPH_RESULT_FULL,             // fixed-capacity container or queue is full
    NPH_RESULT_EMPTY,            // nothing to pop or read
    NPH_RESULT_OVERFLOW,         // arithmetic or buffer bounds would be exceeded
    NPH_RESULT_UNSUPPORTED,      // valid request, but not implemented on this platform or format
    NPH_RESULT_CORRUPT,          // data failed an integrity check
    NPH_RESULT_VERSION_MISMATCH, // data or module version is incompatible
    NPH_RESULT_TYPE_MISMATCH,    // value has a different type than requested
    NPH_RESULT_PARSE_ERROR,      // text or binary stream is malformed
    NPH_RESULT_END_OF_STREAM,    // a reader ran past the end of its data
    NPH_RESULT_IO_ERROR,         // the OS reported a file or device failure
    NPH_RESULT_ACCESS_DENIED,    // the OS refused on permissions or privileges
    NPH_RESULT_BUSY,             // the resource is in use (sharing violation, non-empty directory)
    NPH_RESULT_DEVICE_LOST,      // the GPU (or another device) was lost or reset; recreate it
    NPH_RESULT_UNKNOWN,          // failure with no better classification
    NPH_RESULT_COUNT
} NphResult;

// Returns a static, human-readable name for `r` ("OK", "OUT_OF_MEMORY", ...).
// Out-of-range values yield "INVALID_RESULT". Never fails, never allocates.
// Thread-safety: NPH_THREADSAFE.
const char *nph_result_string(NphResult r);

// Returns from the enclosing function with the result of `expr` unless it is
// NPH_RESULT_OK. For use in functions that return NphResult.
#define NPH_TRY(expr)                                                                              \
    do {                                                                                           \
        const NphResult nph_try_result_ = (expr);                                                  \
        if (nph_try_result_ != NPH_RESULT_OK) {                                                    \
            return nph_try_result_;                                                                \
        }                                                                                          \
    } while (0)

#ifdef __cplusplus
}
#endif
