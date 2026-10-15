// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the text scene loader (engine/scene, scene_io.h). It
// must never crash or leak; a refused file must leave the world empty and
// say why; an accepted one must save to text that loads again and saves to
// the same bytes (the canonical form is a fixed point), and must survive a
// trip through the binary format.

#include "fuzz_scene_common.h"

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    NphWorld *w = fuzz_scene_world();
    NphPrefab *crate = fuzz_scene_crate(w);
    NphSceneLoadDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.resolve = fuzz_scene_resolve;
    ld.resolve_user = crate;
    NphSceneResult res;
    const NphResult r = nph_scene_load_text(w, nph_str((const char *)data, size), &ld, &res);
    if (r != NPH_RESULT_OK) {
        FUZZ_REQUIRE(res.error[0] != '\0');
        FUZZ_REQUIRE(nph_world_entity_count(w) == 0U);
    } else {
        FUZZ_REQUIRE(res.entity_count == nph_world_entity_count(w));
        NphAllocator *a = nph_allocator_system();
        NphSceneSaveDesc sd;
        memset(&sd, 0, sizeof sd);
        sd.resolve = fuzz_scene_resolve;
        sd.resolve_user = crate;
        NphStrBuilder first;
        NphStrBuilder second;
        nph_sb_init(&first, a);
        nph_sb_init(&second, a);
        FUZZ_REQUIRE(nph_scene_save_text(w, &sd, &first, NULL) == NPH_RESULT_OK);
        NphWorld *again = fuzz_scene_world();
        FUZZ_REQUIRE(nph_scene_load_text(again, nph_sb_str(&first), &ld, &res) == NPH_RESULT_OK);
        FUZZ_REQUIRE(nph_scene_save_text(again, &sd, &second, NULL) == NPH_RESULT_OK);
        FUZZ_REQUIRE(nph_str_equals(nph_sb_str(&first), nph_sb_str(&second)));

        // Binary: the loaded world saves, loads, and saves the same text.
        NphBinWriter bw;
        nph_bin_writer_init(&bw, a);
        FUZZ_REQUIRE(nph_scene_save_binary(w, NULL, &bw, NULL) == NPH_RESULT_OK);
        NphWorld *bin = fuzz_scene_world();
        FUZZ_REQUIRE(nph_scene_load_binary(bin, nph_bin_writer_data(&bw), nph_bin_writer_len(&bw),
                                           NULL, &res) == NPH_RESULT_OK);
        nph_sb_clear(&second);
        FUZZ_REQUIRE(nph_scene_save_text(bin, &sd, &second, NULL) == NPH_RESULT_OK);
        FUZZ_REQUIRE(nph_str_equals(nph_sb_str(&first), nph_sb_str(&second)));
        nph_bin_writer_free(&bw);
        nph_world_destroy(bin);
        nph_world_destroy(again);
        nph_sb_free(&first);
        nph_sb_free(&second);
    }
    nph_prefab_destroy(crate);
    nph_world_destroy(w);
    return 0;
}
