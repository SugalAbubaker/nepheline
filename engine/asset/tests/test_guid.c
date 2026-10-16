// SPDX-License-Identifier: Apache-2.0
// The clock-seeded GUID generator (asset/guid.h); the identifier itself is
// tested in core (test_guid.c).
#include <nepheline/asset/guid.h>
#include <nepheline/core/hashmap.h>

#include "test_asset_support.h"

NTEST(guid, generated_guids_are_unique)
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

    // Two generators initialised back to back diverge.
    NphGuidGenerator c;
    NphGuidGenerator d;
    nph_guid_generator_init(&c);
    nph_guid_generator_init(&d);
    NTEST_ASSERT_FALSE(nph_guid_equals(nph_guid_generate(&c), nph_guid_generate(&d)));
}
