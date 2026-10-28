// SPDX-License-Identifier: Apache-2.0
// nph_cook: the command-line asset cooker (CLAUDE.md §8.8).
//
//     nph_cook <source>... -o <out_dir> [--root <dir>] [--pack <file.npk>]
//              [--force] [--lz fast|high] [--usage color|linear|normal|mask|hdr]
//              [--uncompressed] [--no-mips] [--max-extent N]
//              [--no-optimize] [--no-lods] [--voxels] [--sound-codec auto|pcm|lossless|adpcm]
//              [--sound-stream auto|always|never] [--stream-seconds S]
//              [--threads N] [--quiet] [--verbose]
//     nph_cook --pack-only -o <out_dir> --pack <file.npk>
//
// A source may be a directory: every .gltf, .glb, .nmesh, and .wav under it
// is cooked.
//
// Exit status: 0 when every source cooked (or was current), 1 when any
// failed, 2 for a usage error.
#include <nepheline/platform/cpu.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>

#include <cook/cooker.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *out)
{
    fputs("usage: nph_cook <source>... -o <out_dir> [options]\n"
          "       nph_cook --pack-only -o <out_dir> --pack <file.npk>\n"
          "\n"
          "Cooks glTF scenes (.gltf, .glb), images (.png, .jpg, .tga, .bmp, .hdr, .dds),\n"
          "editable meshes (.nmesh), and sounds (.wav) into <out_dir>, keeping\n"
          "cook.db.json there for incremental runs. A directory source cooks every\n"
          ".gltf, .glb, .nmesh, and .wav under it. A sound's <name>.wav.import.json,\n"
          "beside it, overrides the sound options for it (docs/cook.md).\n"
          "\n"
          "options:\n"
          "  -o, --out <dir>       output directory (required)\n"
          "  --root <dir>          sources are named relative to it, and asset GUIDs\n"
          "                        derive from those names (default: the current\n"
          "                        directory); sources outside it are refused\n"
          "  --pack <file.npk>     after cooking, pack every asset in the database\n"
          "  --pack-only           pack without cooking\n"
          "  --force               recook sources the database considers current\n"
          "  --lz fast|high        blob compression level (default high)\n"
          "  --usage <u>           texture usage for standalone images:\n"
          "                        color (default), linear, normal, mask, hdr\n"
          "  --uncompressed        no block compression for textures\n"
          "  --no-mips             base level only\n"
          "  --max-extent <n>      downsample textures larger than n texels a side\n"
          "  --no-optimize         keep the source triangle and vertex order\n"
          "  --no-lods             meshes without levels of detail (level 0 alone)\n"
          "  --voxels              voxel levels for aggregate geometry (glTF meshes)\n"
          "  --sound-codec <c>     auto (default: PCM in memory, lossless streamed),\n"
          "                        pcm, lossless, or adpcm (lossy, in memory only)\n"
          "  --sound-stream <s>    auto (default: sounds of --stream-seconds or more),\n"
          "                        always, or never\n"
          "  --stream-seconds <s>  auto's threshold (default 8)\n"
          "  --threads <n>         worker threads for encoding and levels of detail\n"
          "                        (default: cores - 1)\n"
          "  --quiet               warnings and errors only\n"
          "  --verbose             debug messages too (each mesh's levels of detail)\n",
          out);
}

static bool parse_u32(const char *text, u32 *out)
{
    u64 v;
    if (!nph_str_parse_u64(nph_str_from_cstr(text), &v) || v > NPH_U32_MAX) {
        return false;
    }
    *out = (u32)v;
    return true;
}

int main(int argc, char **argv)
{
    const char *out_dir = NULL;
    const char *root = NULL;
    const char *pack_path = NULL;
    bool pack_only = false;
    bool quiet = false;
    bool verbose = false;
    u32 threads = 0U;
    CookerDesc desc;
    memset(&desc, 0, sizeof desc);
    cook_texture_settings_default(&desc.texture);
    cook_mesh_settings_default(&desc.mesh);
    cook_sound_settings_default(&desc.sound);
    NphAllocator *alloc = nph_allocator_system();
    const char **sources = NPH_ALLOC_ARRAY_ZEROED(alloc, const char *, (u32)argc + 1U);
    u32 source_count = 0U;
    if (sources == NULL) {
        return 2;
    }
    for (int i = 1; i < argc && argv[i] != NULL; ++i) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (next == NULL) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            out_dir = next;
            ++i;
        } else if (strcmp(a, "--root") == 0) {
            if (next == NULL) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            root = next;
            ++i;
        } else if (strcmp(a, "--pack") == 0) {
            if (next == NULL) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            pack_path = next;
            ++i;
        } else if (strcmp(a, "--pack-only") == 0) {
            pack_only = true;
        } else if (strcmp(a, "--force") == 0) {
            desc.force = 1;
        } else if (strcmp(a, "--lz") == 0 && next != NULL) {
            if (strcmp(next, "fast") == 0) {
                desc.texture.lz = NPH_LZ_LEVEL_FAST;
                desc.mesh.lz = NPH_LZ_LEVEL_FAST;
            } else if (strcmp(next, "high") == 0) {
                desc.texture.lz = NPH_LZ_LEVEL_HIGH;
                desc.mesh.lz = NPH_LZ_LEVEL_HIGH;
            } else {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            ++i;
        } else if (strcmp(a, "--usage") == 0 && next != NULL) {
            static const char *names[] = {"color", "linear", "normal", "mask", "hdr"};
            bool found = false;
            for (u32 u = 0; u < NPH_ARRAY_COUNT(names); ++u) {
                if (strcmp(next, names[u]) == 0) {
                    desc.texture.usage = (CookTextureUsage)u;
                    found = true;
                }
            }
            if (!found) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            ++i;
        } else if (strcmp(a, "--uncompressed") == 0) {
            desc.texture.uncompressed = 1;
        } else if (strcmp(a, "--no-mips") == 0) {
            desc.texture.no_mips = 1;
        } else if (strcmp(a, "--max-extent") == 0 && next != NULL) {
            if (!parse_u32(next, &desc.texture.max_extent)) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            ++i;
        } else if (strcmp(a, "--no-optimize") == 0) {
            desc.mesh.no_optimize = 1;
        } else if (strcmp(a, "--no-lods") == 0) {
            desc.mesh.no_lods = 1;
        } else if (strcmp(a, "--voxels") == 0) {
            desc.mesh.voxels = 1;
        } else if ((strcmp(a, "--sound-codec") == 0 || strcmp(a, "--sound-stream") == 0) &&
                   next != NULL) {
            // The per-source settings file's own words (sound.h).
            char json[96];
            snprintf(json, sizeof json, "{\"%s\": \"%s\"}",
                     strcmp(a, "--sound-codec") == 0 ? "codec" : "stream", next);
            if (cook_sound_settings_parse(alloc, json, strlen(json), &desc.sound) !=
                NPH_RESULT_OK) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            ++i;
        } else if (strcmp(a, "--stream-seconds") == 0 && next != NULL) {
            f64 seconds = 0.0;
            if (!nph_str_parse_f64(nph_str_from_cstr(next), &seconds) || !(seconds > 0.0) ||
                seconds > 1.0e9) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            desc.sound.stream_seconds = (f32)seconds;
            ++i;
        } else if (strcmp(a, "--threads") == 0 && next != NULL) {
            if (!parse_u32(next, &threads)) {
                usage(stderr);
                NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
                return 2;
            }
            ++i;
        } else if (strcmp(a, "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(a, "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "nph_cook: unknown option %s\n", a);
            usage(stderr);
            NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
            return 2;
        } else {
            sources[source_count++] = a;
        }
    }
    if (out_dir == NULL || (source_count == 0U && !pack_only) || (pack_only && pack_path == NULL)) {
        usage(stderr);
        NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
        return 2;
    }

    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = alloc;
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    NPH_IGNORE_RESULT(nph_log_add_sink(nph_log_console_sink, &console,
                                       quiet     ? NPH_LOG_LEVEL_WARN
                                       : verbose ? NPH_LOG_LEVEL_DEBUG
                                                 : NPH_LOG_LEVEL_INFO));
    if (verbose) {
        nph_log_set_level(&NPH_LOG_COOK, NPH_LOG_LEVEL_DEBUG);
    }
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    NPH_IGNORE_RESULT(nph_log_thread_start(&log_thread, 5U));

    int status = 0;
    NphJobSystem *jobs = NULL;
    if (threads != 1U) {
        NphJobSystemDesc jd;
        memset(&jd, 0, sizeof jd);
        jd.alloc = alloc;
        jd.worker_count = threads;
        if (nph_job_system_create(&jd, &jobs) != NPH_RESULT_OK) {
            jobs = NULL;
        }
    }
    desc.alloc = alloc;
    desc.out_dir = out_dir;
    desc.source_root = root;
    desc.jobs = jobs;
    Cooker *cooker = NULL;
    const NphStopwatch sw = nph_stopwatch_start();
    const NphResult cr = cooker_create(&desc, &cooker);
    if (cr != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&NPH_LOG_COOK, "cannot start in %s: %s", out_dir, nph_result_string(cr));
        status = 1;
    } else {
        if (!pack_only) {
            for (u32 i = 0; i < source_count; ++i) {
                if (cooker_cook_file(cooker, sources[i]) != NPH_RESULT_OK) {
                    status = 1;
                }
            }
        }
        if (pack_path != NULL && cooker_pack(cooker, pack_path) != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&NPH_LOG_COOK, "packing to %s failed", pack_path);
            status = 1;
        }
        const CookerStats st = cooker_stats(cooker);
        NPH_LOG_INFO(&NPH_LOG_COOK, "%u cooked, %u current, %u removed, %u failed in %.2f s",
                     st.cooked, st.skipped, st.removed, st.failed,
                     nph_stopwatch_elapsed_ms(&sw) / 1000.0);
        cooker_destroy(cooker);
    }
    nph_job_system_destroy(jobs);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    NPH_FREE_ARRAY(alloc, const char *, sources, (u32)argc + 1U);
    return status;
}
