// SPDX-License-Identifier: Apache-2.0
// The console: commands, cvars, tokenizing, history. See console.h.
#include <nepheline/core/assert.h>
#include <nepheline/runtime/console.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(NPH_LOG_CONSOLE, "console", NPH_LOG_LEVEL_INFO);

#define CONSOLE_NAME_MAX 63U

typedef struct Command {
    const char *name;
    const char *help;
    NphConsoleCommandFn fn;
    void *user;
} Command;

struct NphConsole {
    NphAllocator *alloc;
    NphCvarRegistry *cvars;
    Command *commands;
    u32 command_count;
    // History: a ring of NPH_CONSOLE_HISTORY lines.
    char (*history)[NPH_CONSOLE_MAX_LINE];
    u32 history_len[NPH_CONSOLE_HISTORY];
    u32 history_head; // oldest
    u32 history_count;
};

void nph_console_printf(NphConsole *console, const char *fmt, ...)
{
    NPH_UNUSED(console);
    if (!NPH_LOG_ENABLED(&NPH_LOG_CONSOLE, NPH_LOG_LEVEL_INFO)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    nph_log_write_v(&NPH_LOG_CONSOLE, NPH_LOG_LEVEL_INFO, __FILE__, (u32)__LINE__, fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------

u32 nph_console_tokenize(NphStr line, char *buffer, NphStr *out, u32 cap)
{
    NPH_ASSERT(buffer != NULL || line.len == 0U);
    u32 count = 0;
    u64 i = 0;
    u64 w = 0;
    while (i < line.len) {
        while (i < line.len && nph_ascii_is_space(line.data[i])) {
            ++i;
        }
        if (i >= line.len || line.data[i] == '#') {
            break;
        }
        if (count == cap) {
            return NPH_U32_MAX;
        }
        const u64 start = w;
        if (line.data[i] == '"') {
            ++i;
            bool closed = false;
            while (i < line.len) {
                const char c = line.data[i];
                if (c == '"') {
                    ++i;
                    closed = true;
                    break;
                }
                if (c == '\\' && i + 1U < line.len &&
                    (line.data[i + 1U] == '"' || line.data[i + 1U] == '\\')) {
                    buffer[w++] = line.data[i + 1U];
                    i += 2U;
                    continue;
                }
                buffer[w++] = c;
                ++i;
            }
            if (!closed) {
                return NPH_U32_MAX;
            }
        } else {
            while (i < line.len && !nph_ascii_is_space(line.data[i])) {
                buffer[w++] = line.data[i++];
            }
        }
        out[count++] = nph_str(buffer + start, w - start);
    }
    return count;
}

// ---------------------------------------------------------------------------
// Lifecycle and registration
// ---------------------------------------------------------------------------

static bool valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    u32 n = 0;
    for (; name[n] != '\0'; ++n) {
        const char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_')) {
            return false;
        }
    }
    return n <= CONSOLE_NAME_MAX;
}

static const Command *find_command(const NphConsole *c, NphStr name)
{
    for (u32 i = 0; i < c->command_count; ++i) {
        if (nph_str_equals(nph_str_from_cstr(c->commands[i].name), name)) {
            return &c->commands[i];
        }
    }
    return NULL;
}

static NphResult cmd_help(NphConsole *c, void *user, const NphStr *args, u32 count);
static NphResult cmd_cvars(NphConsole *c, void *user, const NphStr *args, u32 count);
static NphResult cmd_echo(NphConsole *c, void *user, const NphStr *args, u32 count);

NphResult nph_console_create(NphAllocator *alloc, NphCvarRegistry *cvars, NphConsole **out)
{
    NPH_ASSERT(alloc != NULL && out != NULL);
    *out = NULL;
    NphConsole *c = NPH_ALLOC_TYPE(alloc, NphConsole);
    if (c == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    c->alloc = alloc;
    c->cvars = cvars;
    c->commands = NPH_ALLOC_ARRAY_ZEROED(alloc, Command, NPH_CONSOLE_MAX_COMMANDS);
    c->history = (char(*)[NPH_CONSOLE_MAX_LINE])NPH_ALLOC(
        alloc, (u64)NPH_CONSOLE_HISTORY * NPH_CONSOLE_MAX_LINE, 16U);
    if (c->commands == NULL || c->history == NULL) {
        nph_console_destroy(c);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NPH_VERIFY(nph_console_register(c, "help", "help [name]: lists the commands, or describes one",
                                    cmd_help, NULL) == NPH_RESULT_OK);
    NPH_VERIFY(nph_console_register(c, "cvars", "cvars [prefix]: lists the cvars and their values",
                                    cmd_cvars, NULL) == NPH_RESULT_OK);
    NPH_VERIFY(nph_console_register(c, "echo", "echo text...: prints its arguments", cmd_echo,
                                    NULL) == NPH_RESULT_OK);
    *out = c;
    return NPH_RESULT_OK;
}

void nph_console_destroy(NphConsole *c)
{
    if (c == NULL) {
        return;
    }
    if (c->commands != NULL) {
        NPH_FREE_ARRAY(c->alloc, Command, c->commands, NPH_CONSOLE_MAX_COMMANDS);
    }
    if (c->history != NULL) {
        NPH_FREE(c->alloc, c->history, (u64)NPH_CONSOLE_HISTORY * NPH_CONSOLE_MAX_LINE, 16U);
    }
    NPH_FREE_TYPE(c->alloc, NphConsole, c);
}

NphResult nph_console_register(NphConsole *c, const char *name, const char *help,
                               NphConsoleCommandFn fn, void *user)
{
    NPH_ASSERT(c != NULL && fn != NULL);
    if (!valid_name(name)) {
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphStr n = nph_str_from_cstr(name);
    if (find_command(c, n) != NULL || (c->cvars != NULL && nph_cvar_find(c->cvars, n) != NULL)) {
        return NPH_RESULT_ALREADY_EXISTS;
    }
    if (c->command_count >= NPH_CONSOLE_MAX_COMMANDS) {
        return NPH_RESULT_FULL;
    }
    Command *cmd = &c->commands[c->command_count++];
    cmd->name = name;
    cmd->help = help != NULL ? help : "";
    cmd->fn = fn;
    cmd->user = user;
    return NPH_RESULT_OK;
}

bool nph_console_unregister(NphConsole *c, const char *name)
{
    NPH_ASSERT(c != NULL && name != NULL);
    const NphStr n = nph_str_from_cstr(name);
    for (u32 i = 0; i < c->command_count; ++i) {
        if (nph_str_equals(nph_str_from_cstr(c->commands[i].name), n)) {
            for (u32 j = i + 1U; j < c->command_count; ++j) {
                c->commands[j - 1U] = c->commands[j];
            }
            c->command_count -= 1U;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

static void remember(NphConsole *c, NphStr line)
{
    u32 slot;
    if (c->history_count < NPH_CONSOLE_HISTORY) {
        slot = (c->history_head + c->history_count) % NPH_CONSOLE_HISTORY;
        c->history_count += 1U;
    } else {
        slot = c->history_head;
        c->history_head = (c->history_head + 1U) % NPH_CONSOLE_HISTORY;
    }
    const u32 len = (u32)nph_min_u64(line.len, NPH_CONSOLE_MAX_LINE - 1U);
    memcpy(c->history[slot], line.data, len);
    c->history[slot][len] = '\0';
    c->history_len[slot] = len;
}

static void print_cvar(NphConsole *c, const NphCvar *cv)
{
    char value[128];
    (void)nph_cvar_format(cv, value, sizeof value);
    nph_console_printf(c, "%s = %s  (%s)", cv->name, value, cv->description);
}

NphResult nph_console_execute(NphConsole *c, NphStr line)
{
    NPH_ASSERT(c != NULL);
    if (line.len >= NPH_CONSOLE_MAX_LINE) {
        nph_console_printf(c, "line too long (%llu bytes; at most %u)",
                           (unsigned long long)line.len, NPH_CONSOLE_MAX_LINE - 1U);
        return NPH_RESULT_OVERFLOW;
    }
    char buffer[NPH_CONSOLE_MAX_LINE];
    NphStr args[NPH_CONSOLE_MAX_ARGS];
    const u32 count = nph_console_tokenize(line, buffer, args, NPH_CONSOLE_MAX_ARGS);
    if (count == NPH_U32_MAX) {
        nph_console_printf(c, "cannot parse: an unterminated quote, or more than %u words",
                           NPH_CONSOLE_MAX_ARGS);
        return NPH_RESULT_PARSE_ERROR;
    }
    if (count == 0U) {
        return NPH_RESULT_OK;
    }
    remember(c, nph_str_trim(line));
    const Command *cmd = find_command(c, args[0]);
    if (cmd != NULL) {
        const NphResult r = cmd->fn(c, cmd->user, args, count);
        if (r != NPH_RESULT_OK) {
            nph_console_printf(c, "%s: %s", cmd->name, nph_result_string(r));
        }
        return r;
    }
    NphCvar *cv = c->cvars != NULL ? nph_cvar_find(c->cvars, args[0]) : NULL;
    if (cv == NULL) {
        nph_console_printf(c, "unknown command or cvar '%" NPH_STR_FMT "' (try help)",
                           NPH_STR_ARG(args[0]));
        return NPH_RESULT_NOT_FOUND;
    }
    if (count == 1U) {
        print_cvar(c, cv);
        return NPH_RESULT_OK;
    }
    if (count > 2U) {
        nph_console_printf(c, "%s takes one value (quote it if it has spaces)", cv->name);
        return NPH_RESULT_INVALID_ARGUMENT;
    }
    const NphResult r = nph_cvar_set_from_string(c->cvars, cv, args[1]);
    if (r != NPH_RESULT_OK) {
        nph_console_printf(c, "%s: cannot set to '%" NPH_STR_FMT "': %s", cv->name,
                           NPH_STR_ARG(args[1]), nph_result_string(r));
        return r;
    }
    print_cvar(c, cv);
    return NPH_RESULT_OK;
}

NphResult nph_console_execute_script(NphConsole *c, NphStr text, u32 *out_error_line)
{
    NPH_ASSERT(c != NULL);
    if (out_error_line != NULL) {
        *out_error_line = 0U;
    }
    NphStr rest = text;
    NphStr line;
    u32 number = 0;
    while (nph_str_split_first(rest, '\n', &line, &rest)) {
        ++number;
        const NphResult r = nph_console_execute(c, nph_str_trim(line));
        if (r != NPH_RESULT_OK) {
            if (out_error_line != NULL) {
                *out_error_line = number;
            }
            return r;
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Built-in commands
// ---------------------------------------------------------------------------

static NphResult cmd_help(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(user);
    if (count >= 2U) {
        const Command *cmd = find_command(c, args[1]);
        if (cmd != NULL) {
            nph_console_printf(c, "%s", cmd->help);
            return NPH_RESULT_OK;
        }
        const NphCvar *cv = c->cvars != NULL ? nph_cvar_find(c->cvars, args[1]) : NULL;
        if (cv != NULL) {
            print_cvar(c, cv);
            return NPH_RESULT_OK;
        }
        return NPH_RESULT_NOT_FOUND;
    }
    for (u32 i = 0; i < c->command_count; ++i) {
        nph_console_printf(c, "  %s", c->commands[i].help);
    }
    nph_console_printf(c, "a cvar name prints the cvar; a name and a value sets it");
    return NPH_RESULT_OK;
}

static NphResult cmd_cvars(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(user);
    if (c->cvars == NULL) {
        return NPH_RESULT_OK;
    }
    const NphStr prefix = count >= 2U ? args[1] : nph_str_empty();
    for (u32 i = 0; i < nph_cvar_registry_count(c->cvars); ++i) {
        const NphCvar *cv = nph_cvar_registry_at(c->cvars, i);
        if (nph_str_starts_with(nph_str_from_cstr(cv->name), prefix)) {
            print_cvar(c, cv);
        }
    }
    return NPH_RESULT_OK;
}

static NphResult cmd_echo(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(user);
    char text[NPH_CONSOLE_MAX_LINE];
    u64 len = 0;
    for (u32 i = 1; i < count; ++i) {
        const u64 n = nph_min_u64(args[i].len, sizeof text - 2U - len);
        memcpy(text + len, args[i].data, n);
        len += n;
        if (i + 1U < count && len + 1U < sizeof text) {
            text[len++] = ' ';
        }
    }
    text[len] = '\0';
    nph_console_printf(c, "%s", text);
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

u32 nph_console_history_count(const NphConsole *c)
{
    NPH_ASSERT(c != NULL);
    return c->history_count;
}

NphStr nph_console_history_at(const NphConsole *c, u32 index)
{
    NPH_ASSERT(c != NULL);
    if (index >= c->history_count) {
        return nph_str_empty();
    }
    const u32 slot = (c->history_head + index) % NPH_CONSOLE_HISTORY;
    return nph_str(c->history[slot], c->history_len[slot]);
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

u32 nph_console_complete(const NphConsole *c, NphStr prefix, const char **out, u32 cap)
{
    NPH_ASSERT(c != NULL && (out != NULL || cap == 0U));
    u32 n = 0;
    for (u32 i = 0; i < c->command_count; ++i) {
        if (nph_str_starts_with(nph_str_from_cstr(c->commands[i].name), prefix)) {
            if (n < cap) {
                out[n] = c->commands[i].name;
            }
            ++n;
        }
    }
    if (c->cvars != NULL) {
        for (u32 i = 0; i < nph_cvar_registry_count(c->cvars); ++i) {
            const char *name = nph_cvar_registry_at(c->cvars, i)->name;
            if (nph_str_starts_with(nph_str_from_cstr(name), prefix)) {
                if (n < cap) {
                    out[n] = name;
                }
                ++n;
            }
        }
    }
    qsort((void *)out, nph_min_u32(n, cap), sizeof *out, compare_names);
    return n;
}

u32 nph_console_command_count(const NphConsole *c)
{
    NPH_ASSERT(c != NULL);
    return c->command_count;
}

const char *nph_console_command_name(const NphConsole *c, u32 index)
{
    NPH_ASSERT(c != NULL && index < c->command_count);
    return index < c->command_count ? c->commands[index].name : "";
}

const char *nph_console_command_help(const NphConsole *c, u32 index)
{
    NPH_ASSERT(c != NULL && index < c->command_count);
    return index < c->command_count ? c->commands[index].help : "";
}
