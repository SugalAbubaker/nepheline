// SPDX-License-Identifier: Apache-2.0
// nbench runtime: registry, timing loop, console table, JSON report.

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <nbench/nbench.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

uint64_t nbench_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // Split to avoid overflow: counts are ~1e7/s, so seconds * 1e9 fits.
    const uint64_t seconds = (uint64_t)counter.QuadPart / (uint64_t)frequency.QuadPart;
    const uint64_t rest = (uint64_t)counter.QuadPart % (uint64_t)frequency.QuadPart;
    return seconds * 1000000000ULL + rest * 1000000000ULL / (uint64_t)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

// ---------------------------------------------------------------------------
// Optimisation barriers
// ---------------------------------------------------------------------------

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4996)
static volatile const void *g_sink_ptr;
static volatile uint64_t g_sink_u64;
void nbench_do_not_optimize(const void *p)
{
    g_sink_ptr = p;
    _ReadWriteBarrier();
}
void nbench_do_not_optimize_u64(uint64_t v)
{
    g_sink_u64 = v;
    _ReadWriteBarrier();
}
void nbench_clobber_memory(void)
{
    _ReadWriteBarrier();
}
#pragma warning(pop)
#else
void nbench_do_not_optimize(const void *p)
{
    __asm__ __volatile__("" : : "r,m"(p) : "memory");
}
void nbench_do_not_optimize_u64(uint64_t v)
{
    __asm__ __volatile__("" : : "r,m"(v) : "memory");
}
void nbench_clobber_memory(void)
{
    __asm__ __volatile__("" : : : "memory");
}
#endif

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

static NbenchCase *g_registry = NULL;
static size_t g_registry_count = 0;

void nbench_register(NbenchCase *bc)
{
    bc->next = g_registry;
    g_registry = bc;
    ++g_registry_count;
}

static int compare_cases(const void *a, const void *b)
{
    const NbenchCase *x = *(const NbenchCase *const *)a;
    const NbenchCase *y = *(const NbenchCase *const *)b;
    const int c = strcmp(x->file, y->file);
    if (c != 0) {
        return c;
    }
    return x->line < y->line ? -1 : (x->line > y->line ? 1 : 0);
}

// ---------------------------------------------------------------------------
// State and loop control
// ---------------------------------------------------------------------------

struct NbenchState {
    uint64_t iterations; // budget for this run
    uint64_t remaining;
    uint64_t items_per_iteration;
    uint64_t start_ns;
    uint64_t elapsed_ns; // accumulated while timing is active
    bool started;
    bool paused;
    const char *label;
};

bool nbench_keep_running(NbenchState *s)
{
    if (!s->started) {
        s->started = true;
        s->remaining = s->iterations;
        s->elapsed_ns = 0;
        s->paused = false;
        s->start_ns = nbench_now_ns();
    }
    if (s->remaining == 0) {
        if (!s->paused) {
            s->elapsed_ns += nbench_now_ns() - s->start_ns;
            s->paused = true;
        }
        return false;
    }
    s->remaining -= 1;
    return true;
}

void nbench_set_items_per_iteration(NbenchState *s, uint64_t n)
{
    s->items_per_iteration = n == 0 ? 1 : n;
}

void nbench_set_label(NbenchState *s, const char *label)
{
    s->label = label;
}

void nbench_pause_timing(NbenchState *s)
{
    if (s->started && !s->paused) {
        s->elapsed_ns += nbench_now_ns() - s->start_ns;
        s->paused = true;
    }
}

void nbench_resume_timing(NbenchState *s)
{
    if (s->started && s->paused) {
        s->paused = false;
        s->start_ns = nbench_now_ns();
    }
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

typedef struct NbenchResult {
    const NbenchCase *bc;
    const char *label;
    uint64_t iterations;
    uint64_t items_per_iteration;
    int repeats;
    double ns_per_op_median;
    double ns_per_op_min;
    double ns_per_op_max;
} NbenchResult;

static double run_once(const NbenchCase *bc, uint64_t iterations, uint64_t *out_items,
                       const char **out_label)
{
    NbenchState s;
    memset(&s, 0, sizeof s);
    s.iterations = iterations;
    s.items_per_iteration = 1;
    bc->fn(&s);
    if (!s.started) {
        fprintf(stderr, "nbench: %s never called nbench_keep_running\n", bc->name);
        exit(2);
    }
    nbench_pause_timing(&s);
    *out_items = s.items_per_iteration;
    *out_label = s.label;
    return (double)s.elapsed_ns;
}

static int compare_double(const void *a, const void *b)
{
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void run_case(const NbenchCase *bc, double min_time_s, int repeats, NbenchResult *r)
{
    double samples[64];
    if (repeats > (int)(sizeof samples / sizeof samples[0])) {
        repeats = (int)(sizeof samples / sizeof samples[0]);
    }
    // Calibrate: grow the iteration count until one run lasts min_time.
    uint64_t iterations = 1;
    uint64_t items = 1;
    const char *label = NULL;
    double elapsed = run_once(bc, iterations, &items, &label);
    const double min_time_ns = min_time_s * 1e9;
    while (elapsed < min_time_ns && iterations < (1ULL << 40)) {
        double scale = elapsed > 0.0 ? (min_time_ns / elapsed) * 1.25 : 10.0;
        if (scale < 2.0) {
            scale = 2.0;
        }
        if (scale > 1000.0) {
            scale = 1000.0;
        }
        iterations = (uint64_t)((double)iterations * scale);
        elapsed = run_once(bc, iterations, &items, &label);
    }
    for (int i = 0; i < repeats; ++i) {
        const double ns = run_once(bc, iterations, &items, &label);
        samples[i] = ns / ((double)iterations * (double)items);
    }
    qsort(samples, (size_t)repeats, sizeof samples[0], compare_double);
    r->bc = bc;
    r->label = label;
    r->iterations = iterations;
    r->items_per_iteration = items;
    r->repeats = repeats;
    r->ns_per_op_min = samples[0];
    r->ns_per_op_max = samples[repeats - 1];
    r->ns_per_op_median = (repeats % 2 == 1)
                              ? samples[repeats / 2]
                              : 0.5 * (samples[repeats / 2 - 1] + samples[repeats / 2]);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static const char *config_name(void)
{
#if defined(NPH_CONFIG_DEBUG)
    return "Debug";
#elif defined(NPH_CONFIG_DEVELOPMENT)
    return "Development";
#elif defined(NPH_CONFIG_PROFILE)
    return "Profile";
#elif defined(NPH_CONFIG_SHIPPING)
    return "Shipping";
#else
    return "unknown";
#endif
}

static void compiler_name(char *out, size_t cap)
{
#if defined(__clang__)
    snprintf(out, cap, "clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(out, cap, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    snprintf(out, cap, "msvc %d", _MSC_FULL_VER);
#else
    snprintf(out, cap, "unknown");
#endif
}

static void json_escape(FILE *f, const char *s)
{
    for (const char *p = s; *p != '\0'; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c < 0x20U) {
            fprintf(f, "\\u%04x", (unsigned int)c);
        } else {
            fputc((int)c, f);
        }
    }
}

static bool write_json(const char *path, const char *exe, const NbenchResult *results, size_t n,
                       double min_time_s)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "nbench: cannot open '%s' for writing\n", path);
        return false;
    }
    char timestamp[32] = "";
    const time_t now = time(NULL);
    const struct tm *utc = gmtime(&now);
    if (utc != NULL) {
        strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", utc);
    }
    char compiler[64];
    compiler_name(compiler, sizeof compiler);
    fprintf(f, "{\n  \"schema\": 1,\n  \"executable\": \"");
    json_escape(f, exe);
    fprintf(f, "\",\n  \"timestamp\": \"%s\",\n  \"config\": \"%s\",\n", timestamp, config_name());
    fprintf(f, "  \"host\": {\"os\": \"%s\", \"arch\": \"%s\", \"compiler\": \"%s\"},\n",
#if defined(_WIN32)
            "windows",
#else
            "linux",
#endif
#if defined(_M_X64) || defined(__x86_64__)
            "x86_64",
#else
            "arm64",
#endif
            compiler);
    fprintf(f, "  \"min_time_s\": %g,\n  \"benchmarks\": [\n", min_time_s);
    for (size_t i = 0; i < n; ++i) {
        const NbenchResult *r = &results[i];
        fprintf(f, "    {\"name\": \"");
        json_escape(f, r->bc->name);
        fprintf(f, "\", \"label\": \"");
        json_escape(f, r->label != NULL ? r->label : "");
        fprintf(f,
                "\", \"iterations\": %" PRIu64 ", \"items_per_iteration\": %" PRIu64
                ", \"repeats\": %d, \"ns_per_op\": %.3f, \"min_ns_per_op\": %.3f, "
                "\"max_ns_per_op\": %.3f}%s\n",
                r->iterations, r->items_per_iteration, r->repeats, r->ns_per_op_median,
                r->ns_per_op_min, r->ns_per_op_max, i + 1 < n ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return true;
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

static bool glob_match(const char *pattern, const char *str)
{
    const char *star_pattern = NULL;
    const char *star_str = NULL;
    while (*str != '\0') {
        if (*pattern == '*') {
            star_pattern = pattern++;
            star_str = str;
        } else if (*pattern == '?' || *pattern == *str) {
            ++pattern;
            ++str;
        } else if (star_pattern != NULL) {
            pattern = star_pattern + 1;
            str = ++star_str;
        } else {
            return false;
        }
    }
    while (*pattern == '*') {
        ++pattern;
    }
    return *pattern == '\0';
}

static void print_usage(FILE *out, const char *exe)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "  --list             List the benchmarks and exit\n"
            "  --filter=GLOB      Run only benchmarks whose name matches ('*', '?')\n"
            "  --min-time=SEC     Minimum time per measured run (default 0.2)\n"
            "  --repeats=N        Measured runs per benchmark (default 5, max 64)\n"
            "  --json=FILE        Write results as JSON\n"
            "  --budgets=FILE     Exit 4 if any median ns/op exceeds its budget\n"
            "                     (lines of `name max_ns`; '#' starts a comment)\n"
            "  --help             Show this help\n",
            exe);
}

// ---------------------------------------------------------------------------
// Budgets
// ---------------------------------------------------------------------------

typedef struct NbenchBudget {
    char name[128];
    double max_ns;
} NbenchBudget;

// Reads `name max_ns` lines. Returns the count, or -1 on a malformed file.
static int load_budgets(const char *path, NbenchBudget **out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "nbench: cannot open budgets file '%s'\n", path);
        return -1;
    }
    NbenchBudget *budgets = NULL;
    int count = 0;
    int capacity = 0;
    char line[512];
    int line_number = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        ++line_number;
        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        NbenchBudget b;
        const int fields = sscanf(line, "%127s", b.name);
        if (fields <= 0) {
            continue; // blank or comment-only line
        }
        // The number follows the name; strtod reports where it stopped.
        const char *rest = strstr(line, b.name) + strlen(b.name);
        char *end = NULL;
        const double max_ns = strtod(rest, &end);
        while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            ++end;
        }
        if (end == rest || end == NULL || *end != '\0' || !(max_ns > 0.0)) {
            fprintf(stderr, "nbench: %s:%d: expected `name max_ns`\n", path, line_number);
            free(budgets);
            fclose(f);
            return -1;
        }
        b.max_ns = max_ns;
        if (count == capacity) {
            capacity = capacity == 0 ? 32 : capacity * 2;
            NbenchBudget *grown =
                (NbenchBudget *)realloc(budgets, (size_t)capacity * sizeof *grown);
            if (grown == NULL) {
                fputs("nbench: out of memory\n", stderr);
                abort();
            }
            budgets = grown;
        }
        budgets[count++] = b;
    }
    fclose(f);
    *out = budgets;
    return count;
}

// Compares every result with its budget. Returns false if any exceeded.
static bool check_budgets(const char *path, const NbenchResult *results, size_t n)
{
    NbenchBudget *budgets = NULL;
    const int count = load_budgets(path, &budgets);
    if (count < 0) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        const NbenchResult *r = &results[i];
        for (int b = 0; b < count; ++b) {
            if (strcmp(budgets[b].name, r->bc->name) != 0) {
                continue;
            }
            if (r->ns_per_op_median > budgets[b].max_ns) {
                printf("BUDGET EXCEEDED  %-40s %.2f ns/op > %.2f\n", r->bc->name,
                       r->ns_per_op_median, budgets[b].max_ns);
                ok = false;
            }
            break;
        }
    }
    printf(ok ? "budgets: all %zu benchmarks within budget (%s)\n"
              : "budgets: FAILED (%zu benchmarks checked against %s)\n",
           n, path);
    free(budgets);
    return ok;
}

int nbench_main(int argc, char **argv)
{
    bool list = false;
    const char *filter = NULL;
    const char *json_path = NULL;
    const char *budgets_path = NULL;
    double min_time_s = 0.2;
    int repeats = 5;
    const char *exe = argc > 0 ? path_basename(argv[0]) : "nbench";

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--list") == 0) {
            list = true;
        } else if (strncmp(a, "--filter=", 9) == 0) {
            filter = a + 9;
        } else if (strncmp(a, "--json=", 7) == 0) {
            json_path = a + 7;
        } else if (strncmp(a, "--budgets=", 10) == 0) {
            budgets_path = a + 10;
        } else if (strncmp(a, "--min-time=", 11) == 0) {
            char *end = NULL;
            min_time_s = strtod(a + 11, &end);
            if (end == NULL || *end != '\0' || !(min_time_s > 0.0)) {
                fprintf(stderr, "nbench: --min-time needs a positive number\n");
                return 2;
            }
        } else if (strncmp(a, "--repeats=", 10) == 0) {
            char *end = NULL;
            const long value = strtol(a + 10, &end, 10);
            if (end == NULL || *end != '\0' || value < 1 || value > 64) {
                fprintf(stderr, "nbench: --repeats needs a count between 1 and 64\n");
                return 2;
            }
            repeats = (int)value;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(stdout, exe);
            return 0;
        } else {
            fprintf(stderr, "nbench: unknown option '%s'\n\n", a);
            print_usage(stderr, exe);
            return 2;
        }
    }

    if (g_registry_count == 0) {
        fprintf(stderr, "nbench: no benchmarks are registered in %s\n", exe);
        return 3;
    }
    const NbenchCase **cases = (const NbenchCase **)malloc(g_registry_count * sizeof *cases);
    NbenchResult *results = (NbenchResult *)calloc(g_registry_count, sizeof *results);
    if (cases == NULL || results == NULL) {
        fputs("nbench: out of memory\n", stderr);
        abort();
    }
    size_t n = 0;
    for (const NbenchCase *bc = g_registry; bc != NULL; bc = bc->next) {
        if (filter == NULL || glob_match(filter, bc->name)) {
            cases[n++] = bc;
        }
    }
    qsort((void *)cases, n, sizeof *cases, compare_cases);

    if (list) {
        for (size_t i = 0; i < n; ++i) {
            printf("%s\n", cases[i]->name);
        }
        free((void *)cases);
        free(results);
        return 0;
    }
    if (n == 0) {
        fprintf(stderr, "nbench: no benchmark matches '%s'\n", filter != NULL ? filter : "");
        free((void *)cases);
        free(results);
        return 3;
    }

    printf("%-44s %14s %14s %14s %12s\n", "benchmark", "ns/op (median)", "min", "max",
           "iterations");
    for (size_t i = 0; i < n; ++i) {
        run_case(cases[i], min_time_s, repeats, &results[i]);
        const NbenchResult *r = &results[i];
        printf("%-44s %14.2f %14.2f %14.2f %12" PRIu64 "\n", r->bc->name, r->ns_per_op_median,
               r->ns_per_op_min, r->ns_per_op_max, r->iterations * r->items_per_iteration);
        fflush(stdout);
    }

    int status = 0;
    if (json_path != NULL && !write_json(json_path, exe, results, n, min_time_s)) {
        status = 2;
    }
    if (budgets_path != NULL && !check_budgets(budgets_path, results, n)) {
        status = 4;
    }
    free((void *)cases);
    free(results);
    return status;
}
