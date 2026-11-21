#include "appdata_test.h"

#include <stdio.h>
#include <string.h>

#include "core/appdata/appdata.h"
#include "core/dialog/dialog.h"
#include "core/permission/permission.h"
#include "core/process/process.h"
#include "core/storage/storage.h"
#include "core_sdk/process.h"
#include "core_sdk/storage.h"

#define SELFTEST__APPDATA_APP "selftest_appdata.elf"
#define SELFTEST__APPDATA_FOLDER "/appdata/selftest_appdata"

static bruce_result_t selftest__appdata_deny(
    const char *title, const char *message, const bruce_dialog_choice_t *choices, size_t choice_count,
    size_t *out_selected
) {
    (void)title;
    (void)message;
    (void)choices;
    (void)choice_count;
    *out_selected = 1;
    return BRUCE_OK;
}

static struct {
    bool ran;
    char path[BRUCE_STORAGE_PATH_MAX];
    bruce_result_t write, read, absolute, escape, list, nested;
    char data[16];
    size_t size, usage, count;
} s_appdata;

static int selftest__appdata_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    appdata__get_path(s_appdata.path, sizeof(s_appdata.path));
    s_appdata.write = appdata__write("notes.txt", "hello", 5);
    s_appdata.read = appdata__read("notes.txt", s_appdata.data, sizeof(s_appdata.data), &s_appdata.size);
    appdata__get_usage(&s_appdata.usage);
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    s_appdata.absolute = storage__open("/selftest_appdata.txt", BRUCE_STORAGE_OPEN_READ, &file);
    s_appdata.escape = storage__open("../other/notes.txt", BRUCE_STORAGE_OPEN_READ, &file);
    s_appdata.nested = storage__mkdir("cache");
    s_appdata.list = storage__list("", NULL, 0, &s_appdata.count);
    s_appdata.ran = true;
    return 0;
}

static bool selftest__appdata_run_app(void) {
    memset(&s_appdata, 0, sizeof(s_appdata));
    process_create_params_t params = {
        .name = "selftest_appdata",
        .entry = selftest__appdata_entry,
        .permission_key = SELFTEST__APPDATA_APP,
        .start_in_background = true,
        .stack_bytes = 6144,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) return false;
    bruce_result_t waited = process__wait(id, 5000);
    return (waited == BRUCE_OK || waited == BRUCE_ERR_NOT_FOUND) && s_appdata.ran;
}

bool selftest__run_appdata_sandbox_case(void) {
    permission__test_reset();
    storage__tree_remove_internal(SELFTEST__APPDATA_FOLDER);
    dialog__test_set_choice_provider(selftest__appdata_deny);
    bool ran = selftest__appdata_run_app();
    dialog__test_set_choice_provider(NULL);

    char saved[96] = "";
    permission__get_appdata(SELFTEST__APPDATA_APP, saved, sizeof(saved));
    bool ok = ran && strcmp(s_appdata.path, SELFTEST__APPDATA_FOLDER) == 0 && s_appdata.write == BRUCE_OK &&
              s_appdata.read == BRUCE_OK && s_appdata.size == 5 && memcmp(s_appdata.data, "hello", 5) == 0 &&
              s_appdata.usage == 5 && s_appdata.absolute == BRUCE_ERR_PERMISSION &&
              s_appdata.escape == BRUCE_ERR_INVALID_PATH && s_appdata.nested == BRUCE_OK &&
              s_appdata.list == BRUCE_OK && s_appdata.count == 2 &&
              strcmp(saved, SELFTEST__APPDATA_FOLDER) == 0 &&
              storage__exists_internal(SELFTEST__APPDATA_FOLDER "/notes.txt");
    printf(
        "[selftest] appdata/sandbox: %s (path=%s write=%d read=%d size=%u usage=%u abs=%d esc=%d "
        "list=%d/%u)\n",
        ok ? "OK" : "FAIL",
        s_appdata.path,
        s_appdata.write,
        s_appdata.read,
        (unsigned)s_appdata.size,
        (unsigned)s_appdata.usage,
        s_appdata.absolute,
        s_appdata.escape,
        s_appdata.list,
        (unsigned)s_appdata.count
    );
    return ok;
}

bool selftest__run_appdata_manage_case(void) {
    permission__test_reset();
    bool setup = storage__mkdir_internal("/appdata") && storage__mkdir_internal(SELFTEST__APPDATA_FOLDER) &&
                 storage__write_file_atomic(SELFTEST__APPDATA_FOLDER "/notes.txt", "hello", 5) &&
                 permission__set_appdata(SELFTEST__APPDATA_APP, SELFTEST__APPDATA_FOLDER) == BRUCE_OK;
    size_t usage = 0;
    bruce_result_t usage_result = appdata__app_usage(SELFTEST__APPDATA_APP, &usage);
    bool sd = storage__sd_is_ready();
    bruce_result_t move = appdata__move(SELFTEST__APPDATA_APP, true);
    bool on_sd = appdata__app_on_sd(SELFTEST__APPDATA_APP);
    bruce_result_t back = appdata__move(SELFTEST__APPDATA_APP, false);
    bool data_back = storage__exists_internal(SELFTEST__APPDATA_FOLDER "/notes.txt");
    bruce_result_t clear = appdata__clear(SELFTEST__APPDATA_APP);
    bool cleared = !storage__exists_internal(SELFTEST__APPDATA_FOLDER);
    permission__test_reset();

    bool moved_ok = sd ? move == BRUCE_OK && on_sd : move == BRUCE_ERR_INVALID_STATE && !on_sd;
    bool ok = setup && usage_result == BRUCE_OK && usage == 5 && moved_ok && back == BRUCE_OK && data_back &&
              clear == BRUCE_OK && cleared;
    printf(
        "[selftest] appdata/manage: %s (usage=%d/%u move=%d back=%d clear=%d)\n",
        ok ? "OK" : "FAIL",
        usage_result,
        (unsigned)usage,
        move,
        back,
        clear
    );
    return ok;
}
