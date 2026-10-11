// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/base.h>
#include <nepheline/core/result.h>

#include <ntest/ntest.h>

#include <string.h>

NTEST(base, detection_macros_are_zero_or_one)
{
    NTEST_ASSERT_EQ_INT(NPH_COMPILER_MSVC + NPH_COMPILER_GCC + NPH_COMPILER_CLANG, 1);
    NTEST_ASSERT_EQ_INT(NPH_PLATFORM_WINDOWS + NPH_PLATFORM_LINUX, 1);
    NTEST_ASSERT_EQ_INT(NPH_ARCH_X64 + NPH_ARCH_ARM64, 1);
    NTEST_ASSERT_EQ_INT(
        NPH_CONFIG_DEBUG + NPH_CONFIG_DEVELOPMENT + NPH_CONFIG_PROFILE + NPH_CONFIG_SHIPPING, 1);
#if NPH_CONFIG_DEBUG || NPH_CONFIG_DEVELOPMENT
    NTEST_ASSERT_EQ_INT(NPH_ASSERTS_ENABLED, 1);
#else
    NTEST_ASSERT_EQ_INT(NPH_ASSERTS_ENABLED, 0);
#endif
}

NTEST(base, min_max_clamp)
{
    NTEST_ASSERT_EQ_UINT(nph_min_u32(3U, 5U), 3U);
    NTEST_ASSERT_EQ_UINT(nph_max_u32(3U, 5U), 5U);
    NTEST_ASSERT_EQ_UINT(nph_min_u64(NPH_U64_MAX, 5U), 5U);
    NTEST_ASSERT_EQ_UINT(nph_max_u64(NPH_U64_MAX, 5U), NPH_U64_MAX);
    NTEST_ASSERT_EQ_INT(nph_min_i32(-3, 5), -3);
    NTEST_ASSERT_EQ_INT(nph_max_i32(-3, 5), 5);
    NTEST_ASSERT_EQ_INT(nph_min_i64(NPH_I64_MIN, 0), NPH_I64_MIN);
    NTEST_ASSERT_EQ_INT(nph_max_i64(NPH_I64_MIN, 0), 0);
    NTEST_ASSERT_EQ_UINT(nph_clamp_u32(7U, 1U, 5U), 5U);
    NTEST_ASSERT_EQ_UINT(nph_clamp_u32(0U, 1U, 5U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_clamp_u32(3U, 1U, 5U), 3U);
    NTEST_ASSERT_EQ_UINT(nph_clamp_u64(7U, 1U, 5U), 5U);
    NTEST_ASSERT_EQ_INT(nph_clamp_i32(-7, -5, 5), -5);
    NTEST_ASSERT_EQ_INT(nph_clamp_i32(7, -5, 5), 5);
}

NTEST(base, popcount)
{
    NTEST_ASSERT_EQ_UINT(nph_popcount_u32(0U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_popcount_u32(1U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_popcount_u32(0xFFFFFFFFU), 32U);
    NTEST_ASSERT_EQ_UINT(nph_popcount_u32(0xA5A5A5A5U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_popcount_u64(0U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_popcount_u64(NPH_U64_MAX), 64U);
    NTEST_ASSERT_EQ_UINT(nph_popcount_u64(0x8000000000000001ULL), 2U);
}

NTEST(base, clz_ctz)
{
    NTEST_ASSERT_EQ_UINT(nph_clz_u32(1U), 31U);
    NTEST_ASSERT_EQ_UINT(nph_clz_u32(0x80000000U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_clz_u32(0x00010000U), 15U);
    NTEST_ASSERT_EQ_UINT(nph_ctz_u32(1U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_ctz_u32(0x80000000U), 31U);
    NTEST_ASSERT_EQ_UINT(nph_ctz_u32(0x00010000U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_clz_u64(1U), 63U);
    NTEST_ASSERT_EQ_UINT(nph_clz_u64(0x8000000000000000ULL), 0U);
    NTEST_ASSERT_EQ_UINT(nph_ctz_u64(0x8000000000000000ULL), 63U);
    NTEST_ASSERT_EQ_UINT(nph_ctz_u64(0x0000000100000000ULL), 32U);
    for (u32 i = 0; i < 32U; ++i) {
        NTEST_ASSERT_EQ_UINT(nph_clz_u32(1U << i), 31U - i);
        NTEST_ASSERT_EQ_UINT(nph_ctz_u32(1U << i), i);
    }
    for (u32 i = 0; i < 64U; ++i) {
        NTEST_ASSERT_EQ_UINT(nph_clz_u64((u64)1U << i), 63U - i);
        NTEST_ASSERT_EQ_UINT(nph_ctz_u64((u64)1U << i), i);
    }
}

NTEST(base, log2)
{
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u32(1U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u32(2U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u32(3U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u32(1024U), 10U);
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u32(1025U), 10U);
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u32(NPH_U32_MAX), 31U);
    NTEST_ASSERT_EQ_UINT(nph_log2_floor_u64(NPH_U64_MAX), 63U);
    NTEST_ASSERT_EQ_UINT(nph_log2_ceil_u32(1U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_log2_ceil_u32(2U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_log2_ceil_u32(3U), 2U);
    NTEST_ASSERT_EQ_UINT(nph_log2_ceil_u32(1024U), 10U);
    NTEST_ASSERT_EQ_UINT(nph_log2_ceil_u32(1025U), 11U);
    NTEST_ASSERT_EQ_UINT(nph_log2_ceil_u64(((u64)1U << 40U) + 1U), 41U);
}

NTEST(base, pow2)
{
    NTEST_ASSERT_FALSE(nph_is_pow2_u32(0U));
    NTEST_ASSERT_TRUE(nph_is_pow2_u32(1U));
    NTEST_ASSERT_TRUE(nph_is_pow2_u32(2U));
    NTEST_ASSERT_FALSE(nph_is_pow2_u32(3U));
    NTEST_ASSERT_TRUE(nph_is_pow2_u32(0x80000000U));
    NTEST_ASSERT_FALSE(nph_is_pow2_u64(0U));
    NTEST_ASSERT_TRUE(nph_is_pow2_u64((u64)1U << 63U));
    NTEST_ASSERT_FALSE(nph_is_pow2_u64(((u64)1U << 63U) + 1U));

    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u32(0U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u32(1U), 1U);
    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u32(2U), 2U);
    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u32(3U), 4U);
    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u32(1000U), 1024U);
    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u32(0x80000000U), 0x80000000U);
    NTEST_ASSERT_EQ_UINT(nph_next_pow2_u64(((u64)1U << 40U) + 1U), (u64)1U << 41U);
}

NTEST(base, rotate_and_byteswap)
{
    NTEST_ASSERT_EQ_UINT(nph_rotl_u32(0x80000001U, 1U), 0x00000003U);
    NTEST_ASSERT_EQ_UINT(nph_rotr_u32(0x00000003U, 1U), 0x80000001U);
    NTEST_ASSERT_EQ_UINT(nph_rotl_u32(0x12345678U, 0U), 0x12345678U);
    NTEST_ASSERT_EQ_UINT(nph_rotl_u32(0x12345678U, 32U), 0x12345678U);
    NTEST_ASSERT_EQ_UINT(nph_rotl_u64(0x8000000000000001ULL, 4U), 0x0000000000000018ULL);
    NTEST_ASSERT_EQ_UINT(nph_rotr_u64(0x0000000000000018ULL, 4U), 0x8000000000000001ULL);
    NTEST_ASSERT_EQ_UINT(nph_rotl_u64(0x12345678U, 64U), 0x12345678U);
    NTEST_ASSERT_EQ_UINT(nph_bswap_u16(0x1234U), 0x3412U);
    NTEST_ASSERT_EQ_UINT(nph_bswap_u32(0x12345678U), 0x78563412U);
    NTEST_ASSERT_EQ_UINT(nph_bswap_u64(0x0102030405060708ULL), 0x0807060504030201ULL);
}

NTEST(base, alignment)
{
    NTEST_ASSERT_EQ_UINT(nph_align_up_u64(0U, 16U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_align_up_u64(1U, 16U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_align_up_u64(16U, 16U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_align_up_u64(17U, 16U), 32U);
    NTEST_ASSERT_EQ_UINT(nph_align_down_u64(17U, 16U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_align_down_u64(31U, 16U), 16U);
    NTEST_ASSERT_EQ_UINT(nph_align_up_u32(5U, 1U), 5U);
    NTEST_ASSERT_EQ_UINT(nph_align_up_u32(4097U, 4096U), 8192U);
    NTEST_ASSERT_EQ_UINT(nph_align_down_u32(4097U, 4096U), 4096U);
    NTEST_ASSERT_TRUE(nph_is_aligned_u64(64U, 64U));
    NTEST_ASSERT_FALSE(nph_is_aligned_u64(65U, 64U));
    NTEST_ASSERT_TRUE(nph_is_aligned_u64(0U, 4096U));

    NPH_ALIGNAS(64) u8 buffer[256];
    NTEST_ASSERT_TRUE(nph_is_ptr_aligned(buffer, 64U));
    NTEST_ASSERT_EQ_PTR(nph_align_ptr_up(buffer + 1, 64U), buffer + 64);
    NTEST_ASSERT_EQ_PTR(nph_align_ptr_up(buffer, 64U), buffer);
    NTEST_ASSERT_EQ_UINT(nph_ptr_diff(buffer, buffer + 100), 100U);
    NTEST_ASSERT_EQ_UINT(NPH_ALIGNOF(u64), 8U);
    NTEST_ASSERT_EQ_UINT(NPH_ALIGNOF(u8), 1U);
}

NTEST(base, byte_count_helpers)
{
    NTEST_ASSERT_EQ_UINT(NPH_KIB(1), 1024U);
    NTEST_ASSERT_EQ_UINT(NPH_MIB(1), 1048576U);
    NTEST_ASSERT_EQ_UINT(NPH_GIB(8), 8589934592ULL);
    const int values[7] = {0};
    NTEST_ASSERT_EQ_UINT(NPH_ARRAY_COUNT(values), 7U);
    NPH_UNUSED(values);
}

NTEST(base, thread_local_storage_is_per_translation_unit_usable)
{
    static NPH_THREAD_LOCAL u32 counter = 0U;
    counter += 1U;
    NTEST_ASSERT_EQ_UINT(counter, 1U);
}

NTEST(result, strings_cover_every_value)
{
    for (int i = 0; i < (int)NPH_RESULT_COUNT; ++i) {
        const char *s = nph_result_string((NphResult)i);
        NTEST_ASSERT_NOT_NULL(s);
        NTEST_ASSERT_NE_STR(s, "INVALID_RESULT");
        NTEST_ASSERT_GT_UINT(strlen(s), 0U);
    }
    NTEST_ASSERT_EQ_STR(nph_result_string(NPH_RESULT_OK), "OK");
    NTEST_ASSERT_EQ_STR(nph_result_string(NPH_RESULT_OUT_OF_MEMORY), "OUT_OF_MEMORY");
    NTEST_ASSERT_EQ_STR(nph_result_string(NPH_RESULT_COUNT), "INVALID_RESULT");
    NTEST_ASSERT_EQ_STR(nph_result_string((NphResult)12345), "INVALID_RESULT");
}

static NphResult try_helper(NphResult first, NphResult second, int *reached)
{
    NPH_TRY(first);
    *reached = 1;
    NPH_TRY(second);
    *reached = 2;
    return NPH_RESULT_OK;
}

NTEST(result, try_macro_returns_first_failure)
{
    int reached = 0;
    NTEST_ASSERT_EQ_INT(try_helper(NPH_RESULT_FULL, NPH_RESULT_OK, &reached), NPH_RESULT_FULL);
    NTEST_ASSERT_EQ_INT(reached, 0);
    NTEST_ASSERT_EQ_INT(try_helper(NPH_RESULT_OK, NPH_RESULT_IO_ERROR, &reached),
                        NPH_RESULT_IO_ERROR);
    NTEST_ASSERT_EQ_INT(reached, 1);
    NTEST_ASSERT_EQ_INT(try_helper(NPH_RESULT_OK, NPH_RESULT_OK, &reached), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(reached, 2);
}
