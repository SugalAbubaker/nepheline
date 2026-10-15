// SPDX-License-Identifier: Apache-2.0
// libFuzzer target for pack archives (pack.h): the image is validated in
// aligned memory and, when it passes, every entry is looked up by path and
// by GUID and its bytes are touched, so the tables that validation accepts
// are safe to use.
#include <nepheline/asset/pack.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0U || size > (1U << 22U)) {
        return 0;
    }
    NphAllocator *alloc = nph_allocator_system();
    // Whole pages: ASan's quarantine counts the bytes asked for, and a
    // page-aligned chunk for a few hundred bytes occupies two or three
    // pages, so with small inputs the quarantine kept gigabytes alive and
    // libFuzzer's RSS limit stopped the nightly run.
    const u64 cap = (size + 4095U) & ~(u64)4095U;
    u8 *copy = (u8 *)NPH_ALLOC(alloc, cap, 4096U);
    if (copy == NULL) {
        return 0;
    }
    memcpy(copy, data, size);
    NphPack pack;
    memset(&pack, 0, sizeof pack);
    if (nph_pack_validate(copy, size, &pack) == NPH_RESULT_OK) {
        pack.mapping.data = copy;
        pack.mapping.size = size;
        u64 checksum = 0U;
        for (u32 i = 0; i < nph_pack_entry_count(&pack); ++i) {
            const NphPackEntry *e = nph_pack_entry_at(&pack, i);
            if (nph_pack_find_path(&pack, nph_pack_entry_path(&pack, e)) != e ||
                nph_pack_find_guid(&pack, e->guid) != e) {
                abort();
            }
            const u8 *bytes = (const u8 *)nph_pack_entry_data(&pack, e);
            for (u64 b = 0; b < e->size; b += 4096U) {
                checksum += bytes[b];
            }
            if (e->size != 0U) {
                checksum += bytes[e->size - 1U];
            }
        }
        (void)checksum;
    }
    NPH_FREE(alloc, copy, cap, 4096U);
    return 0;
}
