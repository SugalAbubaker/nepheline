// SPDX-License-Identifier: Apache-2.0
// The crash handler's in-process tests: its arithmetic and text, install and
// uninstall, the assert hook, and a whole report written without a crash.
// The handler on real faults is tested by crash_helper in child processes
// (run_crash.cmake).
#include <nepheline/core/assert.h>
#include <nepheline/platform/crash.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/platform.h>

#include "crash_internal.h"
#include "test_platform_support.h"

#include <stdio.h>
#include <string.h>

#if NPH_PLATFORM_WINDOWS
// (windows.h comes with crash_internal.h)
#else
#include <signal.h>
#include <ucontext.h>
#endif

typedef struct CrashDir {
    char dir[1024];
} CrashDir;

static u32 g_crash_dir_counter;

NTEST_SETUP(CrashDir)
{
    char tmp[800];
    NTEST_ASSERT_EQ_INT(nph_fs_temp_directory(tmp, sizeof tmp, NULL), NPH_RESULT_OK);
    snprintf(f->dir, sizeof f->dir, "%s/nph_crash_test_%llu_%u", tmp,
             (unsigned long long)nph_platform_process_id(), g_crash_dir_counter++);
    NTEST_ASSERT_EQ_INT(nph_fs_create_directories(f->dir), NPH_RESULT_OK);
}

NTEST_TEARDOWN(CrashDir)
{
    nph_crash_uninstall();
    for (u32 i = 0U; i < NPH_CRASH_NOTE_COUNT; ++i) {
        nph_crash_set_note(i, NULL);
    }
    g_crash.assertion[0] = '\0';
    NTEST_ASSERT_EQ_INT(nph_fs_remove_tree(f->dir), NPH_RESULT_OK);
}

// Reads a whole (small) file into `buf`, terminated; its length, or -1.
static long read_text(const char *path, char *buf, u32 cap)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    const size_t n = fread(buf, 1U, (size_t)cap - 1U, fp);
    (void)fclose(fp);
    buf[n] = '\0';
    return (long)n;
}

static bool contains(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

// ---------------------------------------------------------------------------
// Time and names
// ---------------------------------------------------------------------------

static void expect_time(i64 seconds, u32 y, u32 mo, u32 d, u32 h, u32 mi, u32 s)
{
    const CrashTime t = crash_time_from_unix(seconds);
    NTEST_EXPECT_EQ_UINT(t.year, y);
    NTEST_EXPECT_EQ_UINT(t.month, mo);
    NTEST_EXPECT_EQ_UINT(t.day, d);
    NTEST_EXPECT_EQ_UINT(t.hour, h);
    NTEST_EXPECT_EQ_UINT(t.minute, mi);
    NTEST_EXPECT_EQ_UINT(t.second, s);
}

NTEST(crash, time_from_unix_gives_known_dates)
{
    expect_time(0, 1970U, 1U, 1U, 0U, 0U, 0U);
    expect_time(-1, 1970U, 1U, 1U, 0U, 0U, 0U); // before 1970: clamped
    expect_time(86399, 1970U, 1U, 1U, 23U, 59U, 59U);
    expect_time(86400, 1970U, 1U, 2U, 0U, 0U, 0U);
    expect_time(951782400, 2000U, 2U, 29U, 0U, 0U, 0U); // a leap century
    expect_time(1700000000, 2023U, 11U, 14U, 22U, 13U, 20U);
    expect_time(4102444800, 2100U, 1U, 1U, 0U, 0U, 0U);
    expect_time(4107542399, 2100U, 2U, 28U, 23U, 59U, 59U); // 2100 has no 29th
    expect_time(4107542400, 2100U, 3U, 1U, 0U, 0U, 0U);
    expect_time(253402300799, 9999U, 12U, 31U, 23U, 59U, 59U);
}

static u32 days_in_month(u32 year, u32 month)
{
    static const u32 k_days[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    const bool leap = (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
    return month == 2U && leap ? 29U : k_days[month - 1U];
}

NTEST(crash, time_from_unix_steps_one_day_at_a_time_for_four_centuries)
{
    CrashTime prev = crash_time_from_unix(0);
    u32 failures = 0U;
    for (i64 day = 1; day < 146097 + 365; ++day) {
        const CrashTime t = crash_time_from_unix(day * 86400);
        CrashTime want = prev;
        if (prev.day < days_in_month(prev.year, prev.month)) {
            want.day = prev.day + 1U;
        } else if (prev.month < 12U) {
            want.month = prev.month + 1U;
            want.day = 1U;
        } else {
            want.year = prev.year + 1U;
            want.month = 1U;
            want.day = 1U;
        }
        if (t.year != want.year || t.month != want.month || t.day != want.day || t.hour != 0U ||
            t.minute != 0U || t.second != 0U) {
            ++failures;
        }
        prev = t;
    }
    NTEST_ASSERT_EQ_UINT(failures, 0U);
    // The last day: 146,097 days (400 Gregorian years) to 2370-01-01, then 364.
    NTEST_ASSERT_EQ_UINT(prev.year, 2370U);
    NTEST_ASSERT_EQ_UINT(prev.month, 12U);
    NTEST_ASSERT_EQ_UINT(prev.day, 31U);
}

NTEST(crash, stem_names_the_report_and_refuses_what_does_not_fit)
{
    memcpy(g_crash.dir, "dir/", 5U);
    memcpy(g_crash.app, "app", 4U);
    char buf[64];
    NTEST_ASSERT_EQ_UINT(crash_stem(buf, sizeof buf, 1700000000, 4321U, 0U),
                         (u32)strlen("dir/app-20231114-221320-4321"));
    NTEST_ASSERT_EQ_STR(buf, "dir/app-20231114-221320-4321");
    NTEST_ASSERT_GT_UINT(crash_stem(buf, sizeof buf, 1700000000, 4321U, 3U), 0U);
    NTEST_ASSERT_EQ_STR(buf, "dir/app-20231114-221320-4321-3");
    char tiny[12];
    NTEST_ASSERT_EQ_UINT(crash_stem(tiny, sizeof tiny, 1700000000, 4321U, 0U), 0U);
    NTEST_ASSERT_EQ_STR(tiny, "");
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

// A file in `dir` the test writes through CrashOut.
static bool open_out(const char *dir, const char *name, char *path, u32 cap, NphFile *file)
{
    snprintf(path, cap, "%s/%s", dir, name);
    return nph_file_open(path, NPH_FILE_WRITE | NPH_FILE_CREATE | NPH_FILE_TRUNCATE, file) ==
           NPH_RESULT_OK;
}

NTEST_F(CrashDir, out_buffers_long_writes_and_formats_numbers)
{
    char path[1200];
    NphFile file;
    NTEST_ASSERT_TRUE(open_out(f->dir, "out.txt", path, sizeof path, &file));
    CrashOut o;
    crash_out_init(&o, (i64)file.native);
    char block[5000];
    memset(block, 'a', sizeof block);
    crash_out_bytes(&o, block, sizeof block); // past the buffer: flushed on the way
    crash_out_str(&o, "|");
    crash_out_dec(&o, 0U);
    crash_out_str(&o, "|");
    crash_out_dec(&o, 18446744073709551615ULL);
    crash_out_str(&o, "|");
    crash_out_hex(&o, 0xC0000005U, 8U);
    crash_out_str(&o, "|");
    crash_out_hex(&o, 0x10U, 16U);
    crash_out_str(&o, "|");
    crash_out_hex(&o, 0U, 1U);
    crash_out_flush(&o);
    NTEST_ASSERT_FALSE(o.failed);
    nph_file_close(&file);

    static char text[8192];
    NTEST_ASSERT_GT_INT(read_text(path, text, sizeof text), 5000);
    NTEST_ASSERT_EQ_STR(text + 5000, "|0|18446744073709551615|0xC0000005|0x0000000000000010|0x0");
    for (u32 i = 0U; i < 5000U; ++i) {
        if (text[i] != 'a') {
            NTEST_FAIL("byte %u of the long write is wrong", i);
            break;
        }
    }
}

NTEST_F(CrashDir, header_says_what_failed_where_and_the_notes)
{
    memcpy(g_crash.app, "crash_test", 11U);
    nph_crash_set_note(0U, "first note");
    nph_crash_set_note(3U, "fourth note");
    (void)snprintf(g_crash.assertion, sizeof g_crash.assertion, "x != 0 at a.c:7 in f(): why");

    CrashFault fault;
    memset(&fault, 0, sizeof fault);
    fault.what = "EXCEPTION_ACCESS_VIOLATION";
    fault.detail = "write";
    fault.code = 0xC0000005U;
    fault.address = 0x10U;
    fault.has_address = 1;
    fault.pc = 0x1234U;
    fault.thread_id = 77U;
    fault.thread_name = "worker";
    fault.unix_seconds = 1700000000;
    fault.pid = 4321U;

    char path[1200];
    NphFile file;
    NTEST_ASSERT_TRUE(open_out(f->dir, "header.txt", path, sizeof path, &file));
    CrashOut o;
    crash_out_init(&o, (i64)file.native);
    crash_write_header(&o, &fault);
    fault.what = "SIGSEGV";
    fault.detail = NULL;
    fault.code = 11U;
    fault.has_address = 0;
    crash_write_header(&o, &fault); // a signal's number is written in decimal
    crash_out_flush(&o);
    nph_file_close(&file);

    static char text[8192];
    NTEST_ASSERT_GT_INT(read_text(path, text, sizeof text), 0);
    NTEST_EXPECT_TRUE(contains(text, "Nepheline crash report\n"));
    NTEST_EXPECT_TRUE(contains(text, "program:   crash_test (Nepheline "));
    NTEST_EXPECT_TRUE(contains(text, "time:      2023-11-14 22:13:20 UTC\n"));
    NTEST_EXPECT_TRUE(contains(text, "process:   4321\n"));
    NTEST_EXPECT_TRUE(contains(text, "thread:    77 \"worker\"\n"));
    NTEST_EXPECT_TRUE(contains(
        text, "fault:     EXCEPTION_ACCESS_VIOLATION (0xC0000005), write at 0x0000000000000010\n"));
    NTEST_EXPECT_TRUE(contains(text, "at:        0x0000000000001234\n"));
    NTEST_EXPECT_TRUE(contains(text, "assertion: x != 0 at a.c:7 in f(): why\n"));
    NTEST_EXPECT_TRUE(contains(text, "note 0:    first note\n"));
    NTEST_EXPECT_TRUE(contains(text, "note 3:    fourth note\n"));
    NTEST_EXPECT_FALSE(contains(text, "note 1:"));
    NTEST_EXPECT_TRUE(contains(text, "fault:     SIGSEGV (11)\n"));
}

NTEST_F(CrashDir, the_log_tail_keeps_the_last_records_oldest_first)
{
    NphLogRecord rec;
    memset(&rec, 0, sizeof rec);
    rec.category = "test";
    rec.file = "t.c";
    rec.line = 1U;
    rec.level = (u16)NPH_LOG_LEVEL_INFO;
    // More than the ring holds: the first ones fall out.
    for (u32 i = 0U; i < NPH_CRASH_LOG_LINES + 6U; ++i) {
        (void)snprintf(rec.message, sizeof rec.message, "tail record %02u", i);
        nph_crash_log_sink(&rec, NULL);
    }
    char path[1200];
    NphFile file;
    NTEST_ASSERT_TRUE(open_out(f->dir, "tail.txt", path, sizeof path, &file));
    CrashOut o;
    crash_out_init(&o, (i64)file.native);
    crash_write_log_tail(&o);
    crash_out_flush(&o);
    nph_file_close(&file);

    static char text[65536];
    NTEST_ASSERT_GT_INT(read_text(path, text, sizeof text), 0);
    NTEST_EXPECT_TRUE(contains(text, "log:       the last records delivered, oldest first\n"));
    NTEST_EXPECT_FALSE(contains(text, "tail record 05"));
    const char *first = strstr(text, "tail record 06");
    const char *last = strstr(text, "tail record 69");
    NTEST_ASSERT_NOT_NULL(first);
    NTEST_ASSERT_NOT_NULL(last);
    NTEST_EXPECT_TRUE(first < last);
    u32 lines = 0U;
    for (const char *p = text; (p = strstr(p, "tail record ")) != NULL; ++p) {
        ++lines;
    }
    NTEST_EXPECT_EQ_UINT(lines, NPH_CRASH_LOG_LINES);
}

NTEST(crash, notes_are_truncated_and_cleared)
{
    char longer[NPH_CRASH_NOTE_MAX + 40U];
    memset(longer, 'n', sizeof longer - 1U);
    longer[sizeof longer - 1U] = '\0';
    nph_crash_set_note(1U, longer);
    NTEST_ASSERT_EQ_UINT((u64)strlen(g_crash.notes[1]), (u64)NPH_CRASH_NOTE_MAX - 1U);
    nph_crash_set_note(1U, "short");
    NTEST_ASSERT_EQ_STR(g_crash.notes[1], "short");
    nph_crash_set_note(1U, "");
    NTEST_ASSERT_EQ_STR(g_crash.notes[1], "");
    nph_crash_set_note(1U, "again");
    nph_crash_set_note(1U, NULL);
    NTEST_ASSERT_EQ_STR(g_crash.notes[1], "");
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

static const char *install_or_skip(const char *dir, const char *app)
{
    NphCrashDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.dir = dir;
    desc.app_name = app;
    desc.no_log_drain = 1;
    const NphResult r = nph_crash_install(&desc);
    if (r == NPH_RESULT_UNSUPPORTED) {
        return "the sanitizers own the signals in this build";
    }
    NTEST_ASSERT_EQ_INT(r, NPH_RESULT_OK);
    return NULL;
}

NTEST_F(CrashDir, install_checks_its_description)
{
    NphCrashDesc desc;
    memset(&desc, 0, sizeof desc);
#if NPH_ASAN_ENABLED || NPH_TSAN_ENABLED
    desc.dir = f->dir;
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_UNSUPPORTED);
    NTEST_ASSERT_FALSE(nph_crash_is_installed());
    NTEST_SKIP("the sanitizers own the signals in this build");
#else
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_INVALID_ARGUMENT); // no directory
    desc.dir = f->dir;
    desc.app_name = "bad/name";
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_INVALID_ARGUMENT);
    desc.app_name = "";
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_INVALID_ARGUMENT);
    char long_app[NPH_CRASH_APP_MAX + 1U];
    memset(long_app, 'a', sizeof long_app - 1U);
    long_app[sizeof long_app - 1U] = '\0';
    desc.app_name = long_app;
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_INVALID_ARGUMENT);
    char long_dir[NPH_CRASH_DIR_MAX + 8U];
    memset(long_dir, 'd', sizeof long_dir - 1U);
    long_dir[sizeof long_dir - 1U] = '\0';
    desc.dir = long_dir;
    desc.app_name = NULL;
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_FALSE(nph_crash_is_installed());

    // A directory that does not exist yet is made, its parents with it.
    char nested[1200];
    snprintf(nested, sizeof nested, "%s/reports/today", f->dir);
    desc.dir = nested;
    desc.app_name = "crash_test.v2";
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_crash_is_installed());
    NTEST_ASSERT_TRUE(nph_fs_exists(nested));
    NTEST_ASSERT_EQ_STR(g_crash.app, "crash_test.v2");
    NTEST_ASSERT_EQ_UINT((u64)strlen(g_crash.dir), (u64)strlen(nested) + 1U); // and a separator
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_ALREADY_EXISTS);
    nph_crash_uninstall();
    NTEST_ASSERT_FALSE(nph_crash_is_installed());
    nph_crash_uninstall(); // twice is nothing

    desc.app_name = NULL;
    NTEST_ASSERT_EQ_INT(nph_crash_install(&desc), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(g_crash.app, "nepheline");
    nph_crash_uninstall();
#endif
}

static u32 g_test_assert_calls;

static NphAssertAction test_assert(const NphAssertInfo *info, void *user)
{
    NPH_UNUSED(info);
    NPH_UNUSED(user);
    ++g_test_assert_calls;
    return NPH_ASSERT_ACTION_CONTINUE;
}

NTEST_F(CrashDir, install_wraps_the_assert_handler_and_uninstall_gives_it_back)
{
    NphAssertHandler before = NULL;
    void *before_user = NULL;
    nph_assert_set_handler(test_assert, NULL, &before, &before_user);
    g_test_assert_calls = 0U;
    const char *skip = install_or_skip(f->dir, "crash_test");
    if (skip != NULL) {
        nph_assert_set_handler(before, before_user, NULL, NULL);
        NTEST_SKIP("%s", skip);
        return;
    }
    nph_assert_fail("file.c", "fn", 12U, "1 == 2", "the %s", "message");
    NTEST_EXPECT_EQ_UINT(g_test_assert_calls, 1U); // called on
    NTEST_EXPECT_EQ_STR(g_crash.assertion, "1 == 2 at file.c:12 in fn(): the message");
    nph_assert_fail_nomsg("other.c", "g", 3U, "p != NULL");
    NTEST_EXPECT_EQ_STR(g_crash.assertion, "p != NULL at other.c:3 in g()");
    nph_crash_uninstall();

    NphAssertHandler now = NULL;
    nph_assert_set_handler(before, before_user, &now, NULL);
    NTEST_EXPECT_TRUE(now == test_assert);
}

// ---------------------------------------------------------------------------
// A whole report, written without a crash
// ---------------------------------------------------------------------------

#if NPH_PLATFORM_WINDOWS

NTEST(crash, the_dumps_callback_leaves_out_what_it_cannot_read_and_other_modules_data)
{
    MINIDUMP_CALLBACK_INPUT in;
    MINIDUMP_CALLBACK_OUTPUT out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    // A block dbghelp failed to read is left out and the dump goes on, where
    // by default the one failure fails the whole dump.
    in.CallbackType = ReadMemoryFailureCallback;
    out.Status = E_FAIL;
    NTEST_EXPECT_TRUE(crash_win32_dump_callback(NULL, &in, &out) == TRUE);
    NTEST_EXPECT_EQ_INT((int)out.Status, (int)S_OK);

    // A module elsewhere than beside the executable: its code, not its data.
    wchar_t other[] = L"C:/Windows/System32/other.dll";
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.CallbackType = ModuleCallback;
    in.Module.FullPath = other;
    out.ModuleWriteFlags = (ULONG)ModuleWriteModule | (ULONG)ModuleWriteDataSeg;
    NTEST_EXPECT_TRUE(crash_win32_dump_callback(NULL, &in, &out) == TRUE);
    NTEST_EXPECT_EQ_UINT((u64)out.ModuleWriteFlags, (u64)ModuleWriteModule);

    // Nothing more is asked for: no memory added, no cancel.
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.CallbackType = MemoryCallback;
    NTEST_EXPECT_TRUE(crash_win32_dump_callback(NULL, &in, &out) == FALSE);
    in.CallbackType = CancelCallback;
    NTEST_EXPECT_TRUE(crash_win32_dump_callback(NULL, &in, &out) == FALSE);
}

// A report and a dump written in-process for an access violation at 0x10, its path in `report`.
static void write_test_report(char *report, u32 cap)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    rec.NumberParameters = 2U;
    rec.ExceptionInformation[0] = 1U; // a write
    rec.ExceptionInformation[1] = 0x10U;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the record holds the address as a pointer
    rec.ExceptionAddress = (PVOID)(uptr)ctx.Rip;
    EXCEPTION_POINTERS pointers = {&rec, &ctx};
    crash_win32_write(&pointers, (u32)GetCurrentThreadId(), "tester", NULL, NULL, report, cap);
}

// True when the file at `report` holds `needle`, and the dump beside it is a minidump.
static bool report_says(const char *report, const char *needle)
{
    static char text[65536];
    if (read_text(report, text, sizeof text) <= 0 || !contains(text, needle)) {
        return false;
    }
    char dump[CRASH_PATH_MAX];
    const u64 n = (u64)strlen(report);
    if (n <= 4U || n >= sizeof dump) {
        return false;
    }
    memcpy(dump, report, (size_t)n - 4U);
    memcpy(dump + n - 4U, ".dmp", 5U);
    char sig[8];
    return contains(text, "not written") ||
           (read_text(dump, sig, sizeof sig) >= 4 && memcmp(sig, "MDMP", 4U) == 0);
}

NTEST_F(CrashDir, a_dump_that_fails_is_written_again_then_reduced)
{
    const char *skip = install_or_skip(f->dir, "crash_test");
    if (skip != NULL) {
        NTEST_SKIP("%s", skip);
        return;
    }
    char report[CRASH_PATH_MAX];
    crash_win32_fail_dumps(1U);
    write_test_report(report, sizeof report);
    NTEST_EXPECT_TRUE(report_says(report, "dump:      crash_test-"));
    NTEST_EXPECT_TRUE(report_says(report, ".dmp (written again after error 1784)"));
    crash_win32_fail_dumps(2U);
    write_test_report(report, sizeof report);
    NTEST_EXPECT_TRUE(
        report_says(report, ".dmp (reduced to threads, stacks and modules after error 1784)"));
    crash_win32_fail_dumps(3U);
    write_test_report(report, sizeof report);
    NTEST_EXPECT_TRUE(report_says(report, "dump:      not written (error 1784)"));
    crash_win32_fail_dumps(0U);
}

NTEST_F(CrashDir, a_report_and_its_dump_are_written_for_an_exception)
{
    const char *skip = install_or_skip(f->dir, "crash_test");
    if (skip != NULL) {
        NTEST_SKIP("%s", skip);
        return;
    }
    nph_crash_set_note(2U, "in-process");
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof rec);
    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    rec.NumberParameters = 2U;
    rec.ExceptionInformation[0] = 1U; // a write
    rec.ExceptionInformation[1] = 0x10U;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the record holds the address as a pointer
    rec.ExceptionAddress = (PVOID)(uptr)ctx.Rip;
    EXCEPTION_POINTERS pointers = {&rec, &ctx};
    char report[CRASH_PATH_MAX];
    crash_win32_write(&pointers, (u32)GetCurrentThreadId(), "tester", NULL, NULL, report,
                      sizeof report);
    NTEST_ASSERT_NE_STR(report, "");

    static char text[65536];
    NTEST_ASSERT_GT_INT(read_text(report, text, sizeof text), 0);
    NTEST_EXPECT_TRUE(contains(
        text, "fault:     EXCEPTION_ACCESS_VIOLATION (0xC0000005), write at 0x0000000000000010"));
    NTEST_EXPECT_TRUE(contains(text, "thread:    "));
    NTEST_EXPECT_TRUE(contains(text, "\"tester\""));
    NTEST_EXPECT_TRUE(contains(text, "note 2:    in-process"));
    NTEST_EXPECT_TRUE(contains(text, "module:    platform_tests.exe+0x"));
    NTEST_EXPECT_TRUE(contains(text, "stack:     return addresses, innermost first\n  #0  0x"));
    NTEST_EXPECT_TRUE(contains(text, "  #1  0x")); // the walk went past the first frame
    NTEST_EXPECT_TRUE(contains(text, "dump:      crash_test-"));
    NTEST_EXPECT_FALSE(contains(text, "not written"));

    // The dump beside the report, a minidump by its signature.
    char dump[CRASH_PATH_MAX];
    const u64 n = (u64)strlen(report);
    NTEST_ASSERT_GT_UINT(n, 4U);
    memcpy(dump, report, (size_t)n - 4U);
    memcpy(dump + n - 4U, ".dmp", 5U);
    char sig[8];
    NTEST_ASSERT_GE_INT(read_text(dump, sig, sizeof sig), 4);
    NTEST_EXPECT_EQ_MEM(sig, "MDMP", 4U);
}

#else

NTEST_F(CrashDir, a_report_is_written_for_a_signal)
{
    const char *skip = install_or_skip(f->dir, "crash_test");
    if (skip != NULL) {
        NTEST_SKIP("%s", skip);
        return;
    }
    nph_crash_set_note(2U, "in-process");
    ucontext_t uc;
    memset(&uc, 0, sizeof uc);
    NTEST_ASSERT_EQ_INT(getcontext(&uc), 0);
    static char probe; // the address the fault is said to be at
    siginfo_t info;
    memset(&info, 0, sizeof info);
    info.si_signo = SIGSEGV;
    info.si_code = SEGV_MAPERR;
    info.si_addr = &probe;
    char report[CRASH_PATH_MAX];
    crash_posix_write(SIGSEGV, &info, &uc, false, report, sizeof report);
    NTEST_ASSERT_NE_STR(report, "");

    static char text[262144];
    NTEST_ASSERT_GT_INT(read_text(report, text, sizeof text), 0);
    char fault[128];
    snprintf(fault, sizeof fault,
             "fault:     SIGSEGV (11), SEGV_MAPERR: no mapping there at 0x%016llX",
             (unsigned long long)(uptr)&probe);
    NTEST_EXPECT_TRUE(contains(text, fault));
    NTEST_EXPECT_TRUE(contains(text, "note 2:    in-process"));
    NTEST_EXPECT_TRUE(contains(text, "registers: rip 0x"));
    NTEST_EXPECT_TRUE(contains(text, "stack:     a backtrace"));
    NTEST_EXPECT_TRUE(contains(text, "maps:\n"));
    NTEST_EXPECT_TRUE(contains(text, "r-xp")); // the memory map: an executable mapping
}

typedef struct AltStackSeen {
    NphAtomicU32 enabled;
    NphAtomicU64 size;
} AltStackSeen;

static void read_alt_stack(void *user)
{
    AltStackSeen *seen = (AltStackSeen *)user;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    if (sigaltstack(NULL, &ss) == 0 && (ss.ss_flags & SS_DISABLE) == 0) {
        nph_atomic_store_u32(&seen->enabled, 1U, NPH_MEMORY_ORDER_RELAXED);
        nph_atomic_store_u64(&seen->size, (u64)ss.ss_size, NPH_MEMORY_ORDER_RELAXED);
    }
}

NTEST_F(CrashDir, threads_get_an_alternate_signal_stack_while_installed)
{
    const char *skip = install_or_skip(f->dir, "crash_test");
    if (skip != NULL) {
        NTEST_SKIP("%s", skip);
        return;
    }
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    NTEST_ASSERT_EQ_INT(sigaltstack(NULL, &ss), 0);
    NTEST_EXPECT_EQ_INT(ss.ss_flags & SS_DISABLE, 0); // the installing thread's
    NTEST_EXPECT_EQ_UINT((u64)ss.ss_size, (u64)NPH_CRASH_ALT_STACK_SIZE);

    AltStackSeen seen;
    memset(&seen, 0, sizeof seen);
    NphThreadDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.fn = read_alt_stack;
    desc.user = &seen;
    desc.name = "crash-alt";
    NphThread t;
    NTEST_ASSERT_EQ_INT(nph_thread_create(&t, &desc), NPH_RESULT_OK);
    nph_thread_join(&t);
    NTEST_EXPECT_EQ_UINT(nph_atomic_load_u32(&seen.enabled, NPH_MEMORY_ORDER_RELAXED), 1U);
    NTEST_EXPECT_EQ_UINT(nph_atomic_load_u64(&seen.size, NPH_MEMORY_ORDER_RELAXED),
                         (u64)NPH_CRASH_ALT_STACK_SIZE);

    nph_crash_uninstall();
    NTEST_ASSERT_EQ_INT(sigaltstack(NULL, &ss), 0);
    NTEST_EXPECT_NE_INT(ss.ss_flags & SS_DISABLE, 0); // given back
}

#endif
