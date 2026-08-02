/* A5 acceptance coverage: process-owned opaque Storage handles, `storage`
 * permission enforcement, the permanently-protected /config/bruce.conf and
 * /config/permissions.json paths, per-owner isolation, and automatic cleanup at
 * normal exit and force-kill.
 *
 * Like permission_test.c, the "external app" processes these tests need are
 * created directly via the Core-private process_registry__create() (no ELF/JS
 * loader exists yet to launch a real one). */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "core/dialog/dialog.h"
#include "core/permission/permission.h"
#include "core/storage/storage.h"
#include "core/process/process.h"
#include "core_sdk/dialog.h"
#include "core_sdk/storage.h"
#include "core_sdk/process.h"

#include "storage_test.h"

#define SELFTEST__STORAGE_LEAK_ITERATIONS 24

/* ------------------------------------------------------------------------ */
/* Mock dialog__choice() provider (same pattern as permission_test.c)        */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile int call_count;
    volatile size_t next_selection;
} selftest__storage_dialog_mock_t;

static selftest__storage_dialog_mock_t s_mock;

static bruce_result_t selftest__storage_dialog_mock_provider(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title;
    (void)message;
    (void)choices;
    (void)choice_count;
    s_mock.call_count++;
    *out_selected = s_mock.next_selection;
    return BRUCE_OK;
}

static void selftest__storage_dialog_mock_reset(size_t selection) {
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.next_selection = selection;
    dialog__test_set_choice_provider(selftest__storage_dialog_mock_provider);
}

static void selftest__storage_dialog_mock_clear(void) { dialog__test_set_choice_provider(NULL); }

/* ------------------------------------------------------------------------ */
/* selftest__run_storage_permission_denied_case                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    volatile bruce_result_t result;
    volatile bool ran;
} selftest__storage_open_result_t;

static selftest__storage_open_result_t s_open_result;

static int selftest__storage_open_denied_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    s_open_result.result = storage__open(
        "/selftest_denied.txt",
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    s_open_result.ran = true;
    return 0;
}

bool selftest__run_storage_permission_denied_case(void) {
    permission__test_reset();
    selftest__storage_dialog_mock_reset(1 /* Deny */);
    memset(&s_open_result, 0, sizeof(s_open_result));

    process_create_params_t params = {
        .name = "selftest_storage_denied",
        .entry = selftest__storage_open_denied_entry,
        .argc = 0,
        .argv = NULL,
        .built_in = false,
        .gui_requested = false,
        .permission_key = "selftest_storage_denied.elf",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    bool created = process_registry__create(&params, &id) == BRUCE_OK;
    bruce_result_t wait_result = created ? process__wait(id, 2000) : BRUCE_ERR_INTERNAL;

    selftest__storage_dialog_mock_clear();

    bool ok = created && (wait_result == BRUCE_OK || wait_result == BRUCE_ERR_NOT_FOUND) &&
              s_open_result.ran && s_open_result.result == BRUCE_ERR_PERMISSION;
    printf(
        "[selftest] storage/permission-denied: %s (result=%d)\n", ok ? "OK" : "FAIL", s_open_result.result
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_storage_protected_path_case                                */
/* ------------------------------------------------------------------------ */

bool selftest__run_storage_protected_path_case(void) {
    /* Called directly within selftest's own built-in process context: built-in
     * processes always pass the `storage` permission check, so a denial here can
     * only come from the permanently-protected-path rule itself. */
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t bruce_conf = storage__open("/config/bruce.conf", BRUCE_STORAGE_OPEN_READ, &file);
    bruce_result_t permissions_json =
        storage__open("/config/permissions.json", BRUCE_STORAGE_OPEN_READ, &file);

    bool ok = bruce_conf == BRUCE_ERR_PERMISSION && permissions_json == BRUCE_ERR_PERMISSION;
    printf(
        "[selftest] storage/protected-path: %s (bruce.conf=%d permissions.json=%d)\n",
        ok ? "OK" : "FAIL",
        bruce_conf,
        permissions_json
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_storage_roundtrip_case                                     */
/* ------------------------------------------------------------------------ */

bool selftest__run_storage_roundtrip_case(void) {
    static const char *const path = "/selftest_roundtrip.txt";
    static const char written_text[] = "hello bruce";

    bruce_file_id_t write_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t opened = storage__open(
        path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &write_file
    );
    size_t written = 0;
    bruce_result_t write_result =
        opened == BRUCE_OK ? storage__write(write_file, written_text, sizeof(written_text) - 1, &written)
                           : opened;
    bruce_result_t close_write_result = opened == BRUCE_OK ? storage__close(write_file) : opened;

    bruce_file_id_t read_file = BRUCE_FILE_ID_INVALID;
    bruce_result_t reopened = storage__open(path, BRUCE_STORAGE_OPEN_READ, &read_file);
    char read_buffer[32] = {0};
    size_t read_size = 0;
    bruce_result_t read_result =
        reopened == BRUCE_OK ? storage__read(read_file, read_buffer, sizeof(read_buffer) - 1, &read_size)
                             : reopened;

    uint64_t position = 0;
    bruce_result_t seek_result =
        reopened == BRUCE_OK ? storage__seek(read_file, 0, SEEK_SET, &position) : reopened;
    char reread_buffer[32] = {0};
    size_t reread_size = 0;
    bruce_result_t reread_result =
        reopened == BRUCE_OK
            ? storage__read(read_file, reread_buffer, sizeof(reread_buffer) - 1, &reread_size)
            : reopened;
    bruce_result_t close_read_result = reopened == BRUCE_OK ? storage__close(read_file) : reopened;

    storage__remove(path);

    bool ok = opened == BRUCE_OK && write_result == BRUCE_OK && written == sizeof(written_text) - 1 &&
              close_write_result == BRUCE_OK && reopened == BRUCE_OK && read_result == BRUCE_OK &&
              read_size == sizeof(written_text) - 1 && strcmp(read_buffer, written_text) == 0 &&
              seek_result == BRUCE_OK && position == 0 && reread_result == BRUCE_OK &&
              reread_size == sizeof(written_text) - 1 && strcmp(reread_buffer, written_text) == 0 &&
              close_read_result == BRUCE_OK;
    printf(
        "[selftest] storage/roundtrip: %s (write=%d read=%d text=\"%s\")\n",
        ok ? "OK" : "FAIL",
        write_result,
        read_result,
        read_buffer
    );
    return ok;
}

bool selftest__run_storage_mkdir_case(void) {
    static const char *const path = "/selftest_directory";
    (void)storage__remove(path);
    bruce_result_t created = storage__mkdir(path);
    bruce_result_t existing = storage__mkdir(path);
    bruce_result_t missing_parent = storage__mkdir("/selftest_missing/child");
    bruce_result_t protected_path = storage__mkdir("/config/bruce.conf");

    size_t count = 0;
    bruce_result_t listed = storage__list(path, NULL, 0, &count);
    bruce_result_t removed = storage__remove(path);
    bool ok = created == BRUCE_OK && existing == BRUCE_OK && missing_parent == BRUCE_ERR_NOT_FOUND &&
              protected_path == BRUCE_ERR_PERMISSION && listed == BRUCE_OK && removed == BRUCE_OK;
    printf(
        "[selftest] storage/mkdir: %s (create=%d existing=%d missing=%d protected=%d list=%d remove=%d)\n",
        ok ? "OK" : "FAIL",
        created,
        existing,
        missing_parent,
        protected_path,
        listed,
        removed
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_storage_ownership_case                                     */
/* ------------------------------------------------------------------------ */

static volatile bruce_file_id_t s_owner_file_id;
static volatile bool s_owner_should_exit;
static volatile bool s_owner_ready;

static int selftest__storage_owner_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t opened = storage__open(
        "/selftest_ownership.txt",
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    s_owner_file_id = opened == BRUCE_OK ? file : BRUCE_FILE_ID_INVALID;
    s_owner_ready = true;
    while (!s_owner_should_exit) { vTaskDelay(pdMS_TO_TICKS(10)); }
    if (opened == BRUCE_OK) storage__close(file);
    return 0;
}

typedef struct {
    volatile bruce_result_t result;
    volatile bool ran;
} selftest__storage_access_result_t;

static selftest__storage_access_result_t s_access_result;

static int selftest__storage_intruder_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char buffer[8];
    size_t out_size = 0;
    s_access_result.result = storage__read(s_owner_file_id, buffer, sizeof(buffer), &out_size);
    s_access_result.ran = true;
    return 0;
}

bool selftest__run_storage_ownership_case(void) {
    s_owner_file_id = BRUCE_FILE_ID_INVALID;
    s_owner_should_exit = false;
    s_owner_ready = false;
    memset(&s_access_result, 0, sizeof(s_access_result));

    process_create_params_t owner_params = {
        .name = "selftest_storage_owner",
        .entry = selftest__storage_owner_entry,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .permission_key = "",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t owner_id = BRUCE_PROCESS_ID_INVALID;
    bool owner_created = process_registry__create(&owner_params, &owner_id) == BRUCE_OK;

    for (int i = 0; i < 200 && !s_owner_ready; ++i) { vTaskDelay(pdMS_TO_TICKS(10)); }

    bool ok = false;
    if (owner_created && s_owner_ready && s_owner_file_id != BRUCE_FILE_ID_INVALID) {
        process_create_params_t intruder_params = {
            .name = "selftest_storage_intruder",
            .entry = selftest__storage_intruder_entry,
            .argc = 0,
            .argv = NULL,
            .built_in = true,
            .gui_requested = false,
            .permission_key = "",
            .start_in_background = true,
            .stack_bytes = 4096,
        };
        bruce_process_id_t intruder_id = BRUCE_PROCESS_ID_INVALID;
        bool intruder_created = process_registry__create(&intruder_params, &intruder_id) == BRUCE_OK;
        bruce_result_t wait_result = intruder_created ? process__wait(intruder_id, 2000) : BRUCE_ERR_INTERNAL;
        ok = intruder_created && (wait_result == BRUCE_OK || wait_result == BRUCE_ERR_NOT_FOUND) &&
             s_access_result.ran && s_access_result.result == BRUCE_ERR_PERMISSION;
    }

    s_owner_should_exit = true;
    if (owner_created) process__wait(owner_id, 2000);
    storage__remove("/selftest_ownership.txt");

    printf(
        "[selftest] storage/ownership: %s (owner_ready=%d access_result=%d)\n",
        ok ? "OK" : "FAIL",
        s_owner_ready,
        s_access_result.result
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_storage_no_leak_normal_exit_case                           */
/* ------------------------------------------------------------------------ */

static int selftest__storage_leak_open_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    /* Deliberately never closes: the point of the test is that Core closes
     * it automatically at process exit. */
    bruce_result_t opened = storage__open(
        "/selftest_leak.txt",
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    return opened == BRUCE_OK ? 0 : -1;
}

/* Repeatedly creates a process that opens (and never closes) a file, waiting
 * for each to finish before starting the next. Run more times than any
 * reasonable open-file table size; if handles leaked, the table would fill
 * up partway through and later opens would start failing. No printf here -
 * both no-leak cases below share this and print their own summary line. */
static bool selftest__storage_leak_iterations_ok(void) {
    bool ok = true;
    for (int i = 0; i < SELFTEST__STORAGE_LEAK_ITERATIONS && ok; ++i) {
        process_create_params_t params = {
            .name = "selftest_storage_leak",
            .entry = selftest__storage_leak_open_entry,
            .argc = 0,
            .argv = NULL,
            .built_in = true,
            .gui_requested = false,
            .permission_key = "",
            .start_in_background = true,
            .stack_bytes = 4096,
        };
        bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
        bool created = process_registry__create(&params, &id) == BRUCE_OK;
        bruce_result_t wait_result = created ? process__wait(id, 2000) : BRUCE_ERR_INTERNAL;
        ok = created && (wait_result == BRUCE_OK || wait_result == BRUCE_ERR_NOT_FOUND);
    }
    return ok;
}

bool selftest__run_storage_no_leak_normal_exit_case(void) {
    bool ok = selftest__storage_leak_iterations_ok();
    storage__remove("/selftest_leak.txt");
    printf(
        "[selftest] storage/no-leak-normal-exit: %s (%d iterations without exhausting the open-file table)\n",
        ok ? "OK" : "FAIL",
        SELFTEST__STORAGE_LEAK_ITERATIONS
    );
    return ok;
}

/* ------------------------------------------------------------------------ */
/* selftest__run_storage_no_leak_killed_case                                 */
/* ------------------------------------------------------------------------ */

static int selftest__storage_leak_block_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    storage__open(
        "/selftest_leak_killed.txt",
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &file
    );
    for (;;) { vTaskDelay(pdMS_TO_TICKS(50)); }
    return 0;
}

bool selftest__run_storage_no_leak_killed_case(void) {
    process_create_params_t params = {
        .name = "selftest_storage_leak_killed",
        .entry = selftest__storage_leak_block_entry,
        .argc = 0,
        .argv = NULL,
        .built_in = true,
        .gui_requested = false,
        .permission_key = "",
        .start_in_background = true,
        .stack_bytes = 4096,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    bool created = process_registry__create(&params, &id) == BRUCE_OK;
    vTaskDelay(pdMS_TO_TICKS(100));
    bruce_result_t killed = created ? process__kill(id) : BRUCE_ERR_INTERNAL;
    vTaskDelay(pdMS_TO_TICKS(100));

    /* If the killed process's handle leaked, the table would fill up and this
     * loop's opens would start failing with BRUCE_ERR_RESOURCE_LIMIT. */
    bool reopen_ok = selftest__storage_leak_iterations_ok();
    storage__remove("/selftest_leak.txt");

    storage__remove("/selftest_leak_killed.txt");

    bool ok = created && killed == BRUCE_OK && reopen_ok;
    printf(
        "[selftest] storage/no-leak-killed: %s (killed=%d reopen_ok=%d)\n",
        ok ? "OK" : "FAIL",
        killed,
        reopen_ok
    );
    return ok;
}
