#include "shell_condition.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"
#include "shell_builtins.h"

/* Basic bash-like test(1) subset: unary -z/-n, the -e/-f/-d/-r/-w/-x file
 * tests below, string = and !=, the six numeric comparisons, and -a/-o
 * (test/[) or &&/|| ([[ ]], glued back into one flat command by
 * shell_parser__plan()'s own "[[ ... ]]" span tracking -- see its doc
 * comment) as left-associative logical combinators, && (or -a) binding
 * tighter than || (or -o), joining sub-expressions built from any of the
 * above. Anything else (!, parenthesized groups, [[ ]]'s pattern/regex
 * matching, ...) is intentionally out of scope. */

static bool shell_condition__truthy(const char *value) { return value[0] != '\0'; }

static bool shell_condition__numeric(const char *text, long *out) {
    if (text[0] == '\0') return false;
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *out = value;
    return true;
}

static int shell_condition__compare(const char *op, long a, long b) {
    if (strcmp(op, "-eq") == 0) return a == b ? 0 : 1;
    if (strcmp(op, "-ne") == 0) return a != b ? 0 : 1;
    if (strcmp(op, "-lt") == 0) return a < b ? 0 : 1;
    if (strcmp(op, "-le") == 0) return a <= b ? 0 : 1;
    if (strcmp(op, "-gt") == 0) return a > b ? 0 : 1;
    if (strcmp(op, "-ge") == 0) return a >= b ? 0 : 1;
    return -1;
}

static bool shell_condition__is_numeric_op(const char *op) {
    return strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 || strcmp(op, "-lt") == 0 ||
           strcmp(op, "-le") == 0 || strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0;
}

/* True if `path` (already resolved -- see shell_condition__file_test()
 * below) names a directory. storage__list()'s success/failure is this
 * codebase's own standing idiom for that question -- shell_builtins.c's
 * `cd` and storage.c's own storage__copy() both key off it the same way,
 * rather than exposing a dedicated stat-like call. */
static bool shell_condition__is_directory(const char *path) {
    size_t count = 0;
    return storage__list(path, NULL, 0, &count) == BRUCE_OK;
}

static bool shell_condition__path_exists(const char *path) {
    bool exists = false;
    return storage__exists(path, &exists) == BRUCE_OK && exists;
}

/* Non-destructive: opens `path` with `flags` just to see whether it
 * succeeds, then immediately closes it again. Never passes
 * BRUCE_STORAGE_OPEN_CREATE/TRUNCATE, so a -w probe on a file that already
 * exists can never truncate or create it as a side effect. */
static bool shell_condition__file_openable(const char *path, uint32_t flags) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    if (storage__open(path, flags, &file) != BRUCE_OK) return false;
    (void)storage__close(file);
    return true;
}

/* Handles the six file-test unary operators (-e/-f/-d/-r/-w/-x), resolving
 * `arg` against $PWD first, same as any other shell path argument (see
 * shell_builtins__resolve_path()). Returns 0/1 for a recognized operator,
 * or -1 if `op` isn't one of these at all, so the caller can fall through
 * to its own "unsupported unary operator" error for anything else.
 *
 * This filesystem has no per-file permission bits (LittleFS/SD, reached
 * through one coarse process-level "storage" permission -- see
 * core_sdk/storage.h -- built-ins like this one always hold it), so -r/-w/-x
 * can't distinguish "exists but unreadable/unwritable/non-executable" from
 * plain nonexistence the way a real Unix test(1) can; they're implemented
 * as honest probes (an actual open-for-read/open-for-write attempt for a
 * file, same as -d for a directory) rather than bare aliases for -e, but on
 * this filesystem they will in practice agree with -e for anything that
 * exists. -x in particular has no executable-bit equivalent at all here --
 * a directory is "executable" the same way it's readable (traversable, same
 * as -d), and a file mirrors -r, since this shell runs a script by reading
 * and interpreting it, never via an exec permission bit. */
static int shell_condition__file_test(const shell_state_t *state, const char *op, const char *arg) {
    bool is_e = strcmp(op, "-e") == 0;
    bool is_f = strcmp(op, "-f") == 0;
    bool is_d = strcmp(op, "-d") == 0;
    bool is_r = strcmp(op, "-r") == 0;
    bool is_w = strcmp(op, "-w") == 0;
    bool is_x = strcmp(op, "-x") == 0;
    if (!is_e && !is_f && !is_d && !is_r && !is_w && !is_x) return -1;

    char path[BRUCE_STORAGE_PATH_MAX];
    if (!shell_builtins__resolve_path(state, arg, path)) return 1;

    if (is_e) return shell_condition__path_exists(path) ? 0 : 1;
    if (is_d) return shell_condition__is_directory(path) ? 0 : 1;
    if (is_f) return shell_condition__path_exists(path) && !shell_condition__is_directory(path) ? 0 : 1;
    if (is_r || is_x) {
        return shell_condition__is_directory(path) || shell_condition__file_openable(path, BRUCE_STORAGE_OPEN_READ)
                   ? 0
                   : 1;
    }
    /* is_w */
    return shell_condition__is_directory(path) || shell_condition__file_openable(path, BRUCE_STORAGE_OPEN_WRITE) ? 0
                                                                                                                   : 1;
}

/* The innermost tier: no more &&/||/-a/-o left to split on, just a leaf
 * unary/binary test (or a bare truthiness check). */
static int shell_condition__eval_base(const shell_state_t *state, int argc, char **argv) {
    if (argc == 0) return 1;
    if (argc == 1) return shell_condition__truthy(argv[0]) ? 0 : 1;
    if (argc == 2) {
        if (strcmp(argv[0], "-z") == 0) return shell_condition__truthy(argv[1]) ? 1 : 0;
        if (strcmp(argv[0], "-n") == 0) return shell_condition__truthy(argv[1]) ? 0 : 1;
        int file_test = shell_condition__file_test(state, argv[0], argv[1]);
        if (file_test != -1) return file_test;
        stdio__printf("shell: test: %s: unsupported unary operator\n", argv[0]);
        return 2;
    }
    if (argc == 3) {
        const char *op = argv[1];
        if (strcmp(op, "=") == 0) return strcmp(argv[0], argv[2]) == 0 ? 0 : 1;
        if (strcmp(op, "!=") == 0) return strcmp(argv[0], argv[2]) != 0 ? 0 : 1;
        if (shell_condition__is_numeric_op(op)) {
            long a, b;
            if (!shell_condition__numeric(argv[0], &a) || !shell_condition__numeric(argv[2], &b)) {
                stdio__printf("shell: test: integer expression expected\n");
                return 2;
            }
            return shell_condition__compare(op, a, b);
        }
        stdio__printf("shell: test: %s: unsupported operator\n", op);
        return 2;
    }
    stdio__printf("shell: test: too many arguments\n");
    return 2;
}

/* The AND tier (&&/-a), binding tighter than OR below: finds the first
 * top-level occurrence of either spelling and recurses left/right around
 * it, right-recursing into itself so a chain ("A -a B -a C" / "A && B &&
 * C") still associates left-to-right, same shape as the original
 * -a-only version this replaced. */
static int shell_condition__eval_and(const shell_state_t *state, int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-a") != 0 && strcmp(argv[i], "&&") != 0) continue;
        if (i == 0 || i == argc - 1) {
            stdio__printf("shell: test: %s: missing operand\n", argv[i]);
            return 2;
        }
        int left = shell_condition__eval_base(state, i, argv);
        if (left == 2) return 2;
        int right = shell_condition__eval_and(state, argc - i - 1, argv + i + 1);
        if (right == 2) return 2;
        return left == 0 && right == 0 ? 0 : 1;
    }
    return shell_condition__eval_base(state, argc, argv);
}

/* The OR tier (||/-o): same shape as shell_condition__eval_and() above, one
 * level looser -- splits on the first top-level ||/-o (if any), evaluating
 * each side through the AND tier so "A && B || C" reads as "(A && B) || C"
 * rather than "A && (B || C)". Falls straight through to the AND tier when
 * there's no ||/-o at all, so a plain "test"/"[" expression with only -a
 * (or none of these operators at all) behaves exactly as before. This is
 * shell_condition__run()'s own entry point into the whole expression. */
static int shell_condition__eval_or(const shell_state_t *state, int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-o") != 0 && strcmp(argv[i], "||") != 0) continue;
        if (i == 0 || i == argc - 1) {
            stdio__printf("shell: test: %s: missing operand\n", argv[i]);
            return 2;
        }
        int left = shell_condition__eval_and(state, i, argv);
        if (left == 2) return 2;
        int right = shell_condition__eval_or(state, argc - i - 1, argv + i + 1);
        if (right == 2) return 2;
        return left == 0 || right == 0 ? 0 : 1;
    }
    return shell_condition__eval_and(state, argc, argv);
}

int shell_condition__run(shell_state_t *state, int argc, char **argv) {
    if (argc < 1) return 2;
    bool single_bracket = strcmp(argv[0], "[") == 0;
    bool double_bracket = strcmp(argv[0], "[[") == 0;
    int expr_argc = argc - 1;
    char **expr_argv = argv + 1;
    if (single_bracket || double_bracket) {
        const char *closer = single_bracket ? "]" : "]]";
        if (expr_argc == 0 || strcmp(expr_argv[expr_argc - 1], closer) != 0) {
            stdio__printf("shell: %s: missing '%s'\n", argv[0], closer);
            return 2;
        }
        expr_argc--;
    }
    return shell_condition__eval_or(state, expr_argc, expr_argv);
}
