#include "shell_condition.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/stdio.h"

/* Basic bash-like test(1) subset: unary -z/-n, string = and !=, the six
 * numeric comparisons, and -a as a left-associative logical AND joining two
 * sub-expressions. Anything else (file tests, -o, !, parenthesized groups,
 * [[ ]]'s pattern/regex matching, ...) is intentionally out of scope. */

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

/* Evaluates the expression tokens argv[0..argc). Returns 0/1/2 like
 * shell_condition__run(); recurses once per top-level "-a" so "A -a B -a C"
 * (any of A/B/C themselves one of the forms below) works left-to-right. */
static int shell_condition__eval(int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-a") != 0) continue;
        if (i == 0 || i == argc - 1) {
            stdio__printf("shell: test: -a: missing operand\n");
            return 2;
        }
        int left = shell_condition__eval(i, argv);
        if (left == 2) return 2;
        int right = shell_condition__eval(argc - i - 1, argv + i + 1);
        if (right == 2) return 2;
        return left == 0 && right == 0 ? 0 : 1;
    }

    if (argc == 0) return 1;
    if (argc == 1) return shell_condition__truthy(argv[0]) ? 0 : 1;
    if (argc == 2) {
        if (strcmp(argv[0], "-z") == 0) return shell_condition__truthy(argv[1]) ? 1 : 0;
        if (strcmp(argv[0], "-n") == 0) return shell_condition__truthy(argv[1]) ? 0 : 1;
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

int shell_condition__run(int argc, char **argv) {
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
    return shell_condition__eval(expr_argc, expr_argv);
}
