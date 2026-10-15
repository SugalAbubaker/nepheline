// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the binary scene loader (engine/scene, scene_io.h). The
// input's CRC-32C trailer is recomputed first, so mutations reach the
// parser instead of stopping at the checksum. It must never crash or leak;
// a refused file must leave the world empty and say why; an accepted one
// must save to bytes that load again and save to the same bytes.

#include <nepheline/core/hash.h>

#include "fuzz_scene_common.h"

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    NphAllocator *a = nph_allocator_system();
    u8 *input = NULL;
    if (size >= 4U) {
        input = (u8 *)NPH_ALLOC(a, size, 16U);
        FUZZ_REQUIRE(input != NULL);
        memcpy(input, data, size);
        const u32 crc = nph_crc32c(input, size - 4U, 0U);
        memcpy(input + size - 4U, &crc, sizeof crc);
    }
    NphWorld *w = fuzz_scene_world();
    NphSceneResult res;
    const NphResult r = nph_scene_load_binary(w, input, input != NULL ? size : 0U, NULL, &res);
    if (r != NPH_RESULT_OK) {
        FUZZ_REQUIRE(res.error[0] != '\0');
        FUZZ_REQUIRE(nph_world_entity_count(w) == 0U);
    } else {
        FUZZ_REQUIRE(res.entity_count == nph_world_entity_count(w));
        NphBinWriter first;
        NphBinWriter second;
        nph_bin_writer_init(&first, a);
        nph_bin_writer_init(&second, a);
        FUZZ_REQUIRE(nph_scene_save_binary(w, NULL, &first, NULL) == NPH_RESULT_OK);
        NphWorld *again = fuzz_scene_world();
        FUZZ_REQUIRE(nph_scene_load_binary(again, nph_bin_writer_data(&first),
                                           nph_bin_writer_len(&first), NULL,
                                           &res) == NPH_RESULT_OK);
        FUZZ_REQUIRE(nph_scene_save_binary(again, NULL, &second, NULL) == NPH_RESULT_OK);
        FUZZ_REQUIRE(nph_bin_writer_len(&first) == nph_bin_writer_len(&second));
        FUZZ_REQUIRE(memcmp(nph_bin_writer_data(&first), nph_bin_writer_data(&second),
                            (size_t)nph_bin_writer_len(&first)) == 0);
        // The text form of an accepted file loads too.
        NphStrBuilder text;
        nph_sb_init(&text, a);
        FUZZ_REQUIRE(nph_scene_save_text(w, NULL, &text, NULL) == NPH_RESULT_OK);
        NphWorld *from_text = fuzz_scene_world();
        FUZZ_REQUIRE(nph_scene_load_text(from_text, nph_sb_str(&text), NULL, &res) ==
                     NPH_RESULT_OK);
        nph_world_destroy(from_text);
        nph_sb_free(&text);
        nph_world_destroy(again);
        nph_bin_writer_free(&first);
        nph_bin_writer_free(&second);
    }
    nph_world_destroy(w);
    if (input != NULL) {
        NPH_FREE(a, input, size, 16U);
    }
    return 0;
}
