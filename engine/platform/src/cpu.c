// SPDX-License-Identifier: Apache-2.0
// CPUID decoding (Intel SDM vol. 2A "CPUID"; AMD APM vol. 3 appendix E).
// The OS-specific core counts come from cpu_win32.c / cpu_linux.c.
#include <nepheline/core/assert.h>
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/thread.h>
#include <nepheline/platform/time.h>

#include "platform_internal.h"

#include <string.h>

#if NPH_COMPILER_MSVC
#include <intrin.h>
#elif NPH_ARCH_X64
#include <cpuid.h>
#endif

#if NPH_ARCH_X64

static void cpuid(u32 leaf, u32 subleaf, u32 out[4])
{
#if NPH_COMPILER_MSVC
    int regs[4];
    __cpuidex(regs, (int)leaf, (int)subleaf);
    out[0] = (u32)regs[0];
    out[1] = (u32)regs[1];
    out[2] = (u32)regs[2];
    out[3] = (u32)regs[3];
#else
    u32 a;
    u32 b;
    u32 c;
    u32 d;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
#endif
}

static u64 xgetbv0(void)
{
#if NPH_COMPILER_MSVC
    return (u64)_xgetbv(0);
#else
    u32 eax;
    u32 edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0U));
    return ((u64)edx << 32U) | eax;
#endif
}

static bool bit(u32 reg, u32 index)
{
    return (reg & (1U << index)) != 0U;
}

// Copies 4 registers' worth of ASCII into `dst` without a terminator.
static void copy_regs(char *dst, const u32 regs[4])
{
    memcpy(dst, regs, 16U);
}

// Cache geometry from the deterministic cache parameters leaf (4 on Intel,
// 0x8000001D on AMD with topology extensions): one sub-leaf per cache.
static void caches_from_parameters_leaf(NphCpuInfo *info, u32 leaf)
{
    for (u32 sub = 0; sub < 32U; ++sub) {
        u32 r[4];
        cpuid(leaf, sub, r);
        const u32 type = r[0] & 0x1FU; // 0 none, 1 data, 2 instruction, 3 unified
        if (type == 0U) {
            break;
        }
        const u32 level = (r[0] >> 5U) & 0x7U;
        const u64 ways = ((r[1] >> 22U) & 0x3FFU) + 1U;
        const u64 partitions = ((r[1] >> 12U) & 0x3FFU) + 1U;
        const u64 line = (r[1] & 0xFFFU) + 1U;
        const u64 sets = (u64)r[2] + 1U;
        const u64 size = ways * partitions * line * sets;
        const u32 size32 = size > NPH_U32_MAX ? NPH_U32_MAX : (u32)size;
        if (level == 1U && type == 1U) {
            info->l1d_size = size32;
        } else if (level == 2U && (type == 3U || type == 1U)) {
            info->l2_size = size32;
        } else if (level == 3U && (type == 3U || type == 1U)) {
            info->l3_size = size32;
        }
    }
}

// AMD's older fixed-format cache leaves.
static void caches_from_amd_legacy(NphCpuInfo *info, u32 max_ext)
{
    u32 r[4];
    if (max_ext >= 0x80000005U) {
        cpuid(0x80000005U, 0U, r);
        info->l1d_size = ((r[2] >> 24U) & 0xFFU) * 1024U;
    }
    if (max_ext >= 0x80000006U) {
        cpuid(0x80000006U, 0U, r);
        info->l2_size = ((r[2] >> 16U) & 0xFFFFU) * 1024U;
        info->l3_size = ((r[3] >> 18U) & 0x3FFFU) * 512U * 1024U;
    }
}

// Hardware threads per core, from the topology leaves; 1 when unknown.
static u32 threads_per_core(bool intel, bool amd, u32 max_leaf, u32 max_ext)
{
    u32 r[4];
    if (intel && max_leaf >= 0xBU) {
        cpuid(0xBU, 0U, r);
        const u32 level_type = (r[2] >> 8U) & 0xFFU; // 1: SMT
        const u32 logical = r[1] & 0xFFFFU;
        if (level_type == 1U && logical > 0U) {
            return logical;
        }
    }
    if (amd && max_ext >= 0x8000001EU) {
        cpuid(0x8000001EU, 0U, r);
        return ((r[1] >> 8U) & 0xFFU) + 1U;
    }
    return 1U;
}

static u64 tsc_from_cpuid(bool intel, u32 max_leaf)
{
    if (!intel || max_leaf < 0x15U) {
        return 0U;
    }
    u32 r[4];
    cpuid(0x15U, 0U, r);
    const u64 denominator = r[0];
    const u64 numerator = r[1];
    u64 crystal_hz = r[2];
    if (denominator == 0U || numerator == 0U) {
        return 0U;
    }
    if (crystal_hz == 0U && max_leaf >= 0x16U) {
        // Core crystal not reported: the processor base frequency (MHz)
        // from leaf 0x16 is the TSC rate on the parts that leave 0x15.ECX
        // zero (Skylake client and later; see SDM 18.7.3).
        cpuid(0x16U, 0U, r);
        return (u64)(r[0] & 0xFFFFU) * 1000000U;
    }
    return crystal_hz * numerator / denominator;
}

void nph_cpu_detect(NphCpuInfo *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->cache_line_size = NPH_CACHE_LINE_SIZE;

    u32 r[4];
    cpuid(0U, 0U, r);
    const u32 max_leaf = r[0];
    const u32 vendor_regs[4] = {r[1], r[3], r[2], 0U};
    copy_regs(out->vendor, vendor_regs);
    out->vendor[12] = '\0';
    const bool intel = strcmp(out->vendor, "GenuineIntel") == 0;
    const bool amd = strcmp(out->vendor, "AuthenticAMD") == 0;

    u32 features = 0U;
    bool os_avx = false;
    bool os_avx512 = false;
    if (max_leaf >= 1U) {
        cpuid(1U, 0U, r);
        const u32 eax = r[0];
        const u32 ebx = r[1];
        const u32 ecx = r[2];
        out->stepping = eax & 0xFU;
        u32 model = (eax >> 4U) & 0xFU;
        u32 family = (eax >> 8U) & 0xFU;
        const u32 ext_model = (eax >> 16U) & 0xFU;
        const u32 ext_family = (eax >> 20U) & 0xFFU;
        if (family == 0xFU) {
            family += ext_family;
        }
        if (family == 0x6U || family == 0xFU) {
            model += ext_model << 4U;
        }
        out->family = family;
        out->model = model;
        const u32 line = ((ebx >> 8U) & 0xFFU) * 8U;
        if (line != 0U) {
            out->cache_line_size = line;
        }
        if (bit(ecx, 0U)) {
            features |= NPH_CPU_FEATURE_SSE3;
        }
        if (bit(ecx, 9U)) {
            features |= NPH_CPU_FEATURE_SSSE3;
        }
        if (bit(ecx, 19U)) {
            features |= NPH_CPU_FEATURE_SSE41;
        }
        if (bit(ecx, 20U)) {
            features |= NPH_CPU_FEATURE_SSE42;
        }
        if (bit(ecx, 22U)) {
            features |= NPH_CPU_FEATURE_MOVBE;
        }
        if (bit(ecx, 23U)) {
            features |= NPH_CPU_FEATURE_POPCNT;
        }
        if (bit(ecx, 25U)) {
            features |= NPH_CPU_FEATURE_AES;
        }
        if (bit(ecx, 30U)) {
            features |= NPH_CPU_FEATURE_RDRAND;
        }
        if (bit(ecx, 31U)) {
            features |= NPH_CPU_FEATURE_HYPERVISOR;
        }
        if (bit(ecx, 27U)) { // OSXSAVE: XGETBV is available
            const u64 xcr0 = xgetbv0();
            os_avx = (xcr0 & 0x6U) == 0x6U;                // XMM and YMM state
            os_avx512 = os_avx && (xcr0 & 0xE0U) == 0xE0U; // opmask, ZMM_Hi256, Hi16_ZMM
        }
        if (os_avx) {
            if (bit(ecx, 28U)) {
                features |= NPH_CPU_FEATURE_AVX;
            }
            if (bit(ecx, 12U)) {
                features |= NPH_CPU_FEATURE_FMA;
            }
            if (bit(ecx, 29U)) {
                features |= NPH_CPU_FEATURE_F16C;
            }
        }
    }
    if (max_leaf >= 7U) {
        cpuid(7U, 0U, r);
        const u32 ebx = r[1];
        if (bit(ebx, 3U)) {
            features |= NPH_CPU_FEATURE_BMI1;
        }
        if (bit(ebx, 8U)) {
            features |= NPH_CPU_FEATURE_BMI2;
        }
        if (bit(ebx, 29U)) {
            features |= NPH_CPU_FEATURE_SHA;
        }
        if (os_avx && bit(ebx, 5U)) {
            features |= NPH_CPU_FEATURE_AVX2;
        }
        if (os_avx512 && bit(ebx, 16U)) {
            features |= NPH_CPU_FEATURE_AVX512F;
        }
    }

    cpuid(0x80000000U, 0U, r);
    const u32 max_ext = r[0];
    bool amd_topology_ext = false;
    if (max_ext >= 0x80000001U) {
        cpuid(0x80000001U, 0U, r);
        if (bit(r[2], 5U)) {
            features |= NPH_CPU_FEATURE_LZCNT;
        }
        if (bit(r[3], 27U)) {
            features |= NPH_CPU_FEATURE_RDTSCP;
        }
        amd_topology_ext = amd && bit(r[2], 22U);
    }
    if (max_ext >= 0x80000004U) {
        char brand[49];
        for (u32 i = 0; i < 3U; ++i) {
            cpuid(0x80000002U + i, 0U, r);
            copy_regs(brand + i * 16U, r);
        }
        brand[48] = '\0';
        // Vendors pad the brand string with leading spaces.
        const char *start = brand;
        while (*start == ' ') {
            ++start;
        }
        u64 len = strlen(start);
        while (len > 0U && start[len - 1U] == ' ') {
            --len;
        }
        if (len >= sizeof out->brand) {
            len = sizeof out->brand - 1U;
        }
        memcpy(out->brand, start, len);
        out->brand[len] = '\0';
    }
    if (max_ext >= 0x80000007U) {
        cpuid(0x80000007U, 0U, r);
        if (bit(r[3], 8U)) {
            features |= NPH_CPU_FEATURE_INVARIANT_TSC;
        }
    }

    if (intel && max_leaf >= 4U) {
        caches_from_parameters_leaf(out, 4U);
    } else if (amd_topology_ext && max_ext >= 0x8000001DU) {
        caches_from_parameters_leaf(out, 0x8000001DU);
    } else if (amd) {
        caches_from_amd_legacy(out, max_ext);
    }

    out->features = features;
    out->simd_level = nph_simd_detect_level();
    out->tsc_frequency_hz = tsc_from_cpuid(intel, max_leaf);

    const u32 per_core = threads_per_core(intel, amd, max_leaf, max_ext);
    out->logical_cores = nph_cpu_logical_count();
    out->physical_cores = nph_cpu_physical_count_os();
    if (out->physical_cores == 0U || out->physical_cores > out->logical_cores) {
        out->physical_cores = nph_max_u32(1U, out->logical_cores / nph_max_u32(1U, per_core));
    }
    out->smt = out->logical_cores > out->physical_cores;
}

#else // !NPH_ARCH_X64

void nph_cpu_detect(NphCpuInfo *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->cache_line_size = NPH_CACHE_LINE_SIZE;
    out->simd_level = nph_simd_detect_level();
    out->logical_cores = nph_cpu_logical_count();
    out->physical_cores = nph_cpu_physical_count_os();
    if (out->physical_cores == 0U) {
        out->physical_cores = out->logical_cores;
    }
    out->smt = out->logical_cores > out->physical_cores;
}

#endif

static struct {
    NphOnce once;
    NphCpuInfo info;
} g_cpu = {NPH_ONCE_INIT,
           {{0}, {0}, 0U, 0U, 0U, 0U, 0U, false, 0U, 0U, 0U, 0U, 0U, NPH_SIMD_LEVEL_SCALAR, 0U}};

static void detect_once(void *user)
{
    NPH_UNUSED(user);
    nph_cpu_detect(&g_cpu.info);
}

const NphCpuInfo *nph_cpu_info(void)
{
    nph_once(&g_cpu.once, detect_once, NULL);
    return &g_cpu.info;
}

u64 nph_cpu_measure_tsc_hz(u64 sample_ns)
{
    if (sample_ns == 0U) {
        sample_ns = 10U * NPH_TIME_NS_PER_MS;
    }
    const u64 t0 = nph_time_now_ns();
    const u64 c0 = nph_time_cycles();
    nph_thread_sleep_ns(sample_ns);
    const u64 t1 = nph_time_now_ns();
    const u64 c1 = nph_time_cycles();
    const u64 elapsed_ns = t1 - t0;
    if (elapsed_ns == 0U || c1 <= c0) {
        return 0U;
    }
    return (u64)((f64)(c1 - c0) * 1e9 / (f64)elapsed_ns);
}

const char *nph_cpu_feature_name(NphCpuFeature feature)
{
    switch (feature) {
    case NPH_CPU_FEATURE_SSE3:
        return "sse3";
    case NPH_CPU_FEATURE_SSSE3:
        return "ssse3";
    case NPH_CPU_FEATURE_SSE41:
        return "sse4.1";
    case NPH_CPU_FEATURE_SSE42:
        return "sse4.2";
    case NPH_CPU_FEATURE_POPCNT:
        return "popcnt";
    case NPH_CPU_FEATURE_AVX:
        return "avx";
    case NPH_CPU_FEATURE_AVX2:
        return "avx2";
    case NPH_CPU_FEATURE_FMA:
        return "fma";
    case NPH_CPU_FEATURE_F16C:
        return "f16c";
    case NPH_CPU_FEATURE_BMI1:
        return "bmi1";
    case NPH_CPU_FEATURE_BMI2:
        return "bmi2";
    case NPH_CPU_FEATURE_LZCNT:
        return "lzcnt";
    case NPH_CPU_FEATURE_AES:
        return "aes";
    case NPH_CPU_FEATURE_SHA:
        return "sha";
    case NPH_CPU_FEATURE_AVX512F:
        return "avx512f";
    case NPH_CPU_FEATURE_RDTSCP:
        return "rdtscp";
    case NPH_CPU_FEATURE_INVARIANT_TSC:
        return "invariant-tsc";
    case NPH_CPU_FEATURE_RDRAND:
        return "rdrand";
    case NPH_CPU_FEATURE_MOVBE:
        return "movbe";
    case NPH_CPU_FEATURE_HYPERVISOR:
        return "hypervisor";
    default:
        return "?";
    }
}
