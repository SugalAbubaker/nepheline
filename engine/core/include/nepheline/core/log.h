// SPDX-License-Identifier: Apache-2.0
#pragma once

// Logging (CLAUDE.md §8.1: "levels and categories, a lock-free per-thread
// ring buffer flushed by a logger thread, and sinks for console, file, and
// the editor").
//
//     NPH_LOG_CATEGORY_DEFINE(LOG_RENDER, "render", NPH_LOG_LEVEL_DEBUG);
//     NPH_LOG_INFO(&LOG_RENDER, "swapchain %ux%u", w, h);
//
// A call formats its message into a fixed 256-byte record and pushes it on
// the calling thread's single-producer ring: no lock, no allocation, no
// I/O. nph_log_flush drains every thread's ring in timestamp order into the
// sinks; the runtime calls it from its logger thread (ADR-0005), tools and
// tests call it directly. If a ring is full the record is dropped and
// counted; the next record that gets through carries the count, so a loss
// is visible rather than silent.
//
// Before nph_log_init, records go synchronously to stderr so early failures
// are never lost. Level checks are an atomic load per call and are
// compiled out below NPH_LOG_COMPILED_MIN_LEVEL (INFO in Shipping).
//
// Thread-safety: NPH_LOG_* and nph_log_write are NPH_THREADSAFE. Flush is
// single-flusher (a second concurrent flush returns 0). Sinks run on the
// flushing thread. Init and shutdown are NPH_MAIN_THREAD_ONLY.

#include <nepheline/core/atomic.h>
#include <nepheline/core/result.h>
#include <nepheline/core/string.h>

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphLogLevel {
    NPH_LOG_LEVEL_TRACE = 0,
    NPH_LOG_LEVEL_DEBUG = 1,
    NPH_LOG_LEVEL_INFO = 2,
    NPH_LOG_LEVEL_WARN = 3,
    NPH_LOG_LEVEL_ERROR = 4,
    NPH_LOG_LEVEL_FATAL = 5,
    NPH_LOG_LEVEL_COUNT = 6,
    NPH_LOG_LEVEL_OFF = NPH_LOG_LEVEL_COUNT, // disables a category or sink
} NphLogLevel;

// A category is a static object owned by the module that logs through it.
typedef struct NphLogCategory {
    const char *name;
    NphAtomicU32 min_level; // NphLogLevel; records below it are skipped
} NphLogCategory;

#define NPH_LOG_CATEGORY_DEFINE(ident, name, default_level)                                        \
    NphLogCategory ident = {(name), NPH_ATOMIC_U32_INIT((u32)(default_level))}
#define NPH_LOG_CATEGORY_DECLARE(ident) extern NphLogCategory ident

// Core's own category.
NPH_LOG_CATEGORY_DECLARE(NPH_LOG_CORE);

#define NPH_LOG_MESSAGE_MAX 216U

typedef struct NphLogRecord {
    u64 timestamp_ns;     // UTC, nanoseconds since the Unix epoch
    const char *category; // static name
    const char *file;     // static path of the call site
    u32 line;
    u16 level;          // NphLogLevel
    u16 thread_index;   // 1-based index assigned when the thread first logged
    u32 sequence;       // per-thread, increments per record (dropped ones included)
    u32 dropped_before; // records this thread dropped since its previous delivered one
    char message[NPH_LOG_MESSAGE_MAX]; // NUL-terminated; long messages end in "..."
} NphLogRecord;

NPH_STATIC_ASSERT(sizeof(NphLogRecord) == 256U, "a log record is four cache lines");

typedef void (*NphLogSinkFn)(const NphLogRecord *record, void *user);

#define NPH_LOG_MAX_SINKS   8U
#define NPH_LOG_MAX_THREADS 256U

typedef struct NphLogConfig {
    NphAllocator *alloc; // required: thread rings are allocated from it
    u32 ring_capacity;   // records per thread ring, power of two (0: 1024)
    u32 max_threads;     // threads that may log concurrently (0: 64, max 256)
} NphLogConfig;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NPH_MUST_USE NphResult nph_log_init(const NphLogConfig *config);

// Flushes, releases every thread ring, removes the sinks. Threads other
// than the caller must have detached (or exited without logging again).
void nph_log_shutdown(void);

bool nph_log_is_initialized(void);

// Registers a sink that receives records at or above `min_level`.
// NPH_RESULT_FULL after NPH_LOG_MAX_SINKS. Takes effect at the next flush.
NPH_MUST_USE NphResult nph_log_add_sink(NphLogSinkFn fn, void *user, NphLogLevel min_level);
bool nph_log_remove_sink(NphLogSinkFn fn, void *user);

// Per-category and global thresholds (both must pass).
void nph_log_set_level(NphLogCategory *category, NphLogLevel level);
NphLogLevel nph_log_get_level(const NphLogCategory *category);
void nph_log_set_global_level(NphLogLevel level);
NphLogLevel nph_log_get_global_level(void);

// Prepares the calling thread's ring (otherwise done on its first write,
// which then allocates). Idempotent. NPH_RESULT_FULL when max_threads
// rings exist.
NPH_MUST_USE NphResult nph_log_thread_attach(void);

// Marks the calling thread's ring for release after its remaining records
// are flushed. Call before the thread exits.
void nph_log_thread_detach(void);

// Delivers pending records, oldest first across threads, to the sinks.
// `max_records` bounds one call (0: everything pending). Returns the
// number delivered; 0 also when another flush is in progress.
u32 nph_log_flush(u32 max_records);

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// True when a record at `level` would be stored for `category`.
#define NPH_LOG_ENABLED(category, level)                                                           \
    ((u32)(level) >= nph_atomic_load_u32(&(category)->min_level, NPH_MEMORY_ORDER_RELAXED) &&      \
     (u32)(level) >= nph_log_get_global_level_fast())

#define NPH_LOG(category, level, ...)                                                              \
    do {                                                                                           \
        if (NPH_LOG_ENABLED(category, level)) {                                                    \
            nph_log_write((category), (level), __FILE__, (u32)__LINE__, __VA_ARGS__);              \
        }                                                                                          \
    } while (0)

// TRACE and DEBUG are compiled out of Shipping builds (their arguments are
// not evaluated); the lowest level that survives is NPH_LOG_COMPILED_MIN_LEVEL.
#if NPH_CONFIG_SHIPPING
#define NPH_LOG_COMPILED_MIN_LEVEL   NPH_LOG_LEVEL_INFO
#define NPH_LOG_TRACE(category, ...) ((void)0)
#define NPH_LOG_DEBUG(category, ...) ((void)0)
#else
#define NPH_LOG_COMPILED_MIN_LEVEL   NPH_LOG_LEVEL_TRACE
#define NPH_LOG_TRACE(category, ...) NPH_LOG(category, NPH_LOG_LEVEL_TRACE, __VA_ARGS__)
#define NPH_LOG_DEBUG(category, ...) NPH_LOG(category, NPH_LOG_LEVEL_DEBUG, __VA_ARGS__)
#endif
#define NPH_LOG_INFO(category, ...)  NPH_LOG(category, NPH_LOG_LEVEL_INFO, __VA_ARGS__)
#define NPH_LOG_WARN(category, ...)  NPH_LOG(category, NPH_LOG_LEVEL_WARN, __VA_ARGS__)
#define NPH_LOG_ERROR(category, ...) NPH_LOG(category, NPH_LOG_LEVEL_ERROR, __VA_ARGS__)
#define NPH_LOG_FATAL(category, ...) NPH_LOG(category, NPH_LOG_LEVEL_FATAL, __VA_ARGS__)

// Formats and stores a record. Called by the macros (which do the level
// check); usable directly by wrappers that already checked.
NPH_PRINTF_LIKE(5, 6)
void nph_log_write(NphLogCategory *category, NphLogLevel level, const char *file, u32 line,
                   const char *fmt, ...);
NPH_PRINTF_LIKE(5, 0)
void nph_log_write_v(NphLogCategory *category, NphLogLevel level, const char *file, u32 line,
                     const char *fmt, va_list args);
void nph_log_write_str(NphLogCategory *category, NphLogLevel level, const char *file, u32 line,
                       NphStr message);

// The global threshold as a plain load, for the macros.
u32 nph_log_get_global_level_fast(void);

// ---------------------------------------------------------------------------
// Formatting and built-in sinks
// ---------------------------------------------------------------------------

// "INFO", "WARN", ... ("?" for out-of-range).
const char *nph_log_level_name(NphLogLevel level);

// Writes "HH:MM:SS.mmm LEVEL [category] message (file:line)" plus a note
// about dropped records when applicable, into `out` (NUL-terminated,
// truncated to `cap`), optionally with ANSI colours. Returns the length
// that would have been written, like snprintf.
u64 nph_log_format(const NphLogRecord *record, char *out, u64 cap, bool color, bool with_location);

typedef struct NphLogConsoleSink {
    bool color;         // ANSI colour codes (the platform layer enables VT on Windows)
    bool with_location; // append file:line to every record (always for WARN and above)
} NphLogConsoleSink;

// Sink writing to stdout (below WARN) and stderr (WARN and above). `user`
// is an NphLogConsoleSink* or NULL for the defaults (no colour).
void nph_log_console_sink(const NphLogRecord *record, void *user);

typedef struct NphLogFileSink {
    FILE *file;
} NphLogFileSink;

// Opens `path` for appending. NPH_RESULT_IO_ERROR on failure.
NPH_MUST_USE NphResult nph_log_file_sink_open(NphLogFileSink *sink, const char *path);
void nph_log_file_sink_close(NphLogFileSink *sink);
// `user` is the NphLogFileSink*.
void nph_log_file_sink(const NphLogRecord *record, void *user);

#ifdef __cplusplus
}
#endif
