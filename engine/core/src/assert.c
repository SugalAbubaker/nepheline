// SPDX-License-Identifier: Apache-2.0
#include <nepheline/core/assert.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// The handler is process-wide diagnostic state, in the same category as the
// logging sinks that CLAUDE.md §2 exempts from the "no globals" rule.
static NphAssertHandler g_handler = NULL;
static void *g_handler_user = NULL;

static NphAssertAction default_handler(const NphAssertInfo *info, void *user)
{
    NPH_UNUSED(user);
    fprintf(stderr, "\nAssertion failed: %s\n  at %s:%u in %s()\n", info->condition, info->file,
            info->line, info->function);
    if (info->message[0] != '\0') {
        fprintf(stderr, "  %s\n", info->message);
    }
    fflush(stderr);
    return NPH_ASSERT_ACTION_ABORT;
}

void nph_assert_set_handler(NphAssertHandler handler, void *user, NphAssertHandler *out_previous,
                            void **out_previous_user)
{
    if (out_previous != NULL) {
        *out_previous = g_handler;
    }
    if (out_previous_user != NULL) {
        *out_previous_user = g_handler_user;
    }
    g_handler = handler;
    g_handler_user = user;
}

void nph_assert_fail(const char *file, const char *function, u32 line, const char *condition,
                     const char *fmt, ...)
{
    char message[1024];
    message[0] = '\0';
    if (fmt != NULL && fmt[0] != '\0') {
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(message, sizeof message, fmt, args);
        va_end(args);
        if (n < 0) {
            message[0] = '\0';
        }
    }

    const NphAssertInfo info = {file, function, line, condition, message};
    const NphAssertHandler handler = g_handler != NULL ? g_handler : default_handler;
    const NphAssertAction action = handler(&info, g_handler_user);

    switch (action) {
    case NPH_ASSERT_ACTION_ABORT:
        // abort() stops in the debugger when one is attached; without one, a
        // breakpoint instruction would only replace the abort with a trap.
#if NPH_MSVC_ABI
        // The debug CRT otherwise shows a modal "abort() has been called"
        // box, and Windows Error Reporting may show another; both hang an
        // unattended run (CI, CTest). The message above already said why.
        _set_abort_behavior(0U, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        abort();
    case NPH_ASSERT_ACTION_BREAK:
        NPH_DEBUG_BREAK();
        break;
    case NPH_ASSERT_ACTION_CONTINUE:
        break;
    }
}
