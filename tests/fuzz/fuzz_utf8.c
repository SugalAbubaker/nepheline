// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: UTF-8 decoding of arbitrary bytes, with the invariant
// that every decoded code point re-encodes to the bytes it was decoded from
// (or was a replacement for an invalid sequence).
#include <nepheline/core/string.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const NphStr s = nph_str((const char *)data, size);
    const bool valid = nph_utf8_is_valid(s);
    u64 pos = 0U;
    u32 cp;
    u64 count = 0U;
    bool saw_invalid = false;
    while (nph_utf8_decode(s, &pos, &cp)) {
        ++count;
        if (pos > size) {
            abort();
        }
        char bytes[4];
        const u32 n = nph_utf8_encode(cp, bytes);
        if (cp == NPH_UTF8_REPLACEMENT) {
            saw_invalid =
                saw_invalid || n == 0U || pos < 3U || memcmp(bytes, data + pos - 3U, 3U) != 0;
            continue;
        }
        if (n == 0U || pos < n || memcmp(bytes, data + pos - n, n) != 0) {
            abort(); // a decoded code point must re-encode to its source bytes
        }
    }
    if (count != nph_utf8_count(s)) {
        abort();
    }
    if (valid && saw_invalid) {
        abort(); // is_valid disagrees with the decoder
    }
    return 0;
}
