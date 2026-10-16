// SPDX-License-Identifier: Apache-2.0
#pragma once

// The console: named commands and console variables behind one command
// line (CLAUDE.md §8.11 "a debug overlay with frame stats, cvars, and a
// console"; §8.13 "Console/log"; docs/runtime.md "The console").
//
//     nph_console_execute(console, NPH_STR("sim.rate 120"));  // sets a cvar
//     nph_console_execute(console, NPH_STR("sim.rate"));      // prints it
//     nph_console_execute(console, NPH_STR("trace \"my trace.json\""));
//
// A line is a command name or a cvar name followed by arguments. Tokens
// are separated by spaces; double quotes group a token, and inside quotes
// \" and \\ escape. A '#' outside quotes starts a comment. A cvar with no
// argument prints its value and description; with one, it is set through
// the registry (core/cvar.h), with its type checks, range clamping, and
// read-only and cheat rules.
//
// Output goes to the log under the "console" category, so it reaches
// every sink: the terminal, the log file, and the editor's console panel.
// Executed lines are kept in a history for the editor's console.
//
// Costs (§1.4): create allocates the command table and the history; a
// registration copies nothing (names and help texts must be static
// strings); an execution allocates nothing.
//
// Thread-safety: NPH_MAIN_THREAD_ONLY (commands act on the engine, which
// belongs to the main thread).

#include <nepheline/core/cvar.h>
#include <nepheline/core/log.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_CONSOLE);

typedef struct NphConsole NphConsole;

#define NPH_CONSOLE_MAX_ARGS     16U
#define NPH_CONSOLE_MAX_LINE     512U
#define NPH_CONSOLE_MAX_COMMANDS 256U
#define NPH_CONSOLE_HISTORY      64U

// A command: `args[0]` is the command's name, `args[1 .. count)` its
// arguments (views into the console's line buffer, valid during the call).
// The result is what nph_console_execute returns.
typedef NphResult (*NphConsoleCommandFn)(NphConsole *console, void *user, const NphStr *args,
                                         u32 count);

// NPH_RESULT_OUT_OF_MEMORY on failure. `cvars` may be NULL (commands only).
NPH_MUST_USE NphResult nph_console_create(NphAllocator *alloc, NphCvarRegistry *cvars,
                                          NphConsole **out);
void nph_console_destroy(NphConsole *console);

// Adds a command. `name` ([a-z0-9._], at most 63 bytes) and `help` are
// static strings. NPH_RESULT_ALREADY_EXISTS when a command or cvar has the
// name, NPH_RESULT_INVALID_ARGUMENT for a malformed name, NPH_RESULT_FULL
// at NPH_CONSOLE_MAX_COMMANDS. The built-in commands are help, cvars, and
// echo.
NPH_MUST_USE NphResult nph_console_register(NphConsole *console, const char *name, const char *help,
                                            NphConsoleCommandFn fn, void *user);

// Removes a command; false when there was none.
bool nph_console_unregister(NphConsole *console, const char *name);

// Runs one line. NPH_RESULT_NOT_FOUND for an unknown name,
// NPH_RESULT_PARSE_ERROR for an unterminated quote or more than
// NPH_CONSOLE_MAX_ARGS tokens, NPH_RESULT_OVERFLOW for a line longer than
// NPH_CONSOLE_MAX_LINE, a cvar setter's result, or the command's. An
// empty or comment-only line does nothing. Each failure is also printed.
NphResult nph_console_execute(NphConsole *console, NphStr line);

// Runs every line of `text` in order, stopping at the first failure,
// whose 1-based line number goes to *out_error_line (may be NULL).
NphResult nph_console_execute_script(NphConsole *console, NphStr text, u32 *out_error_line);

// Prints a line of output (to the log, "console" category, info level).
NPH_PRINTF_LIKE(2, 3)
void nph_console_printf(NphConsole *console, const char *fmt, ...);

// Splits a line into tokens (the rules above) in place: `buffer` receives
// the unescaped text and `out` views into it. Returns the token count, or
// NPH_U32_MAX for an unterminated quote or more than `cap` tokens.
// `buffer` holds at least line.len bytes.
u32 nph_console_tokenize(NphStr line, char *buffer, NphStr *out, u32 cap);

// The executed lines, oldest first (at most NPH_CONSOLE_HISTORY).
u32 nph_console_history_count(const NphConsole *console);
NphStr nph_console_history_at(const NphConsole *console, u32 index);

// Command and cvar names that start with `prefix`, sorted, up to `cap`;
// returns how many matched (which may exceed `cap`).
u32 nph_console_complete(const NphConsole *console, NphStr prefix, const char **out, u32 cap);

// The registered commands, in registration order.
u32 nph_console_command_count(const NphConsole *console);
const char *nph_console_command_name(const NphConsole *console, u32 index);
const char *nph_console_command_help(const NphConsole *console, u32 index);

#ifdef __cplusplus
}
#endif
