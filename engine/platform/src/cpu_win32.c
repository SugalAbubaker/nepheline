// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/cpu.h>

#include "platform_internal.h"

u32 nph_cpu_logical_count(void)
{
    // Processors this process may run on. The affinity mask covers one
    // processor group (64 cores); beyond that, report the whole machine.
    DWORD_PTR process_mask = 0U;
    DWORD_PTR system_mask = 0U;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) &&
        process_mask != 0U) {
        const u32 n = nph_popcount_u64((u64)process_mask);
        if (n < 64U) {
            return n;
        }
    }
    const DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return all != 0U ? (u32)all : 1U;
}

u32 nph_cpu_physical_count_os(void)
{
    // One SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX record per core, each of
    // variable size; 32 KiB holds several hundred cores. Larger machines
    // fall back to the CPUID estimate in cpu.c.
    NPH_ALIGNAS(8) u8 buffer[32U * 1024U];
    DWORD length = (DWORD)sizeof buffer;
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buffer, &length)) {
        return 0U;
    }
    u32 cores = 0U;
    DWORD offset = 0U;
    while (offset < length) {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info =
            (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buffer + offset);
        if (info->Relationship == RelationProcessorCore) {
            ++cores;
        }
        offset += info->Size;
    }
    return cores;
}
