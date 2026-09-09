#include <stdbool.h>
#include <string.h>

#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/storage.h"

/*
 * Fixture app for BruceOS's ELF loader selftests
 * (src/modules/selftest/elf_loader_test.c's elf_loader/xip and
 * elf_loader/xip_streaming_fallback cases) -- NOT a user-facing example.
 * Deliberately tiny, but exercises both relocation types the Xtensa
 * relocator (esp_elf_xtensa.c) supports against a real, compiled ELF
 * instead of the hand-built manifest-only fixture fake_elf.c produces:
 *
 *  - kMarker's address is a load-time constant baked into .text (Xtensa's
 *    literal-pool ABI keeps it there, not a separate .got) by the compiler
 *    as a vaddr placeholder, patched at relocation time via
 *    R_XTENSA_RELATIVE.
 *  - Calling runtime__delay(), a real Core SDK function resolved through
 *    the loader's symbol table, goes through R_XTENSA_JMP_SLOT.
 *
 * The selftest can't read this process's return value directly -- what
 * app_runner__run_path() hands back is the "elf" loader command's own
 * dispatch result, not this spawned child's eventual exit code (see
 * elf_loader_app.c's elf_loader__entry(), which is this process's actual
 * entry point and return-value target, running independently of that
 * dispatch call). So instead of an exit code, this writes its own verdict
 * to a well-known file: proof the child really ran app_main() to
 * completion with both relocation types resolved correctly, not just that
 * something got spawned. A corrupted marker byte or a crash/hang calling
 * through a bad function pointer -- exactly what a wrong relocation would
 * cause -- leaves that file missing or wrong instead of holding kResultOk.
 *
 * See native_apps/README.md for how to rebuild this after editing:
 *   python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app loader_selftest
 * then run src/modules/selftest/tool/gen_elf_loader_xip_fixture.py to
 * refresh the committed byte array (see that script's own header comment).
 */

static const char kMarker[] = "BRUCE_ELF_XIP_SELFTEST_OK";

#define LOADER_SELFTEST_RESULT_PATH "/selftest_elf_loader_xip_result.txt"
#define LOADER_SELFTEST_RESULT_OK "OK"
#define LOADER_SELFTEST_RESULT_FAIL "FAIL"

static void loader_selftest__write_result(const char *text) {
    bruce_file_id_t file;
    if (storage__open(
            LOADER_SELFTEST_RESULT_PATH,
            BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file
        ) != BRUCE_OK) {
        return;
    }
    size_t written;
    (void)storage__write(file, text, strlen(text), &written);
    (void)storage__close(file);
}

int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    volatile const char *marker = kMarker;
    bool marker_ok = marker[0] == 'B' && marker[sizeof(kMarker) - 2] == 'K';

    bool call_ok = runtime__delay(0) == BRUCE_OK;

    loader_selftest__write_result((marker_ok && call_ok) ? LOADER_SELFTEST_RESULT_OK : LOADER_SELFTEST_RESULT_FAIL);
    return 0;
}
