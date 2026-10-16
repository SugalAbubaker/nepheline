// SPDX-License-Identifier: Apache-2.0
// The console: tokenizing, commands, cvars, scripts, history, completion,
// and the engine command line parser.
#include <nepheline/runtime/config.h>
#include <nepheline/runtime/console.h>

#include <ntest/ntest.h>

#include <string.h>

static NPH_CVAR_INT(cv_rate, "sim.rate", 240, 1, 10000, "simulation steps per second", 0);
static NPH_CVAR_BOOL(cv_vsync, "r.vsync", true, "wait for vertical blank", 0);
static NPH_CVAR_STRING(cv_title, "window.title", "Nepheline", "window title", 0);
static NPH_CVAR_INT(cv_locked, "jobs.workers", 0, 0, 64, "worker threads", NPH_CVAR_FLAG_READ_ONLY);

typedef struct ConsoleFixture {
    NphCvarRegistry cvars;
    NphConsole *console;
    u32 calls;
    NphStr last_args[NPH_CONSOLE_MAX_ARGS];
    char last_text[NPH_CONSOLE_MAX_ARGS][64];
    u32 last_count;
} ConsoleFixture;

NTEST_SETUP(ConsoleFixture)
{
    NTEST_ASSERT_EQ_INT(nph_cvar_registry_init(&f->cvars, nph_allocator_system()), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->cvars, &cv_rate), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->cvars, &cv_vsync), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->cvars, &cv_title), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_register(&f->cvars, &cv_locked), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_create(nph_allocator_system(), &f->cvars, &f->console),
                        NPH_RESULT_OK);
}

NTEST_TEARDOWN(ConsoleFixture)
{
    nph_console_destroy(f->console);
    nph_cvar_registry_free(&f->cvars);
}

static NphResult record(NphConsole *c, void *user, const NphStr *args, u32 count)
{
    NPH_UNUSED(c);
    ConsoleFixture *f = (ConsoleFixture *)user;
    f->calls += 1U;
    f->last_count = count;
    for (u32 i = 0; i < count; ++i) {
        const u64 n = nph_min_u64(args[i].len, 63U);
        memcpy(f->last_text[i], args[i].data, n);
        f->last_text[i][n] = '\0';
    }
    return count > 1U && nph_str_equals(args[1], NPH_STR("fail")) ? NPH_RESULT_BUSY : NPH_RESULT_OK;
}

NTEST(console, tokenizing)
{
    char buffer[128];
    NphStr out[8];
    NTEST_ASSERT_EQ_UINT(nph_console_tokenize(NPH_STR("  a  bc\td "), buffer, out, 8U), 3U);
    NTEST_ASSERT_TRUE(nph_str_equals(out[0], NPH_STR("a")));
    NTEST_ASSERT_TRUE(nph_str_equals(out[1], NPH_STR("bc")));
    NTEST_ASSERT_TRUE(nph_str_equals(out[2], NPH_STR("d")));
    NTEST_ASSERT_EQ_UINT(
        nph_console_tokenize(NPH_STR("x \"two words\" \"q\\\"uo\\\\te\" # comment"), buffer, out,
                             8U),
        3U);
    NTEST_ASSERT_TRUE(nph_str_equals(out[1], NPH_STR("two words")));
    NTEST_ASSERT_TRUE(nph_str_equals(out[2], NPH_STR("q\"uo\\te")));
    NTEST_ASSERT_EQ_UINT(nph_console_tokenize(NPH_STR("\"\""), buffer, out, 8U), 1U);
    NTEST_ASSERT_EQ_UINT(out[0].len, 0U);
    NTEST_ASSERT_EQ_UINT(nph_console_tokenize(NPH_STR("# all comment"), buffer, out, 8U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_console_tokenize(nph_str_empty(), buffer, out, 8U), 0U);
    NTEST_ASSERT_EQ_UINT(nph_console_tokenize(NPH_STR("\"open"), buffer, out, 8U), NPH_U32_MAX);
    NTEST_ASSERT_EQ_UINT(nph_console_tokenize(NPH_STR("a b c"), buffer, out, 2U), NPH_U32_MAX);
}

NTEST_F(ConsoleFixture, commands_receive_their_arguments)
{
    NTEST_ASSERT_EQ_INT(
        nph_console_register(f->console, "test.cmd", "test.cmd: records", record, f),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("test.cmd one \"two three\"")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(f->calls, 1U);
    NTEST_ASSERT_EQ_UINT(f->last_count, 3U);
    NTEST_ASSERT_EQ_STR(f->last_text[0], "test.cmd");
    NTEST_ASSERT_EQ_STR(f->last_text[2], "two three");
    // A command's failure is the execution's result.
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("test.cmd fail")), NPH_RESULT_BUSY);
    // Registration rules.
    NTEST_ASSERT_EQ_INT(nph_console_register(f->console, "test.cmd", "", record, f),
                        NPH_RESULT_ALREADY_EXISTS);
    NTEST_ASSERT_EQ_INT(nph_console_register(f->console, "sim.rate", "", record, f),
                        NPH_RESULT_ALREADY_EXISTS); // a cvar's name
    NTEST_ASSERT_EQ_INT(nph_console_register(f->console, "Bad Name", "", record, f),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_console_register(f->console, "", "", record, f),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_TRUE(nph_console_unregister(f->console, "test.cmd"));
    NTEST_ASSERT_FALSE(nph_console_unregister(f->console, "test.cmd"));
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("test.cmd")), NPH_RESULT_NOT_FOUND);
}

NTEST_F(ConsoleFixture, cvars_print_and_set)
{
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("sim.rate")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("sim.rate 120")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_rate), 120);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("sim.rate 99999")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_rate), 10000); // clamped
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("sim.rate fast")),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("r.vsync off")), NPH_RESULT_OK);
    NTEST_ASSERT_FALSE(nph_cvar_get_bool(&cv_vsync));
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("window.title \"My Game\"")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_cvar_get_string(&cv_title), NPH_STR("My Game")));
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("window.title a b")),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("jobs.workers 4")),
                        NPH_RESULT_INVALID_ARGUMENT); // read-only
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("no.such.thing 1")),
                        NPH_RESULT_NOT_FOUND);
}

NTEST_F(ConsoleFixture, malformed_lines)
{
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("echo \"unterminated")),
                        NPH_RESULT_PARSE_ERROR);
    NTEST_ASSERT_EQ_INT(
        nph_console_execute(f->console, NPH_STR("echo 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17")),
        NPH_RESULT_PARSE_ERROR);
    char long_line[NPH_CONSOLE_MAX_LINE + 8U];
    memset(long_line, 'a', sizeof long_line);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, nph_str(long_line, sizeof long_line)),
                        NPH_RESULT_OVERFLOW);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("   ")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("# nothing")), NPH_RESULT_OK);
}

NTEST_F(ConsoleFixture, built_in_commands)
{
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("help")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("help echo")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("help sim.rate")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("help nothing")),
                        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("cvars")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("cvars sim.")), NPH_RESULT_OK);
    NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, NPH_STR("echo hello there")),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(nph_console_command_count(f->console), 3U);
    NTEST_ASSERT_EQ_STR(nph_console_command_name(f->console, 0U), "help");
    NTEST_ASSERT_NOT_NULL(nph_console_command_help(f->console, 2U));
}

NTEST_F(ConsoleFixture, scripts_stop_at_the_first_failure)
{
    u32 line = 99U;
    NTEST_ASSERT_EQ_INT(nph_console_execute_script(
                            f->console, NPH_STR("# setup\nsim.rate 60\n\nr.vsync false\n"), &line),
                        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_UINT(line, 0U);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_rate), 60);
    NTEST_ASSERT_EQ_INT(
        nph_console_execute_script(f->console, NPH_STR("sim.rate 30\nbogus\nsim.rate 10"), &line),
        NPH_RESULT_NOT_FOUND);
    NTEST_ASSERT_EQ_UINT(line, 2U);
    NTEST_ASSERT_EQ_INT(nph_cvar_get_int(&cv_rate), 30); // the third line never ran
}

NTEST_F(ConsoleFixture, history_and_completion)
{
    for (u32 i = 0; i < NPH_CONSOLE_HISTORY + 5U; ++i) {
        char line[32];
        (void)snprintf(line, sizeof line, "echo %u", i);
        NTEST_ASSERT_EQ_INT(nph_console_execute(f->console, nph_str_from_cstr(line)),
                            NPH_RESULT_OK);
    }
    NTEST_ASSERT_EQ_UINT(nph_console_history_count(f->console), NPH_CONSOLE_HISTORY);
    NTEST_ASSERT_TRUE(nph_str_equals(nph_console_history_at(f->console, 0U), NPH_STR("echo 5")));
    NTEST_ASSERT_TRUE(nph_str_equals(nph_console_history_at(f->console, NPH_CONSOLE_HISTORY - 1U),
                                     NPH_STR("echo 68")));
    NTEST_ASSERT_EQ_UINT(nph_console_history_at(f->console, 999U).len, 0U);

    const char *names[8];
    NTEST_ASSERT_EQ_UINT(nph_console_complete(f->console, NPH_STR("e"), names, 8U), 1U);
    NTEST_ASSERT_EQ_STR(names[0], "echo");
    NTEST_ASSERT_EQ_UINT(nph_console_complete(f->console, NPH_STR(""), names, 8U), 7U);
    NTEST_ASSERT_EQ_STR(names[0], "cvars"); // sorted
    NTEST_ASSERT_EQ_UINT(nph_console_complete(f->console, NPH_STR("s"), names, 8U), 1U);
    NTEST_ASSERT_EQ_STR(names[0], "sim.rate");
    NTEST_ASSERT_EQ_UINT(nph_console_complete(f->console, NPH_STR(""), names, 2U), 7U);
}

// ---------------------------------------------------------------------------
// The engine command line
// ---------------------------------------------------------------------------

NTEST(config, parses_options_and_assignments)
{
    char *argv[] = {
        "player",         "--game=game.dll",       "--scene=level.scene", "--pack=a.npk",
        "--pack=b.npk",   "--content=cooked",      "--config=engine.cfg", "sim.rate=120",
        "r.vsync=false",  "--frames=300",          "--headless",          "--trace=t.json",
        "--script=s.txt", "--user-config=user.cfg"};
    NphEngineConfig c;
    char error[NPH_ENGINE_CONFIG_ERROR_MAX];
    NTEST_ASSERT_EQ_INT(
        nph_engine_config_parse((int)NPH_ARRAY_COUNT(argv), argv, &c, error, sizeof error),
        NPH_RESULT_OK);
    NTEST_ASSERT_EQ_STR(c.game, "game.dll");
    NTEST_ASSERT_EQ_STR(c.scene, "level.scene");
    NTEST_ASSERT_EQ_UINT(c.pack_count, 2U);
    NTEST_ASSERT_EQ_STR(c.packs[1], "b.npk");
    NTEST_ASSERT_EQ_UINT(c.content_count, 1U);
    NTEST_ASSERT_EQ_UINT(c.config_count, 1U);
    NTEST_ASSERT_EQ_UINT(c.assignment_count, 2U);
    NTEST_ASSERT_EQ_STR(c.assignments[0], "sim.rate=120");
    NTEST_ASSERT_TRUE(c.has_max_frames);
    NTEST_ASSERT_EQ_UINT(c.max_frames, 300U);
    NTEST_ASSERT_TRUE(c.headless);
    NTEST_ASSERT_EQ_STR(c.trace, "t.json");
    NTEST_ASSERT_EQ_STR(c.script, "s.txt");
    NTEST_ASSERT_EQ_STR(c.user_config, "user.cfg");
    NTEST_ASSERT_FALSE(c.help);
    NTEST_ASSERT_NOT_NULL(strstr(nph_engine_config_usage(), "--game"));
}

NTEST(config, rejects_malformed_arguments)
{
    NphEngineConfig c;
    char error[NPH_ENGINE_CONFIG_ERROR_MAX];
    char *unknown[] = {"x", "--bogus"};
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(2, unknown, &c, error, sizeof error),
                        NPH_RESULT_INVALID_ARGUMENT);
    NTEST_ASSERT_NOT_NULL(strstr(error, "--bogus"));
    char *bad_frames[] = {"x", "--frames=ten"};
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(2, bad_frames, &c, error, sizeof error),
                        NPH_RESULT_INVALID_ARGUMENT);
    char *empty[] = {"x", "--game="};
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(2, empty, &c, NULL, 0U),
                        NPH_RESULT_INVALID_ARGUMENT);
    char *empty_list[] = {"x", "--pack="};
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(2, empty_list, &c, error, sizeof error),
                        NPH_RESULT_INVALID_ARGUMENT);
    char *equals_only[] = {"x", "=value"};
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(2, equals_only, &c, error, sizeof error),
                        NPH_RESULT_INVALID_ARGUMENT);
    char *too_many[NPH_ENGINE_CONFIG_MAX_LIST + 2U];
    too_many[0] = "x";
    for (u32 i = 1; i < NPH_ENGINE_CONFIG_MAX_LIST + 2U; ++i) {
        too_many[i] = "--pack=a.npk";
    }
    NTEST_ASSERT_EQ_INT(
        nph_engine_config_parse((int)NPH_ARRAY_COUNT(too_many), too_many, &c, error, sizeof error),
        NPH_RESULT_INVALID_ARGUMENT);
    char *help[] = {"x", "-h"};
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(2, help, &c, error, sizeof error), NPH_RESULT_OK);
    NTEST_ASSERT_TRUE(c.help);
    NTEST_ASSERT_EQ_INT(nph_engine_config_parse(0, NULL, &c, error, sizeof error), NPH_RESULT_OK);
}
