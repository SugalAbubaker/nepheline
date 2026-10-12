// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/time.h>

#include "test_platform_support.h"

#include <string.h>

NTEST(cpu, detect_reports_the_baseline)
{
    NphCpuInfo info;
    nph_cpu_detect(&info);
    NTEST_LOG("vendor '%s' brand '%s' family %u model %u stepping %u", info.vendor, info.brand,
              info.family, info.model, info.stepping);
    NTEST_LOG("%u logical / %u physical cores, smt %d, line %u, L1d %u KiB, L2 %u KiB, L3 %u KiB",
              info.logical_cores, info.physical_cores, (int)info.smt, info.cache_line_size,
              info.l1d_size / 1024U, info.l2_size / 1024U, info.l3_size / 1024U);
    NTEST_LOG("features 0x%x, simd %s, tsc %llu Hz", info.features,
              nph_simd_level_name(info.simd_level), (unsigned long long)info.tsc_frequency_hz);
#if NPH_ARCH_X64
    NTEST_ASSERT_TRUE(strlen(info.vendor) > 0U);
    NTEST_ASSERT_TRUE(strlen(info.brand) > 0U);
    // CLAUDE.md §3: the baseline is SSE4.2 with POPCNT; every CPU that
    // runs the test suite meets it.
    NTEST_ASSERT_TRUE(nph_cpu_has(&info, NPH_CPU_FEATURE_SSE3 | NPH_CPU_FEATURE_SSSE3 |
                                             NPH_CPU_FEATURE_SSE41 | NPH_CPU_FEATURE_SSE42 |
                                             NPH_CPU_FEATURE_POPCNT));
    NTEST_ASSERT_GT_UINT(info.family, 0U);
#endif
    NTEST_ASSERT_EQ_UINT(info.cache_line_size, 64U);
    NTEST_ASSERT_GE_UINT(info.logical_cores, 1U);
    NTEST_ASSERT_GE_UINT(info.physical_cores, 1U);
    NTEST_ASSERT_LE_UINT(info.physical_cores, info.logical_cores);
    NTEST_ASSERT_EQ_INT(info.smt, info.logical_cores > info.physical_cores);
    NTEST_ASSERT_EQ_UINT(info.logical_cores, nph_cpu_logical_count());
}

NTEST(cpu, simd_level_agrees_with_feature_bits)
{
    const NphCpuInfo *info = nph_cpu_info();
    NTEST_ASSERT_EQ_INT(info->simd_level, nph_simd_detect_level());
    const bool avx2_set = nph_cpu_has(info, NPH_CPU_FEATURE_AVX | NPH_CPU_FEATURE_AVX2 |
                                                NPH_CPU_FEATURE_FMA | NPH_CPU_FEATURE_F16C);
    NTEST_ASSERT_EQ_INT(info->simd_level == NPH_SIMD_LEVEL_AVX2, avx2_set);
    // AVX2 implies AVX; AVX-512 implies AVX2.
    if (nph_cpu_has(info, NPH_CPU_FEATURE_AVX2)) {
        NTEST_ASSERT_TRUE(nph_cpu_has(info, NPH_CPU_FEATURE_AVX));
    }
    if (nph_cpu_has(info, NPH_CPU_FEATURE_AVX512F)) {
        NTEST_ASSERT_TRUE(nph_cpu_has(info, NPH_CPU_FEATURE_AVX2));
    }
}

NTEST(cpu, info_is_cached_and_stable)
{
    const NphCpuInfo *a = nph_cpu_info();
    const NphCpuInfo *b = nph_cpu_info();
    NTEST_ASSERT_EQ_PTR(a, b);
    NphCpuInfo fresh;
    nph_cpu_detect(&fresh);
    NTEST_ASSERT_EQ_MEM(a, &fresh, sizeof fresh);
}

NTEST(cpu, cache_sizes_are_plausible_when_reported)
{
    const NphCpuInfo *info = nph_cpu_info();
    if (info->l1d_size != 0U) {
        NTEST_ASSERT_GE_UINT(info->l1d_size, 8U * 1024U);
        NTEST_ASSERT_LE_UINT(info->l1d_size, 1024U * 1024U);
    }
    if (info->l2_size != 0U) {
        NTEST_ASSERT_GE_UINT(info->l2_size, 128U * 1024U);
        NTEST_ASSERT_GE_UINT(info->l2_size, info->l1d_size);
    }
    if (info->l3_size != 0U) {
        NTEST_ASSERT_GE_UINT(info->l3_size, info->l2_size);
    }
}

NTEST(cpu, measured_tsc_rate_is_plausible)
{
    const NphCpuInfo *info = nph_cpu_info();
    const u64 measured = nph_cpu_measure_tsc_hz(20U * NPH_TIME_NS_PER_MS);
    NTEST_LOG("measured TSC %llu Hz", (unsigned long long)measured);
    NTEST_ASSERT_GT_UINT(measured, 0U);
    if (nph_cpu_has(info, NPH_CPU_FEATURE_INVARIANT_TSC)) {
        // Every x86-64 part in the §3 range ticks between 1 and 6 GHz.
        NTEST_ASSERT_GT_UINT(measured, 500000000ULL);
        NTEST_ASSERT_LT_UINT(measured, 8000000000ULL);
        if (info->tsc_frequency_hz != 0U) {
            const u64 reported = info->tsc_frequency_hz;
            const u64 diff = measured > reported ? measured - reported : reported - measured;
            NTEST_ASSERT_LT_UINT(diff, reported / 10U); // within 10 %
        }
    }
}

NTEST(cpu, feature_names)
{
    NTEST_ASSERT_EQ_STR(nph_cpu_feature_name(NPH_CPU_FEATURE_SSE42), "sse4.2");
    NTEST_ASSERT_EQ_STR(nph_cpu_feature_name(NPH_CPU_FEATURE_AVX2), "avx2");
    NTEST_ASSERT_EQ_STR(nph_cpu_feature_name(NPH_CPU_FEATURE_INVARIANT_TSC), "invariant-tsc");
    NTEST_ASSERT_EQ_STR(nph_cpu_feature_name((NphCpuFeature)(1U << 30U)), "?");
    for (u32 bit = 0; bit < 20U; ++bit) {
        NTEST_ASSERT_NE_STR(nph_cpu_feature_name((NphCpuFeature)(1U << bit)), "?");
    }
}
