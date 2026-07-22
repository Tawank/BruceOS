/* Backgrounds/foregrounds itself, allocates tracked memory, registers a
 * tracked resource it deliberately never releases, then exits normally so
 * Core must release both automatically. */
#include "core_sdk/task.h"
#include "core_sdk/memory.h"
#include "core/task/task.h"
#include <stdio.h>
#include <string.h>

#include "freertos/idf_additions.h"
#include "selftest.h"

static int selftest__worker_normal_exit(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (task__foreground(task__current_id()) != BRUCE_OK) {
        return -1;
    }
    s_shared.foregrounded_self = true;

    if (task__to_background() != BRUCE_OK) {
        return -1;
    }
    s_shared.backgrounded_self = true;

    void *block = memory__malloc(256);
    if (block == NULL) {
        return -1;
    }
    memset(block, 0xAB, 256);
    s_shared.allocated_memory = true;

    bruce_resource_id_t resource = task_registry__resource_register(selftest__resource_cleanup, &s_shared);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        memory__free(block);
        return -1;
    }
    s_shared.registered_resource = true;

    bruce_task_snapshot_t snapshot;
    if (task__snapshot(task__current_id(), &snapshot) != BRUCE_OK || snapshot.memory_bytes < 256 ||
        snapshot.resource_count < 2) {
        memory__free(block);
        return -1;
    }

    /* Release the memory explicitly but leave `resource` registered on
     * purpose: normal exit must clean it up automatically. */
    memory__free(block);
    return 0;
}

/* Allocates tracked memory and registers a tracked resource, then blocks
 * forever; the harness force-kills this task and expects both released. */
static int selftest__worker_killed(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    void *block = memory__malloc(128);
    if (block == NULL) {
        return -1;
    }
    s_shared.allocated_memory = true;
    task_registry__resource_register(selftest__resource_cleanup, &s_shared);
    s_shared.registered_resource = true;

    for (;;) {
        runtime__delay(1000);
    }
}

bool selftest__run_task_normal_exit_case(void)
{
    memset(&s_shared, 0, sizeof(s_shared));

    task_create_params_t params = {
        .name = "selftest_exit",
        .entry = selftest__worker_normal_exit,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) {
        printf("[selftest] task/normal-exit: create failed\n");
        return false;
    }
    if (task__wait(id, 2000) != BRUCE_OK) {
        printf("[selftest] task/normal-exit: worker did not exit in time\n");
        return false;
    }

    bruce_task_snapshot_t snapshot;
    if (task__snapshot(id, &snapshot) == BRUCE_OK) {
        printf("[selftest] task/normal-exit: snapshot still present after exit\n");
        return false;
    }

    if (!s_shared.foregrounded_self || !s_shared.backgrounded_self || !s_shared.allocated_memory ||
        !s_shared.registered_resource || !s_shared.resource_cleanup_ran) {
        printf("[selftest] task/normal-exit: worker did not complete or leaked a resource\n");
        return false;
    }
    printf("[selftest] task/normal-exit: OK\n");
    return true;
}

bool selftest__run_task_killed_case(void)
{
    memset(&s_shared, 0, sizeof(s_shared));

    task_create_params_t params = {
        .name = "selftest_kill",
        .entry = selftest__worker_killed,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_task_id_t id = BRUCE_TASK_ID_INVALID;
    if (task_registry__create(&params, &id) != BRUCE_OK) {
        printf("[selftest] task/killed: create failed\n");
        return false;
    }

    /* Give the worker a moment to allocate and register before killing it. */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (task__kill(id) != BRUCE_OK) {
        printf("[selftest] task/killed: kill failed\n");
        return false;
    }

    bruce_task_snapshot_t snapshot;
    if (task__snapshot(id, &snapshot) == BRUCE_OK) {
        printf("[selftest] task/killed: snapshot still present after kill\n");
        return false;
    }

    if (!s_shared.allocated_memory || !s_shared.registered_resource || !s_shared.resource_cleanup_ran) {
        printf("[selftest] task/killed: worker did not reach setup or leaked a resource\n");
        return false;
    }
    printf("[selftest] task/killed: OK\n");
    return true;
}
