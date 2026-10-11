// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the binary reader over arbitrary bytes must only ever
// report results, never read out of bounds.
#include <nepheline/core/serialize.h>

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    u64 payload = size;
    (void)nph_bin_verify_checksum(data, size, &payload);
    NphBinReader r = nph_bin_reader(data, size);
    u32 version;
    (void)nph_bin_read_header(&r, 0x4E504853U, 0U, 10U, &version);
    for (u32 guard = 0; guard < 100000U && !nph_bin_reader_at_end(&r); ++guard) {
        u32 tag;
        u32 section_size;
        if (nph_bin_read_section(&r, &tag, &section_size) == NPH_RESULT_OK) {
            NphBinReader body = nph_bin_reader(r.data + r.pos, section_size);
            NphStr s;
            f64 d;
            u8 b;
            while (!nph_bin_reader_at_end(&body)) {
                if (nph_bin_read_str(&body, &s) == NPH_RESULT_OK) {
                    continue;
                }
                if (nph_bin_read_f64(&body, &d) == NPH_RESULT_OK) {
                    continue;
                }
                if (nph_bin_read_u8(&body, &b) != NPH_RESULT_OK) {
                    break;
                }
            }
            (void)nph_bin_read_skip(&r, section_size);
        } else {
            u8 byte;
            if (nph_bin_read_u8(&r, &byte) != NPH_RESULT_OK) {
                break;
            }
        }
    }
    (void)nph_bin_read_align(&r, 16U);
    return 0;
}
