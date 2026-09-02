#include "shell_executor.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/app_runner.h"
#include "core_sdk/ext_mem_loader.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "core_sdk/tty.h"
#include "shell_arith.h"
#include "shell_builtins.h"
#include "shell_compound.h"

/* Initial size, and doubling step, for the external-memory buffer pipes
 * capture a producer's output into (see shell_executor__buffer_append()). */
#define SHELL_PIPE_CHUNK (512u)

/* $0/$1../$9/$# resolve here rather than through the variable table: they're
 * per-call state (see shell_compound__call_function()), not assignable
 * NAME=value variables. Everything else falls through to the variables
 * shell_builtins__set()/export() manage. */
const char *shell_executor__lookup(void *context, const char *name) {
    shell_state_t *state = (shell_state_t *)context;
    if (name[0] != '\0' && name[1] == '\0') {
        if (name[0] == '0') return state->arg0 != NULL ? state->arg0 : "";
        if (name[0] >= '1' && name[0] <= '9') {
            int index = name[0] - '1';
            return index < state->positional_count ? state->positional[index] : NULL;
        }
        if (name[0] == '#') {
            snprintf(state->positional_count_text, sizeof(state->positional_count_text), "%d", state->positional_count);
            return state->positional_count_text;
        }
    }
    return shell_builtins__get(state, name);
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
    /* The graphical terminal itself carries GUI=1. Shell commands are CLI by
     * default, so mask that inherited value; a command-local GUI=1 entry is
     * copied after this default and therefore still wins. This also covers
     * producer and destination launches, whose callers pass no overlay. */
    if (environment_count > SHELL__MAX_VARIABLES) return BRUCE_ERR_RESOURCE_LIMIT;
    bruce_environment_variable_t cli_environment[SHELL__MAX_VARIABLES + 1u];
    cli_environment[0] = (bruce_environment_variable_t){.name = "GUI", .value = "0"};
    if (environment_count > 0) {
        memcpy(cli_environment + 1u, environment, environment_count * sizeof(*environment));
    }
    environment = cli_environment;
    environment_count++;

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
    const void *data;
    size_t capacity;
    size_t length;
} shell_executor__buffer_t;

static void shell_executor__buffer_free(shell_executor__buffer_t *buffer) {
    if (buffer->data != NULL) (void)memory__external_free(buffer->data);
    buffer->data = NULL;
    buffer->capacity = 0;
    buffer->length = 0;
}

/* Appends to an external-memory-backed buffer, growing it by doubling: each
 * growth step allocates a new, bigger memory__external_malloc() allocation
 * and copies the old bytes across, since external allocations can't be
 * resized in place (same approach as http__external_body_reserve() in
 * core/http/http.c). Routing pipe data through PSRAM/swap this way -- rather
 * than a memory__malloc() buffer capped well below it -- means a captured
 * command's output isn't bounded by the internal heap's largest free block,
 * which on a board without PSRAM or a committed swap partition can be as
 * little as ~100 KiB of fragmented internal RAM shared with everything else.
 * This only fails once the backing store itself (PSRAM, swap, or as a
 * last-resort plain internal RAM) is actually exhausted. */
static bool shell_executor__buffer_append(shell_executor__buffer_t *buffer, const void *data, size_t size) {
    if (size == 0) return true;
    size_t required = buffer->length + size;
    if (required < buffer->length) return false;
    if (required > buffer->capacity) {
        size_t new_capacity = buffer->capacity == 0 ? SHELL_PIPE_CHUNK : buffer->capacity;
        while (new_capacity < required) {
            size_t doubled = new_capacity * 2u;
            if (doubled < new_capacity) return false;
            new_capacity = doubled;
        }
        const void *grown = memory__external_malloc(new_capacity);
        if (grown == NULL) return false;
        if (buffer->length > 0 && memory__external_memcpy(grown, 0, buffer->data, buffer->length) != BRUCE_OK) {
            (void)memory__external_free(grown);
            return false;
        }
        if (buffer->data != NULL) (void)memory__external_free(buffer->data);
        buffer->data = grown;
        buffer->capacity = new_capacity;
    }
    if (memory__external_memcpy(buffer->data, buffer->length, data, size) != BRUCE_OK) return false;
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

/* Implements "$(...)" / "`...`" command substitution (see
 * shell_command_substitution_fn in shell_parser.h): runs `command_text` as a
 * nested "shell -c '<command_text>'" child process and captures its output
 * via shell_executor__capture_external() -- the same "spawn a real child,
 * route its stdio through a session this process created, read it back"
 * trick every other captured-output path here already relies on, just
 * pointed at "shell" itself instead of a single external command, since
 * substitution content can be arbitrary shell syntax (multiple commands,
 * pipes, if/for/while, ...), not just one external command's argv.
 *
 * This is a real, separate process, not bash's copy-on-write subshell: it
 * only ever sees what any other external command this shell launches would
 * -- exported variables and the filesystem, via shell_executor__external()'s
 * usual environment plumbing -- never the calling shell's own unexported
 * variables or function definitions. "$(myfunc)" referencing a function
 * defined earlier in this same interactive session therefore fails ("myfunc:
 * not found") exactly the way calling myfunc from any other external
 * command's child process already would.
 *
 * Also inherits shell_executor__capture_external()'s existing "discards the
 * captured buffer on a nonzero exit" behavior (see its own doc comment) --
 * so unlike bash, "$(cmd_that_fails)" expands to empty rather than whatever
 * cmd_that_fails printed before exiting nonzero. This keeps exactly one
 * discard-on-failure rule across every captured-output path in this shell
 * (">"/">>" redirection, pipes, and now substitution) instead of a special
 * case just for this one.
 *
 * Trailing newlines are stripped from the captured output, matching bash's
 * own "$(...)"/"`...`" behavior (a real command substitution runs *inside*
 * word-splitting, so this also means the result never itself gets
 * word-split on whitespace -- consistent with how a plain $NAME expansion
 * already behaves in this shell, see shell_parser__expand()).
 *
 * Like every other memory__external_malloc()-backed captured-output path
 * here, a *successful* substitution's captured bytes are unreliable under
 * QEMU's swap-backend fallback (see selftest__run_shell_bnu_text_pipe_case()
 * in shell_test.c) -- this is a pre-existing limitation of that backing
 * store under QEMU, not something specific to substitution. */
char *shell_executor__run_substitution(void *context, const char *command_text, size_t length) {
    (void)context; /* nothing of the caller's own shell_state_t is usable across the process boundary; see above */
    if (length >= SHELL__LINE_MAX) {
        stdio__printf("shell: command substitution too long\n");
        return NULL;
    }
    char line[SHELL__LINE_MAX];
    memcpy(line, command_text, length);
    line[length] = '\0';
    char *argv[] = {"shell", "-c", line, NULL};
    shell_executor__buffer_t buffer = {0};
    (void)shell_executor__capture_external(3, argv, &buffer);
    while (buffer.length > 0 && ((const char *)buffer.data)[buffer.length - 1] == '\n') buffer.length--;
    char *result = memory__malloc(buffer.length + 1u);
    if (result != NULL) {
        if (buffer.length > 0) memcpy(result, buffer.data, buffer.length);
        result[buffer.length] = '\0';
    }
    shell_executor__buffer_free(&buffer);
    return result;
}

/* Evaluates "$((...))" arithmetic-expansion *words* (see shell_arith_word_fn
 * in shell_parser.h and shell_parser__expand()'s doubled-paren detection,
 * which is what routes here instead of to shell_executor__run_substitution()
 * above). Unlike command substitution this never spawns a nested process --
 * `context` is this shell's own shell_state_t, evaluated in place exactly
 * like the standalone "((...))" statement form (see
 * shell_executor__is_arith_command()/shell_arith__eval() below), so it sees
 * and can mutate this shell's own variables, assignment side effects
 * included: "echo $((x = 5))" sets $x here just as "((x = 5))" would. */
char *shell_executor__eval_arith_word(void *context, const char *text, size_t length, const char **error) {
    long value = 0;
    if (!shell_arith__eval((shell_state_t *)context, text, length, &value, error)) return NULL;
    char formatted[24];
    int written = snprintf(formatted, sizeof(formatted), "%ld", value);
    if (written <= 0) {
        *error = "arithmetic formatting error";
        return NULL;
    }
    char *result = memory__malloc((size_t)written + 1u);
    if (result != NULL) memcpy(result, formatted, (size_t)written + 1u);
    return result;
}

/* Expands a redirection target span (e.g. the "file.txt" in "cmd > file.txt")
 * exactly the way an ordinary argv word is expanded -- quotes, escapes, and
 * $variable references all work -- by wrapping it in a one-off
 * shell_command_t and running it through the normal word-splitter, then
 * resolves the result against $PWD via shell_builtins__resolve_path() (the
 * same resolution `cd`'s own argument goes through), since storage__open()
 * requires an absolute, normalized path and a redirection target is
 * otherwise written relative like any other shell path argument. Exactly one
 * resulting word is required: zero (an expansion that came out empty) or
 * more than one (e.g. an unquoted variable containing whitespace) is
 * reported as an error rather than guessing which word was meant. On
 * success, *out_path is heap-allocated and owned by the caller. */
static int shell_executor__resolve_redirect_target(
    shell_state_t *state, const shell_word_span_t *span, char **out_path, const char **error
) {
    shell_command_t pseudo = {.text = span->text, .length = span->length};
    char **words = NULL;
    int argc = 0;
    if (shell_parser__words(
            &pseudo, &words, &argc, shell_executor__lookup, shell_executor__run_substitution,
            shell_executor__eval_arith_word, state, state->last_status, error
        ) != 0) {
        return -1;
    }
    if (argc != 1) {
        shell_parser__free_words(words, argc);
        *error = argc == 0 ? "missing redirection target" : "ambiguous redirection target";
        return -1;
    }
    char resolved[BRUCE_STORAGE_PATH_MAX];
    bool resolved_ok = shell_builtins__resolve_path(state, words[0], resolved);
    shell_parser__free_words(words, argc);
    if (!resolved_ok) {
        *error = "invalid redirection target path";
        return -1;
    }
    char *heap_path = memory__malloc(strlen(resolved) + 1u);
    if (heap_path == NULL) {
        *error = "out of memory";
        return -1;
    }
    memcpy(heap_path, resolved, strlen(resolved) + 1u);
    *out_path = heap_path;
    return 0;
}

static uint32_t shell_executor__redirect_open_flags(bool append) {
    return BRUCE_STORAGE_OPEN_CREATE |
           (append ? BRUCE_STORAGE_OPEN_APPEND : (BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_TRUNCATE));
}

/* Writes `size` bytes of `data` to the already-open `file`, retrying as
 * needed until every byte is accounted for (a single storage__write() call
 * isn't guaranteed to take it all at once). Shared by shell_executor__write_file()
 * (a whole captured buffer at once) and shell_executor__stream_external_to_file()
 * (one output chunk at a time) below. */
static bool shell_executor__write_chunk(bruce_file_id_t file, const void *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        size_t written = 0;
        bruce_result_t result = storage__write(file, (const char *)data + offset, size - offset, &written);
        if (result != BRUCE_OK || written == 0) return false;
        offset += written;
    }
    return true;
}

/* Creates (or appends to) `path` and writes `buffer` to it in full. Used for
 * a bare "> file" with no command at all, which just truncates/creates an
 * empty file, matching bash, and by shell_executor__external_with_input()
 * below for the one redirected-output case that still has to buffer first
 * (an external command with both a "<"/heredoc input *and* a ">"/">>"
 * output). A plain "cmd > file"/"cmd >> file" instead streams straight to
 * the file as the command runs -- see shell_executor__stream_external_to_file()
 * and shell_executor__external_redirected(), neither of which goes through
 * this function at all. */
static bool shell_executor__write_file(const char *path, bool append, const shell_executor__buffer_t *buffer) {
    bruce_file_id_t file;
    if (storage__open(path, shell_executor__redirect_open_flags(append), &file) != BRUCE_OK) return false;
    bool ok = buffer->length == 0 || shell_executor__write_chunk(file, buffer->data, buffer->length);
    (void)storage__close(file);
    return ok;
}

/* Reads the whole content of `path` into a fresh external-memory-backed
 * buffer (see shell_executor__buffer_append()'s own doc comment on why
 * that backing is used for this shell's captured/fed data in general).
 * Used for "cmd < file" -- see shell_executor__external_input_redirected()
 * below. Returns false (leaving *out_buffer untouched) if `path` can't be
 * opened for reading, or on any read/out-of-memory error partway through --
 * in the latter case whatever had been read so far is freed rather than
 * left dangling. */
static bool shell_executor__read_file(const char *path, shell_executor__buffer_t *out_buffer) {
    bruce_file_id_t file;
    if (storage__open(path, BRUCE_STORAGE_OPEN_READ, &file) != BRUCE_OK) return false;
    shell_executor__buffer_t buffer = {0};
    bool ok = true;
    for (;;) {
        char chunk[256];
        size_t size = 0;
        if (storage__read(file, chunk, sizeof(chunk), &size) != BRUCE_OK) {
            ok = false;
            break;
        }
        if (size == 0) break;
        if (!shell_executor__buffer_append(&buffer, chunk, size)) {
            ok = false;
            break;
        }
    }
    (void)storage__close(file);
    if (!ok) {
        shell_executor__buffer_free(&buffer);
        return false;
    }
    *out_buffer = buffer;
    return true;
}

/* shell_executor__capture_external()'s counterpart for "cmd > file"/
 * "cmd >> file": runs the same way (a fresh stdio session, foreground,
 * per-command NAME=value assignments dropped -- the same pre-existing
 * limitation a pipe producer already has), but writes each output chunk to
 * the already-open `file` as it arrives instead of buffering the whole
 * thing in external memory first. Unlike shell_executor__capture_external()
 * (which this file's other redirected-output/pipe/substitution paths still
 * use, and which discards its buffer on a nonzero exit), this keeps
 * whatever partial output the command wrote before exiting nonzero --
 * matching bash's own "> file" behavior, and no longer subject to that
 * memory__external_malloc()-backed buffer's QEMU swap-backend unreliability
 * (see shell_executor__run_substitution()'s doc comment) since nothing is
 * captured there at all. Returns the command's own exit status, or -1 if a
 * write to `file` failed partway through (distinguishable from every real
 * exit/signal status, both of which are non-negative). */
static int shell_executor__stream_external_to_file(int argc, char **argv, bruce_file_id_t file) {
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
    bruce_process_status_t status = {0};
    bool complete = false;
    bool write_failed = false;
    while (!complete) {
        char chunk[256];
        size_t size = 0;
        while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
            if (size == 0) continue;
            if (!write_failed && !shell_executor__write_chunk(file, chunk, size)) write_failed = true;
        }
        bruce_result_t waited = process__wait_status((bruce_process_id_t)launched, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!complete) (void)runtime__delay(1);
    }
    /* Same "drain whatever arrived between the last read and the process
     * actually exiting" tail shell_executor__capture_external() has. */
    char chunk[256];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        if (size == 0) continue;
        if (!write_failed && !shell_executor__write_chunk(file, chunk, size)) write_failed = true;
    }
    (void)stdio__session_close(session);
    if (write_failed) return -1;
    if (!complete) return 1;
    if (status.reason == BRUCE_PROCESS_TERMINATED || status.reason == BRUCE_PROCESS_KILLED) {
        return 128 + (int)status.signal;
    }
    return status.exit_code < 0 ? 1 : status.exit_code & 0xff;
}

/* "cmd > file" / "cmd >> file" for an external command with no "<"/heredoc
 * input of its own -- see shell_executor__stream_external_to_file() above
 * for how its output actually reaches `path`. Like bash, the file is still
 * created/truncated even if the command's own exit status is nonzero. */
static int shell_executor__external_redirected(
    shell_state_t *state, int argc, char **argv, const shell_command_t *command
) {
    const char *error = NULL;
    char *path = NULL;
    if (shell_executor__resolve_redirect_target(state, &command->redirect_target, &path, &error) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "redirection error");
        return 2;
    }
    bruce_file_id_t file;
    if (storage__open(path, shell_executor__redirect_open_flags(command->redirect == SHELL_REDIRECT_APPEND), &file) !=
        BRUCE_OK) {
        stdio__printf("shell: %s: cannot open\n", path);
        memory__free(path);
        return 2;
    }
    int status = shell_executor__stream_external_to_file(argc, argv, file);
    (void)storage__close(file);
    if (status == -1) {
        stdio__printf("shell: %s: write failed\n", path);
        status = 1;
    }
    memory__free(path);
    return status;
}

/* "builtin > file" / "builtin >> file" / "myfunc > file" -- a builtin or
 * shell function has no separate child process whose output could be
 * relayed the way shell_executor__external_redirected() above (a real
 * child) or shell_executor__pipeline()'s "echo" pseudo-producer (nothing to
 * run at all) can: it runs in-line, on the shell's own task, writing
 * through whatever session *this process* is currently routed to. So
 * instead, this temporarily reroutes the shell's own session (not its
 * children's -- see stdio__session_capture_self()'s doc comment) into a
 * private capture session, runs the builtin/function, restores the
 * previous routing, and writes whatever got captured to the target file --
 * the same capture-fully-then-write shape
 * shell_executor__external_with_input() uses for a real child, just with
 * nothing to wait on afterward since running the builtin/function already
 * happened synchronously by the time capture_self() is undone.
 *
 * Only a plain ">"/">>" is handled here -- a "<"/heredoc feeding a
 * builtin/function's *input* is a separate, harder problem (its own
 * stdio__read_line()/stdio__read() calls would need the capture session's
 * *input* side pre-loaded instead) and stays rejected by the caller.
 *
 * Nests correctly for a function whose body itself redirects another
 * builtin/function: each call's capture/release pair is scoped to this
 * single call, so nested calls push/pop their own session in turn, same as
 * ordinary recursion. */
static int shell_executor__builtin_redirected(
    shell_state_t *state, int argc, char **argv, const shell_command_t *command, bool is_function
) {
    const char *error = NULL;
    char *path = NULL;
    if (shell_executor__resolve_redirect_target(state, &command->redirect_target, &path, &error) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "redirection error");
        return 2;
    }
    bruce_stdio_session_t capture = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&capture) != BRUCE_OK || stdio__session_capture_self(capture) != BRUCE_OK) {
        stdio__printf("shell: %s: out of memory\n", path);
        if (capture != BRUCE_STDIO_SESSION_INVALID) (void)stdio__session_close(capture);
        memory__free(path);
        return 2;
    }
    int status = is_function ? shell_compound__call_function(state, argc, argv) : shell_builtins__run(state, argc, argv);
    (void)stdio__session_release_self();

    shell_executor__buffer_t buffer = {0};
    bool out_of_memory = false;
    for (;;) {
        char chunk[256];
        size_t size = 0;
        if (stdio__session_read_output(capture, chunk, sizeof(chunk), &size) != BRUCE_OK || size == 0) break;
        if (!shell_executor__buffer_append(&buffer, chunk, size)) {
            out_of_memory = true;
            break;
        }
    }
    (void)stdio__session_close(capture);

    bool write_ok = shell_executor__write_file(path, command->redirect == SHELL_REDIRECT_APPEND, &buffer);
    if (out_of_memory) stdio__printf("shell: %s: out of memory\n", path);
    else if (!write_ok) stdio__printf("shell: %s: write failed\n", path);
    shell_executor__buffer_free(&buffer);
    memory__free(path);
    return out_of_memory || !write_ok ? 1 : status;
}

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

/* Drains output already produced by a pipe destination. This must also run
 * while its captured input is being fed: filters such as xxd expand their
 * input, so waiting until the entire input has been written can fill the
 * destination's output channel and deadlock both sides. */
static bool shell_executor__pipe_drain_output(bruce_stdio_session_t session) {
    bool activity = false;
    char chunk[256];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        if (size == 0) continue;
        (void)stdio__write(chunk, size);
        activity = true;
    }
    return activity;
}

/* Same draining duty as shell_executor__pipe_drain_output() above, but for a
 * "producer | consumer > file" destination: its output is being collected
 * into `out_buffer` for shell_executor__write_file() rather than relayed
 * live to the shell's own stdio, since a redirected destination isn't
 * interactive and has nothing to relay to. Sets *out_of_memory and stops
 * appending (but keeps draining the channel, so the destination doesn't
 * stall on a full output pipe) if the buffer can't grow any further. */
static bool
shell_executor__pipe_drain_output_capture(bruce_stdio_session_t session, shell_executor__buffer_t *out_buffer, bool *out_of_memory) {
    bool activity = false;
    char chunk[256];
    size_t size = 0;
    while (stdio__session_read_output(session, chunk, sizeof(chunk), &size) == BRUCE_OK) {
        if (size == 0) continue;
        if (!*out_of_memory && !shell_executor__buffer_append(out_buffer, chunk, size)) *out_of_memory = true;
        activity = true;
    }
    return activity;
}

/* shell_executor__pipe_relay()'s counterpart for a redirected pipe
 * destination: waits for `child` to exit, collecting all of its output into
 * `out_buffer` instead of relaying it live and forwarding the shell's own
 * stdin (a redirected destination has no terminal to be interactive with).
 * Returns the same status convention as pipe_relay(), plus -1 -- which
 * cannot collide with a real exit/signal status, both of which are
 * non-negative -- if the buffer ran out of memory partway through. */
static int shell_executor__pipe_relay_capture(
    bruce_stdio_session_t session, bruce_process_id_t child, shell_executor__buffer_t *out_buffer
) {
    bruce_process_status_t status = {0};
    bool complete = false;
    bool out_of_memory = false;
    while (!complete) {
        bool activity = shell_executor__pipe_drain_output_capture(session, out_buffer, &out_of_memory);
        if (out_of_memory) break;
        bruce_result_t waited = process__wait_status(child, 0, &status);
        complete = waited == BRUCE_OK;
        if (!complete && waited != BRUCE_ERR_TIMEOUT) break;
        if (!activity && !complete) (void)runtime__delay(20);
    }
    if (!out_of_memory) (void)shell_executor__pipe_drain_output_capture(session, out_buffer, &out_of_memory);
    if (out_of_memory) {
        if (!complete) {
            (void)process__kill(child);
            (void)process__wait_status(child, 500, &status);
        }
        return -1;
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
 * input can size its read. Like any other external command the shell
 * launches, a pipe destination -- "text", "less", or anything else -- runs
 * in the background with no GUI env of its own, and is relayed live to the
 * shell's own terminal by shell_executor__pipe_relay() once the buffer is
 * delivered; a target that wants the physical display instead needs the
 * same explicit "GUI=1" its non-piped invocation would (see
 * shell_executor__dispatch()'s big comment on GUI=1/BRUCE_LAUNCH_FOREGROUND).
 * `capture`, when non-NULL, redirects this relaying: the target's output is
 * collected into `*capture` (for a caller such as
 * shell_executor__pipeline() to write out via
 * shell_executor__write_file()) instead of being relayed live to the shell's
 * own stdio, and the shell's own stdin is not forwarded to the target either
 * -- a "producer | consumer > file" destination is never interactive. Takes
 * ownership of `buffer` and frees it itself as soon as every byte has been
 * written to the target's stdin (the caller must not touch or free it
 * afterward): once `capture` is in play, `buffer` (the producer's captured
 * output) and `*capture` (the consumer's, being accumulated) are both
 * memory__external_*-backed allocations, and freeing the source the moment
 * it's no longer needed -- rather than leaving it alive for the rest of this
 * call, as the caller freeing it only after this function returns would --
 * keeps only one such buffer live at a time. */
static int shell_executor__pipe_write(
    int target_argc, char **target_words, shell_executor__buffer_t *buffer, shell_executor__buffer_t *capture
) {
    bruce_stdio_session_t session = BRUCE_STDIO_SESSION_INVALID;
    if (stdio__session_create(&session) != BRUCE_OK) {
        shell_executor__buffer_free(buffer);
        return 1;
    }
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
        shell_executor__buffer_free(buffer);
        return 1;
    }
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "--stdin-size %u", (unsigned)buffer->length);
    int launched =
        shell_executor__launch_external(target_argc, target_words, prefix, NULL, 0, BRUCE_LAUNCH_BACKGROUND);
    (void)stdio__session_route_children(BRUCE_STDIO_SESSION_INVALID);
    if (launched <= 0) {
        (void)stdio__session_close(session);
        shell_executor__buffer_free(buffer);
        return 1;
    }
    int status = 0;
    size_t offset = 0;
    bool out_of_memory = false;
    while (offset < buffer->length) {
        if (capture != NULL) {
            (void)shell_executor__pipe_drain_output_capture(session, capture, &out_of_memory);
            if (out_of_memory) {
                status = 1;
                break;
            }
        } else {
            (void)shell_executor__pipe_drain_output(session);
        }
        size_t chunk = buffer->length - offset > 128u ? 128u : buffer->length - offset;
        bruce_result_t written = stdio__session_write_input(session, (const char *)buffer->data + offset, chunk);
        if (written == BRUCE_OK) {
            offset += chunk;
        } else if (written == BRUCE_ERR_RESOURCE_LIMIT) {
            /* The child may be waiting for room in its output channel before
             * it can consume more input. Draining above breaks that cycle. */
            (void)runtime__delay(1);
        } else {
            status = 1;
            break;
        }
    }
    /* The source buffer's job ends here -- everything of it that's going to
     * reach the target has already been written to its stdin. Freeing it now
     * rather than leaving it to the caller (after this function returns)
     * means it's no longer live during the relay/capture phase below, so at
     * most one memory__external_*-backed pipe buffer (this one, or `*capture`
     * once relaying starts collecting into it) is ever alive at a time. */
    shell_executor__buffer_free(buffer);
    if (status == 0) {
        status = capture != NULL ? shell_executor__pipe_relay_capture(session, (bruce_process_id_t)launched, capture)
                                  : shell_executor__pipe_relay(session, (bruce_process_id_t)launched);
        if (status == -1) {
            out_of_memory = true;
            status = 1;
        }
    } else {
        (void)process__kill((bruce_process_id_t)launched);
        bruce_process_status_t child_status;
        (void)process__wait_status((bruce_process_id_t)launched, 500, &child_status);
    }
    (void)stdio__session_close(session);
    if (out_of_memory) stdio__printf("shell: pipe: out of memory buffering output\n");
    return status;
}

/* "cmd < file" or "cmd <<DELIM"/"<<-DELIM" for an external command --
 * `input` is the file's content or the (already expanded, unless the
 * heredoc's delimiter was quoted) heredoc body respectively, fed to the
 * command's stdin the exact same way a pipe producer's captured output
 * would be (shell_executor__pipe_write() -- including its "--stdin-size N"
 * convention any external program written to expect piped input already
 * relies on). Always takes ownership of `input`, freeing it via
 * shell_executor__pipe_write() even on an early exit below.
 *
 * When `command` carries no output redirect of its own, the command's own
 * output is relayed live to the shell's stdio, exactly like a plain,
 * non-redirected external command -- only its stdin came from somewhere
 * other than a terminal. When it also has a ">"/">>" redirect, this is the
 * one case that still buffers instead of streaming (see
 * shell_executor__stream_external_to_file()'s own doc comment for the
 * streaming path this does *not* get to use): the command's output is
 * captured into memory via `pipe_write`'s own `capture` parameter -- the
 * same mechanism shell_executor__pipeline() already relies on for
 * "producer | consumer > file" -- and written out to the target file once
 * it finishes, sharing that path's pre-existing memory__external_malloc()
 * QEMU-swap-backend caveat and discard-on-out-of-memory behavior. */
static int shell_executor__external_with_input(
    shell_state_t *state, int argc, char **argv, const shell_command_t *command, shell_executor__buffer_t *input
) {
    if (command->redirect == SHELL_REDIRECT_NONE) return shell_executor__pipe_write(argc, argv, input, NULL);
    const char *error = NULL;
    char *path = NULL;
    if (shell_executor__resolve_redirect_target(state, &command->redirect_target, &path, &error) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "redirection error");
        shell_executor__buffer_free(input);
        return 2;
    }
    shell_executor__buffer_t capture = {0};
    int status = shell_executor__pipe_write(argc, argv, input, &capture);
    if (!shell_executor__write_file(path, command->redirect == SHELL_REDIRECT_APPEND, &capture)) {
        stdio__printf("shell: %s: write failed\n", path);
        if (status == 0) status = 1;
    }
    shell_executor__buffer_free(&capture);
    memory__free(path);
    return status;
}

/* "cmd < file" -- reads `file` in full (shell_executor__read_file()) and
 * hands it to shell_executor__external_with_input() above. */
static int shell_executor__external_input_redirected(
    shell_state_t *state, int argc, char **argv, const shell_command_t *command
) {
    const char *error = NULL;
    char *path = NULL;
    if (shell_executor__resolve_redirect_target(state, &command->input_target, &path, &error) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "redirection error");
        return 2;
    }
    shell_executor__buffer_t input = {0};
    bool ok = shell_executor__read_file(path, &input);
    if (!ok) stdio__printf("shell: %s: cannot open\n", path);
    memory__free(path);
    if (!ok) return 1;
    return shell_executor__external_with_input(state, argc, argv, command, &input);
}

/* "cmd <<DELIM"/"cmd <<-DELIM" -- copies the already-collected, already
 * (unless quoted) $expanded heredoc body (see shell_command_t's own doc
 * comment on `heredoc_body`) into a fresh buffer and hands it to
 * shell_executor__external_with_input() above the same way a "<"-redirected
 * file's content would be. */
static int shell_executor__external_heredoc_redirected(
    shell_state_t *state, int argc, char **argv, const shell_command_t *command
) {
    shell_executor__buffer_t input = {0};
    size_t body_length = strlen(command->heredoc_body);
    if (body_length > 0 && !shell_executor__buffer_append(&input, command->heredoc_body, body_length)) {
        stdio__printf("shell: out of memory\n");
        shell_executor__buffer_free(&input);
        return 1;
    }
    return shell_executor__external_with_input(state, argc, argv, command, &input);
}

/* Runs a full "|"-chain of `count` (>= 2) commands, feeding each stage's
 * captured stdout to the next stage's stdin in turn: `commands[0]` is the
 * producer, `commands[count-1]` is the final destination, and everything
 * between is both a consumer (of the previous stage) and a producer (for the
 * next) -- e.g. "a | b | c" runs as count==3 with b in that middle role.
 * This is shell_executor__pipe_write()'s buffer-and-feed model (see its big
 * comment above) chained across more than one hop: since this shell has no
 * fd-level fork/exec, every stage still runs to completion, fully buffered
 * in external memory, before the next stage starts -- there is no concurrent
 * streaming between stages, same as the two-stage case already had. Only
 * `commands[0]` may be the "echo" pseudo-producer (see below); every other
 * stage must be a real external command, since only a real process has
 * stdin to feed the previous stage's captured output into. */
static int shell_executor__pipeline(shell_state_t *state, const shell_command_t *commands, size_t count) {
    char **stage_words[SHELL__MAX_COMMANDS] = {0};
    int stage_argc[SHELL__MAX_COMMANDS] = {0};
    const char *error = NULL;
    bool parse_failed = false;
    for (size_t i = 0; i < count && !parse_failed; ++i) {
        if (shell_parser__words(
                &commands[i], &stage_words[i], &stage_argc[i], shell_executor__lookup,
                shell_executor__run_substitution, shell_executor__eval_arith_word, state, state->last_status, &error
            ) != 0) {
            parse_failed = true;
        }
    }
    if (parse_failed) {
        stdio__printf("shell: %s\n", error != NULL ? error : "pipe parse error");
        for (size_t i = 0; i < count; ++i) shell_parser__free_words(stage_words[i], stage_argc[i]);
        return 2;
    }
    bool source_is_echo = stage_argc[0] > 0 && strcmp(stage_words[0][0], "echo") == 0;
    bool invalid = false;
    bool has_stage_input = false;
    for (size_t i = 0; i < count; ++i) {
        if (commands[i].input_redirect || commands[i].heredoc_body != NULL) has_stage_input = true;
        if (stage_argc[i] == 0) {
            invalid = true;
        } else if (i == 0) {
            if (!source_is_echo && shell_builtins__is_builtin(stage_words[i][0])) invalid = true;
        } else if (shell_builtins__is_builtin(stage_words[i][0])) {
            invalid = true;
        }
    }
    if (invalid) {
        stdio__printf("shell: pipes currently require an external producer and an external destination\n");
        for (size_t i = 0; i < count; ++i) shell_parser__free_words(stage_words[i], stage_argc[i]);
        return 2;
    }
    /* Every stage but the first already gets its stdin from the stage before
     * it; a "<"/heredoc on one of those would have nowhere to go, and one on
     * the first stage -- feeding it *instead* of piping into it -- isn't
     * wired up here at all yet. Rejected outright rather than silently
     * ignored (which is what would otherwise happen: nothing here ever
     * consults commands[i].input_redirect/heredoc_body). */
    if (has_stage_input) {
        stdio__printf("shell: '<'/heredoc input is not supported on a piped command\n");
        for (size_t i = 0; i < count; ++i) shell_parser__free_words(stage_words[i], stage_argc[i]);
        return 2;
    }

    /* "... | last > file" / "... | last >> file" -- the final stage's own
     * redirection is honored the same way a lone "cmd > file" is (see
     * shell_executor__external_redirected() above): its captured output is
     * written to `path` via shell_executor__write_file() once it finishes,
     * instead of being relayed live to the shell's own stdio. Resolved up
     * front, before the first stage even runs, so a bad redirection target
     * is reported without wasting any of the pipeline's work. Only the last
     * stage's redirect is consulted -- same as before, a redirect on any
     * earlier stage is silently ignored (see the header comment above). */
    const shell_command_t *last = &commands[count - 1];
    char *redirect_path = NULL;
    bool redirect_append = false;
    if (last->redirect != SHELL_REDIRECT_NONE) {
        if (shell_executor__resolve_redirect_target(state, &last->redirect_target, &redirect_path, &error) != 0) {
            stdio__printf("shell: %s\n", error != NULL ? error : "redirection error");
            for (size_t i = 0; i < count; ++i) shell_parser__free_words(stage_words[i], stage_argc[i]);
            return 2;
        }
        redirect_append = last->redirect == SHELL_REDIRECT_APPEND;
    }

    shell_executor__buffer_t buffer = {0};
    int status = 0;
    if (source_is_echo) {
        for (int i = 1; status == 0 && i < stage_argc[0]; ++i) {
            if (i > 1 && !shell_executor__buffer_append(&buffer, " ", 1u)) status = 1;
            if (status == 0 &&
                !shell_executor__buffer_append(&buffer, stage_words[0][i], strlen(stage_words[0][i]))) {
                status = 1;
            }
        }
        if (status == 0 && !shell_executor__buffer_append(&buffer, "\n", 1u)) status = 1;
        if (status != 0) {
            stdio__printf("shell: pipe: out of memory\n");
            shell_executor__buffer_free(&buffer);
        }
    } else {
        status = shell_executor__capture_external(stage_argc[0], stage_words[0], &buffer);
    }
    /* Each hop feeds the previous stage's captured output in as `buffer` and
     * gets back its own captured output in `stage_out`, which becomes the
     * next hop's `buffer` in turn -- except the very last hop, which relays
     * live to the shell's own stdio (leaving `stage_out` untouched, so
     * assigning it back is a no-op) unless the pipeline as a whole is
     * redirected to a file. */
    for (size_t i = 1; status == 0 && i < count; ++i) {
        bool is_last = i == count - 1;
        shell_executor__buffer_t stage_out = {0};
        bool want_capture = !is_last || redirect_path != NULL;
        /* pipe_write() takes ownership of `buffer` and frees it itself. */
        status = shell_executor__pipe_write(stage_argc[i], stage_words[i], &buffer, want_capture ? &stage_out : NULL);
        buffer = stage_out;
        if (is_last && redirect_path != NULL) {
            if (!shell_executor__write_file(redirect_path, redirect_append, &stage_out)) {
                stdio__printf("shell: %s: write failed\n", redirect_path);
                if (status == 0) status = 1;
            }
        }
    }
    shell_executor__buffer_free(&buffer);
    memory__free(redirect_path);
    for (size_t i = 0; i < count; ++i) shell_parser__free_words(stage_words[i], stage_argc[i]);
    return status;
}

static bool shell_executor__buffer_append_text(shell_executor__buffer_t *buffer, const char *text) {
    return shell_executor__buffer_append(buffer, text, strlen(text));
}

static int shell_executor__page_buffer(shell_executor__buffer_t *buffer) {
    char *less_argv[] = {"less", NULL};
    /* pipe_write() takes ownership of `buffer` and frees it itself. */
    return shell_executor__pipe_write(1, less_argv, buffer, NULL);
}

int shell_executor__page_help(void) {
    shell_executor__buffer_t buffer = {0};
    static const char introduction[] = "Bruce shell\n"
                                       "\n"
                                       "Usage:\n"
                                       "  command [argument ...]\n"
                                       "  NAME=value command\n"
                                       "  producer | consumer\n"
                                       "  producer | filter | ... | consumer\n"
                                       "  external-command > file\n"
                                       "  external-command >> file\n"
                                       "  external-command < file\n"
                                       "  external-command <<DELIM ... DELIM (script files only)\n"
                                       "\n"
                                       "Operators:\n"
                                       "  ;    run commands in sequence\n"
                                       "  &&   run the next command after success\n"
                                       "  ||   run the next command after failure\n"
                                       "  |    pipe external commands together, any number of hops\n"
                                       "  >    redirect an external command's output, truncating the file\n"
                                       "  >>   redirect an external command's output, appending to the file\n"
                                       "  <    feed a file's content to an external command's stdin\n"
                                       "  <<   feed a heredoc body to an external command's stdin (scripts only)\n"
                                       "\n"
                                       "Shell built-ins:\n";
    if (!shell_executor__buffer_append_text(&buffer, introduction)) goto out_of_memory;

    for (size_t i = 0; i < shell_builtins__count(); ++i) {
        const char *name = shell_builtins__name(i);
        if (name != NULL && (!shell_executor__buffer_append_text(&buffer, "  ") ||
                             !shell_executor__buffer_append_text(&buffer, name) ||
                             !shell_executor__buffer_append_text(&buffer, "\n"))) {
            goto out_of_memory;
        }
    }

    if (!shell_executor__buffer_append_text(&buffer, "\nList of external commands run: man\n"))
        goto out_of_memory;
    if (!shell_executor__buffer_append_text(&buffer, "Use man <command> for detailed command help.\n")) {
        goto out_of_memory;
    }
    return shell_executor__page_buffer(&buffer);

out_of_memory:
    shell_executor__buffer_free(&buffer);
    stdio__printf("shell: help: out of memory\n");
    return 1;
}

/* Consumes leading NAME=value assignment words, then dispatches the
 * remainder (if any) to a builtin or an external process. `words` is
 * borrowed; the caller owns and frees it. `command` is only consulted for
 * its redirect fields (see shell_parser.h) -- `words`/`argc` are already the
 * result of word-splitting `command`'s (possibly redirect-shortened) text. */
static int shell_executor__dispatch(shell_state_t *state, const shell_command_t *command, char **words, int argc) {
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
        /* A bare "< file" / "> file" / ">> file" with no command at all is
         * valid, same as in bash: "<" just checks the file opens for
         * reading (bash itself does the same -- no command means nothing
         * ever actually reads it), and ">"/">>" creates/truncates (or
         * appends nothing to) the target file. A bare "<<DELIM" heredoc
         * with no command is rejected instead: unlike a file, its body has
         * already been read from the script by the time this runs (see
         * shell_command_t's own doc comment on `heredoc_body`), so silently
         * discarding it would be more surprising than useful. */
        if (command->heredoc_body != NULL) {
            stdio__printf("shell: heredoc with no command has no effect\n");
            return 2;
        }
        bool ok = true;
        if (command->input_redirect) {
            const char *in_error = NULL;
            char *in_path = NULL;
            if (shell_executor__resolve_redirect_target(state, &command->input_target, &in_path, &in_error) != 0) {
                stdio__printf("shell: %s\n", in_error != NULL ? in_error : "redirection error");
                return 2;
            }
            bruce_file_id_t probe;
            ok = storage__open(in_path, BRUCE_STORAGE_OPEN_READ, &probe) == BRUCE_OK;
            if (ok) (void)storage__close(probe);
            else stdio__printf("shell: %s: cannot open\n", in_path);
            memory__free(in_path);
        }
        if (command->redirect == SHELL_REDIRECT_NONE) return ok ? 0 : 1;
        const char *error = NULL;
        char *path = NULL;
        if (shell_executor__resolve_redirect_target(state, &command->redirect_target, &path, &error) != 0) {
            stdio__printf("shell: %s\n", error != NULL ? error : "redirection error");
            return 2;
        }
        shell_executor__buffer_t empty = {0};
        bool write_ok = shell_executor__write_file(path, command->redirect == SHELL_REDIRECT_APPEND, &empty);
        if (!write_ok) stdio__printf("shell: %s: write failed\n", path);
        memory__free(path);
        return ok && write_ok ? 0 : 1;
    }
    int remaining = argc - first_command;
    char **argv = words + first_command;
    /* "time" wraps whatever command follows it (function, builtin, or
     * external) rather than being one itself -- peel it off here, before
     * is_function/is_builtin below ever see it, so the timing wraps the
     * *wrapped* command's dispatch. It stays in shell_builtins__is_builtin()
     * so pipelines reject it with the usual builtin-in-a-pipe message
     * instead of "command not found" (see shell_executor__pipeline). */
    bool timed = strcmp(argv[0], "time") == 0;
    if (timed) {
        if (remaining < 2) {
            stdio__printf("shell: time: missing command\n");
            shell_executor__environment_free(&environment);
            return 2;
        }
        argv++;
        remaining--;
    }
    /* A user-defined function shadows a builtin or external command of the
     * same name, the same as in bash. */
    bool is_function = shell_compound__is_function(state, argv[0]);
    bool is_builtin = !is_function && shell_builtins__is_builtin(argv[0]);
    int result;
    uint64_t started_at = timed ? runtime__now() : 0;
    bool redirected = command->redirect != SHELL_REDIRECT_NONE || command->input_redirect ||
                       command->heredoc_body != NULL;
    bool output_only_redirect =
        command->redirect != SHELL_REDIRECT_NONE && !command->input_redirect && command->heredoc_body == NULL;
    if (redirected) {
        if ((is_function || is_builtin) && output_only_redirect) {
            result = shell_executor__builtin_redirected(state, remaining, argv, command, is_function);
        } else if (is_function || is_builtin) {
            stdio__printf("shell: '<'/heredoc input redirection currently requires an external command\n");
            result = 2;
        } else if (command->input_redirect) {
            result = shell_executor__external_input_redirected(state, remaining, argv, command);
        } else if (command->heredoc_body != NULL) {
            result = shell_executor__external_heredoc_redirected(state, remaining, argv, command);
        } else {
            result = shell_executor__external_redirected(state, remaining, argv, command);
        }
    } else {
        result = is_function ? shell_compound__call_function(state, remaining, argv)
                  : is_builtin
                      ? shell_builtins__run(state, remaining, argv)
                      : shell_executor__external(remaining, argv, environment.items, environment.count, mode);
    }
    /* Only wall-clock ("real") time is available here -- the process SDK
     * exposes a live cpu_percent gauge (see bruce_process_snapshot_t) but no
     * cumulative user/sys CPU time to report alongside it, unlike bash's
     * three-line `time` output. */
    if (timed) {
        uint64_t elapsed_ms = runtime__now() - started_at;
        stdio__printf(
            "real\t%um%u.%03us\n",
            (unsigned)(elapsed_ms / 60000u),
            (unsigned)((elapsed_ms / 1000u) % 60u),
            (unsigned)(elapsed_ms % 1000u)
        );
    }
    shell_executor__environment_free(&environment);
    return result;
}

/* True when `command`'s trimmed text is a whole "((...))" arithmetic
 * command (see shell_arith.c) -- *inner_start / *inner_len then bound the
 * text strictly between the outer "((" and "))". Nested single parens
 * inside (grouping) don't confuse this: it only looks at the very first two
 * and very last two non-whitespace characters, the same shape
 * shell_parser__plan()'s arith_depth tracking already requires to have kept
 * this whole span glued together as one flat command in the first place. */
static bool
shell_executor__is_arith_command(const shell_command_t *command, size_t *inner_start, size_t *inner_len) {
    size_t start = 0, end = command->length;
    while (start < end && isspace((unsigned char)command->text[start])) start++;
    while (end > start && isspace((unsigned char)command->text[end - 1])) end--;
    if (end - start < 4 || command->text[start] != '(' || command->text[start + 1] != '(' ||
        command->text[end - 1] != ')' || command->text[end - 2] != ')') {
        return false;
    }
    *inner_start = start + 2;
    *inner_len = (end - 2) - (start + 2);
    return true;
}

/* True when `command`'s trimmed text merely *opens* with "((" -- no valid
 * command name starts with '(', so this is always an attempted arithmetic
 * command, whether or not it goes on to close with a matching "))". Used to
 * tell a genuinely malformed one (e.g. "(( 1 +", missing its close) apart
 * from an ordinary command lookup once shell_executor__is_arith_command()
 * has already said this isn't a well-formed one: without this, such a line
 * falls through to word-splitting and dispatch, where "((" becomes argv[0]
 * and fails as an unknown command (status 127) instead of being reported as
 * the syntax error it actually is (status 2, matching every other malformed
 * construct this shell reports). */
static bool shell_executor__starts_with_arith(const shell_command_t *command) {
    size_t start = 0, end = command->length;
    while (start < end && isspace((unsigned char)command->text[start])) start++;
    return end - start >= 2 && command->text[start] == '(' && command->text[start + 1] == '(';
}

static int shell_executor__command(shell_state_t *state, const shell_command_t *command) {
    size_t inner_start, inner_len;
    if (shell_executor__is_arith_command(command, &inner_start, &inner_len)) {
        if (command->redirect != SHELL_REDIRECT_NONE || command->input_redirect || command->heredoc_body != NULL) {
            stdio__printf("shell: redirection is not supported on '((...))'\n");
            return 2;
        }
        long value = 0;
        const char *arith_error = NULL;
        if (!shell_arith__eval(state, command->text + inner_start, inner_len, &value, &arith_error)) {
            stdio__printf("shell: ((: %s\n", arith_error != NULL ? arith_error : "syntax error");
            return 2;
        }
        return value != 0 ? 0 : 1; /* bash: (( )) is "true" (status 0) iff the result is nonzero */
    }
    if (shell_executor__starts_with_arith(command)) {
        stdio__printf("shell: ((: missing '))'\n");
        return 2;
    }
    char **words = NULL;
    int argc = 0;
    const char *error = NULL;
    if (shell_parser__words(
            command, &words, &argc, shell_executor__lookup, shell_executor__run_substitution,
            shell_executor__eval_arith_word, state, state->last_status, &error
        ) != 0) {
        stdio__printf("shell: %s\n", error != NULL ? error : "parse error");
        return 2;
    }
    int result = shell_executor__dispatch(state, command, words, argc);
    shell_parser__free_words(words, argc);
    return result;
}

int shell_executor__plan(shell_state_t *state, const shell_plan_t *plan) {
    int status = state->last_status;
    /* A "break" mid-batch (e.g. the ";"-joined "break; echo unreached" in
     * "if x; then break; echo unreached; fi") must stop the rest of this
     * batch immediately, the same way exit_requested already does -- see
     * shell_state_t.break_requested and shell_compound__run_for()/
     * run_while(), which are what actually consume it. */
    for (size_t i = 0; i < plan->count && !state->exit_requested && state->break_requested == 0; ++i) {
        shell_connector_t connector = plan->commands[i].connector;
        bool run = connector == SHELL_CONNECT_NONE || connector == SHELL_CONNECT_SEQUENCE ||
                   (connector == SHELL_CONNECT_AND && status == 0) ||
                   (connector == SHELL_CONNECT_OR && status != 0);
        if (!run) continue;
        if (i + 1u < plan->count && plan->commands[i + 1u].connector == SHELL_CONNECT_PIPE) {
            /* Collect the whole run of pipe-joined commands starting here --
             * plan->commands[i + n].connector == SHELL_CONNECT_PIPE means
             * that command is piped from the one before it, so a chain of
             * any length shows up as a run of such connectors -- and run the
             * lot as one pipeline instead of just the next one command. */
            size_t pipeline_count = 2;
            while (i + pipeline_count < plan->count && plan->commands[i + pipeline_count].connector == SHELL_CONNECT_PIPE) {
                pipeline_count++;
            }
            status = shell_executor__pipeline(state, &plan->commands[i], pipeline_count);
            i += pipeline_count - 1u;
        } else if (connector == SHELL_CONNECT_PIPE) {
            status = 2;
        } else {
            status = shell_executor__command(state, &plan->commands[i]);
        }
        state->last_status = status;
    }
    return status;
}
