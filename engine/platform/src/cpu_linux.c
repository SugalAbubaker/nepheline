// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/cpu.h>

#include "platform_internal.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_CPUS 1024U

u32 nph_cpu_logical_count(void)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0) {
            return (u32)n;
        }
    }
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? (u32)online : 1U;
}

static bool read_u32_file(const char *path, u32 *out)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char line[64];
    const bool ok = fgets(line, (int)sizeof line, f) != NULL;
    fclose(f);
    if (!ok) {
        return false;
    }
    *out = (u32)strtoul(line, NULL, 10);
    return true;
}

u32 nph_cpu_physical_count_os(void)
{
    // Count distinct (package, core) pairs over the CPUs this process may
    // use, from the topology sysfs publishes per logical CPU.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) {
        return 0U;
    }
    u64 seen[MAX_CPUS];
    u32 seen_count = 0U;
    for (u32 cpu = 0; cpu < MAX_CPUS && seen_count < MAX_CPUS; ++cpu) {
        if (!CPU_ISSET(cpu, &set)) {
            continue;
        }
        char path[128];
        u32 core = 0U;
        u32 package = 0U;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/topology/core_id", cpu);
        if (!read_u32_file(path, &core)) {
            return 0U;
        }
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/topology/physical_package_id",
                 cpu);
        if (!read_u32_file(path, &package)) {
            return 0U;
        }
        const u64 key = ((u64)package << 32U) | core;
        bool known = false;
        for (u32 i = 0; i < seen_count; ++i) {
            if (seen[i] == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            seen[seen_count++] = key;
        }
    }
    return seen_count;
}
