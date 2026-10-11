// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the JSON parser must never crash, leak, or produce a
// tree that fails to round-trip through the writer.
#include <nepheline/core/json.h>

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    NphAllocator *a = nph_allocator_system();
    NphJsonDoc doc;
    const NphResult r = nph_json_parse(nph_str((const char *)data, size), a, &doc);
    if (r == NPH_RESULT_OK) {
        NphStrBuilder first;
        NphStrBuilder second;
        nph_sb_init(&first, a);
        nph_sb_init(&second, a);
        if (nph_json_write(doc.root, &first, false) == NPH_RESULT_OK) {
            NphJsonDoc again;
            if (nph_json_parse(nph_sb_str(&first), a, &again) != NPH_RESULT_OK) {
                abort(); // the writer produced text the parser rejects
            }
            if (nph_json_write(again.root, &second, false) != NPH_RESULT_OK ||
                !nph_str_equals(nph_sb_str(&first), nph_sb_str(&second))) {
                abort(); // compact output is not a fixed point
            }
            nph_json_doc_free(&again);
        }
        nph_sb_free(&first);
        nph_sb_free(&second);
    } else if (r == NPH_RESULT_PARSE_ERROR && doc.error_message == NULL) {
        abort(); // every parse error carries a message
    }
    nph_json_doc_free(&doc);
    return 0;
}
