#include "task_app.h"

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"

#define TASK_APP__MAX_TASKS 16

static int task_app__parse_id(const char *value, bruce_task_id_t *out_id) {
    if (value == NULL || value[0] == '\0') return BRUCE_ERR_INVALID_ARGUMENT;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == BRUCE_TASK_ID_INVALID ||
        parsed > UINT32_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    *out_id = (bruce_task_id_t)parsed;
    return BRUCE_OK;
}

static int task_app__switch(const char *target) {
    if (strcmp(target, "next") == 0) return task__switch_next();
    if (strcmp(target, "prev") == 0) return task__switch_previous();

    bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
    int parsed = task_app__parse_id(target, &task_id);
    return parsed == BRUCE_OK ? task__foreground(task_id) : parsed;
}

static int task_app__resolve_kill_target(const char *target, bruce_task_id_t *out_id) {
    int parsed = task_app__parse_id(target, out_id);
    if (parsed == BRUCE_OK) return BRUCE_OK;
    if (target == NULL || target[0] == '\0' || (target[0] >= '0' && target[0] <= '9')) return parsed;

    bruce_task_snapshot_t tasks[TASK_APP__MAX_TASKS];
    size_t count = 0;
    int result = task__list(tasks, TASK_APP__MAX_TASKS, &count);
    if (result != BRUCE_OK) return result;

    bruce_task_id_t match = BRUCE_TASK_ID_INVALID;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(tasks[i].name, target) != 0) continue;
        if (match != BRUCE_TASK_ID_INVALID) return BRUCE_ERR_BUSY;
        match = tasks[i].id;
    }
    if (match == BRUCE_TASK_ID_INVALID) return BRUCE_ERR_NOT_FOUND;
    *out_id = match;
    return BRUCE_OK;
}

static int task_app__kill(const char *target) {
    bruce_task_id_t task_id = BRUCE_TASK_ID_INVALID;
    int resolved = task_app__resolve_kill_target(target, &task_id);
    return resolved == BRUCE_OK ? task__kill(task_id) : resolved;
}

int task_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Switch or force-kill managed tasks.");

    ArgParser *switch_command = ap_new_cmd(root, "switch");
    ArgParser *kill_command = ap_new_cmd(root, "kill");
    if (switch_command == NULL || kill_command == NULL) {
        ap_free(root);
        return BRUCE_ERR_NO_MEMORY;
    }
    ap_set_helptext(switch_command, "Switch foreground focus: switch <next|prev|id>");
    ap_add_required_arg(switch_command, "target", "next, prev, or a task ID");
    ap_unknown_options_as_args(switch_command);
    ap_set_helptext(kill_command, "Force-kill a task: kill <id|name>");
    ap_add_required_arg(kill_command, "target", "Task ID or exact task name");
    ap_unknown_options_as_args(kill_command);

    if (!ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        if (status != AP_STATUS_HELP && status != AP_STATUS_VERSION)
            ap_print_help(ap_get_cmd_parser(root) != NULL ? ap_get_cmd_parser(root) : root);
        int result = status == AP_STATUS_HELP || status == AP_STATUS_VERSION ? BRUCE_OK
                     : status == AP_STATUS_NO_MEMORY                         ? BRUCE_ERR_NO_MEMORY
                                                                             : BRUCE_ERR_INVALID_ARGUMENT;
        ap_free(root);
        return result;
    }

    ArgParser *command = ap_get_cmd_parser(root);
    int result;
    if (command == switch_command) {
        result = task_app__switch(ap_get_arg(switch_command, "target"));
    } else if (command == kill_command) {
        result = task_app__kill(ap_get_arg(kill_command, "target"));
    } else {
        ap_print_help(root);
        result = BRUCE_ERR_INVALID_ARGUMENT;
    }
    ap_free(root);
    return result;
}
