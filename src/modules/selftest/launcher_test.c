#include "launcher_test.h"

#include <stdio.h>
#include <string.h>

#include "core/dialog/dialog.h"
#include "core/storage/storage.h"
#include "core_sdk/app_runner.h"
#include "core_sdk/dialog.h"
#include "core_sdk/result.h"
#include "core_sdk/task.h"
#include "fake_elf.h"
#include "modules/loaders/elf/elf_loader_app.h"

/* ------------------------------------------------------------------------ */
/* Launcher menu: drives the dialog__choice provider to select the /apps/    */
/* entry discovered by the launcher, then select Exit on the next loop.     */
/* ------------------------------------------------------------------------ */

static int s_launcher_test_call;
static const char *s_launcher_test_target_label;
static bool s_launcher_test_found;

static bruce_result_t selftest__launcher_choice_provider(const char *title, const char *message,
                                                         const bruce_dialog_choice_t *choices, size_t choice_count,
                                                         size_t *out_selected)
{
    (void)title;
    (void)message;

    if (s_launcher_test_call == 0) {
        for (size_t i = 0; i < choice_count; ++i) {
            if (strstr(choices[i].label, s_launcher_test_target_label) != NULL) {
                *out_selected = i;
                s_launcher_test_found = true;
                break;
            }
        }
        if (!s_launcher_test_found) {
            *out_selected = choice_count - 1; /* Exit */
        }
    } else {
        *out_selected = choice_count - 1; /* Exit */
    }

    s_launcher_test_call++;
    return BRUCE_OK;
}

bool selftest__run_launcher_apps_discovery_case(void)
{
    const char *path = "/apps/launcher_test_app.elf";
    storage__remove(path);

    if (!selftest__write_fake_elf(path, "Launcher Test App", NULL, 0)) {
        printf("[selftest] launcher/apps: could not create fake ELF\n");
        return false;
    }

    s_launcher_test_call = 0;
    s_launcher_test_target_label = "Launcher Test App";
    s_launcher_test_found = false;

    dialog__test_set_choice_provider(selftest__launcher_choice_provider);
    size_t calls_before = elf_loader__debug_call_count();
    int result = app_runner__run("bruce_launcher", "", true);
    bruce_result_t wait_result = result > 0 ? task__wait((bruce_task_id_t)result, 5000) : BRUCE_ERR_INVALID_ARGUMENT;
    dialog__test_set_choice_provider(NULL);

    storage__remove(path);

    bool ok = s_launcher_test_found && result > 0 && wait_result == BRUCE_OK &&
              elf_loader__debug_call_count() == calls_before + 1;
    if (!ok) {
        printf("[selftest] launcher/apps: found=%d result=%d wait=%d calls %zu -> %zu\n", s_launcher_test_found, result,
               wait_result, calls_before, elf_loader__debug_call_count());
        return false;
    }

    printf("[selftest] launcher/apps: OK\n");
    return true;
}
