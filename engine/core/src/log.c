// SPDX-License-Identifier: Apache-2.0
// Logging: per-thread SPSC rings, timestamp-ordered flush, sinks. See log.h.
#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/core/ring.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_CORE, "core", NPH_LOG_LEVEL_TRACE);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct LogThread {
    NphSpscRing ring;
    NphAtomicU32 retired; // set by the owning thread on detach
    NphAtomicU32 dropped; // records lost since the last delivered one
    u32 index;            // 1-based
    u32 sequence;
} LogThread;

typedef struct LogSink {
    NphLogSinkFn fn;
    void *user;
    u32 min_level;
} LogSink;

// Process-wide diagnostic state (the exception CLAUDE.md §2 allows).
static struct {
    bool initialized;
    NphAllocator *alloc;
    u32 ring_capacity;
    u32 max_threads;
    NphAtomicU32 global_level;
    NphAtomicU32 next_thread_index;
    NphAtomicU32 flushing;
    NphAtomicPtr threads[NPH_LOG_MAX_THREADS];
    NphSpinLock sink_lock;
    LogSink sinks[NPH_LOG_MAX_SINKS];
    u32 sink_count;
} g_log = {false,
           NULL,
           0U,
           0U,
           NPH_ATOMIC_U32_INIT(0U),
           NPH_ATOMIC_U32_INIT(0U),
           NPH_ATOMIC_U32_INIT(0U),
           {{NULL}},
           NPH_SPIN_LOCK_INIT,
           {{NULL, NULL, 0U}},
           0U};

static NPH_THREAD_LOCAL LogThread *t_log_thread;

u32 nph_log_get_global_level_fast(void)
{
    return nph_atomic_load_u32(&g_log.global_level, NPH_MEMORY_ORDER_RELAXED);
}

static u64 now_ns(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0U;
    }
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult nph_log_init(const NphLogConfig *config)
{
    NPH_ASSERT(config != NULL && config->alloc != NULL);
    if (g_log.initialized) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    const u32 capacity = config->ring_capacity != 0U ? config->ring_capacity : 1024U;
    const u32 threads = config->max_threads != 0U ? config->max_threads : 64U;
    if (!nph_is_pow2_u32(capacity) || capacity < 2U || threads > NPH_LOG_MAX_THREADS) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    g_log.alloc = config->alloc;
    g_log.ring_capacity = capacity;
    g_log.max_threads = threads;
    nph_atomic_store_u32(&g_log.next_thread_index, 0U, NPH_MEMORY_ORDER_RELAXED);
    nph_atomic_store_u32(&g_log.flushing, 0U, NPH_MEMORY_ORDER_RELAXED);
    for (u32 i = 0; i < NPH_LOG_MAX_THREADS; ++i) {
        nph_atomic_store_ptr(&g_log.threads[i], NULL, NPH_MEMORY_ORDER_RELAXED);
    }
    nph_spin_lock_init(&g_log.sink_lock);
    g_log.sink_count = 0U;
    nph_atomic_thread_fence(NPH_MEMORY_ORDER_SEQ_CST);
    g_log.initialized = true;
    return NPH_RESULT_OK;
}

static void free_thread(LogThread *thread)
{
    nph_spsc_ring_free(&thread->ring);
    NPH_FREE_TYPE(g_log.alloc, LogThread, thread);
}

void nph_log_shutdown(void)
{
    if (!g_log.initialized) {
        return;
    }
    (void)nph_log_flush(0U);
    for (u32 i = 0; i < g_log.max_threads; ++i) {
        LogThread *thread =
            (LogThread *)nph_atomic_exchange_ptr(&g_log.threads[i], NULL, NPH_MEMORY_ORDER_ACQ_REL);
        if (thread != NULL) {
            free_thread(thread);
        }
    }
    t_log_thread = NULL;
    g_log.sink_count = 0U;
    g_log.initialized = false;
}

bool nph_log_is_initialized(void)
{
    return g_log.initialized;
}

NphResult nph_log_add_sink(NphLogSinkFn fn, void *user, NphLogLevel min_level)
{
    NPH_ASSERT(fn != NULL);
    nph_spin_lock_acquire(&g_log.sink_lock);
    NphResult r = NPH_RESULT_FULL;
    if (g_log.sink_count < NPH_LOG_MAX_SINKS) {
        LogSink *s = &g_log.sinks[g_log.sink_count++];
        s->fn = fn;
        s->user = user;
        s->min_level = (u32)min_level;
        r = NPH_RESULT_OK;
    }
    nph_spin_lock_release(&g_log.sink_lock);
    return r;
}

bool nph_log_remove_sink(NphLogSinkFn fn, void *user)
{
    bool removed = false;
    nph_spin_lock_acquire(&g_log.sink_lock);
    for (u32 i = 0; i < g_log.sink_count; ++i) {
        if (g_log.sinks[i].fn == fn && g_log.sinks[i].user == user) {
            for (u32 j = i + 1U; j < g_log.sink_count; ++j) {
                g_log.sinks[j - 1U] = g_log.sinks[j];
            }
            g_log.sink_count -= 1U;
            removed = true;
            break;
        }
    }
    nph_spin_lock_release(&g_log.sink_lock);
    return removed;
}

void nph_log_set_level(NphLogCategory *category, NphLogLevel level)
{
    NPH_ASSERT(category != NULL);
    nph_atomic_store_u32(&category->min_level, (u32)level, NPH_MEMORY_ORDER_RELAXED);
}

NphLogLevel nph_log_get_level(const NphLogCategory *category)
{
    NPH_ASSERT(category != NULL);
    return (NphLogLevel)nph_atomic_load_u32(&category->min_level, NPH_MEMORY_ORDER_RELAXED);
}

void nph_log_set_global_level(NphLogLevel level)
{
    nph_atomic_store_u32(&g_log.global_level, (u32)level, NPH_MEMORY_ORDER_RELAXED);
}

NphLogLevel nph_log_get_global_level(void)
{
    return (NphLogLevel)nph_log_get_global_level_fast();
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

static NphResult attach_thread(LogThread **out)
{
    if (t_log_thread != NULL) {
        *out = t_log_thread;
        return NPH_RESULT_OK;
    }
    LogThread *thread = NPH_ALLOC_TYPE(g_log.alloc, LogThread);
    if (thread == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    const NphResult r =
        nph_spsc_ring_init(&thread->ring, g_log.alloc, sizeof(NphLogRecord), g_log.ring_capacity);
    if (r != NPH_RESULT_OK) {
        NPH_FREE_TYPE(g_log.alloc, LogThread, thread);
        return r;
    }
    thread->index =
        nph_atomic_fetch_add_u32(&g_log.next_thread_index, 1U, NPH_MEMORY_ORDER_RELAXED) + 1U;
    // Claim a free registry slot. The release pairs with the flusher's
    // acquire, publishing the initialised ring.
    for (u32 i = 0; i < g_log.max_threads; ++i) {
        void *expected = NULL;
        if (nph_atomic_compare_exchange_ptr(&g_log.threads[i], &expected, thread,
                                            NPH_MEMORY_ORDER_RELEASE, NPH_MEMORY_ORDER_RELAXED)) {
            t_log_thread = thread;
            *out = thread;
            return NPH_RESULT_OK;
        }
    }
    free_thread(thread);
    return NPH_RESULT_FULL;
}

NphResult nph_log_thread_attach(void)
{
    if (!g_log.initialized) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    LogThread *thread;
    return attach_thread(&thread);
}

void nph_log_thread_detach(void)
{
    LogThread *thread = t_log_thread;
    if (thread == NULL) {
        return;
    }
    // The flusher frees the ring once it is empty; the thread must not
    // touch it afterwards, so forget it now.
    t_log_thread = NULL;
    nph_atomic_store_u32(&thread->retired, 1U, NPH_MEMORY_ORDER_RELEASE);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static void fill_record(NphLogRecord *rec, const NphLogCategory *category, NphLogLevel level,
                        const char *file, u32 line)
{
    rec->timestamp_ns = now_ns();
    rec->category = category->name;
    rec->file = file;
    rec->line = line;
    rec->level = (u16)level;
    rec->thread_index = 0U;
    rec->sequence = 0U;
    rec->dropped_before = 0U;
}

// Copies the message, truncating with "..." when it does not fit.
static void set_message(NphLogRecord *rec, const char *text, u64 len)
{
    if (len >= NPH_LOG_MESSAGE_MAX) {
        memcpy(rec->message, text, NPH_LOG_MESSAGE_MAX - 4U);
        memcpy(rec->message + NPH_LOG_MESSAGE_MAX - 4U, "...", 4U);
    } else {
        memcpy(rec->message, text, (size_t)len);
        rec->message[len] = '\0';
    }
}

// Before init (or when a thread cannot attach) a record goes straight to
// stderr so it is never lost.
static void write_synchronous(const NphLogRecord *rec)
{
    char line[NPH_LOG_MESSAGE_MAX + 128U];
    nph_log_format(rec, line, sizeof line, false, true);
    fputs(line, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

static void store_record(NphLogRecord *rec)
{
    if (!g_log.initialized) {
        write_synchronous(rec);
        return;
    }
    LogThread *thread;
    if (attach_thread(&thread) != NPH_RESULT_OK) {
        write_synchronous(rec);
        return;
    }
    rec->thread_index = (u16)thread->index;
    rec->sequence = thread->sequence++;
    rec->dropped_before = nph_atomic_exchange_u32(&thread->dropped, 0U, NPH_MEMORY_ORDER_RELAXED);
    if (!nph_spsc_ring_push(&thread->ring, rec)) {
        // Put the count back (plus this one) for the next record that fits.
        nph_atomic_fetch_add_u32(&thread->dropped, rec->dropped_before + 1U,
                                 NPH_MEMORY_ORDER_RELAXED);
    }
}

void nph_log_write_v(NphLogCategory *category, NphLogLevel level, const char *file, u32 line,
                     const char *fmt, va_list args)
{
    NPH_ASSERT(category != NULL && fmt != NULL);
    NphLogRecord rec;
    fill_record(&rec, category, level, file, line);
    // Format straight into the record; vsnprintf truncates and reports the
    // full length so the "..." marker can be applied.
    const int n = vsnprintf(rec.message, sizeof rec.message, fmt, args);
    if (n < 0) {
        set_message(&rec, "<format error>", 14U);
    } else if ((u64)n >= NPH_LOG_MESSAGE_MAX) {
        memcpy(rec.message + NPH_LOG_MESSAGE_MAX - 4U, "...", 4U);
    }
    store_record(&rec);
}

void nph_log_write(NphLogCategory *category, NphLogLevel level, const char *file, u32 line,
                   const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    nph_log_write_v(category, level, file, line, fmt, args);
    va_end(args);
}

void nph_log_write_str(NphLogCategory *category, NphLogLevel level, const char *file, u32 line,
                       NphStr message)
{
    NPH_ASSERT(category != NULL);
    NphLogRecord rec;
    fill_record(&rec, category, level, file, line);
    set_message(&rec, message.data != NULL ? message.data : "", message.len);
    store_record(&rec);
}

// ---------------------------------------------------------------------------
// Flushing
// ---------------------------------------------------------------------------

static void deliver(const NphLogRecord *rec)
{
    // Sinks may be added or removed between flushes; copy the table under
    // the lock so a sink's own logging (which must not happen anyway) or a
    // slow sink never holds it.
    LogSink sinks[NPH_LOG_MAX_SINKS];
    nph_spin_lock_acquire(&g_log.sink_lock);
    const u32 count = g_log.sink_count;
    memcpy(sinks, g_log.sinks, sizeof sinks);
    nph_spin_lock_release(&g_log.sink_lock);
    for (u32 i = 0; i < count; ++i) {
        if ((u32)rec->level >= sinks[i].min_level) {
            sinks[i].fn(rec, sinks[i].user);
        }
    }
}

u32 nph_log_flush(u32 max_records)
{
    if (!g_log.initialized) {
        return 0U;
    }
    u32 expected = 0U;
    if (!nph_atomic_compare_exchange_u32(&g_log.flushing, &expected, 1U, NPH_MEMORY_ORDER_ACQUIRE,
                                         NPH_MEMORY_ORDER_RELAXED)) {
        return 0U;
    }
    u32 delivered = 0U;
    const u32 limit = max_records != 0U ? max_records : NPH_U32_MAX;
    while (delivered < limit) {
        // k-way merge by timestamp over the heads of every ring.
        LogThread *best = NULL;
        u64 best_time = NPH_U64_MAX;
        for (u32 i = 0; i < g_log.max_threads; ++i) {
            LogThread *thread =
                (LogThread *)nph_atomic_load_ptr(&g_log.threads[i], NPH_MEMORY_ORDER_ACQUIRE);
            if (thread == NULL) {
                continue;
            }
            const NphLogRecord *head = (const NphLogRecord *)nph_spsc_ring_peek(&thread->ring);
            if (head != NULL && head->timestamp_ns < best_time) {
                best_time = head->timestamp_ns;
                best = thread;
            }
        }
        if (best == NULL) {
            break;
        }
        NphLogRecord rec;
        (void)nph_spsc_ring_pop(&best->ring, &rec);
        deliver(&rec);
        ++delivered;
    }
    // Release the rings of detached threads once they are drained.
    for (u32 i = 0; i < g_log.max_threads; ++i) {
        LogThread *thread =
            (LogThread *)nph_atomic_load_ptr(&g_log.threads[i], NPH_MEMORY_ORDER_ACQUIRE);
        if (thread != NULL &&
            nph_atomic_load_u32(&thread->retired, NPH_MEMORY_ORDER_ACQUIRE) != 0U &&
            nph_spsc_ring_peek(&thread->ring) == NULL) {
            // Records dropped after the thread's last delivered one had no
            // later record to carry their count, and the thread is gone.
            // Report them in its name so the loss is never silent.
            const u32 dropped = nph_atomic_load_u32(&thread->dropped, NPH_MEMORY_ORDER_RELAXED);
            if (dropped != 0U) {
                NphLogRecord rec;
                fill_record(&rec, &NPH_LOG_CORE, NPH_LOG_LEVEL_WARN, __FILE__, (u32)__LINE__);
                rec.thread_index = (u16)thread->index;
                rec.sequence = thread->sequence;
                rec.dropped_before = dropped;
                set_message(&rec, "log thread detached with dropped records", 40U);
                deliver(&rec);
                ++delivered;
            }
            nph_atomic_store_ptr(&g_log.threads[i], NULL, NPH_MEMORY_ORDER_RELEASE);
            free_thread(thread);
        }
    }
    nph_atomic_store_u32(&g_log.flushing, 0U, NPH_MEMORY_ORDER_RELEASE);
    return delivered;
}

// ---------------------------------------------------------------------------
// Formatting and sinks
// ---------------------------------------------------------------------------

const char *nph_log_level_name(NphLogLevel level)
{
    switch (level) {
    case NPH_LOG_LEVEL_TRACE:
        return "TRACE";
    case NPH_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case NPH_LOG_LEVEL_INFO:
        return "INFO";
    case NPH_LOG_LEVEL_WARN:
        return "WARN";
    case NPH_LOG_LEVEL_ERROR:
        return "ERROR";
    case NPH_LOG_LEVEL_FATAL:
        return "FATAL";
    case NPH_LOG_LEVEL_COUNT:
        break;
    }
    return "?";
}

static const char *level_color(u16 level)
{
    switch ((NphLogLevel)level) {
    case NPH_LOG_LEVEL_TRACE:
        return "\x1b[90m"; // bright black
    case NPH_LOG_LEVEL_DEBUG:
        return "\x1b[36m"; // cyan
    case NPH_LOG_LEVEL_INFO:
        return "\x1b[32m"; // green
    case NPH_LOG_LEVEL_WARN:
        return "\x1b[33m"; // yellow
    case NPH_LOG_LEVEL_ERROR:
        return "\x1b[31m"; // red
    case NPH_LOG_LEVEL_FATAL:
        return "\x1b[35m"; // magenta
    case NPH_LOG_LEVEL_COUNT:
        break;
    }
    return "";
}

static const char *path_basename(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

u64 nph_log_format(const NphLogRecord *rec, char *out, u64 cap, bool color, bool with_location)
{
    NPH_ASSERT(rec != NULL && out != NULL);
    const u64 total_ms = rec->timestamp_ns / 1000000ULL;
    const u64 day_ms = total_ms % (24ULL * 3600ULL * 1000ULL);
    const unsigned hours = (unsigned)(day_ms / 3600000ULL);
    const unsigned minutes = (unsigned)(day_ms / 60000ULL % 60ULL);
    const unsigned seconds = (unsigned)(day_ms / 1000ULL % 60ULL);
    const unsigned millis = (unsigned)(day_ms % 1000ULL);
    const bool location = with_location || rec->level >= (u16)NPH_LOG_LEVEL_WARN;
    const char *reset = color ? "\x1b[0m" : "";
    int n;
    if (location) {
        n = snprintf(out, (size_t)cap, "%02u:%02u:%02u.%03u %s%-5s%s [%s] %s (%s:%u)", hours,
                     minutes, seconds, millis, color ? level_color(rec->level) : "",
                     nph_log_level_name((NphLogLevel)rec->level), reset, rec->category,
                     rec->message, path_basename(rec->file != NULL ? rec->file : ""), rec->line);
    } else {
        n = snprintf(out, (size_t)cap, "%02u:%02u:%02u.%03u %s%-5s%s [%s] %s", hours, minutes,
                     seconds, millis, color ? level_color(rec->level) : "",
                     nph_log_level_name((NphLogLevel)rec->level), reset, rec->category,
                     rec->message);
    }
    if (n < 0) {
        if (cap > 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    u64 len = (u64)n;
    if (rec->dropped_before > 0U && len < cap) {
        const int m = snprintf(out + len, (size_t)(cap - len), " [%u earlier records dropped]",
                               rec->dropped_before);
        if (m > 0) {
            len += (u64)m;
        }
    }
    return len;
}

void nph_log_console_sink(const NphLogRecord *rec, void *user)
{
    const NphLogConsoleSink *cfg = (const NphLogConsoleSink *)user;
    char line[NPH_LOG_MESSAGE_MAX + 160U];
    nph_log_format(rec, line, sizeof line, cfg != NULL && cfg->color,
                   cfg != NULL && cfg->with_location);
    FILE *stream = rec->level >= (u16)NPH_LOG_LEVEL_WARN ? stderr : stdout;
    fputs(line, stream);
    fputc('\n', stream);
    if (stream == stderr) {
        fflush(stream);
    }
}

NphResult nph_log_file_sink_open(NphLogFileSink *sink, const char *path)
{
    NPH_ASSERT(sink != NULL && path != NULL);
    sink->file = fopen(path, "ab");
    return sink->file != NULL ? NPH_RESULT_OK : NPH_RESULT_IO_ERROR;
}

void nph_log_file_sink_close(NphLogFileSink *sink)
{
    if (sink != NULL && sink->file != NULL) {
        fclose(sink->file);
        sink->file = NULL;
    }
}

void nph_log_file_sink(const NphLogRecord *rec, void *user)
{
    NphLogFileSink *sink = (NphLogFileSink *)user;
    if (sink == NULL || sink->file == NULL) {
        return;
    }
    char line[NPH_LOG_MESSAGE_MAX + 160U];
    nph_log_format(rec, line, sizeof line, false, true);
    fputs(line, sink->file);
    fputc('\n', sink->file);
    if (rec->level >= (u16)NPH_LOG_LEVEL_ERROR) {
        fflush(sink->file);
    }
}
