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

static int man_app__list_commands(void) {
    stdio__printf("Available commands:\n");
    size_t count = app_runner__command_count();
    for (size_t i = 0; i < count; ++i) {
        const char *name = app_runner__command_name(i);
        if (name != NULL) stdio__printf("%s\n", name);
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
    if (registered != NULL) snprintf(shell_args, capacity, "-c 'man %s | less'", registered);
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
