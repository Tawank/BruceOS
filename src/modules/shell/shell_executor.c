#include "shell_executor.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"
#include "shell_builtins.h"

/* Initial size, and doubling step, for the external-memory buffer pipes
 * capture a producer's output into (see shell_executor__buffer_append()). */
#define SHELL_PIPE_CHUNK (512u)

static const char *shell_executor__lookup(void *context, const char *name) {
    return shell_builtins__get((const shell_state_t *)context, name);
}

typedef struct {
    bruce_environment_variable_t *items;
    size_t count;
    size_t capacity;
} shell_executor__environment_t;

static void shell_executor__environment_free(shell_executor__environment_t *environment) {
    if (environment == NULL) return;
    memory__free(environment->items);
    environment->items = NULL;
    environment->count = 0;
    environment->capacity = 0;
}

static bool shell_executor__environment_push(
    shell_executor__environment_t *environment, const bruce_environment_variable_t *variable
) {
    if (environment->count >= SHELL__MAX_VARIABLES) return false;
    if (environment->count >= environment->capacity) {
        size_t new_capacity = environment->capacity == 0 ? 4u : environment->capacity * 2u;
        if (new_capacity > SHELL__MAX_VARIABLES) new_capacity = SHELL__MAX_VARIABLES;
        bruce_environment_variable_t *grown =
            memory__realloc(environment->items, new_capacity * sizeof(*grown));
        if (grown == NULL) return false;
        environment->items = grown;
        environment->capacity = new_capacity;
    }
    environment->items[environment->count++] = *variable;
    return true;
}

static bool shell_executor__append_arg(char *out, size_t capacity, size_t *used, const char *arg) {
    size_t needed = *used > 0 ? 1u : 0u;
    needed += 2;
    for (const char *p = arg; *p != '\0'; ++p) needed += (*p == '\\' || *p == '"') ? 2u : 1u;
    if (*used + needed >= capacity) return false;
    if (*used > 0) out[(*used)++] = ' ';
    out[(*used)++] = '"';
    for (const char *p = arg; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '"') out[(*used)++] = '\\';
        out[(*used)++] = *p;
    }
    out[(*used)++] = '"';
    out[*used] = '\0';
    return true;
}

static int shell_executor__wait(bruce_process_id_t child) {
    bruce_process_status_t status;
    for (;;) {
        bruce_result_t waited = process__wait_status(child, 100, &status);
        if (waited == BRUCE_OK) break;
        if (waited == BRUCE_ERR_TIMEOUT) continue;
        if (waited == BRUCE_ERR_CANCELLED) {
            bruce_process_signal_t signal = process__current_signal();
            if (signal != BRUCE_PROCESS_SIGNAL_INT && signal != BRUCE_PROCESS_SIGNAL_TERM) {
                signal = BRUCE_PROCESS_SIGNAL_TERM;
            }
            (void)process__signal(child, signal);
            if (process__wait_status(child, 500, &status) != BRUCE_OK) {
                (void)process__kill(child);
                (void)process__wait_status(child, 500, &status);
            }
            return 128 + (int)signal;
        }
        return 1;
    }
    if (status.reason == BRUCE_PROCESS_TERMINATED || status.reason == BRUCE_PROCESS_KILLED) {
        return 128 + (int)status.signal;
    }
    if (status.exit_code < 0) return 1;
    return status.exit_code & 0xff;
}

static int shell_executor__launch_external(
    int argc, char **argv, const char *prefix, const bruce_environment_variable_t *environment,
    size_t environment_count, bruce_launch_mode_t mode
) {
    size_t capacity = SHELL__LINE_MAX * 2;
    char *arguments = memory__malloc(capacity);
    if (arguments == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    size_t used = 0;
    arguments[0] = '\0';
    if (prefix != NULL) {
        size_t prefix_length = strlen(prefix);
        if (prefix_length >= capacity) {
            memory__free(arguments);
            return BRUCE_ERR_RESOURCE_LIMIT;
        }
        memcpy(arguments, prefix, prefix_length + 1u);
        used = prefix_length;
    }
    for (int i = 1; i < argc; ++i) {
        if (!shell_executor__append_arg(arguments, capacity, &used, argv[i])) {
            stdio__printf("shell: arguments too long\n");
            memory__free(arguments);
            return 2;
        }
    }
    int launched;
    if (argv[0][0] == '/' || strncmp(argv[0], "./", 2) == 0) {
        launched = app_runner__run_path_with_environment(
            argv[0], used > 0 ? arguments : NULL, mode, environment, environment_count
        );
    } else {
        launched = app_runner__run_with_environment(
            argv[0], used > 0 ? arguments : NULL, mode, environment, environment_count
        );
    }
    memory__free(arguments);
    return launched;
}

static int shell_executor__external(
    int argc, char **argv, const bruce_environment_variable_t *environment, size_t environment_count,
    bruce_launch_mode_t mode
) {
    int launched = shell_executor__launch_external(argc, argv, NULL, environment, environment_count, mode);
    if (launched == BRUCE_ERR_NOT_FOUND || launched == BRUCE_ERR_INVALID_PATH) {
        stdio__printf("shell: %s: not found\n", argv[0]);
        return 127;
    }
    if (launched <= 0) {
        stdio__printf("shell: %s: launch failed (%d)\n", argv[0], launched);
        return 1;
    }
    return shell_executor__wait((bruce_process_id_t)launched);
}

typedef struct {
    bruce_memory_object_t object;
    size_t length;
} shell_executor__buffer_t;

static void shell_executor__buffer_free(shell_executor__buffer_t *buffer) {
    if (buffer->object.backend != BRUCE_MEMORY_BACKEND_INVALID) (void)memory__external_free(&buffer->object);
    buffer->length = 0;
}

/* Appends to an external-memory-backed buffer, growing it by doubling: each
 * growth step allocates a new, bigger memory__external object and copies the
 * old bytes across, since external objects can't be resized in place (same
 * approach as http__external_body_reserve() in core/http/http.c). Routing
 * pipe data through PSRAM/swap this way -- rather than a memory__malloc()
 * buffer capped well below it -- means a captured command's output isn't
 * bounded by the internal heap's largest free block, which on a board
 * without PSRAM or a committed swap partition can be as little as ~100 KiB
 * of fragmented internal RAM shared with everything else. This only fails
 * once the backing store itself (PSRAM, swap, or as a last-resort plain
 * internal RAM -- see memory__external_alloc()) is actually exhausted. */
static bool shell_executor__buffer_append(shell_executor__buffer_t *buffer, const void *data, size_t size) {
    if (size == 0) return true;
    size_t capacity = buffer->object.backend != BRUCE_MEMORY_BACKEND_INVALID ? buffer->object.size : 0;
    size_t required = buffer->length + size;
    if (required < buffer->length) return false;
    if (required > capacity) {
        size_t new_capacity = capacity == 0 ? SHELL_PIPE_CHUNK : capacity;
        while (new_capacity < required) {
            size_t doubled = new_capacity * 2u;
            if (doubled < new_capacity) return false;
            new_capacity = doubled;
        }
        bruce_memory_object_t grown;
        if (memory__external_alloc(new_capacity, &grown) != BRUCE_OK) return false;
        if (buffer->length > 0) {
            const void *old_data = NULL;
            bruce_result_t result = memory__external_map(&buffer->object, &old_data);
            if (result == BRUCE_OK) result = memory__external_write(&grown, 0, old_data, buffer->length);
            if (result != BRUCE_OK) {
                (void)memory__external_free(&grown);
                return false;
            }
        }
        if (buffer->object.backend != BRUCE_MEMORY_BACKEND_INVALID)
            (void)memory__external_free(&buffer->object);
        buffer->object = grown;
    }
    if (memory__external_write(&buffer->object, buffer->length, data, size) != BRUCE_OK) return false;
    buffer->length += size;
    return true;
}

static int shell_executor__capture_external(int argc, char **argv, shell_executor__buffer_t *out_buffer) {
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) return 1;
    if (stdio__session_route_children(session) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return 1;
    }
    int launched = shell_executor__launch_external(argc, argv, NULL, NULL, 0, BRUCE_LAUNCH_FOREGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        return 1;
    }

    shell_executor__buffer_t buffer = {0};
    bruce_process_status_t status = {0};
    bool complete = false;
    bool out_of_memory = false;
    while (!complete && !out_of_memory) {
        char chunk[256];
        size_t size = 0;
        while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
            if (!shell_executor__buffer_append(&buffer, chunk, size)) {
                out_of_memory = true;
                break;
            }
        }
        if (out_of_memory) break;
        bruce_result_t waited = process__wait_status((bruce_process_id_t)launched, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!complete) (void)runtime__delay(1);
    }
    if (out_of_memory) {
        (void)process__kill((bruce_process_id_t)launched);
        (void)process__wait_status((bruce_process_id_t)launched, 500, &status);
        shell_executor__buffer_free(&buffer);
        (void)stdio__session_close(session);
        stdio__printf("shell: pipe: out of memory buffering output\n");
        return 1;
    }
    /* The producer may have written more output between the last read above
     * and it actually exiting; drain whatever is left before closing. */
    char chunk[256];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        if (!shell_executor__buffer_append(&buffer, chunk, size)) {
            out_of_memory = true;
            break;
        }
    }
    (void)stdio__session_close(session);
    if (out_of_memory) {
        shell_executor__buffer_free(&buffer);
        stdio__printf("shell: pipe: out of memory buffering output\n");
        return 1;
    }
    if (!complete || status.reason != BRUCE_PROCESS_EXITED || status.exit_code != 0) {
        shell_executor__buffer_free(&buffer);
        return status.exit_code != 0 ? status.exit_code : 1;
    }
    *out_buffer = buffer;
    return 0;
}

/* The one pipe destination that bypasses stdio and takes over the *physical*
 * display/input directly is the "text" editor/viewer -- see
 * shell_executor__dispatch()'s big comment on GUI=1/BRUCE_LAUNCH_FOREGROUND.
 * Everything else (a pager like "less", or any other external command) reads
 * and writes through the stdio session like a normal foreground shell
 * command, so shell_executor__pipe_write() relays it live to the shell's own
 * terminal instead of launching it as a physical-display app. */
static bool shell_executor__pipe_target_wants_display(const char *name) { return strcmp(name, "text") == 0; }

/* Pumps `session` bidirectionally between `child` and the shell process's own
 * routed stdio -- i.e. whatever real terminal (terminal_app.c, ssh, the
 * physical console, ...) the shell itself is running under -- until `child`
 * exits, so an interactive pipe destination that renders through ANSI
 * escapes instead of the physical display can actually be seen and typed
 * into. This mirrors terminal_app.c's own output-drain/input-forward loop,
 * one process level further up. `pending`/`pending_len` hold input bytes
 * read from our own stdin that `child`'s session had no room for yet, so a
 * full input channel delays forwarding instead of dropping bytes. */
static int shell_executor__pipe_relay(bruce_stdio_session_t session, bruce_process_id_t child) {
    bruce_process_status_t status = {0};
    bool complete = false;
    char pending[64];
    size_t pending_len = 0;
    while (!complete) {
        bool activity = false;
        char chunk[256];
        size_t size = 0;
        while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
            if (size == 0) continue;
            (void)stdio__write(chunk, size);
            activity = true;
        }
        if (pending_len == 0) {
            size_t read_size = 0;
            if (stdio__read(pending, sizeof(pending), 0, &read_size) == BRUCE_OK && read_size > 0) {
                pending_len = read_size;
            }
        }
        if (pending_len > 0) {
            bruce_result_t written = stdio__session_write_input(session, pending, pending_len);
            if (written == BRUCE_OK) {
                pending_len = 0;
                activity = true;
            } else if (written != BRUCE_ERR_RESOURCE_LIMIT) {
                pending_len = 0; /* channel broken: stop forwarding input, output still drains above */
            }
        }
        bruce_result_t waited = process__wait_status(child, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!activity && !complete) (void)runtime__delay(20);
    }
    char chunk[256];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        if (size > 0) (void)stdio__write(chunk, size);
    }
    if (!complete) return 1;
    if (status.reason == BRUCE_PROCESS_TERMINATED || status.reason == BRUCE_PROCESS_KILLED) {
        return 128 + (int)status.signal;
    }
    return status.exit_code < 0 ? 1 : status.exit_code & 0xff;
}

/* Feeds a captured buffer to `target`'s stdin over a fresh stdio session,
 * telling it up front how many bytes are coming via a "--stdin-size N"
 * argument prefix (see text_app.c's --stdin-size handling for the convention
 * this follows) so any external program written to expect a piped shell
 * input can size its read. "text" additionally gets GUI=1 and runs in the
 * foreground (see shell_executor__pipe_target_wants_display()); every other
 * destination runs in the background and is relayed live to the shell's own
 * terminal by shell_executor__pipe_relay() once the buffer is delivered. */
static int
shell_executor__pipe_write(int target_argc, char **target_words, const shell_executor__buffer_t *buffer) {
    const void *data = NULL;
    if (buffer->length > 0 && memory__external_map(&buffer->object, &data) != BRUCE_OK) return 1;
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) return 1;
    /* Establishes the new session's tty geometry before the target starts, so
     * an interactive destination (e.g. "less") sees a real screen size from
     * its very first tty__isatty()/tty__get_size() call instead of looking
     * like a non-tty pipe and falling back to dumping its input -- mirrors
     * terminal_app.c's own tty__set_size() call ahead of launching the shell.
     * Left unset (and so non-tty) when the shell itself isn't running in a
     * sized terminal, matching real pager behavior for a non-interactive
     * parent. */
    bruce_tty_size_t size;
    if (tty__get_size(&size) == BRUCE_OK) (void)tty__set_size(session, size.columns, size.rows);
    if (stdio__session_route_children(session) != BRUCE_OK) {
        (void)stdio__session_close(session);
        return 1;
    }
    bool wants_display = shell_executor__pipe_target_wants_display(target_words[0]);
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "--stdin-size %u", (unsigned)buffer->length);
    const bruce_environment_variable_t gui_env[] = {
        {.name = "GUI", .value = "1"}
    };
    int launched = shell_executor__launch_external(
        target_argc, target_words, prefix, wants_display ? gui_env : NULL, wants_display ? 1u : 0u,
        wants_display ? BRUCE_LAUNCH_FOREGROUND : BRUCE_LAUNCH_BACKGROUND
    );
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        return 1;
    }
    int status = 0;
    size_t offset = 0;
    while (offset < buffer->length) {
        size_t chunk = buffer->length - offset > 128u ? 128u : buffer->length - offset;
        bruce_result_t written = stdio__session_write_input(session, (const char *)data + offset, chunk);
        if (written == BRUCE_OK) {
            offset += chunk;
        } else if (written == BRUCE_ERR_RESOURCE_LIMIT) {
            (void)runtime__delay(1);
        } else {
            status = 1;
            break;
        }
    }
    if (status == 0) {
        status = wants_display ? shell_executor__wait((bruce_process_id_t)launched)
                                : shell_executor__pipe_relay(session, (bruce_process_id_t)launched);
    } else {
        (void)process__kill((bruce_process_id_t)launched);
        bruce_process_status_t child_status;
        (void)process__wait_status((bruce_process_id_t)launched, 500, &child_status);
    }
    (void)stdio__session_close(session);
    return status;
}

static int shell_executor__pipe_to_external(
    shell_state_t *state, const shell_command_t *source, const shell_command_t *target
) {
    char **source_words = NULL;
    char **target_words = NULL;
    int source_argc = 0;
    int target_argc = 0;
    const char *error = NULL;
    if (shell_parser__words(
            source, &source_words, &source_argc, shell_executor__lookup, state, state->last_status, &error
        ) != 0 ||
        shell_parser__words(
            target, &target_words, &target_argc, shell_executor__lookup, state, state->last_status, &error
        ) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "pipe parse error");
        shell_parser__free_words(source_words, source_argc);
        shell_parser__free_words(target_words, target_argc);
        return 2;
    }
    bool source_is_echo = source_argc > 0 && strcmp(source_words[0], "echo") == 0;
    if (source_argc == 0 || target_argc == 0 || shell_builtins__is_builtin(target_words[0]) ||
        (shell_builtins__is_builtin(source_words[0]) && !source_is_echo)) {
        stdio__printf("shell: pipes currently require an external producer and an external destination\n");
        shell_parser__free_words(source_words, source_argc);
        shell_parser__free_words(target_words, target_argc);
        return 2;
    }

    shell_executor__buffer_t buffer = {0};
    int status = 0;
    if (source_is_echo) {
        for (int i = 1; status == 0 && i < source_argc; ++i) {
            if (i > 1 && !shell_executor__buffer_append(&buffer, " ", 1u)) status = 1;
            if (status == 0 &&
                !shell_executor__buffer_append(&buffer, source_words[i], strlen(source_words[i]))) {
                status = 1;
            }
        }
        if (status == 0 && !shell_executor__buffer_append(&buffer, "\n", 1u)) status = 1;
        if (status != 0) {
            stdio__printf("shell: pipe: out of memory\n");
            shell_executor__buffer_free(&buffer);
        }
    } else {
        status = shell_executor__capture_external(source_argc, source_words, &buffer);
    }
    if (status == 0) {
        status = shell_executor__pipe_write(target_argc, target_words, &buffer);
        shell_executor__buffer_free(&buffer);
    }
    shell_parser__free_words(source_words, source_argc);
    shell_parser__free_words(target_words, target_argc);
    return status;
}

/* Consumes leading NAME=value assignment words, then dispatches the
 * remainder (if any) to a builtin or an external process. `words` is
 * borrowed; the caller owns and frees it. */
static int shell_executor__dispatch(shell_state_t *state, char **words, int argc) {
    int first_command = 0;
    shell_executor__environment_t environment = {0};
    /* Defaults to background: the shell itself is always a background
     * process piping text through a stdio session (see terminal_app.c and
     * serial_commands_app.c, which both only ever launch "shell" with
     * BRUCE_LAUNCH_BACKGROUND), and a plain external command (ssh, cat,
     * ping, an ELF app run without GUI=1, ...) has no screen of its own --
     * its I/O already flows through that same inherited stdio session.
     * Foreground mode instead makes the process claim the *physical*
     * display/keyboard (see process_registry__create()), which a non-GUI
     * command can't productively use; while it holds that claim, whatever
     * actually owns the screen (e.g. the Terminal app hosting this shell)
     * goes dark/unresponsive until something else cycles foreground away
     * from it. An explicit "BG=0" still forces foreground below, and a
     * command that wants the physical screen tags itself "GUI=1" the same
     * way autostart__run() and input_keyboard__run_hotkey() do for their
     * own foreground launches. */
    bruce_launch_mode_t mode = BRUCE_LAUNCH_BACKGROUND;
    bool bg_explicit = false;
    while (first_command < argc) {
        char *equals = strchr(words[first_command], '=');
        if (equals == NULL) break;
        size_t name_length = (size_t)(equals - words[first_command]);
        if (!shell_parser__valid_name(words[first_command], name_length)) {
            stdio__printf("shell: invalid variable name\n");
            shell_executor__environment_free(&environment);
            return 2;
        }
        if (name_length >= SHELL__VARIABLE_NAME_MAX || environment.count >= SHELL__MAX_VARIABLES) {
            stdio__printf("shell: variable name too long\n");
            shell_executor__environment_free(&environment);
            return 2;
        }
        *equals = '\0';
        const char *name = words[first_command];
        const char *value = equals + 1;
        if (strcmp(name, "BG") == 0) {
            bg_explicit = true;
            if (strcmp(value, "0") == 0) mode = BRUCE_LAUNCH_FOREGROUND;
            else if (strcmp(value, "1") == 0) mode = BRUCE_LAUNCH_BACKGROUND;
            else {
                stdio__printf("shell: BG must be 0 or 1\n");
                shell_executor__environment_free(&environment);
                return 2;
            }
        }
        bruce_environment_variable_t variable = {.name = name, .value = value};
        if (!shell_executor__environment_push(&environment, &variable)) {
            stdio__printf("shell: out of memory\n");
            shell_executor__environment_free(&environment);
            return 1;
        }
        first_command++;
    }
    if (!bg_explicit && app_runner__environment_requests_gui(environment.items, environment.count)) {
        mode = BRUCE_LAUNCH_FOREGROUND;
    }
    if (first_command == argc) {
        for (size_t i = 0; i < environment.count; ++i) {
            int assigned = shell_builtins__set(state, environment.items[i].name, environment.items[i].value);
            if (assigned != 0) {
                shell_executor__environment_free(&environment);
                return assigned;
            }
        }
        shell_executor__environment_free(&environment);
        return 0;
    }
    int remaining = argc - first_command;
    char **argv = words + first_command;
    int result = shell_builtins__is_builtin(argv[0])
                     ? shell_builtins__run(state, remaining, argv)
                     : shell_executor__external(remaining, argv, environment.items, environment.count, mode);
    shell_executor__environment_free(&environment);
    return result;
}

static int shell_executor__command(shell_state_t *state, const shell_command_t *command) {
    char **words = NULL;
    int argc = 0;
    const char *error = NULL;
    if (shell_parser__words(
            command, &words, &argc, shell_executor__lookup, state, state->last_status, &error
        ) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "parse error");
        return 2;
    }
    int result = shell_executor__dispatch(state, words, argc);
    shell_parser__free_words(words, argc);
    return result;
}

int shell_executor__plan(shell_state_t *state, const shell_plan_t *plan) {
    int status = state->last_status;
    for (size_t i = 0; i < plan->count && !state->exit_requested; ++i) {
        shell_connector_t connector = plan->commands[i].connector;
        bool run = connector == SHELL_CONNECT_NONE || connector == SHELL_CONNECT_SEQUENCE ||
                   (connector == SHELL_CONNECT_AND && status == 0) ||
                   (connector == SHELL_CONNECT_OR && status != 0);
        if (!run) continue;
        if (i + 1u < plan->count && plan->commands[i + 1u].connector == SHELL_CONNECT_PIPE) {
            if (i + 2u < plan->count && plan->commands[i + 2u].connector == SHELL_CONNECT_PIPE) {
                stdio__printf("shell: chained pipes are unsupported\n");
                status = 2;
            } else {
                status = shell_executor__pipe_to_external(state, &plan->commands[i], &plan->commands[i + 1u]);
            }
            i++;
        } else if (connector == SHELL_CONNECT_PIPE) {
            status = 2;
        } else {
            status = shell_executor__command(state, &plan->commands[i]);
        }
        state->last_status = status;
    }
    return status;
}
