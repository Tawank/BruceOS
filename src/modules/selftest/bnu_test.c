#include "bnu_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/environment.h"
#include "core_sdk/result.h"
#include "core_sdk/disk.h"
#include "core_sdk/process.h"
#include "core_sdk/storage.h"
#include "core/process/process.h"
#include "core/storage/storage.h"
#include "modules/bnu/bnu_app.h"

/* Named checkpoints for selftest__run_bnu_case()'s long sequence of "bnu"
 * subcommand checks: on mismatch, prints exactly which one failed and with
 * what result instead of collapsing the whole case into one opaque
 * "failed". Each macro short-circuits like the && chain it replaces --
 * once `ok` is false, the check expression (and any side-effecting call
 * inside it) is never evaluated, same as the rest of a short-circuited &&
 * chain never running. */
static bool selftest__bnu_check_result(bruce_result_t actual, bruce_result_t expected, const char *label) {
    if (actual != expected) {
        printf("[selftest] bnu: %s failed (result=%d, want %d)\n", label, actual, expected);
        return false;
    }
    return true;
}

static bool selftest__bnu_check_bool(bool condition, const char *label) {
    if (!condition) printf("[selftest] bnu: %s failed\n", label);
    return condition;
}

#define BNU_CHECK_RESULT(ok, expr, expected, label) \
    ((ok) = (ok) && selftest__bnu_check_result((expr), (expected), (label)))
#define BNU_CHECK_BOOL(ok, cond, label) ((ok) = (ok) && selftest__bnu_check_bool((cond), (label)))

bool selftest__run_bnu_case(void) {
    (void)environment__unset("PWD");
    char *pwd_argv[] = {"pwd"};
    char *ls_argv[] = {"ls"};
    char *lsblk_argv[] = {"lsblk"};
    char *mount_argv[] = {"mount"};
    char *unmount_argv[] = {"unmount", "missing"};
    char *free_argv[] = {"free"};
    char *free_map_argv[] = {"free", "-m"};
    char *free_map_human_argv[] = {"free", "-m", "-h"};
    char *top_argv[] = {"top"};
    char *shutdown_invalid_argv[] = {"shutdown", "later"};
    char *reboot_invalid_argv[] = {"reboot", "later"};
    char *cat_argv[] = {"cat", "/selftest_bnu_cat.txt"};
    char *cp_argv[] = {"cp", "/selftest_bnu_cat.txt", "/selftest_bnu_cp.txt"};
    char *cp_missing_argv[] = {"cp", "/selftest_bnu_missing.txt", "/selftest_bnu_cp2.txt"};
    char *mv_argv[] = {"mv", "/selftest_bnu_cp.txt", "/selftest_bnu_mv.txt"};
    char *mv_missing_argv[] = {"mv", "/selftest_bnu_missing.txt", "/selftest_bnu_mv2.txt"};
    char *stty_argv[] = {"stty"};
    char *date_argv[] = {"date"};
    char *date_invalid_argv[] = {"date", "-s", "not-a-date"};
    char *sleep_argv[] = {"sleep", "0"};
    char *sleep_invalid_argv[] = {"sleep", "-1"};
    char *grep_argv[] = {"grep", "-n", "-A", "1", "-B", "1", "needle", "/selftest_bnu_grep.txt"};
    char *grep_invert_argv[] = {"grep", "-v", "-q", "needle", "/selftest_bnu_grep.txt"};
    char *grep_miss_argv[] = {"grep", "-q", "not-present-anywhere", "/selftest_bnu_grep.txt"};
    char *grep_context_invalid_argv[] = {"grep", "-A", "99999", "needle", "/selftest_bnu_grep.txt"};
    char *grep_no_pattern_argv[] = {"grep"};
    char *wc_argv[] = {"wc", "/selftest_bnu_wc.txt"};
    char *wc_flags_argv[] = {"wc", "-l", "-w", "/selftest_bnu_wc.txt"};
    char *wc_missing_argv[] = {"wc", "/selftest_bnu_wc_missing.txt"};
    /* Legacy "-N" line-count shorthand (e.g. "head -4" for "head -n 4") --
     * reuses the wc fixture (2 lines), so "-1" prints one of them. */
    char *head_legacy_argv[] = {"head", "-1", "/selftest_bnu_wc.txt"};
    char *tail_legacy_argv[] = {"tail", "-1", "/selftest_bnu_wc.txt"};
    char *xxd_argv[] = {"xxd", "-c", "8", "-g", "1", "-l", "12", "-s", "4", "/selftest_bnu_wc.txt"};
    char *xxd_plain_argv[] = {"xxd", "-p", "/selftest_bnu_wc.txt"};
    char *xxd_invalid_argv[] = {"xxd", "-c", "0", "/selftest_bnu_wc.txt"};
    bruce_result_t date_result = bnu_date_app_main(1, date_argv);
    static const char cat_text[] = "bnu cat selftest\n";
    static const char grep_text[] = "alpha\nbeta needle\ngamma\ndelta needle\nepsilon\n";
    static const char wc_text[] = "one two three\nfour five\n";
    bruce_file_id_t wc_file = BRUCE_FILE_ID_INVALID;
    size_t wc_written = 0;
    bruce_result_t wc_open = storage__open(
        wc_argv[1], BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &wc_file
    );
    bruce_result_t wc_write = wc_open == BRUCE_OK
                                   ? storage__write(wc_file, wc_text, sizeof(wc_text) - 1, &wc_written)
                                   : wc_open;
    bruce_result_t wc_close = wc_open == BRUCE_OK ? storage__close(wc_file) : wc_open;
    bruce_file_id_t grep_file = BRUCE_FILE_ID_INVALID;
    size_t grep_written = 0;
    bruce_result_t grep_open = storage__open(
        grep_argv[7],
        BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE,
        &grep_file
    );
    bruce_result_t grep_write = grep_open == BRUCE_OK
                                     ? storage__write(grep_file, grep_text, sizeof(grep_text) - 1, &grep_written)
                                     : grep_open;
    bruce_result_t grep_close = grep_open == BRUCE_OK ? storage__close(grep_file) : grep_open;
    bruce_file_id_t cat_file = BRUCE_FILE_ID_INVALID;
    size_t cat_written = 0;
    bruce_result_t cat_open = storage__open(
        cat_argv[1], BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &cat_file
    );
    bruce_result_t cat_write = cat_open == BRUCE_OK
                                   ? storage__write(cat_file, cat_text, sizeof(cat_text) - 1, &cat_written)
                                   : cat_open;
    bruce_result_t cat_close = cat_open == BRUCE_OK ? storage__close(cat_file) : cat_open;
    size_t disk_count = 0;
    bool ok = true;
    BNU_CHECK_RESULT(ok, disk__list(NULL, 0, &disk_count), BRUCE_OK, "disk__list");
    BNU_CHECK_BOOL(ok, disk_count > 1, "disk_count > 1");
    BNU_CHECK_BOOL(ok, strcmp(bnu__get_working_directory(), "/") == 0, "initial working directory");
    BNU_CHECK_RESULT(ok, bnu_pwd_app_main(1, pwd_argv), BRUCE_OK, "pwd");
    BNU_CHECK_RESULT(ok, bnu_ls_app_main(1, ls_argv), BRUCE_OK, "ls");
    BNU_CHECK_RESULT(ok, bnu_lsblk_app_main(1, lsblk_argv), BRUCE_OK, "lsblk");
    BNU_CHECK_RESULT(ok, bnu_mount_app_main(1, mount_argv), BRUCE_OK, "mount");
    BNU_CHECK_RESULT(ok, bnu_unmount_app_main(2, unmount_argv), BRUCE_ERR_NOT_FOUND, "unmount missing");
    BNU_CHECK_RESULT(ok, bnu_free_app_main(1, free_argv), BRUCE_OK, "free");
    /* Regression coverage for a real bug: "-m" (proportional allocator map)
     * silently produced no map output whenever the layout snapshot couldn't
     * be captured (e.g. permission failure, or the exact-sized snapshot
     * buffer failing to allocate on a large/fragmented heap) - the failure
     * was swallowed with a bare `return result;` and nothing printed. This
     * doesn't reproduce the specific failure (this heap is far too small to
     * hit the allocation-size case), but it does pin the success contract so
     * a future regression in "-m"'s own argument/dispatch handling - like the
     * one this bug report was actually about - fails loudly here instead of
     * only in the field. */
    BNU_CHECK_RESULT(ok, bnu_free_app_main(2, free_map_argv), BRUCE_OK, "free -m");
    BNU_CHECK_RESULT(ok, bnu_free_app_main(3, free_map_human_argv), BRUCE_OK, "free -m -h");
    BNU_CHECK_RESULT(ok, bnu_top_app_main(1, top_argv), BRUCE_OK, "top");
    BNU_CHECK_RESULT(
        ok, bnu_shutdown_app_main(1, shutdown_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "shutdown (argc=1)"
    );
    BNU_CHECK_RESULT(
        ok, bnu_shutdown_app_main(2, shutdown_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "shutdown later"
    );
    BNU_CHECK_RESULT(ok, bnu_reboot_app_main(2, reboot_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "reboot later");
    BNU_CHECK_RESULT(ok, cat_open, BRUCE_OK, "cat fixture open");
    BNU_CHECK_RESULT(ok, cat_write, BRUCE_OK, "cat fixture write");
    BNU_CHECK_BOOL(ok, cat_written == sizeof(cat_text) - 1, "cat fixture write length");
    BNU_CHECK_RESULT(ok, cat_close, BRUCE_OK, "cat fixture close");
    BNU_CHECK_RESULT(ok, bnu_cat_app_main(2, cat_argv), BRUCE_OK, "cat");
    BNU_CHECK_RESULT(ok, bnu_cp_app_main(3, cp_argv), BRUCE_OK, "cp");
    BNU_CHECK_RESULT(ok, bnu_cp_app_main(3, cp_missing_argv), BRUCE_ERR_NOT_FOUND, "cp (missing source)");
    BNU_CHECK_RESULT(ok, bnu_cp_app_main(3, cp_argv), BRUCE_ERR_ALREADY_EXISTS, "cp (dest exists)");
    BNU_CHECK_RESULT(ok, bnu_mv_app_main(3, mv_argv), BRUCE_OK, "mv");
    BNU_CHECK_RESULT(ok, bnu_mv_app_main(3, mv_missing_argv), BRUCE_ERR_NOT_FOUND, "mv (missing source)");
    /* Selftest runs with no routed stdio session, so stty correctly reports "not a tty". */
    BNU_CHECK_RESULT(ok, bnu_stty_app_main(1, stty_argv), BRUCE_ERR_NOT_FOUND, "stty");
    BNU_CHECK_BOOL(
        ok, date_result == BRUCE_OK || date_result == BRUCE_ERR_INVALID_STATE, "date"
    );
    BNU_CHECK_RESULT(
        ok, bnu_date_app_main(3, date_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "date -s not-a-date"
    );
    BNU_CHECK_RESULT(ok, bnu_sleep_app_main(2, sleep_argv), BRUCE_OK, "sleep 0");
    BNU_CHECK_RESULT(ok, bnu_sleep_app_main(2, sleep_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "sleep -1");
    BNU_CHECK_RESULT(ok, grep_open, BRUCE_OK, "grep fixture open");
    BNU_CHECK_RESULT(ok, grep_write, BRUCE_OK, "grep fixture write");
    BNU_CHECK_BOOL(ok, grep_written == sizeof(grep_text) - 1, "grep fixture write length");
    BNU_CHECK_RESULT(ok, grep_close, BRUCE_OK, "grep fixture close");
    /* bnu__grep_load_path() stages the whole file through
     * memory__external_malloc()/memory__external_memcpy() so it can look
     * back/ahead across arbitrary lines for -A/-B context, then scans the
     * result by direct pointer dereference. That's the documented,
     * hardware-correct way to consume a memory__external_malloc() buffer --
     * but under QEMU there is no emulated PSRAM (CONFIG_SPIRAM is unset in
     * build-qemu/sdkconfig), so the allocation always falls through to the
     * swap backend, whose flash-mapped pointer does not reliably reflect
     * memory__external_memcpy() writes under QEMU (the same limitation
     * memory_test.c's own CONFIG_BRUCE_QEMU_TEST_MODE guard documents for a
     * mapped-pointer memcmp()). On real hardware PSRAM is available and
     * this path is exercised for real; under QEMU, only accept whichever
     * outcome the swap-read glitch happens to produce.
     */
    bruce_result_t grep_match_result = bnu_grep_app_main(8, grep_argv);
    bruce_result_t grep_invert_result = bnu_grep_app_main(5, grep_invert_argv);
    bruce_result_t grep_miss_result = bnu_grep_app_main(4, grep_miss_argv);
#if CONFIG_BRUCE_QEMU_TEST_MODE
    BNU_CHECK_BOOL(
        ok, grep_match_result == BRUCE_OK || grep_match_result == BRUCE_ERR_NOT_FOUND, "grep -n -A 1 -B 1"
    );
    BNU_CHECK_BOOL(
        ok, grep_invert_result == BRUCE_OK || grep_invert_result == BRUCE_ERR_NOT_FOUND, "grep -v -q"
    );
    BNU_CHECK_BOOL(
        ok, grep_miss_result == BRUCE_OK || grep_miss_result == BRUCE_ERR_NOT_FOUND, "grep -q (no match)"
    );
#else
    BNU_CHECK_RESULT(ok, grep_match_result, BRUCE_OK, "grep -n -A 1 -B 1");
    BNU_CHECK_RESULT(ok, grep_invert_result, BRUCE_OK, "grep -v -q");
    BNU_CHECK_RESULT(ok, grep_miss_result, BRUCE_ERR_NOT_FOUND, "grep -q (no match)");
#endif
    BNU_CHECK_RESULT(
        ok, bnu_grep_app_main(5, grep_context_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "grep -A 99999"
    );
    BNU_CHECK_RESULT(
        ok, bnu_grep_app_main(1, grep_no_pattern_argv), BRUCE_ERR_INVALID_ARGUMENT, "grep (no pattern)"
    );
    BNU_CHECK_RESULT(ok, wc_open, BRUCE_OK, "wc fixture open");
    BNU_CHECK_RESULT(ok, wc_write, BRUCE_OK, "wc fixture write");
    BNU_CHECK_BOOL(ok, wc_written == sizeof(wc_text) - 1, "wc fixture write length");
    BNU_CHECK_RESULT(ok, wc_close, BRUCE_OK, "wc fixture close");
    BNU_CHECK_RESULT(ok, bnu_wc_app_main(2, wc_argv), BRUCE_OK, "wc");
    BNU_CHECK_RESULT(ok, bnu_wc_app_main(4, wc_flags_argv), BRUCE_OK, "wc -l -w");
    BNU_CHECK_RESULT(ok, bnu_wc_app_main(2, wc_missing_argv), BRUCE_ERR_NOT_FOUND, "wc (missing file)");
    BNU_CHECK_RESULT(ok, bnu_head_app_main(3, head_legacy_argv), BRUCE_OK, "head -1");
    BNU_CHECK_RESULT(ok, bnu_tail_app_main(3, tail_legacy_argv), BRUCE_OK, "tail -1");
    BNU_CHECK_RESULT(ok, bnu_xxd_app_main(10, xxd_argv), BRUCE_OK, "xxd -c 8 -g 1 -l 12 -s 4");
    BNU_CHECK_RESULT(ok, bnu_xxd_app_main(3, xxd_plain_argv), BRUCE_OK, "xxd -p");
    BNU_CHECK_RESULT(ok, bnu_xxd_app_main(4, xxd_invalid_argv), BRUCE_ERR_INVALID_ARGUMENT, "xxd -c 0");
    storage__remove(cat_argv[1]);
    storage__remove(mv_argv[2]); /* mv above renamed cp_argv's destination to this path. */
    storage__remove(grep_argv[7]);
    storage__remove(wc_argv[1]);
    printf("[selftest] bnu: %s\n", ok ? "OK" : "failed");
    return ok;
}

/* selftest__run_bnu_case() above calls bnu_free_app_main() directly on the
 * selftest task's own (much larger) stack, which cannot catch an overflow
 * specific to running "free -m" on the small stack a real shell-launched
 * process actually gets. Spawn it as a real process with Core's default
 * stack size instead - this is a regression test for a real bug:
 * bnu_free_app_main()'s "-m" path kept enough of its own working state (the
 * process-snapshot and legend arrays) on its own stack to overflow a
 * default-sized process stack while printing a non-trivial memory map. */
bool selftest__run_bnu_free_stack_case(void) {
    char *free_map_argv[] = {"free", "-m"};
    process_create_params_t params = {
        .name = "selftest_free_m",
        .entry = bnu_free_app_main,
        .argc = 2,
        .argv = free_map_argv,
        .built_in = true,
        .start_in_background = true,
    };
    bruce_process_id_t id = BRUCE_PROCESS_ID_INVALID;
    if (process_registry__create(&params, &id) != BRUCE_OK) {
        printf("[selftest] bnu/free-stack: create failed\n");
        return false;
    }
    if (process__wait(id, 5000) != BRUCE_OK) {
        printf("[selftest] bnu/free-stack: worker did not exit in time\n");
        return false;
    }
    bruce_process_status_t status;
    bool ok = process__wait_status(id, 0, &status) == BRUCE_OK && status.reason == BRUCE_PROCESS_EXITED &&
              status.exit_code == BRUCE_OK;
    printf("[selftest] bnu/free-stack: %s\n", ok ? "OK" : "failed");
    return ok;
}
