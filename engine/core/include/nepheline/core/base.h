// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compiler and platform detection, portable attributes, bit tricks, and
// alignment helpers (CLAUDE.md §8.1 "Base"). Every detection macro is always
// defined as 0 or 1 so that `#if NPH_X` is safe under -Wundef.

#include <nepheline/core/types.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------

#if defined(__clang__)
#define NPH_COMPILER_CLANG 1
#define NPH_COMPILER_GCC   0
#define NPH_COMPILER_MSVC  0
#elif defined(__GNUC__)
#define NPH_COMPILER_CLANG 0
#define NPH_COMPILER_GCC   1
#define NPH_COMPILER_MSVC  0
#elif defined(_MSC_VER)
#define NPH_COMPILER_CLANG 0
#define NPH_COMPILER_GCC   0
#define NPH_COMPILER_MSVC  1
#else
#error "Nepheline: unsupported compiler (MSVC, GCC, or Clang required)"
#endif

// clang-cl: Clang front end with MSVC-compatible ABI, intrinsics, and CRT.
#if defined(_MSC_VER)
#define NPH_MSVC_ABI 1
#else
#define NPH_MSVC_ABI 0
#endif

// ---------------------------------------------------------------------------
// Platform and architecture
// ---------------------------------------------------------------------------

#if defined(_WIN32)
#define NPH_PLATFORM_WINDOWS 1
#define NPH_PLATFORM_LINUX   0
#elif defined(__linux__)
#define NPH_PLATFORM_WINDOWS 0
#define NPH_PLATFORM_LINUX   1
#else
#error "Nepheline: unsupported platform (Windows or Linux required)"
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define NPH_ARCH_X64   1
#define NPH_ARCH_ARM64 0
#elif defined(_M_ARM64) || defined(__aarch64__)
#define NPH_ARCH_X64   0
#define NPH_ARCH_ARM64 1
#else
#error "Nepheline: unsupported architecture (x86-64 or ARM64 required)"
#endif

// AddressSanitizer presence, for poisoning allocator-owned memory (alloc.h).
#if defined(__SANITIZE_ADDRESS__)
#define NPH_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NPH_ASAN_ENABLED 1
#else
#define NPH_ASAN_ENABLED 0
#endif
#else
#define NPH_ASAN_ENABLED 0
#endif

// Build configuration flags are defined by CMake (cmake/NphConfigurations.cmake):
// NPH_CONFIG_{DEBUG,DEVELOPMENT,PROFILE,SHIPPING}, NPH_ASSERTS_ENABLED,
// NPH_GPU_VALIDATION_ENABLED, NPH_PROFILER_ENABLED. Normalise the ones code
// tests with #if so they are always 0 or 1.
#if !defined(NPH_CONFIG_DEBUG)
#define NPH_CONFIG_DEBUG 0
#endif
#if !defined(NPH_CONFIG_DEVELOPMENT)
#define NPH_CONFIG_DEVELOPMENT 0
#endif
#if !defined(NPH_CONFIG_PROFILE)
#define NPH_CONFIG_PROFILE 0
#endif
#if !defined(NPH_CONFIG_SHIPPING)
#define NPH_CONFIG_SHIPPING 0
#endif
#if !defined(NPH_ASSERTS_ENABLED)
#define NPH_ASSERTS_ENABLED 0
#endif
#if !defined(NPH_PROFILER_ENABLED)
#define NPH_PROFILER_ENABLED 0
#endif
#if !defined(NPH_GPU_VALIDATION_ENABLED)
#define NPH_GPU_VALIDATION_ENABLED 0
#endif

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

#if NPH_COMPILER_MSVC
#define NPH_FORCE_INLINE __forceinline
#define NPH_NO_INLINE    __declspec(noinline)
#define NPH_NORETURN     __declspec(noreturn)
#define NPH_RESTRICT     __restrict
#define NPH_ALIGNAS(n)   __declspec(align(n))
#define NPH_MUST_USE     _Check_return_
#define NPH_THREAD_LOCAL __declspec(thread)
#define NPH_PRINTF_LIKE(fmt_index, first_arg)
#define NPH_ANALYZER_NORETURN
#define NPH_LIKELY(x)     (x)
#define NPH_UNLIKELY(x)   (x)
#define NPH_DEBUG_BREAK() __debugbreak()
#else
#define NPH_FORCE_INLINE                      inline __attribute__((always_inline))
#define NPH_NO_INLINE                         __attribute__((noinline))
#define NPH_NORETURN                          __attribute__((noreturn))
#define NPH_RESTRICT                          __restrict__
#define NPH_ALIGNAS(n)                        __attribute__((aligned(n)))
#define NPH_MUST_USE                          __attribute__((warn_unused_result))
#define NPH_THREAD_LOCAL                      _Thread_local
#define NPH_PRINTF_LIKE(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#define NPH_LIKELY(x)                         __builtin_expect(!!(x), 1)
#define NPH_UNLIKELY(x)                       __builtin_expect(!!(x), 0)
// Tells the Clang static analyzer that a function does not return (an assert
// handler that may continue in tests), so it stops following the failure path.
#if NPH_COMPILER_CLANG
#define NPH_ANALYZER_NORETURN __attribute__((analyzer_noreturn))
#else
#define NPH_ANALYZER_NORETURN
#endif
// A breakpoint the debugger can step past (unlike __builtin_trap, which is
// fatal). Without a debugger attached the process receives SIGTRAP.
#if NPH_COMPILER_CLANG
#define NPH_DEBUG_BREAK() __builtin_debugtrap()
#elif NPH_ARCH_X64
#define NPH_DEBUG_BREAK() __asm__ __volatile__("int3")
#else
#define NPH_DEBUG_BREAK() __asm__ __volatile__("brk #0")
#endif
#endif

// `static inline` for header-defined functions.
#define NPH_INLINE static inline

// Alignment of a type, as a u32.
#define NPH_ALIGNOF(T) ((u32) _Alignof(T))

// Thread-safety annotations (CLAUDE.md §5). Documentation only; they expand
// to nothing and are listed in .clang-format as attribute macros.
#define NPH_THREADSAFE
#define NPH_MAIN_THREAD_ONLY
#define NPH_RENDER_THREAD_ONLY

// Destination cache-line size assumed for padding and false-sharing avoidance.
// x86-64 and most ARM64 cores use 64 bytes; adjacent-line prefetch pairs are
// handled by padding to 128 where it is measured to matter.
#define NPH_CACHE_LINE_SIZE 64U

// Tells the optimiser that control cannot reach this point. In builds with
// asserts it is a hard failure instead (assert.h redefines it).
#if NPH_COMPILER_MSVC
#define NPH_UNREACHABLE_HINT() __assume(0)
#else
#define NPH_UNREACHABLE_HINT() __builtin_unreachable()
#endif

// Concatenation and stringification that expand their arguments first.
#define NPH_CONCAT_(a, b) a##b
#define NPH_CONCAT(a, b)  NPH_CONCAT_(a, b)
#define NPH_STRINGIZE_(x) #x
#define NPH_STRINGIZE(x)  NPH_STRINGIZE_(x)

// ---------------------------------------------------------------------------
// Scalar helpers (inline functions, so arguments are evaluated once)
// ---------------------------------------------------------------------------

NPH_INLINE u32 nph_min_u32(u32 a, u32 b)
{
    return a < b ? a : b;
}
NPH_INLINE u32 nph_max_u32(u32 a, u32 b)
{
    return a > b ? a : b;
}
NPH_INLINE u64 nph_min_u64(u64 a, u64 b)
{
    return a < b ? a : b;
}
NPH_INLINE u64 nph_max_u64(u64 a, u64 b)
{
    return a > b ? a : b;
}
NPH_INLINE i32 nph_min_i32(i32 a, i32 b)
{
    return a < b ? a : b;
}
NPH_INLINE i32 nph_max_i32(i32 a, i32 b)
{
    return a > b ? a : b;
}
NPH_INLINE i64 nph_min_i64(i64 a, i64 b)
{
    return a < b ? a : b;
}
NPH_INLINE i64 nph_max_i64(i64 a, i64 b)
{
    return a > b ? a : b;
}
NPH_INLINE u32 nph_clamp_u32(u32 v, u32 lo, u32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
NPH_INLINE u64 nph_clamp_u64(u64 v, u64 lo, u64 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
NPH_INLINE i32 nph_clamp_i32(i32 v, i32 lo, i32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// Bit operations
// ---------------------------------------------------------------------------

// Number of set bits.
NPH_INLINE u32 nph_popcount_u32(u32 v)
{
#if NPH_COMPILER_MSVC
    return (u32)__popcnt(v);
#else
    return (u32)__builtin_popcount(v);
#endif
}
NPH_INLINE u32 nph_popcount_u64(u64 v)
{
#if NPH_COMPILER_MSVC
    return (u32)__popcnt64(v);
#else
    return (u32)__builtin_popcountll(v);
#endif
}

// Count of leading zero bits. Undefined for v == 0; callers check first.
NPH_INLINE u32 nph_clz_u32(u32 v)
{
#if NPH_COMPILER_MSVC
    unsigned long index;
    _BitScanReverse(&index, v);
    return 31U - (u32)index;
#else
    return (u32)__builtin_clz(v);
#endif
}
NPH_INLINE u32 nph_clz_u64(u64 v)
{
#if NPH_COMPILER_MSVC
    unsigned long index;
    _BitScanReverse64(&index, v);
    return 63U - (u32)index;
#else
    return (u32)__builtin_clzll(v);
#endif
}

// Count of trailing zero bits. Undefined for v == 0; callers check first.
NPH_INLINE u32 nph_ctz_u32(u32 v)
{
#if NPH_COMPILER_MSVC
    unsigned long index;
    _BitScanForward(&index, v);
    return (u32)index;
#else
    return (u32)__builtin_ctz(v);
#endif
}
NPH_INLINE u32 nph_ctz_u64(u64 v)
{
#if NPH_COMPILER_MSVC
    unsigned long index;
    _BitScanForward64(&index, v);
    return (u32)index;
#else
    return (u32)__builtin_ctzll(v);
#endif
}

// floor(log2(v)). Undefined for v == 0.
NPH_INLINE u32 nph_log2_floor_u32(u32 v)
{
    return 31U - nph_clz_u32(v);
}
NPH_INLINE u32 nph_log2_floor_u64(u64 v)
{
    return 63U - nph_clz_u64(v);
}

// ceil(log2(v)). Undefined for v == 0. log2_ceil(1) == 0.
NPH_INLINE u32 nph_log2_ceil_u32(u32 v)
{
    return v <= 1U ? 0U : 32U - nph_clz_u32(v - 1U);
}
NPH_INLINE u32 nph_log2_ceil_u64(u64 v)
{
    return v <= 1U ? 0U : 64U - nph_clz_u64(v - 1U);
}

NPH_INLINE bool nph_is_pow2_u32(u32 v)
{
    return v != 0U && (v & (v - 1U)) == 0U;
}
NPH_INLINE bool nph_is_pow2_u64(u64 v)
{
    return v != 0U && (v & (v - 1U)) == 0U;
}

// Smallest power of two >= v. next_pow2(0) == 1. Wraps to 0 above 2^31 / 2^63.
NPH_INLINE u32 nph_next_pow2_u32(u32 v)
{
    return v <= 1U ? 1U : (u32)1U << nph_log2_ceil_u32(v);
}
NPH_INLINE u64 nph_next_pow2_u64(u64 v)
{
    return v <= 1U ? 1U : (u64)1U << nph_log2_ceil_u64(v);
}

NPH_INLINE u32 nph_rotl_u32(u32 v, u32 n)
{
    return (v << (n & 31U)) | (v >> ((32U - n) & 31U));
}
NPH_INLINE u64 nph_rotl_u64(u64 v, u32 n)
{
    return (v << (n & 63U)) | (v >> ((64U - n) & 63U));
}
NPH_INLINE u32 nph_rotr_u32(u32 v, u32 n)
{
    return (v >> (n & 31U)) | (v << ((32U - n) & 31U));
}
NPH_INLINE u64 nph_rotr_u64(u64 v, u32 n)
{
    return (v >> (n & 63U)) | (v << ((64U - n) & 63U));
}

NPH_INLINE u16 nph_bswap_u16(u16 v)
{
    return (u16)((v << 8U) | (v >> 8U));
}
NPH_INLINE u32 nph_bswap_u32(u32 v)
{
#if NPH_COMPILER_MSVC
    return (u32)_byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
NPH_INLINE u64 nph_bswap_u64(u64 v)
{
#if NPH_COMPILER_MSVC
    return (u64)_byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}

// ---------------------------------------------------------------------------
// Alignment helpers. `align` must be a power of two.
// ---------------------------------------------------------------------------

NPH_INLINE u64 nph_align_up_u64(u64 v, u64 align)
{
    return (v + align - 1U) & ~(align - 1U);
}
NPH_INLINE u64 nph_align_down_u64(u64 v, u64 align)
{
    return v & ~(align - 1U);
}
NPH_INLINE u32 nph_align_up_u32(u32 v, u32 align)
{
    return (v + align - 1U) & ~(align - 1U);
}
NPH_INLINE u32 nph_align_down_u32(u32 v, u32 align)
{
    return v & ~(align - 1U);
}
NPH_INLINE bool nph_is_aligned_u64(u64 v, u64 align)
{
    return (v & (align - 1U)) == 0U;
}
NPH_INLINE void *nph_align_ptr_up(void *p, u64 align)
{
    // Pointer arithmetic rather than an integer-to-pointer cast keeps the
    // result's provenance tied to `p`.
    return (u8 *)p + (nph_align_up_u64((uptr)p, align) - (uptr)p);
}
NPH_INLINE bool nph_is_ptr_aligned(const void *p, u64 align)
{
    return nph_is_aligned_u64((uptr)p, align);
}

// Byte offset of `p` from `base`, for pointer bookkeeping.
NPH_INLINE u64 nph_ptr_diff(const void *base, const void *p)
{
    return (u64)((uptr)p - (uptr)base);
}

#ifdef __cplusplus
}
#endif
