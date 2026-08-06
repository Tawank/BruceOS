#include "serial_commands_app.h"

#include "core_sdk/app_runner.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "modules/shell/shell_console.h"

#define SERIAL_COMMANDS__LINE_MAX 256
#define SERIAL_COMMANDS__WAIT_INTERVAL_MS 10

int serial_commands__run_line(const char *line, bool in_background) {
    return app_runner__run_command(line, in_background ? BRUCE_LAUNCH_BACKGROUND : BRUCE_LAUNCH_FOREGROUND);
}

int serial_commands_app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

#if !CONFIG_BRUCE_QEMU_TEST_MODE
    /* Wait for a first byte before spawning the interactive shell, instead
     * of launching "shell -i" unconditionally at boot. That process runs
     * for the device's entire uptime and immediately prints its prompt to
     * the serial monitor, whether or not anyone ever types into it -- this
     * way the shell (and its prompt) don't appear until someone actually
     * presses a key. The triggering byte itself is only a wake-up signal;
     * it's discarded rather than fed to the shell, the same way
     * dialog__gui_wait_for_any_key() discards its own wake press. Uses the
     * same indefinite-timeout stdio__read() pattern already relied on by
     * shell_console__read_byte(UINT32_MAX) for the shell's own read loop.
     * Skipped under CONFIG_BRUCE_QEMU_TEST_MODE: nothing types over the
     * QEMU serial link, and main.c's serial_commands__wait_ready() call
     * expects the shell to already be starting up right after boot. */
    char woke_byte;
    size_t woke_size = 0;
    if (stdio__read(&woke_byte, 1, UINT32_MAX, &woke_size) != BRUCE_OK) { return BRUCE_ERR_CANCELLED; }
    (void)woke_byte;
#endif

    shell_console__reset_ready();
    int result = app_runner__run("shell", "-i", BRUCE_LAUNCH_BACKGROUND);
    if (result < 0) return result;
    while (process__wait((bruce_process_id_t)result, 100) == BRUCE_ERR_TIMEOUT) {
        if (runtime__delay(10) != BRUCE_OK) return BRUCE_ERR_CANCELLED;
    }
    return 0;
}

bool serial_commands__wait_ready(uint32_t timeout_ms) {
    uint64_t started = runtime__now();
    while (!shell_console__is_ready() && runtime__now() - started < timeout_ms) {
        if (runtime__delay(SERIAL_COMMANDS__WAIT_INTERVAL_MS) != BRUCE_OK) return false;
    }
    return shell_console__is_ready();
}
