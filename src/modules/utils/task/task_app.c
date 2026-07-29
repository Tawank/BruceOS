#include "task_app.h"

#include <errno.h> // IWYU pragma: keep
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/task.h"

static int task_app__usage(void) {
    stdio__printf("usage: task switch <next|prev|id>\n");
    return BRUCE_ERR_INVALID_ARGUMENT;
}

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

int task_app_main(int argc, char **argv) {
    if (argc != 3 || argv == NULL || strcmp(argv[1], "switch") != 0) return task_app__usage();
    return task_app__switch(argv[2]);
}
