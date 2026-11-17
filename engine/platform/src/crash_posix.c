// SPDX-License-Identifier: Apache-2.0
// The crash handler on Linux (docs/adr/0103-crash-handler.md).
//
// One signal handler takes the signals that end a process on a fault. It
// writes the report with async-signal-safe calls (POSIX.1-2017 §2.4.3),
// on the thread's alternate signal stack, then gives the signal back its
// default action and raises it again, so that the process ends as the fault
// would have ended it: its exit status, and a core file where the system
// writes them. backtrace() is the one call outside the list: glibc's loads
// the unwinder on its first call, which install makes, and after that only
// reads the stack and the unwind tables.
#include <nepheline/core/log.h>
#include <nepheline/platform/crash.h>

#include "crash_internal.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

// How long delivering the log's pending records may take before a watchdog
// (SIGALRM, whose default action ends the process) stops it.
#define CRASH_DRAIN_TIMEOUT_S 5U

static const int k_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};
#define CRASH_SIGNAL_COUNT ((u32)(sizeof k_signals / sizeof k_signals[0]))

static struct sigaction g_prev[CRASH_SIGNAL_COUNT];
static NphAtomicU64 g_crashing; // 0, or the id of the thread being reported

// The calling thread's alternate signal stack: its mapping, a guard page
// below the stack included, and the stack it replaced.
static NPH_THREAD_LOCAL void *t_alt_base;
static NPH_THREAD_LOCAL u64 t_alt_size;
static NPH_THREAD_LOCAL stack_t t_alt_prev;

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

bool crash_os_write(i64 file, const void *data, u64 size)
{
    const u8 *p = (const u8 *)data;
    while (size != 0U) {
        const ssize_t n = write((int)file, p, (size_t)size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= (u64)n;
    }
    return true;
}

i64 crash_os_stderr(void)
{
    return (i64)STDERR_FILENO;
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    case SIGTRAP:
        return "SIGTRAP";
    case SIGSYS:
        return "SIGSYS";
    default:
        return "a signal";
    }
}

// What si_code says, for the signals the handler takes.
static const char *code_name(int sig, int code)
{
    if (code <= 0) {
        // Sent rather than raised by the processor: SI_USER (kill), SI_TKILL
        // (tgkill, raise, abort), SI_QUEUE and the others.
        return code == SI_USER ? "sent by kill" : code == SI_TKILL ? "raised" : "sent";
    }
    switch (sig) {
    case SIGSEGV:
        return code == SEGV_MAPERR   ? "SEGV_MAPERR: no mapping there"
               : code == SEGV_ACCERR ? "SEGV_ACCERR: the mapping forbids it"
                                     : "SEGV";
    case SIGBUS:
        return code == BUS_ADRALN   ? "BUS_ADRALN: misaligned"
               : code == BUS_ADRERR ? "BUS_ADRERR: no such physical address"
               : code == BUS_OBJERR ? "BUS_OBJERR: the object's error"
                                    : "BUS";
    case SIGILL:
        return code == ILL_ILLOPC   ? "ILL_ILLOPC: illegal opcode"
               : code == ILL_ILLOPN ? "ILL_ILLOPN: illegal operand"
               : code == ILL_PRVOPC ? "ILL_PRVOPC: privileged opcode"
                                    : "ILL";
    case SIGFPE:
        return code == FPE_INTDIV   ? "FPE_INTDIV: integer division by zero"
               : code == FPE_INTOVF ? "FPE_INTOVF: integer overflow"
               : code == FPE_FLTDIV ? "FPE_FLTDIV: floating-point division by zero"
               : code == FPE_FLTINV ? "FPE_FLTINV: invalid floating-point operation"
                                    : "FPE";
    case SIGTRAP:
        return code == TRAP_BRKPT ? "TRAP_BRKPT: a breakpoint" : "TRAP";
    default:
        return NULL;
    }
}

// Copies the file at `path` into `out`, as far as it reads.
static void copy_file(int out, const char *path)
{
    const int in = open(path, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        return;
    }
    char buf[1024];
    for (;;) {
        const ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0 || !crash_os_write(out, buf, (u64)n)) {
            break;
        }
    }
    (void)close(in);
}

static bool join(char *out, u32 cap, const char *stem, const char *ext)
{
    const u64 a = (u64)strlen(stem);
    const u64 b = (u64)strlen(ext);
    if (a + b + 1U > cap) {
        return false;
    }
    memcpy(out, stem, (size_t)a + 1U); // its terminator, which `ext` then replaces
    memcpy(out + a, ext, (size_t)b + 1U);
    return true;
}

#if NPH_ARCH_X64

typedef struct CrashReg {
    const char *name;
    int index;
} CrashReg;

static const CrashReg k_regs[] = {
    {"rip", REG_RIP}, {"rsp", REG_RSP}, {"rbp", REG_RBP}, {"rax", REG_RAX}, {"rbx", REG_RBX},
    {"rcx", REG_RCX}, {"rdx", REG_RDX}, {"rsi", REG_RSI}, {"rdi", REG_RDI}, {"r8", REG_R8},
    {"r9", REG_R9},   {"r10", REG_R10}, {"r11", REG_R11}, {"r12", REG_R12}, {"r13", REG_R13},
    {"r14", REG_R14}, {"r15", REG_R15},
};

static void out_registers(CrashOut *o, const ucontext_t *uc)
{
    crash_out_str(o, "registers:");
    for (u32 i = 0U; i < (u32)NPH_ARRAY_COUNT(k_regs); ++i) {
        crash_out_str(o, i % 4U == 0U ? (i == 0U ? " " : "\n           ") : "  ");
        crash_out_str(o, k_regs[i].name);
        crash_out_str(o, " ");
        crash_out_hex(o, (u64)uc->uc_mcontext.gregs[k_regs[i].index], 16U);
    }
    crash_out_str(o, "\n");
}

#endif

void crash_posix_write(int sig, const void *info_ptr, const void *ucontext, bool drain,
                       char *out_path, u32 cap)
{
    const siginfo_t *info = (const siginfo_t *)info_ptr;
    const ucontext_t *uc = (const ucontext_t *)ucontext;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        now.tv_sec = 0;
    }
    const u64 pid = (u64)getpid();

    // The report's name: the first that no report of this process took yet.
    char stem[CRASH_PATH_MAX];
    char path[CRASH_PATH_MAX];
    int fd = -1;
    for (u32 n = 0U; n < 16U && fd < 0; ++n) {
        if (crash_stem(stem, sizeof stem, (i64)now.tv_sec, pid, n) == 0U ||
            !join(path, sizeof path, stem, ".txt")) {
            break;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0 && errno != EEXIST) {
            break;
        }
    }
    if (out_path != NULL && cap != 0U) {
        out_path[0] = '\0';
    }
    if (fd < 0) {
        crash_write_stderr_line(signal_name(sig), "not written: the directory cannot be written");
        if (drain) {
            (void)alarm(CRASH_DRAIN_TIMEOUT_S);
            crash_drain_log();
        }
        return;
    }

    char thread_name[17];
    memset(thread_name, 0, sizeof thread_name);
    (void)prctl(PR_GET_NAME, thread_name, 0, 0, 0);

    CrashFault f;
    memset(&f, 0, sizeof f);
    f.what = signal_name(sig);
    f.detail = info != NULL ? code_name(sig, info->si_code) : NULL;
    f.code = (u64)sig;
    if (info != NULL && info->si_code > 0 && (sig == SIGSEGV || sig == SIGBUS)) {
        f.address = (u64)(uptr)info->si_addr;
        f.has_address = 1;
    }
#if NPH_ARCH_X64
    if (uc != NULL) {
        f.pc = (u64)uc->uc_mcontext.gregs[REG_RIP];
    }
#endif
    f.thread_id = (u64)syscall(SYS_gettid);
    f.thread_name = thread_name;
    f.unix_seconds = (i64)now.tv_sec;
    f.pid = pid;

    CrashOut o;
    crash_out_init(&o, (i64)fd);
    crash_write_header(&o, &f);
#if NPH_ARCH_X64
    if (uc != NULL) {
        out_registers(&o, uc);
    }
#endif
    crash_out_str(
        &o, "stack:     a backtrace from the signal handler, innermost first; the frames past\n"
            "           the kernel's signal frame are the fault's (addr2line -e <module> "
            "<+offset>)\n");
    crash_out_flush(&o);
    void *frames[CRASH_FRAMES_MAX];
    const int count = backtrace(frames, (int)CRASH_FRAMES_MAX);
    if (count > 0) {
        backtrace_symbols_fd(frames, count, fd);
    }
    crash_write_stderr_line(f.what, path);
    if (out_path != NULL && cap != 0U) {
        const u64 n = (u64)strlen(path);
        if (n < cap) {
            memcpy(out_path, path, (size_t)n + 1U);
        }
    }

    // What follows may never finish (the log's sinks take locks the failure
    // may hold): what is above is with the kernel already, and the watchdog
    // (SIGALRM's default action) ends the process if it hangs.
    if (drain) {
        (void)alarm(CRASH_DRAIN_TIMEOUT_S);
        crash_drain_log();
    }
    crash_write_log_tail(&o);
    crash_out_str(&o, "maps:\n");
    crash_out_flush(&o);
    copy_file(fd, "/proc/self/maps");
    (void)fsync(fd);
    (void)close(fd);
}

// ---------------------------------------------------------------------------
// The handler
// ---------------------------------------------------------------------------

// Gives `sig` its default action back and raises it: it stays pending while
// the handler runs, and ends the process once the handler returns.
static void reset_and_raise(int sig)
{
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    (void)sigemptyset(&dfl.sa_mask);
    (void)sigaction(sig, &dfl, NULL);
    (void)raise(sig);
}

static void crash_signal(int sig, siginfo_t *info, void *ucontext)
{
    const int saved_errno = errno;
    const u64 tid = (u64)syscall(SYS_gettid);
    u64 expected = 0U;
    if (!nph_atomic_compare_exchange_u64(&g_crashing, &expected, tid, NPH_MEMORY_ORDER_ACQ_REL,
                                         NPH_MEMORY_ORDER_ACQUIRE)) {
        if (expected != tid) {
            for (;;) {
                (void)pause(); // another thread is reported; the process ends after it
            }
        }
        reset_and_raise(sig); // this thread failed again while it was reported
        errno = saved_errno;
        return;
    }
    crash_posix_write(sig, info, ucontext, true, NULL, 0U);
    reset_and_raise(sig);
    errno = saved_errno;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

NphResult crash_os_install(void)
{
    // glibc's backtrace() loads the unwinder (dlopen, malloc) on its first
    // call: made here, so that the handler's never does.
    void *warm[2];
    (void)backtrace(warm, 2);
    nph_atomic_store_u64(&g_crashing, 0U, NPH_MEMORY_ORDER_RELAXED);
    NPH_TRY(crash_os_thread_attach());

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    (void)sigemptyset(&sa.sa_mask);
    for (u32 i = 0U; i < CRASH_SIGNAL_COUNT; ++i) {
        if (sigaction(k_signals[i], &sa, &g_prev[i]) != 0) {
            const int error = errno;
            for (u32 j = 0U; j < i; ++j) {
                (void)sigaction(k_signals[j], &g_prev[j], NULL);
            }
            crash_os_thread_detach();
            return nph_posix_fail("sigaction", error);
        }
    }
    return NPH_RESULT_OK;
}

void crash_os_uninstall(void)
{
    for (u32 i = 0U; i < CRASH_SIGNAL_COUNT; ++i) {
        (void)sigaction(k_signals[i], &g_prev[i], NULL);
    }
    crash_os_thread_detach();
}

NphResult crash_os_thread_attach(void)
{
    if (t_alt_base != NULL) {
        return NPH_RESULT_OK;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    const u64 page = page_size > 0 ? (u64)page_size : 4096U;
    const u64 size = NPH_CRASH_ALT_STACK_SIZE + page;
    void *base = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) {
        return nph_posix_fail("mmap(alternate signal stack)", errno);
    }
    // A guard page below: an overflow of the handler's own stack faults
    // instead of writing over whatever is mapped there.
    if (mprotect(base, (size_t)page, PROT_NONE) != 0) {
        const int error = errno;
        (void)munmap(base, (size_t)size);
        return nph_posix_fail("mprotect(alternate signal stack)", error);
    }
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = (u8 *)base + page;
    ss.ss_size = (size_t)NPH_CRASH_ALT_STACK_SIZE;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, &t_alt_prev) != 0) {
        const int error = errno;
        (void)munmap(base, (size_t)size);
        return nph_posix_fail("sigaltstack", error);
    }
    t_alt_base = base;
    t_alt_size = size;
    return NPH_RESULT_OK;
}

void crash_os_thread_detach(void)
{
    if (t_alt_base == NULL) {
        return;
    }
    // The stack the thread had before (usually none: the flag disables it).
    (void)sigaltstack(&t_alt_prev, NULL);
    (void)munmap(t_alt_base, (size_t)t_alt_size);
    t_alt_base = NULL;
    t_alt_size = 0U;
}
