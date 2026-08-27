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
    "Text",
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

static int man_app__list_commands(void) {
    stdio__printf("Available commands:\n");
    size_t count = app_runner__command_count();

    for (size_t category_index = 0; category_index < MAN_APP_CATEGORY_COUNT; ++category_index) {
        man_app__list_category(MAN_APP_CATEGORY_ORDER[category_index], count);
    }

    /* Uncategorized commands, then anything registered under a category name
     * this list doesn't know about (excluding hidden ones like "Test"). Each
     * unknown category is printed once, the first time it's encountered. */
    man_app__list_category(NULL, count);
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
        if (!already_handled) man_app__list_category(category, count);
    }

    stdio__printf("\nType:\nman <command>\nto open a command manual\n");
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
    if (tty__isatty()) result = man_app__page(command);
    else result = command != NULL ? man_app__show_command(command) : man_app__list_commands();
    ap_free(parser);
    return result;
}
