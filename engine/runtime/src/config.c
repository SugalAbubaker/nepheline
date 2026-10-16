// SPDX-License-Identifier: Apache-2.0
// Command-line parsing for the engine. See config.h.
#include <nepheline/core/assert.h>
#include <nepheline/core/string.h>
#include <nepheline/runtime/config.h>

#include <stdio.h>
#include <string.h>

static const char g_usage[] =
    "options:\n"
    "  --game=PATH         the game module to load (and hot-reload)\n"
    "  --scene=PATH        a scene file to load into the world\n"
    "  --pack=PATH         a pack to mount at the VFS root (repeatable)\n"
    "  --content=DIR       a directory of cooked files to mount (repeatable)\n"
    "  --config=PATH       a cvar config file to apply (repeatable)\n"
    "  --user-config=PATH  where the saved cvars are read from and written to\n"
    "  --script=PATH       console commands to run once the engine is up\n"
    "  --trace=PATH        write a Chrome trace of the last frames at exit\n"
    "  --frames=N          stop after N frames\n"
    "  --headless          no window and no GPU: simulation only\n"
    "  --help              this text\n"
    "  name=value          set a cvar (after the config files)\n";

const char *nph_engine_config_usage(void)
{
    return g_usage;
}

static NphResult fail(char *error, u64 cap, const char *what, const char *arg)
{
    if (error != NULL && cap != 0U) {
        (void)snprintf(error, (size_t)cap, "%s: %s", what, arg);
    }
    return NPH_RESULT_INVALID_ARGUMENT;
}

// The value of "--name=value", or NULL when `arg` is another option.
static const char *value_of(const char *arg, const char *name)
{
    const u64 n = strlen(name);
    if (strncmp(arg, name, (size_t)n) == 0 && arg[n] == '=') {
        return arg + n + 1U;
    }
    return NULL;
}

static NphResult push(const char **list, u32 *count, const char *value, char *error, u64 cap,
                      const char *arg)
{
    if (value[0] == '\0') {
        return fail(error, cap, "an empty value", arg);
    }
    if (*count >= NPH_ENGINE_CONFIG_MAX_LIST) {
        return fail(error, cap, "too many of this option", arg);
    }
    list[(*count)++] = value;
    return NPH_RESULT_OK;
}

// A single-valued option: the last occurrence wins; empty is refused.
static NphResult single(const char **slot, const char *value, char *error, u64 cap, const char *arg)
{
    if (value[0] == '\0') {
        return fail(error, cap, "an empty value", arg);
    }
    *slot = value;
    return NPH_RESULT_OK;
}

NphResult nph_engine_config_parse(int argc, char **argv, NphEngineConfig *out, char *error,
                                  u64 error_cap)
{
    NPH_ASSERT(out != NULL && (argv != NULL || argc <= 0));
    memset(out, 0, sizeof *out);
    if (error != NULL && error_cap != 0U) {
        error[0] = '\0';
    }
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *v = NULL;
        if (strcmp(arg, "--headless") == 0) {
            out->headless = 1;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            out->help = 1;
        } else if ((v = value_of(arg, "--game")) != NULL) {
            NPH_TRY(single(&out->game, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--scene")) != NULL) {
            NPH_TRY(single(&out->scene, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--user-config")) != NULL) {
            NPH_TRY(single(&out->user_config, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--script")) != NULL) {
            NPH_TRY(single(&out->script, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--trace")) != NULL) {
            NPH_TRY(single(&out->trace, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--pack")) != NULL) {
            NPH_TRY(push(out->packs, &out->pack_count, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--content")) != NULL) {
            NPH_TRY(push(out->contents, &out->content_count, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--config")) != NULL) {
            NPH_TRY(push(out->configs, &out->config_count, v, error, error_cap, arg));
        } else if ((v = value_of(arg, "--frames")) != NULL) {
            u64 frames = 0;
            if (!nph_str_parse_u64(nph_str_from_cstr(v), &frames)) {
                return fail(error, error_cap, "not a frame count", arg);
            }
            out->max_frames = frames;
            out->has_max_frames = 1;
        } else if (arg[0] != '-' && strchr(arg, '=') != NULL && arg[0] != '=') {
            if (out->assignment_count >= NPH_ENGINE_CONFIG_MAX_ASSIGNMENTS) {
                return fail(error, error_cap, "too many cvar assignments", arg);
            }
            out->assignments[out->assignment_count++] = arg;
        } else {
            return fail(error, error_cap, "unknown argument", arg);
        }
    }
    return NPH_RESULT_OK;
}
