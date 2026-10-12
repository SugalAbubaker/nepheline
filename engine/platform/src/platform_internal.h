// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shared by the platform sources only: the OS headers, error-code mapping,
// and the UTF-8/UTF-16 conversions the Windows implementation needs. Never
// included from a public header (CLAUDE.md §1.8: OS headers stay behind the
// module's interface).

#include <nepheline/core/log.h>
#include <nepheline/core/result.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/thread.h>

#if NPH_PLATFORM_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows 10 1803 (RS4): SetThreadDescription (1607) and high-resolution
// waitable timers (1803) are the newest APIs the layer uses.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000005
#endif
#include <windows.h>

// Longest path the Windows implementation handles, in UTF-16 units; the
// \\?\ limit. Stack buffers of this size are used for conversions.
#define NPH_WIN32_PATH_MAX 4096U

// Maps a GetLastError() value to the closest NphResult (NPH_RESULT_IO_ERROR
// when nothing fits). ERROR_SUCCESS maps to NPH_RESULT_IO_ERROR as well: a
// failed call never reports success.
NphResult nph_win32_result(DWORD error);

// Formats the system message for `error` into `buf` (NUL terminated,
// trailing newline removed). Returns the length written.
u32 nph_win32_error_message(DWORD error, char *buf, u32 cap);

// Logs "<what>: <system message> (<error>)" at ERROR on the platform
// category and returns nph_win32_result(error).
NphResult nph_win32_fail(const char *what, DWORD error);

// UTF-8 to UTF-16 into `out` (cap in wchar_t units, NUL terminated).
// NPH_RESULT_OVERFLOW when it does not fit, NPH_RESULT_INVALID_ARGUMENT for
// malformed UTF-8.
NphResult nph_win32_utf8_to_wide(const char *utf8, wchar_t *out, u32 cap);

// UTF-16 to UTF-8 into `out` (cap in bytes, NUL terminated). *out_len is
// the length without the terminator (may be NULL) and is reported even on
// NPH_RESULT_OVERFLOW.
NphResult nph_win32_wide_to_utf8(const wchar_t *wide, char *out, u64 cap, u64 *out_len);

#else

#include <errno.h>

// Maps an errno value to the closest NphResult (NPH_RESULT_IO_ERROR when
// nothing fits). 0 maps to NPH_RESULT_IO_ERROR: a failed call never reports
// success.
NphResult nph_posix_result(int error);

// Logs "<what>: <strerror> (<errno>)" at ERROR on the platform category and
// returns nph_posix_result(error).
NphResult nph_posix_fail(const char *what, int error);

#endif

// The kernel-semaphore half of NphSemaphore (thread_common.c owns the
// user-space count and calls these on the slow path).
NphResult nph_semaphore_os_init(NphSemaphore *s);
void nph_semaphore_os_destroy(NphSemaphore *s);
void nph_semaphore_os_wait(NphSemaphore *s);
bool nph_semaphore_os_wait_timeout(NphSemaphore *s, u64 timeout_ns);
bool nph_semaphore_os_try_wait(NphSemaphore *s);
void nph_semaphore_os_post(NphSemaphore *s, u32 n);

// Per-thread cleanup run by the thread trampoline after the user function
// returns (and available to threads the engine did not create).
void nph_thread_exit_cleanup(void);

// Copies a thread name into a NPH_THREAD_NAME_MAX buffer, truncating.
void nph_thread_copy_name(char *dst, const char *src);

// The OS's count of physical cores for cpu.c, or 0 when it cannot tell.
u32 nph_cpu_physical_count_os(void);
