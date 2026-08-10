/* Backgrounds/foregrounds itself, allocates tracked memory, registers a
 * tracked resource it deliberately never releases, then exits normally so
 * Core must release both automatically. */
#include "core/process/process.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "modules/utils/process/process_app.h"
#include <stdio.h>
#include <string.h>

#include "freertos/idf_additions.h"
#include "selftest.h"

static selftest__shared_t s_shared;
static volatile bool s_status_worker_started;

#define SELFTEST__PROCESS_STRESS_COUNT 17
#define SELFTEST__RESOURCE_STRESS_COUNT 33

static volatile size_t s_resource_cleanup_count;

bool selftest__run_runtime_now_case(void) {
    uint64_t before = runtime__now();
    if (runtime__delay(2) != BRUCE_OK || runtime__now() <= before) {
        printf("[selftest] process/runtime-now: monotonic clock did not advance\n");
        return false;
    }
    printf("[selftest] process/runtime-now: OK\n");
    return true;
}

bool selftest__run_runtime_timer_case(void) {
    volatile uint32_t ticks = 0;
    bruce_timer_id_t timer = BRUCE_TIMER_ID_INVALID;
    bool ok = runtime__timer_start(1000, &ticks, &timer) == BRUCE_OK &&
              runtime__timer_wait(timer, 20) == BRUCE_OK && __atomic_load_n(&ticks, __ATOMIC_RELAXED) > 0 &&
              runtime__timer_stop(timer) == BRUCE_OK;
    printf("[selftest] process/runtime-timer: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

static int selftest__worker_normal_exit(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (process__to_foreground() != BRUCE_OK) { return -1; }
    s_shared.foregrounded_self = true;

    if (process__to_background() != BRUCE_OK) { return -1; }
    s_shared.backgrounded_self = true;

    unsigned char *block = memory__calloc(256, 1);
    if (block == NULL) { return -1; }
    for (size_t i = 0; i < 256; ++i) {
        if (block[i] != 0) {
            memory__free(block);
            return -1;
        }
    }
    memset(block, 0xAB, 256);
    unsigned char *grown = memory__realloc(block, 384);
    if (grown == NULL) {
        memory__free(block);
        return -1;
    }
    block = grown;
    for (size_t i = 0; i < 256; ++i) {
        if (block[i] != 0xAB) {
            memory__free(block);
            return -1;
        }
    }
    s_shared.allocated_memory = true;

    bruce_resource_id_t resource = process_registry__resource_register(selftest__resource_cleanup, &s_shared);
    if (resource == BRUCE_RESOURCE_ID_INVALID) {
        memory__free(block);
        return -1;
    }
    s_shared.registered_resource = true;

    bruce_process_snapshot_t snapshot;
    if (process__snapshot(process__current_id(), &snapshot) != BRUCE_OK || !snapshot.gui_requested ||
        snapshot.memory_bytes < 384 || snapshot.resource_count < 2) {
        memory__free(block);
        return -1;
    }

    /* Release the memory explicitly but leave `resource` registered on
     * purpose: normal exit must clean it up automatically. */
    memory__free(block);
    return 0;
}

/* Allocates tracked memory and registers a tracked resource, then blocks
 * forever; the harness force-kills this process and expects both released. */
static int selftest__worker_killed(int argc, char **argv) {
    (void)argc;
    (void)argv;

    void *block = memory__malloc(128);
    if (block == NULL) { return -1; }
    s_shared.allocated_memory = true;
    process_registry__resource_register(selftest__resource_cleanup, &s_shared);
    s_shared.registered_resource = true;

    for (;;) { runtime__delay(1000); }
}

static int selftest__worker_wait_for_kill(int argc, char **argv) {
    (void)argc;
    (void)argv;
    while (runtime__delay(1000) == BRUCE_OK) {}
    return 0;
}

static void selftest__count_resource_cleanup(void *context) {
    (void)context;
    s_resource_cleanup_count++;
}

static int selftest__worker_resource_growth(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < SELFTEST__RESOURCE_STRESS_COUNT; ++i) {
        if (process_registry__resource_register(selftest__count_resource_cleanup, NULL) == BRUCE_RESOURCE_ID_INVALID) {
            return -1;
        }
    }
    bruce_process_snapshot_t snapshot;
    return process__snapshot(process__current_id(), &snapshot) == BRUCE_OK &&
                   snapshot.resource_count == SELFTEST__RESOURCE_STRESS_COUNT
               ? 0
               : -1;
}

bool selftest__run_process_registry_growth_case(void) {
    process_create_params_t params = {
        .name = "selftest_grow",
        .entry = selftest__worker_wait_for_kill,
        .built_in = true,
        .start_in_background = true,
        .stack_bytes = 2048,
    };
    bruce_process_id_t ids[SELFTEST__PROCESS_STRESS_COUNT] = {0};
    for (size_t i = 0; i < SELFTEST__PROCESS_STRESS_COUNT; ++i) {
        if (process_registry__create(&params, &ids[i]) != BRUCE_OK) {
            for (size_t j = 0; j < i; ++j) (void)process__kill(ids[j]);
            printf("[selftest] process/registry-growth: create %u failed\n", (unsigned int)i);
            return false;
        }
    }

    bool ok = true;
    for (size_t i = 0; i < SELFTEST__PROCESS_STRESS_COUNT; ++i) {
        bruce_process_snapshot_t snapshot;
        ok = ok && process__snapshot(ids[i], &snapshot) == BRUCE_OK &&
             snapshot.state == BRUCE_PROCESS_BACKGROUND;
    }
    for (size_t i = 0; i < SELFTEST__PROCESS_STRESS_COUNT; ++i) {
        ok = ok && process__kill(ids[i]) == BRUCE_OK;
        bruce_process_status_t status;
        ok = ok && process__wait_status(ids[i], 0, &status) == BRUCE_OK &&
             status.reason == BRUCE_PROCESS_KILLED;
    }
    printf("[selftest] process/registry-growth: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_process_resource_growth_case(void) {
    s_resource_cleanup_count = 0;
    process_create_params_t params = {
        .name = "selftest_resources",
        .entry = selftest__worker_resource_growth,
        .built_in = true,
        .start_in_background = true,
        .stack_bytes = 2048,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    bruce_process_status_t status;
    bool ok = process_registry__create(&params, &id) == BRUCE_OK &&
              process__wait_status(id, 2000, &status) == BRUCE_OK &&
              status.reason == BRUCE_PROCESS_EXITED && status.exit_code == 0 &&
              s_resource_cleanup_count == SELFTEST__RESOURCE_STRESS_COUNT;
    printf("[selftest] process/resource-growth: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_process_normal_exit_case(void) {
    memset(&s_shared, 0, sizeof(s_shared));

    process_create_params_t params = {
        .name = "selftest_exit",
        .entry = selftest__worker_normal_exit,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) {
        printf("[selftest] process/normal-exit: create failed\n");
        return false;
    }
    if (process__wait(id, 2000) != BRUCE_OK) {
        printf("[selftest] process/normal-exit: worker did not exit in time\n");
        return false;
    }
    bruce_process_status_t status;
    if (process__wait_status(id, 0, &status) != BRUCE_OK || status.reason != BRUCE_PROCESS_EXITED ||
        status.exit_code != 0 || status.signal != 0) {
        printf("[selftest] process/normal-exit: incorrect completion status\n");
        return false;
    }

    bruce_process_snapshot_t snapshot;
    if (process__snapshot(id, &snapshot) == BRUCE_OK) {
        printf("[selftest] process/normal-exit: snapshot still present after exit\n");
        return false;
    }

    if (!s_shared.foregrounded_self || !s_shared.backgrounded_self || !s_shared.allocated_memory ||
        !s_shared.registered_resource || !s_shared.resource_cleanup_ran) {
        printf("[selftest] process/normal-exit: worker did not complete or leaked a resource\n");
        return false;
    }
    printf("[selftest] process/normal-exit: OK\n");
    return true;
}

static int selftest__worker_nonzero_exit(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 37;
}

static int selftest__worker_terminates(int argc, char **argv) {
    (void)argc;
    (void)argv;
    s_status_worker_started = true;
    while (runtime__delay(1000) == BRUCE_OK) {}
    return 91;
}

bool selftest__run_process_status_case(void) {
    process_create_params_t exit_params = {
        .name = "selftest_status_exit",
        .entry = selftest__worker_nonzero_exit,
        .built_in = true,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t exit_id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&exit_params, &exit_id) != BRUCE_OK) {
        printf("[selftest] process/status: exit worker create failed\n");
        return false;
    }

    /* Retrieve after the live slot is gone, and prove process__wait does not consume. */
    vTaskDelay(pdMS_TO_TICKS(30));
    bruce_process_snapshot_t snapshot;
    bruce_process_status_t status = {
        .reason = BRUCE_PROCESS_KILLED,
        .exit_code = -123,
        .signal = BRUCE_PROCESS_SIGNAL_KILL,
    };
    if (process__snapshot(exit_id, &snapshot) != BRUCE_ERR_NOT_FOUND ||
        process__wait(exit_id, 0) != BRUCE_OK || process__wait_status(exit_id, 0, &status) != BRUCE_OK ||
        status.reason != BRUCE_PROCESS_EXITED || status.exit_code != 37 || status.signal != 0) {
        printf("[selftest] process/status: delayed nonzero status failed\n");
        return false;
    }
    bruce_process_status_t unchanged = {
        .reason = BRUCE_PROCESS_KILLED,
        .exit_code = 456,
        .signal = BRUCE_PROCESS_SIGNAL_KILL,
    };
    if (process__wait_status(exit_id, 0, &unchanged) != BRUCE_ERR_NOT_FOUND ||
        unchanged.reason != BRUCE_PROCESS_KILLED || unchanged.exit_code != 456 ||
        unchanged.signal != BRUCE_PROCESS_SIGNAL_KILL) {
        printf("[selftest] process/status: completion was not one-shot\n");
        return false;
    }

    s_status_worker_started = false;
    process_create_params_t term_params = {
        .name = "selftest_status_term",
        .entry = selftest__worker_terminates,
        .built_in = true,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t term_id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&term_params, &term_id) != BRUCE_OK) {
        printf("[selftest] process/status: terminate worker create failed\n");
        return false;
    }
    for (int i = 0; i < 20 && !s_status_worker_started; ++i) vTaskDelay(pdMS_TO_TICKS(5));

    unchanged.exit_code = 789;
    if (!s_status_worker_started || process__wait_status(term_id, 0, &unchanged) != BRUCE_ERR_TIMEOUT ||
        unchanged.exit_code != 789) {
        (void)process__kill(term_id);
        printf("[selftest] process/status: poll timeout mutated output\n");
        return false;
    }
    if (process__terminate(term_id) != BRUCE_OK || process__wait_status(term_id, 2000, &status) != BRUCE_OK ||
        status.reason != BRUCE_PROCESS_TERMINATED || status.exit_code != 0 ||
        status.signal != BRUCE_PROCESS_SIGNAL_TERM) {
        (void)process__kill(term_id);
        printf("[selftest] process/status: terminate status failed\n");
        return false;
    }

    printf("[selftest] process/status: OK\n");
    return true;
}

bool selftest__run_process_killed_case(void) {
    memset(&s_shared, 0, sizeof(s_shared));

    process_create_params_t params = {
        .name = "selftest_kill",
        .entry = selftest__worker_killed,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) {
        printf("[selftest] process/killed: create failed\n");
        return false;
    }

    /* Give the worker a moment to allocate and register before killing it. */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (process__kill(id) != BRUCE_OK) {
        printf("[selftest] process/killed: kill failed\n");
        return false;
    }

    bruce_process_snapshot_t snapshot;
    if (process__snapshot(id, &snapshot) == BRUCE_OK) {
        printf("[selftest] process/killed: snapshot still present after kill\n");
        return false;
    }

    bruce_process_status_t status;
    if (process__wait_status(id, 0, &status) != BRUCE_OK || status.reason != BRUCE_PROCESS_KILLED ||
        status.exit_code != 0 || status.signal != BRUCE_PROCESS_SIGNAL_KILL) {
        printf("[selftest] process/killed: incorrect completion status\n");
        return false;
    }

    if (!s_shared.allocated_memory || !s_shared.registered_resource || !s_shared.resource_cleanup_ran) {
        printf("[selftest] process/killed: worker did not reach setup or leaked a resource\n");
        return false;
    }
    printf("[selftest] process/killed: OK\n");
    return true;
}

static int selftest__process_switch_target(int argc, char **argv) {
    (void)argc;
    (void)argv;
    for (;;) (void)runtime__delay(1000);
    return 0;
}

bool selftest__run_process_app_switch_case(void) {
    char *invalid_argv[] = {"process", "switch", "not-an-id", NULL};
    if (process_app_main(3, invalid_argv) != BRUCE_ERR_INVALID_ARGUMENT) {
        printf("[selftest] process/app-switch: invalid target accepted\n");
        return false;
    }

    process_create_params_t params = {
        .name = "selftest_switch_target",
        .entry = selftest__process_switch_target,
        .built_in = true,
        .gui_requested = true,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t target = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &target) != BRUCE_OK) {
        printf("[selftest] process/app-switch: target create failed\n");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    bruce_process_id_t self = process__current_id();
    char target_arg[16];
    snprintf(target_arg, sizeof(target_arg), "%lu", (unsigned long)target);
    char *switch_argv[] = {"process", "switch", target_arg, NULL};
    int switched = process_app_main(3, switch_argv);
    bruce_process_snapshot_t snapshot;
    bool foreground =
        process__snapshot(target, &snapshot) == BRUCE_OK && snapshot.state == BRUCE_PROCESS_FOREGROUND;
    bool registry_foreground = process_registry__foreground_id() == target;

    (void)process__foreground(self);
    bool caller_not_reselected = false;
    if (process__to_background() == BRUCE_OK) {
        bruce_result_t relative = process__switch_next();
        caller_not_reselected = (relative == BRUCE_OK || relative == BRUCE_ERR_NOT_FOUND) &&
                                process__snapshot(self, &snapshot) == BRUCE_OK &&
                                snapshot.state == BRUCE_PROCESS_BACKGROUND;
    }
    (void)process__kill(target);
    (void)process__foreground(self);
    bool ok = switched == BRUCE_OK && foreground && registry_foreground && caller_not_reselected &&
              process_registry__foreground_id() == self;
    printf("[selftest] process/app-switch: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

static bool selftest__process_app_create_target(const char *name, bruce_process_id_t *out_id) {
    process_create_params_t params = {
        .name = name,
        .entry = selftest__process_switch_target,
        .built_in = true,
        .gui_requested = false,
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    if (process_registry__create(&params, out_id) != BRUCE_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

bool selftest__run_process_app_kill_case(void) {
    bruce_process_id_t by_id = BRUCE_PROCESS_ID_INVALID;
    if (!selftest__process_app_create_target("selftest_kill_by_id", &by_id)) {
        printf("[selftest] process/app-kill: ID target create failed\n");
        return false;
    }

    char id_arg[16];
    snprintf(id_arg, sizeof(id_arg), "%lu", (unsigned long)by_id);
    char *id_argv[] = {"process", "kill", id_arg, NULL};
    if (process_app_main(3, id_argv) != BRUCE_OK) {
        (void)process__kill(by_id);
        printf("[selftest] process/app-kill: kill by ID failed\n");
        return false;
    }

    bruce_process_snapshot_t snapshot;
    if (process__snapshot(by_id, &snapshot) == BRUCE_OK) {
        (void)process__kill(by_id);
        printf("[selftest] process/app-kill: ID target still present\n");
        return false;
    }

    bruce_process_id_t by_name = BRUCE_PROCESS_ID_INVALID;
    if (!selftest__process_app_create_target("selftest_kill_by_name", &by_name)) {
        printf("[selftest] process/app-kill: name target create failed\n");
        return false;
    }

    char *name_argv[] = {"process", "kill", "selftest_kill_by_name", NULL};
    if (process_app_main(3, name_argv) != BRUCE_OK) {
        (void)process__kill(by_name);
        printf("[selftest] process/app-kill: kill by name failed\n");
        return false;
    }
    if (process__snapshot(by_name, &snapshot) == BRUCE_OK) {
        (void)process__kill(by_name);
        printf("[selftest] process/app-kill: name target still present\n");
        return false;
    }

    char *missing_argv[] = {"process", "kill", "selftest_missing", NULL};
    bool ok = process_app_main(3, missing_argv) == BRUCE_ERR_NOT_FOUND;
    printf("[selftest] process/app-kill: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
