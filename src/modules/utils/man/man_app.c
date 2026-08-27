#include "man_app.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"

static const char *man_app__find_command(const char *command) {
    size_t count = app_runner__command_count();
    for (size_t i = 0; i < count; ++i) {
        const char *name = app_runner__command_name(i);
        if (name != NULL && strcmp(name, command) == 0) return name;
    }
    return NULL;
}

static int man_app__wait(bruce_process_id_t process_id) {
    for (;;) {
        int result = process__wait(process_id, 100);
        if (result == BRUCE_OK || result == BRUCE_ERR_NOT_FOUND) return BRUCE_OK;
        if (result != BRUCE_ERR_TIMEOUT) return result;
        if (runtime__delay(10) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

/* Categories in display order. Anything uncategorized (NULL/"") prints under
 * a trailing "Other" section, and any category name outside this list still
 * gets its own section after that - so a stray/misspelled category shows up
 * as its own header instead of silently vanishing. */
static const char *const MAN_APP_CATEGORY_ORDER[] = {
    "System",
    "Storage",
    "Network",
    "Radio",
    "Runtime",
    "Shell",
    "Content",
};
#define MAN_APP_CATEGORY_COUNT (sizeof(MAN_APP_CATEGORY_ORDER) / sizeof(MAN_APP_CATEGORY_ORDER[0]))

static bool man_app__is_hidden_category(const char *category) {
    return category != NULL && strcmp(category, "Test") == 0;
}

static bool man_app__category_matches(const char *category, const char *bucket) {
    if (category == NULL || category[0] == '\0') return false;
    return strcmp(category, bucket) == 0;
}

static void man_app__list_category(const char *bucket, size_t count) {
    bool header_printed = false;
    for (size_t i = 0; i < count; ++i) {
        const char *category = app_runner__command_category(i);
        bool in_this_bucket = bucket != NULL ? man_app__category_matches(category, bucket)
                                             : category == NULL || category[0] == '\0';
        if (!in_this_bucket) continue;

        const char *name = app_runner__command_name(i);
        if (name == NULL) continue;
        if (!header_printed) {
            stdio__printf("\n%s:\n", bucket != NULL ? bucket : "Other");
            header_printed = true;
        }
        const char *description = app_runner__command_description(i);
        stdio__printf("%s - %s\n", name, description != NULL ? description : "");
    }
}

/* Drives `visit` once per section: each known category in display order, then
 * "Other" (bucket == NULL) for uncategorized commands, then any category name
 * this list doesn't know about (excluding hidden ones like "Test"), each
 * printed once the first time it's encountered. Shared by the plain-text
 * listing and `--gen-md` so both agree on section order and hidden/unknown
 * category handling. */
static void man_app__visit_all_sections(
    size_t count, void (*visit)(const char *bucket, size_t count, void *context), void *context
) {
    for (size_t category_index = 0; category_index < MAN_APP_CATEGORY_COUNT; ++category_index) {
        visit(MAN_APP_CATEGORY_ORDER[category_index], count, context);
    }
    visit(NULL, count, context);
    for (size_t i = 0; i < count; ++i) {
        const char *category = app_runner__command_category(i);
        if (category == NULL || category[0] == '\0' || man_app__is_hidden_category(category)) continue;

        bool already_handled = false;
        for (size_t j = 0; j < MAN_APP_CATEGORY_COUNT && !already_handled; ++j) {
            already_handled = man_app__category_matches(category, MAN_APP_CATEGORY_ORDER[j]);
        }
        for (size_t earlier = 0; earlier < i && !already_handled; ++earlier) {
            already_handled = man_app__category_matches(category, app_runner__command_category(earlier));
        }
        if (!already_handled) visit(category, count, context);
    }
}

static void man_app__list_section(const char *bucket, size_t count, void *context) {
    (void)context;
    man_app__list_category(bucket, count);
}

static int man_app__list_commands(void) {
    stdio__printf("Available commands:\n");
    size_t count = app_runner__command_count();
    man_app__visit_all_sections(count, man_app__list_section, NULL);
    stdio__printf("\nType:\nman <command>\nto open a command manual\n");
    return BRUCE_OK;
}

/* Some commands don't actually check for --help (they just launch a picker
 * or otherwise block waiting for input), which would hang --gen-md forever.
 * Give each command this long to exit before it gets killed and skipped. */
#define MAN_APP_GEN_MD_HELP_TIMEOUT_MS 5000u

/* Runs `command --help` on a throwaway stdio session and streams everything
 * it prints straight to the screen as it arrives - the same live output
 * `man <command>` shows, just looped over every command in a row instead of
 * one. Sets `*out_bytes` to how much was printed. Returns BRUCE_ERR_TIMEOUT
 * (and kills the command) if it doesn't exit on its own within the timeout
 * above. */
static bruce_result_t man_app__print_help(const char *command, size_t *out_bytes) {
    *out_bytes = 0;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) return BRUCE_ERR_IO;
    if (stdio__session_route_children(session) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return BRUCE_ERR_IO;
    }
    int process_id = app_runner__run(command, "--help", BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (process_id < 0) {
        (void)stdio__session_close(session);
        return (bruce_result_t)process_id;
    }

    uint64_t deadline = runtime__now() + MAN_APP_GEN_MD_HELP_TIMEOUT_MS;
    bruce_process_status_t status = {0};
    bool complete = false;
    bool timed_out = false;
    while (!complete && !timed_out) {
        char chunk[256];
        size_t size = 0;
        while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
            (void)stdio__write(chunk, size);
            *out_bytes += size;
        }
        bruce_result_t waited = process__wait_status((bruce_process_id_t)process_id, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!complete) {
            if (runtime__now() >= deadline) {
                timed_out = true;
                break;
            }
            (void)runtime__delay(20);
        }
    }
    if (timed_out) {
        (void)process__kill((bruce_process_id_t)process_id);
        (void)process__wait_status((bruce_process_id_t)process_id, 500, &status);
    }
    char chunk[256];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        (void)stdio__write(chunk, size);
        *out_bytes += size;
    }
    (void)stdio__session_close(session);
    return timed_out ? BRUCE_ERR_TIMEOUT : BRUCE_OK;
}

typedef struct {
    size_t timed_out;
} man_app__gen_md_ctx_t;

/* Prints one command's table-of-contents bullet: a link to its own "###
 * name" heading further down, plus its one-line description. */
static void man_app__gen_md_toc_entry(size_t index) {
    const char *name = app_runner__command_name(index);
    if (name == NULL) return;
    const char *description = app_runner__command_description(index);
    stdio__printf("- [`%s`](#%s) - %s\n", name, name, description != NULL ? description : "");
}

/* Prints one command's full section: a "### name" heading, its category and
 * description, then its captured `--help` text in a fenced code block. */
static void man_app__gen_md_body_entry(size_t index, man_app__gen_md_ctx_t *ctx) {
    const char *name = app_runner__command_name(index);
    if (name == NULL) return;
    const char *description = app_runner__command_description(index);
    const char *category = app_runner__command_category(index);
    const char *category_label = category != NULL && category[0] != '\0' ? category : "Uncategorized";

    stdio__printf("\n### %s\n\n**Category:** %s\n\n", name, category_label);
    if (description != NULL && description[0] != '\0') stdio__printf("%s\n\n", description);

    stdio__printf("```\n");
    size_t bytes = 0;
    bruce_result_t captured = man_app__print_help(name, &bytes);
    if (captured == BRUCE_ERR_TIMEOUT) {
        stdio__printf(
            "(timed out waiting for --help output - command may ignore --help and block\n"
            "on input; it was killed so --gen-md could continue)\n"
        );
        ctx->timed_out++;
    } else if (captured != BRUCE_OK || bytes == 0) {
        stdio__printf("(no --help output captured)\n");
    }
    stdio__printf("```\n");
}

/* Shared by both gen-md passes below: walks one category bucket, printing
 * its "### "/"## " header (named `bucket`, or "Other" for NULL) once, right
 * before the first command found in it, via `print_header`. */
static void man_app__gen_md_walk_bucket(
    const char *bucket, size_t count, const char *header_format, void (*visit_entry)(size_t, void *),
    void *context
) {
    bool header_printed = false;
    for (size_t i = 0; i < count; ++i) {
        const char *category = app_runner__command_category(i);
        bool in_this_bucket = bucket != NULL ? man_app__category_matches(category, bucket)
                                             : category == NULL || category[0] == '\0';
        if (!in_this_bucket) continue;
        if (app_runner__command_name(i) == NULL) continue;

        if (!header_printed) {
            stdio__printf(header_format, bucket != NULL ? bucket : "Other");
            header_printed = true;
        }
        visit_entry(i, context);
    }
}

static void man_app__gen_md_toc_visit(size_t index, void *context) {
    (void)context;
    man_app__gen_md_toc_entry(index);
}

static void man_app__gen_md_toc_section(const char *bucket, size_t count, void *context) {
    man_app__gen_md_walk_bucket(bucket, count, "\n### %s\n\n", man_app__gen_md_toc_visit, context);
}

static void man_app__gen_md_body_visit(size_t index, void *context) {
    man_app__gen_md_body_entry(index, context);
}

static void man_app__gen_md_body_section(const char *bucket, size_t count, void *context) {
    man_app__gen_md_walk_bucket(bucket, count, "\n## %s\n", man_app__gen_md_body_visit, context);
}

/* `man --gen-md`: prints a single Markdown doc covering every command
 * straight to the screen - a table of contents grouped by category linking
 * to each command's own section, followed by the sections themselves,
 * streamed out as each command is processed rather than built up in memory.
 * Never touches storage: it's meant to be captured off the terminal (or
 * piped, e.g. `man --gen-md > COMMANDS.md` from a host serial terminal),
 * since a doc this size written to the device's own flash can fill it. */
static int man_app__gen_md(void) {
    size_t count = app_runner__command_count();

    stdio__printf("# BruceOS Command Reference\n\nAuto-generated by `man --gen-md`.\n\n## Contents\n");
    man_app__visit_all_sections(count, man_app__gen_md_toc_section, NULL);

    stdio__write("\n---\n", 5);

    man_app__gen_md_ctx_t ctx = {.timed_out = 0};
    man_app__visit_all_sections(count, man_app__gen_md_body_section, &ctx);

    if (ctx.timed_out > 0) {
        stdio__printf("\n(man: --gen-md: %zu command(s) timed out waiting for --help)\n", ctx.timed_out);
    }
    return BRUCE_OK;
}

static int man_app__show_command(const char *command) {
    int process_id = app_runner__run(command, "--help", BRUCE_LAUNCH_BACKGROUND);
    return process_id < 0 ? process_id : man_app__wait((bruce_process_id_t)process_id);
}

static int man_app__page(const char *command) {
    const char *registered = NULL;
    if (command != NULL) {
        registered = man_app__find_command(command);
        if (registered == NULL) {
            stdio__printf("man: %s: no manual entry\n", command);
            return BRUCE_ERR_NOT_FOUND;
        }
        for (const unsigned char *p = (const unsigned char *)registered; *p != 0; ++p) {
            if (!isalnum(*p) && *p != '_' && *p != '-') {
                stdio__printf("man: %s: unsupported command name\n", command);
                return BRUCE_ERR_INVALID_ARGUMENT;
            }
        }
    }

    size_t capacity = (registered != NULL ? strlen(registered) : 0u) + 32u;
    char *shell_args = memory__malloc(capacity);
    if (shell_args == NULL) return BRUCE_ERR_NO_MEMORY;
    if (registered != NULL) snprintf(shell_args, capacity, "-c '%s --help | less'", registered);
    else snprintf(shell_args, capacity, "-c 'man | less'");

    int process_id = app_runner__run("shell", shell_args, BRUCE_LAUNCH_BACKGROUND);
    memory__free(shell_args);
    return process_id < 0 ? process_id : man_app__wait((bruce_process_id_t)process_id);
}

int man_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "List commands or show the manual for one command.");
    ap_add_optional_arg(parser, "command", "Registered command name");
    ap_add_flag(parser, "gen-md");
    ap_set_opt_help(parser, "gen-md", "Print a single Markdown doc covering every command to the screen");

    if (!ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION) ap_print_help(parser);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(parser);
        return result;
    }

    const char *command = ap_get_arg(parser, "command");
    int result;
    if (ap_found(parser, "gen-md")) result = man_app__gen_md();
    else if (tty__isatty()) result = man_app__page(command);
    else result = command != NULL ? man_app__show_command(command) : man_app__list_commands();
    ap_free(parser);
    return result;
}
