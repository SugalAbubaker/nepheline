// SPDX-License-Identifier: Apache-2.0
#include <nepheline/platform/time.h>

#include "platform_internal.h"

#include <time.h>

u64 nph_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // cannot fail for a valid clock id
    return (u64)ts.tv_sec * NPH_TIME_NS_PER_S + (u64)ts.tv_nsec;
}

u64 nph_time_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * NPH_TIME_NS_PER_S + (u64)ts.tv_nsec;
}
