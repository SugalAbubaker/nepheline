// SPDX-License-Identifier: Apache-2.0
#pragma once

// CPU detection (CLAUDE.md §8.3 "CPU feature detection (CPUID)").
//
// Identifies the processor and what it can do: vendor and model strings,
// core counts, cache geometry, and the instruction-set features above the
// §3 baseline. The SIMD level the math kernels dispatch on comes from
// nph_simd_detect_level (math/kernels.h, ADR-0008 §9); this module adds
// the rest and the OS's view of the topology. The job system sizes its
// worker pool from physical_cores; allocators use cache_line_size and the
// L2 size to lay out hot data.
//
// Thread-safety: NPH_THREADSAFE. nph_cpu_info runs the detection once and
// caches it.

#include <nepheline/math/kernels.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NphCpuFeature {
    NPH_CPU_FEATURE_SSE3 = 1U << 0U,
    NPH_CPU_FEATURE_SSSE3 = 1U << 1U,
    NPH_CPU_FEATURE_SSE41 = 1U << 2U,
    NPH_CPU_FEATURE_SSE42 = 1U << 3U,
    NPH_CPU_FEATURE_POPCNT = 1U << 4U,
    NPH_CPU_FEATURE_AVX = 1U << 5U, // with OS support for the YMM state
    NPH_CPU_FEATURE_AVX2 = 1U << 6U,
    NPH_CPU_FEATURE_FMA = 1U << 7U,
    NPH_CPU_FEATURE_F16C = 1U << 8U,
    NPH_CPU_FEATURE_BMI1 = 1U << 9U,
    NPH_CPU_FEATURE_BMI2 = 1U << 10U,
    NPH_CPU_FEATURE_LZCNT = 1U << 11U,
    NPH_CPU_FEATURE_AES = 1U << 12U,
    NPH_CPU_FEATURE_SHA = 1U << 13U,
    NPH_CPU_FEATURE_AVX512F = 1U << 14U, // with OS support for the ZMM state
    NPH_CPU_FEATURE_RDTSCP = 1U << 15U,
    NPH_CPU_FEATURE_INVARIANT_TSC = 1U << 16U, // nph_time_cycles ticks at a constant rate
    NPH_CPU_FEATURE_RDRAND = 1U << 17U,
    NPH_CPU_FEATURE_MOVBE = 1U << 18U,
    NPH_CPU_FEATURE_HYPERVISOR = 1U << 19U, // running under a hypervisor
} NphCpuFeature;

typedef struct NphCpuInfo {
    char vendor[16]; // "GenuineIntel", "AuthenticAMD"; empty when unknown
    char brand[64];  // "AMD Ryzen 7 5800X 8-Core Processor"; empty when unknown
    u32 family;      // display family (with the extended field folded in)
    u32 model;       // display model
    u32 stepping;

    u32 logical_cores;  // hardware threads this process may run on
    u32 physical_cores; // cores, SMT siblings counted once; <= logical_cores
    bool smt;           // more than one hardware thread per core

    u32 cache_line_size; // bytes; 64 on every supported CPU
    u32 l1d_size;        // bytes, per core; 0 when unknown
    u32 l2_size;         // bytes, per core (per core complex on some AMD parts)
    u32 l3_size;         // bytes, total; 0 when unknown or absent

    u32 features;            // bitwise or of NphCpuFeature
    NphSimdLevel simd_level; // the level nph_simd_detect_level reports
    u64 tsc_frequency_hz;    // from CPUID when the CPU reports it (Intel); otherwise 0
} NphCpuInfo;

// Runs the detection into *out. Never fails; fields it cannot determine
// are 0 or empty. Costs a few microseconds (CPUID serialises the core).
void nph_cpu_detect(NphCpuInfo *out);

// The detection result, computed on first use and cached for the process.
const NphCpuInfo *nph_cpu_info(void);

// True when every bit of `features` is present.
NPH_INLINE bool nph_cpu_has(const NphCpuInfo *info, u32 features)
{
    return (info->features & features) == features;
}

// Number of logical cores available to the process, straight from the OS
// (cheap; no CPUID).
u32 nph_cpu_logical_count(void);

// Measures the timestamp counter against the monotonic clock over
// `sample_ns` (a few milliseconds suffice; 0 uses 10 ms) and returns the
// TSC rate in Hz. For CPUs whose CPUID does not report it. Blocks for the
// sample duration.
u64 nph_cpu_measure_tsc_hz(u64 sample_ns);

// A static name for a single feature bit ("avx2"); "?" for unknown bits.
const char *nph_cpu_feature_name(NphCpuFeature feature);

#ifdef __cplusplus
}
#endif
