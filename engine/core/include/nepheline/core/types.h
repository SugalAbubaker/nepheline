// SPDX-License-Identifier: Apache-2.0
#pragma once

// Fixed-width scalar types used throughout the engine (CLAUDE.md §5).
//
// `size_t` appears only at C library boundaries; engine APIs express byte
// counts as u64 and element counts as u32 or u64, so the width of every
// quantity is visible at the call site.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;

// Boolean stored in 32 bits. Zero is false, anything else is true. Used in
// structs and file formats where `bool` (which has an unspecified size) is
// unsuitable; local logic may use `bool` from <stdbool.h>.
typedef int32_t b32;

// Pointer-sized integers for address arithmetic.
typedef uintptr_t uptr;
typedef intptr_t iptr;

#define NPH_U8_MAX  UINT8_MAX
#define NPH_U16_MAX UINT16_MAX
#define NPH_U32_MAX UINT32_MAX
#define NPH_U64_MAX UINT64_MAX
#define NPH_I8_MIN  INT8_MIN
#define NPH_I8_MAX  INT8_MAX
#define NPH_I16_MIN INT16_MIN
#define NPH_I16_MAX INT16_MAX
#define NPH_I32_MIN INT32_MIN
#define NPH_I32_MAX INT32_MAX
#define NPH_I64_MIN INT64_MIN
#define NPH_I64_MAX INT64_MAX

// Byte-count helpers. The argument is evaluated once; the result is u64.
#define NPH_KIB(n) ((u64)(n) << 10U)
#define NPH_MIB(n) ((u64)(n) << 20U)
#define NPH_GIB(n) ((u64)(n) << 30U)

// Number of elements of a fixed-size array. Not for pointers.
#define NPH_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Marks a parameter or variable as intentionally unused.
#define NPH_UNUSED(x) ((void)(x))

// Compile-time size and alignment checks. The headers are included by the
// one C++ translation unit in the runtime (the VMA implementation), where
// the keyword is static_assert.
#ifdef __cplusplus
#define NPH_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define NPH_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

NPH_STATIC_ASSERT(sizeof(u8) == 1, "u8 must be one byte");
NPH_STATIC_ASSERT(sizeof(u16) == 2, "u16 must be two bytes");
NPH_STATIC_ASSERT(sizeof(u32) == 4, "u32 must be four bytes");
NPH_STATIC_ASSERT(sizeof(u64) == 8, "u64 must be eight bytes");
NPH_STATIC_ASSERT(sizeof(f32) == 4, "f32 must be IEEE-754 binary32");
NPH_STATIC_ASSERT(sizeof(f64) == 8, "f64 must be IEEE-754 binary64");
NPH_STATIC_ASSERT(sizeof(b32) == 4, "b32 must be four bytes");
NPH_STATIC_ASSERT(sizeof(uptr) == sizeof(void *), "uptr must hold a pointer");
NPH_STATIC_ASSERT(sizeof(void *) == 8, "Nepheline targets 64-bit platforms only");

#ifdef __cplusplus
}
#endif
