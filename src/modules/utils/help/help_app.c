#include "help_app.h"

#include <stddef.h>

#include "args.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"

static int help_app__list_commands(void) {
    stdio__printf("Available commands:\n");
    size_t count = app_runner__command_count();
    for (size_t i = 0; i < count; ++i) {
        const char *name = app_runner__command_name(i);
        if (name != NULL) stdio__printf("%s%s", name, i + 1u < count ? " " : "\n");
    }
    stdio__printf("\nType:\nhelp <command>\nto get help about a command\n");
    return BRUCE_OK;
}

static int help_app__show_command(const char *command) {
    int process_id = app_runner__run(command, "--help", BRUCE_LAUNCH_BACKGROUND);
    if (process_id < 0) return process_id;

    for (;;) {
        int result = process__wait((bruce_process_id_t)process_id, 100);
        if (result == BRUCE_OK || result == BRUCE_ERR_NOT_FOUND) return BRUCE_OK;
        if (result != BRUCE_ERR_TIMEOUT) return result;
        if (runtime__delay(10) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
}

int help_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "List commands or show help for one command.");
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
    int result = command != NULL ? help_app__show_command(command) : help_app__list_commands();
    ap_free(parser);
    return result;
}
