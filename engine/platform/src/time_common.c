// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/time.h>

#if NPH_COMPILER_MSVC
#include <intrin.h>
#elif NPH_ARCH_X64
#include <x86intrin.h>
#endif

u64 nph_time_cycles(void)
{
#if NPH_ARCH_X64
    return (u64)__rdtsc();
#else
    u64 v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
}
