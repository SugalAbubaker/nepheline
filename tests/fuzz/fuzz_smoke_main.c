// SPDX-License-Identifier: Apache-2.0
// Runs a libFuzzer target without libFuzzer (CLAUDE.md §9 "Fuzzing"):
// every file of its seed corpus, then deterministic mutations of each
// (bit flips, byte substitutions with boundary values, truncations,
// removed and duplicated ranges, and splices of two seeds). A target with
// no corpus gets random inputs instead.
//
// It is built for every fuzz target in every configuration and registered
// with CTest as fuzz_smoke.<name>, so a crash on a seed or on a shallow
// mutation shows up under each toolchain's sanitizers on the next push.
// The nightly workflow (.github/workflows/fuzz.yml) runs the real fuzzers,
// which explore with coverage guidance, for minutes per target.
//
//     <name>_fuzz_smoke <corpus dir> [mutations per seed]
#include <nepheline/platform/file.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define SMOKE_MAX_SEEDS 256U
#define SMOKE_MAX_INPUT (1U << 20U)

typedef struct Seed {
    u8 *data;
    u64 size;
} Seed;

static u64 smoke_rng(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ULL); // splitmix64
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

static u64 below(u64 *rng, u64 n)
{
    return n == 0U ? 0U : smoke_rng(rng) % n;
}

// One mutation of `in` into `out` (capacity SMOKE_MAX_INPUT); returns the
// mutated size. `other` is a second seed for splices.
static u64 mutate(u64 *rng, const Seed *in, const Seed *other, u8 *out)
{
    u64 n = in->size < SMOKE_MAX_INPUT ? in->size : SMOKE_MAX_INPUT;
    memcpy(out, in->data, (size_t)n);
    const u32 rounds = 1U + (u32)below(rng, 4U);
    for (u32 r = 0; r < rounds; ++r) {
        switch (below(rng, 7U)) {
        case 0: // flip a bit
            if (n != 0U) {
                out[below(rng, n)] ^= (u8)(1U << below(rng, 8U));
            }
            break;
        case 1: { // a boundary value
            static const u8 values[] = {0x00U, 0x01U, 0x7FU, 0x80U, 0xFEU, 0xFFU};
            if (n != 0U) {
                out[below(rng, n)] = values[below(rng, NPH_ARRAY_COUNT(values))];
            }
            break;
        }
        case 2: // a random byte
            if (n != 0U) {
                out[below(rng, n)] = (u8)smoke_rng(rng);
            }
            break;
        case 3: // truncate
            n = below(rng, n + 1U);
            break;
        case 4: { // remove a range
            if (n != 0U) {
                const u64 at = below(rng, n);
                const u64 len = 1U + below(rng, n - at);
                memmove(out + at, out + at + len, (size_t)(n - at - len));
                n -= len;
            }
            break;
        }
        case 5: { // duplicate a range
            if (n != 0U && n < SMOKE_MAX_INPUT) {
                const u64 at = below(rng, n);
                u64 len = 1U + below(rng, nph_min_u64(n - at, 64U));
                len = nph_min_u64(len, SMOKE_MAX_INPUT - n);
                memmove(out + at + len, out + at, (size_t)(n - at));
                n += len;
            }
            break;
        }
        default: { // splice the tail of another seed
            if (other->size != 0U) {
                const u64 at = below(rng, n + 1U);
                const u64 from = below(rng, other->size);
                const u64 len = nph_min_u64(other->size - from, SMOKE_MAX_INPUT - at);
                memcpy(out + at, other->data + from, (size_t)len);
                n = at + len;
            }
            break;
        }
        }
    }
    return n;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(((const NphDirEntry *)a)->name, ((const NphDirEntry *)b)->name);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <corpus dir> [mutations per seed]\n", argv[0]);
        return 2;
    }
    const u32 mutations = argc >= 3 ? (u32)strtoul(argv[2], NULL, 10) : 200U;
    NphAllocator *alloc = nph_allocator_system();
    Seed *seeds = NPH_ALLOC_ARRAY_ZEROED(alloc, Seed, SMOKE_MAX_SEEDS);
    NphDirEntry *names = NPH_ALLOC_ARRAY_ZEROED(alloc, NphDirEntry, SMOKE_MAX_SEEDS);
    u8 *buffer = NPH_ALLOC_ARRAY(alloc, u8, SMOKE_MAX_INPUT);
    if (seeds == NULL || names == NULL || buffer == NULL) {
        return 1;
    }
    // The corpus, sorted by name so runs are reproducible.
    u32 count = 0U;
    NphDirIterator it;
    if (nph_dir_open(argv[1], &it) == NPH_RESULT_OK) {
        NphDirEntry e;
        while (count < SMOKE_MAX_SEEDS && nph_dir_next(&it, &e)) {
            if (!e.is_directory && e.name[0] != '.') {
                names[count++] = e;
            }
        }
        nph_dir_close(&it);
    }
    qsort(names, count, sizeof *names, compare_names);
    u32 loaded = 0U;
    for (u32 i = 0; i < count; ++i) {
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", argv[1], names[i].name);
        void *data = NULL;
        u64 size = 0U;
        if (nph_file_read_all(path, alloc, &data, &size) != NPH_RESULT_OK) {
            fprintf(stderr, "%s: cannot read\n", path);
            return 1;
        }
        seeds[loaded].data = (u8 *)data;
        seeds[loaded].size = size;
        ++loaded;
    }
    u64 rng = 0x6E657068656C696EULL; // "nephelin"
    u64 runs = 0U;
    if (loaded == 0U) {
        // No corpus: random inputs of random lengths.
        for (u32 i = 0; i < mutations * 8U; ++i) {
            const u64 n = below(&rng, 4096U);
            for (u64 b = 0; b < n; ++b) {
                buffer[b] = (u8)smoke_rng(&rng);
            }
            LLVMFuzzerTestOneInput(buffer, (size_t)n);
            ++runs;
        }
    }
    for (u32 i = 0; i < loaded; ++i) {
        LLVMFuzzerTestOneInput(seeds[i].data, (size_t)seeds[i].size);
        ++runs;
        for (u32 m = 0; m < mutations; ++m) {
            const Seed *other = &seeds[below(&rng, loaded)];
            const u64 n = mutate(&rng, &seeds[i], other, buffer);
            LLVMFuzzerTestOneInput(buffer, (size_t)n);
            ++runs;
        }
    }
    printf("%s: %u seeds, %llu inputs, no crash\n", argv[0], loaded, (unsigned long long)runs);
    for (u32 i = 0; i < loaded; ++i) {
        nph_file_free_all(alloc, seeds[i].data, seeds[i].size);
    }
    NPH_FREE_ARRAY(alloc, u8, buffer, SMOKE_MAX_INPUT);
    NPH_FREE_ARRAY(alloc, NphDirEntry, names, SMOKE_MAX_SEEDS);
    NPH_FREE_ARRAY(alloc, Seed, seeds, SMOKE_MAX_SEEDS);
    return 0;
}
