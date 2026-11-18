// SPDX-License-Identifier: Apache-2.0
// The crash handler's common half (docs/adr/0103-crash-handler.md): its
// settings, the notes, install and uninstall, the assert hook, and the
// report's text. What a report is written with avoids the C library's
// formatting and allocation: it may run inside a signal handler.
#include <nepheline/core/assert.h>
#include <nepheline/core/version.h>
#include <nepheline/platform/crash.h>
#include <nepheline/platform/file.h>

#include "crash_internal.h"

#include <stdio.h>
#include <string.h>

// Process-wide, as the OS hooks are (crash.h).
CrashState g_crash;

static NphAssertHandler g_prev_assert;
static void *g_prev_assert_user;

// The log's last records (nph_crash_log_sink): a ring of formatted lines and
// how many were kept, which a report reads without a lock.
#define CRASH_LOG_LINE_MAX (NPH_LOG_MESSAGE_MAX + 128U)
static char g_log_lines[NPH_CRASH_LOG_LINES][CRASH_LOG_LINE_MAX];
static NphAtomicU32 g_log_count;

#if NPH_CONFIG_DEBUG
#define CRASH_CONFIG "Debug"
#elif NPH_CONFIG_DEVELOPMENT
#define CRASH_CONFIG "Development"
#elif NPH_CONFIG_PROFILE
#define CRASH_CONFIG "Profile"
#else
#define CRASH_CONFIG "Shipping"
#endif

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void crash_out_init(CrashOut *o, i64 file)
{
    o->file = file;
    o->len = 0U;
    o->failed = 0;
}

void crash_out_flush(CrashOut *o)
{
    if (o->len != 0U && !o->failed && !crash_os_write(o->file, o->buf, o->len)) {
        o->failed = 1;
    }
    o->len = 0U;
}

void crash_out_bytes(CrashOut *o, const char *s, u64 n)
{
    while (n != 0U) {
        if (o->len == sizeof o->buf) {
            crash_out_flush(o);
        }
        u64 take = sizeof o->buf - o->len;
        if (take > n) {
            take = n;
        }
        memcpy(o->buf + o->len, s, (size_t)take);
        o->len += (u32)take;
        s += take;
        n -= take;
    }
}

void crash_out_str(CrashOut *o, const char *s)
{
    if (s != NULL) {
        crash_out_bytes(o, s, (u64)strlen(s));
    }
}

void crash_out_dec(CrashOut *o, u64 v)
{
    char digits[20];
    u32 n = 0U;
    do {
        digits[sizeof digits - 1U - n] = (char)('0' + (char)(v % 10U));
        v /= 10U;
        ++n;
    } while (v != 0U);
    crash_out_bytes(o, digits + sizeof digits - n, n);
}

void crash_out_hex(CrashOut *o, u64 v, u32 digits)
{
    static const char k_hex[] = "0123456789ABCDEF";
    char text[18];
    u32 n = 0U;
    do {
        text[sizeof text - 1U - n] = k_hex[v & 0xFU];
        v >>= 4U;
        ++n;
    } while ((v != 0U || n < digits) && n < 16U);
    crash_out_bytes(o, "0x", 2U);
    crash_out_bytes(o, text + sizeof text - n, n);
}

// ---------------------------------------------------------------------------
// Time and names
// ---------------------------------------------------------------------------

CrashTime crash_time_from_unix(i64 seconds)
{
    const u64 s = seconds > 0 ? (u64)seconds : 0U;
    const u64 days = s / 86400U;
    const u64 rem = s % 86400U;
    CrashTime t;
    t.hour = (u32)(rem / 3600U);
    t.minute = (u32)(rem % 3600U / 60U);
    t.second = (u32)(rem % 60U);
    // civil_from_days with the era counted from 0000-03-01; days are not
    // negative here, so neither is the era.
    const u64 z = days + 719468U;
    const u64 era = z / 146097U;
    const u64 doe = z - era * 146097U;                                         // [0, 146096]
    const u64 yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U; // [0, 399]
    const u64 doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);                // [0, 365]
    const u64 mp = (5U * doy + 2U) / 153U;                                     // [0, 11]
    t.day = (u32)(doy - (153U * mp + 2U) / 5U + 1U);
    t.month = (u32)(mp < 10U ? mp + 3U : mp - 9U);
    t.year = (u32)(yoe + era * 400U + (t.month <= 2U ? 1U : 0U));
    return t;
}

typedef struct CrashText {
    char *buf;
    u32 cap;
    u32 len;
    b32 overflow;
} CrashText;

static void text_bytes(CrashText *t, const char *s, u32 n)
{
    if (t->overflow || t->len + n >= t->cap) {
        t->overflow = 1;
        return;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
}

// `v` in decimal, zero-padded to `width` digits.
static void text_dec(CrashText *t, u64 v, u32 width)
{
    char digits[20];
    u32 n = 0U;
    do {
        digits[sizeof digits - 1U - n] = (char)('0' + (char)(v % 10U));
        v /= 10U;
        ++n;
    } while ((v != 0U || n < width) && n < sizeof digits);
    text_bytes(t, digits + sizeof digits - n, n);
}

u32 crash_stem(char *out, u32 cap, i64 unix_seconds, u64 pid, u32 n)
{
    const CrashTime ct = crash_time_from_unix(unix_seconds);
    CrashText t = {out, cap, 0U, 0};
    text_bytes(&t, g_crash.dir, (u32)strlen(g_crash.dir));
    text_bytes(&t, g_crash.app, (u32)strlen(g_crash.app));
    text_bytes(&t, "-", 1U);
    text_dec(&t, ct.year, 4U);
    text_dec(&t, ct.month, 2U);
    text_dec(&t, ct.day, 2U);
    text_bytes(&t, "-", 1U);
    text_dec(&t, ct.hour, 2U);
    text_dec(&t, ct.minute, 2U);
    text_dec(&t, ct.second, 2U);
    text_bytes(&t, "-", 1U);
    text_dec(&t, pid, 1U);
    if (n != 0U) {
        text_bytes(&t, "-", 1U);
        text_dec(&t, n, 1U);
    }
    if (t.overflow || cap == 0U) {
        if (cap != 0U) {
            out[0] = '\0';
        }
        return 0U;
    }
    out[t.len] = '\0';
    return t.len;
}

// ---------------------------------------------------------------------------
// The report's beginning
// ---------------------------------------------------------------------------

static void out_two(CrashOut *o, u32 v)
{
    char d[2] = {(char)('0' + (char)(v / 10U % 10U)), (char)('0' + (char)(v % 10U))};
    crash_out_bytes(o, d, 2U);
}

void crash_write_header(CrashOut *o, const CrashFault *f)
{
    crash_out_str(o, "Nepheline crash report\n");
    crash_out_str(o, "program:   ");
    crash_out_str(o, g_crash.app);
    crash_out_str(o, " (Nepheline " NPH_VERSION_STRING ", " CRASH_CONFIG ")\n");

    const CrashTime t = crash_time_from_unix(f->unix_seconds);
    crash_out_str(o, "time:      ");
    crash_out_dec(o, t.year);
    crash_out_str(o, "-");
    out_two(o, t.month);
    crash_out_str(o, "-");
    out_two(o, t.day);
    crash_out_str(o, " ");
    out_two(o, t.hour);
    crash_out_str(o, ":");
    out_two(o, t.minute);
    crash_out_str(o, ":");
    out_two(o, t.second);
    crash_out_str(o, " UTC\n");

    crash_out_str(o, "process:   ");
    crash_out_dec(o, f->pid);
    crash_out_str(o, "\nthread:    ");
    crash_out_dec(o, f->thread_id);
    if (f->thread_name != NULL && f->thread_name[0] != '\0') {
        crash_out_str(o, " \"");
        crash_out_str(o, f->thread_name);
        crash_out_str(o, "\"");
    }

    crash_out_str(o, "\nfault:     ");
    crash_out_str(o, f->what);
    crash_out_str(o, " (");
    if (f->code > 0xFFFFU) {
        crash_out_hex(o, f->code, 8U);
    } else {
        crash_out_dec(o, f->code);
    }
    crash_out_str(o, ")");
    if (f->detail != NULL && f->detail[0] != '\0') {
        crash_out_str(o, ", ");
        crash_out_str(o, f->detail);
    }
    if (f->has_address) {
        crash_out_str(o, " at ");
        crash_out_hex(o, f->address, 16U);
    }
    crash_out_str(o, "\nat:        ");
    crash_out_hex(o, f->pc, 16U);
    crash_out_str(o, "\n");

    if (g_crash.assertion[0] != '\0') {
        crash_out_str(o, "assertion: ");
        crash_out_str(o, g_crash.assertion);
        crash_out_str(o, "\n");
    }
    for (u32 i = 0U; i < NPH_CRASH_NOTE_COUNT; ++i) {
        // A note being set may be torn; the terminator at the end of the
        // buffer always bounds it.
        const char *note = g_crash.notes[i];
        if (note[0] == '\0') {
            continue;
        }
        crash_out_str(o, "note ");
        crash_out_dec(o, i);
        crash_out_str(o, ":    ");
        crash_out_bytes(o, note, (u64)strnlen(note, NPH_CRASH_NOTE_MAX - 1U));
        crash_out_str(o, "\n");
    }
}

void crash_write_stderr_line(const char *what, const char *report_path)
{
    CrashOut o;
    crash_out_init(&o, crash_os_stderr());
    crash_out_str(&o, "\n");
    crash_out_str(&o, g_crash.app);
    crash_out_str(&o, ": ");
    crash_out_str(&o, what);
    crash_out_str(&o, "; crash report ");
    crash_out_str(&o, report_path);
    crash_out_str(&o, "\n");
    crash_out_flush(&o);
}

// ---------------------------------------------------------------------------
// The log's last records
// ---------------------------------------------------------------------------

// Copies `text` into `dst` (cap bytes) first byte last, so that a reader
// meanwhile sees the old text, an empty one, or the new one; never one
// without a terminator inside `cap`.
static void store_text(char *dst, u64 cap, const char *text)
{
    const u64 n = (u64)strnlen(text, (size_t)cap - 1U);
    dst[0] = '\0';
    if (n == 0U) {
        return;
    }
    memcpy(dst + 1, text + 1, (size_t)(n - 1U));
    dst[n] = '\0';
    dst[0] = text[0];
}

void nph_crash_log_sink(const NphLogRecord *record, void *user)
{
    NPH_UNUSED(user);
    char line[CRASH_LOG_LINE_MAX];
    (void)nph_log_format(record, line, sizeof line, false, true);
    const u32 n = nph_atomic_load_u32(&g_log_count, NPH_MEMORY_ORDER_RELAXED);
    store_text(g_log_lines[n % NPH_CRASH_LOG_LINES], CRASH_LOG_LINE_MAX, line);
    nph_atomic_store_u32(&g_log_count, n + 1U, NPH_MEMORY_ORDER_RELEASE);
}

void crash_write_log_tail(CrashOut *o)
{
    const u32 count = nph_atomic_load_u32(&g_log_count, NPH_MEMORY_ORDER_ACQUIRE);
    if (count == 0U) {
        return;
    }
    const u32 first = count > NPH_CRASH_LOG_LINES ? count - NPH_CRASH_LOG_LINES : 0U;
    crash_out_str(o, "log:       the last records delivered, oldest first\n");
    for (u32 i = first; i != count; ++i) {
        const char *line = g_log_lines[i % NPH_CRASH_LOG_LINES];
        if (line[0] == '\0') {
            continue; // being written when the process failed
        }
        crash_out_str(o, "  ");
        crash_out_bytes(o, line, (u64)strnlen(line, CRASH_LOG_LINE_MAX - 1U));
        crash_out_str(o, "\n");
    }
}

void crash_drain_log(void)
{
    if (g_crash.drain_log && nph_log_is_initialized()) {
        (void)nph_log_drain();
        (void)fflush(NULL); // a file sink keeps what is below an error buffered
    }
}

// ---------------------------------------------------------------------------
// Notes and the assert hook
// ---------------------------------------------------------------------------

void nph_crash_set_note(u32 slot, const char *text)
{
    NPH_ASSERT(slot < NPH_CRASH_NOTE_COUNT);
    if (slot >= NPH_CRASH_NOTE_COUNT) {
        return;
    }
    store_text(g_crash.notes[slot], NPH_CRASH_NOTE_MAX, text != NULL ? text : "");
}

static NphAssertAction crash_assert_hook(const NphAssertInfo *info, void *user)
{
    NPH_UNUSED(user);
    // An assertion fails in a process that still works, so the C library's
    // formatting is fine here.
    const char *msg = info->message != NULL ? info->message : "";
    (void)snprintf(g_crash.assertion, sizeof g_crash.assertion, "%s at %s:%u in %s()%s%s",
                   info->condition, info->file, info->line, info->function,
                   msg[0] != '\0' ? ": " : "", msg);
    const NphAssertHandler next =
        g_prev_assert != NULL ? g_prev_assert : nph_assert_default_handler;
    return next(info, g_prev_assert_user);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

bool nph_crash_is_installed(void)
{
    return nph_atomic_load_u32(&g_crash.installed, NPH_MEMORY_ORDER_ACQUIRE) != 0U;
}

static bool app_name_ok(const char *name)
{
    const u64 n = (u64)strlen(name);
    if (n == 0U || n >= NPH_CRASH_APP_MAX) {
        return false;
    }
    for (u64 i = 0U; i < n; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

NphResult nph_crash_install(const NphCrashDesc *desc)
{
    NPH_ASSERT(desc != NULL);
    if (nph_crash_is_installed()) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
#if NPH_ASAN_ENABLED || NPH_TSAN_ENABLED
    // The sanitizers handle these signals themselves (crash.h). Named so that
    // what only an install uses is not reported unused.
    NPH_UNUSED(desc);
    (void)app_name_ok;
    (void)crash_assert_hook;
    return NPH_RESULT_UNSUPPORTED;
#else
    const char *app = desc->app_name != NULL ? desc->app_name : "nepheline";
    if (desc->dir == NULL || desc->dir[0] == '\0' || !app_name_ok(app)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    // The directory and the separator a name follows.
    const u64 dir_len = (u64)strlen(desc->dir);
    const char last = desc->dir[dir_len - 1U];
    const bool has_sep = last == '/' || last == '\\';
    if (dir_len + (has_sep ? 0U : 1U) >= NPH_CRASH_DIR_MAX) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    NPH_TRY(nph_fs_create_directories(desc->dir));

    memcpy(g_crash.dir, desc->dir, (size_t)dir_len + 1U); // with its terminator
    if (!has_sep) {
        g_crash.dir[dir_len] = '/';
        g_crash.dir[dir_len + 1U] = '\0';
    }
    memcpy(g_crash.app, app, strlen(app) + 1U);
    g_crash.full_dump = desc->full_dump;
    g_crash.drain_log = !desc->no_log_drain;
    g_crash.assertion[0] = '\0';

    nph_assert_set_handler(crash_assert_hook, NULL, &g_prev_assert, &g_prev_assert_user);
    const NphResult r = crash_os_install();
    if (r != NPH_RESULT_OK) {
        nph_assert_set_handler(g_prev_assert, g_prev_assert_user, NULL, NULL);
        NPH_LOG_ERROR(&NPH_LOG_PLATFORM, "crash handler: %s", nph_result_string(r));
        return r;
    }
    nph_atomic_store_u32(&g_crash.installed, 1U, NPH_MEMORY_ORDER_RELEASE);
    // With the machine, which the log shows where a failure writes no report at all (ADR-0103).
    const char *machine = crash_os_system();
    if (machine[0] != '\0') {
        // (The machine first: a long directory would cut it off at the log's line length.)
        NPH_LOG_INFO(&NPH_LOG_PLATFORM, "crash handler on system: %s; reports go to %s", machine,
                     g_crash.dir);
    } else {
        NPH_LOG_INFO(&NPH_LOG_PLATFORM, "crash handler: reports go to %s", g_crash.dir);
    }
    return NPH_RESULT_OK;
#endif
}

void nph_crash_uninstall(void)
{
    if (!nph_crash_is_installed()) {
        return;
    }
    crash_os_uninstall();
    nph_assert_set_handler(g_prev_assert, g_prev_assert_user, NULL, NULL);
    g_prev_assert = NULL;
    g_prev_assert_user = NULL;
    nph_atomic_store_u32(&g_crash.installed, 0U, NPH_MEMORY_ORDER_RELEASE);
}

NphResult nph_crash_thread_attach(void)
{
    return nph_crash_is_installed() ? crash_os_thread_attach() : NPH_RESULT_OK;
}

void nph_crash_thread_detach(void)
{
    crash_os_thread_detach();
}
