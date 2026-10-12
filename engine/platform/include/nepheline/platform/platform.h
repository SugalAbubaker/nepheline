// SPDX-License-Identifier: Apache-2.0
#pragma once

// Process-level helpers that do not belong to a larger subsystem: where
// the executable is, environment variables, the console.
//
// Paths and strings are UTF-8 everywhere in the engine; the Windows
// implementation converts to and from UTF-16 at the OS boundary. Output
// paths use the platform's native separator.
//
// Thread-safety: every function is NPH_THREADSAFE.

#include <nepheline/core/log.h>
#include <nepheline/core/result.h>

#ifdef __cplusplus
extern "C" {
#endif

// The platform layer's log category, for the OS messages it reports.
NPH_LOG_CATEGORY_DECLARE(NPH_LOG_PLATFORM);

// Writes the absolute path of the running executable into `buf` (NUL
// terminated, at most `cap` bytes). *out_len receives the length without
// the terminator (may be NULL). NPH_RESULT_OVERFLOW when `cap` is too small
// (the length is still reported).
NphResult nph_platform_executable_path(char *buf, u64 cap, u64 *out_len);

// As nph_platform_executable_path, without the file name and without the
// trailing separator.
NphResult nph_platform_executable_dir(char *buf, u64 cap, u64 *out_len);

// Reads the environment variable `name` into `buf`. NPH_RESULT_NOT_FOUND
// when unset; NPH_RESULT_OVERFLOW when `cap` is too small.
NphResult nph_platform_env_get(const char *name, char *buf, u64 cap, u64 *out_len);

// The OS process id.
u64 nph_platform_process_id(void);

// "windows" or "linux".
const char *nph_platform_os_name(void);

// Enables ANSI escape sequences on the console's stdout and stderr (a no-op
// on Linux, where terminals speak them natively). Returns true when the
// console accepts them, so the caller can turn on colour in the log
// console sink. False when output is redirected or the console is too old.
bool nph_platform_console_enable_vt(void);

// True when a debugger is attached (IsDebuggerPresent; on Linux, the
// TracerPid field of /proc/self/status).
bool nph_platform_debugger_present(void);

#ifdef __cplusplus
}
#endif
