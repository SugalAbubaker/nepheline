// SPDX-License-Identifier: Apache-2.0
#include <nepheline/asset/guid.h>
#include <nepheline/core/hashmap.h>

#include "test_asset_support.h"

NTEST(guid, null_and_equality)
{
    const NphGuid none = NPH_GUID_NULL;
    NTEST_ASSERT_TRUE(nph_guid_is_null(none));
    const NphGuid a = {1U, 2U};
    const NphGuid b = {1U, 2U};
    const NphGuid c = {2U, 1U};
    NTEST_ASSERT_FALSE(nph_guid_is_null(a));
    NTEST_ASSERT_TRUE(nph_guid_equals(a, b));
    NTEST_ASSERT_FALSE(nph_guid_equals(a, c));
    NTEST_ASSERT_EQ_INT(nph_guid_compare(a, b), 0);
    NTEST_ASSERT_GT_INT(nph_guid_compare(a, c), 0); // hi 2 > hi 1
    NTEST_ASSERT_LT_INT(nph_guid_compare(c, a), 0);
    const NphGuid d = {0U, 2U};
    NTEST_ASSERT_GT_INT(nph_guid_compare(a, d), 0); // same hi, lo 1 > 0
    NTEST_ASSERT_NE_UINT(nph_guid_hash(a), nph_guid_hash(c));
}

NTEST(guid, from_str_is_deterministic_and_distinct)
{
    const NphGuid a = nph_guid_from_cstr("textures/wood.png");
    const NphGuid b = nph_guid_from_str(NPH_STR("textures/wood.png"));
    const NphGuid c = nph_guid_from_cstr("textures/wood.png#mip");
    const NphGuid e = nph_guid_from_str(nph_str_empty());
    NTEST_ASSERT_TRUE(nph_guid_equals(a, b));
    NTEST_ASSERT_FALSE(nph_guid_equals(a, c));
    NTEST_ASSERT_FALSE(nph_guid_is_null(a));
    NTEST_ASSERT_FALSE(nph_guid_is_null(e));
    // Pinned: a derived GUID is part of the asset format (cooked
    // references store it), so its value must never change.
    NTEST_ASSERT_EQ_UINT(a.lo, nph_hash64("textures/wood.png", 17U, 0x4E50484755494431ULL));
    NTEST_ASSERT_EQ_UINT(a.hi, nph_hash64("textures/wood.png", 17U, 0x4E50484755494432ULL));
}

NTEST(guid, string_round_trip)
{
    const NphGuid g = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    char text[NPH_GUID_STRING_LEN + 1U];
    nph_guid_to_string(g, text);
    NTEST_ASSERT_EQ_STR(text, "fedcba98765432100123456789abcdef");
    NphGuid back = NPH_GUID_NULL;
    NTEST_ASSERT_TRUE(nph_guid_parse(nph_str_from_cstr(text), &back));
    NTEST_ASSERT_TRUE(nph_guid_equals(g, back));
    // Hyphens and upper case are accepted.
    NTEST_ASSERT_TRUE(nph_guid_parse(NPH_STR("FEDCBA98-7654-3210-0123-456789ABCDEF"), &back));
    NTEST_ASSERT_TRUE(nph_guid_equals(g, back));
    NTEST_ASSERT_TRUE(nph_guid_parse(NPH_STR("00000000000000000000000000000000"), &back));
    NTEST_ASSERT_TRUE(nph_guid_is_null(back));
}

NTEST(guid, parse_rejects_malformed_text)
{
    NphGuid g = {7U, 7U};
    NTEST_ASSERT_FALSE(nph_guid_parse(NPH_STR(""), &g));
    NTEST_ASSERT_FALSE(nph_guid_parse(NPH_STR("0123456789abcdef"), &g)); // too short
    NTEST_ASSERT_FALSE(
        nph_guid_parse(NPH_STR("0123456789abcdef0123456789abcdef0"), &g));               // too long
    NTEST_ASSERT_FALSE(nph_guid_parse(NPH_STR("0123456789abcdef0123456789abcdeg"), &g)); // not hex
    NTEST_ASSERT_FALSE(nph_guid_parse(NPH_STR("{0123456789abcdef0123456789abcdef}"), &g));
    NTEST_ASSERT_EQ_UINT(g.lo, 7U); // untouched
    NTEST_ASSERT_EQ_UINT(g.hi, 7U);
}

NTEST(guid, generated_guids_are_unique_and_reproducible_by_seed)
{
    NphAllocator *alloc = nph_allocator_system();
    NphHashMap seen;
    NphHashMapDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.key_size = sizeof(NphGuid);
    NTEST_ASSERT_EQ_INT(nph_hashmap_init(&seen, alloc, &desc), NPH_RESULT_OK);

    NphGuidGenerator g;
    nph_guid_generator_init(&g);
    for (u32 i = 0; i < 20000U; ++i) {
        const NphGuid id = nph_guid_generate(&g);
        NTEST_ASSERT_FALSE(nph_guid_is_null(id));
        NTEST_ASSERT_EQ_INT(nph_hashmap_insert(&seen, &id, NULL), NPH_RESULT_OK);
    }
    nph_hashmap_free(&seen);

    NphGuidGenerator a;
    NphGuidGenerator b;
    nph_guid_generator_seed(&a, 42U);
    nph_guid_generator_seed(&b, 42U);
    for (u32 i = 0; i < 16U; ++i) {
        NTEST_ASSERT_TRUE(nph_guid_equals(nph_guid_generate(&a), nph_guid_generate(&b)));
    }
    nph_guid_generator_seed(&b, 43U);
    NTEST_ASSERT_FALSE(nph_guid_equals(nph_guid_generate(&a), nph_guid_generate(&b)));

    // Two generators initialised back to back diverge.
    NphGuidGenerator c;
    NphGuidGenerator d;
    nph_guid_generator_init(&c);
    nph_guid_generator_init(&d);
    NTEST_ASSERT_FALSE(nph_guid_equals(nph_guid_generate(&c), nph_guid_generate(&d)));
}
