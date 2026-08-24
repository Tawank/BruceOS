#include "shell_arith.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell_builtins.h"

/* Recursive-descent parser/evaluator over the raw text directly (no separate
 * tokenizer pass) -- the same approach shell_parser.c takes for word
 * splitting. `p`/`end` bound the remaining unparsed text; `error` is set
 * once, by whichever parse_* function first fails, and left alone by every
 * caller unwinding above it. */
typedef struct {
    const char *p;
    const char *end;
    shell_state_t *state;
    const char *error;
} shell_arith__cursor_t;

/* A parsed sub-expression's value, plus (when it was exactly a bare
 * variable reference, not something computed from one) the variable's name,
 * so a caller one level up -- assignment, ++/-- -- can write back through
 * it. Anything computed (a sum, a parenthesized group, ...) has
 * is_lvalue == false and must not be assigned/incremented. */
typedef struct {
    long value;
    bool is_lvalue;
    char name[SHELL__VARIABLE_NAME_MAX];
} shell_arith__value_t;

static void shell_arith__skip_ws(shell_arith__cursor_t *c) {
    while (c->p < c->end && isspace((unsigned char)*c->p)) c->p++;
}

static bool shell_arith__store(shell_arith__cursor_t *c, const char *name, long value) {
    char text[24];
    snprintf(text, sizeof(text), "%ld", value);
    if (shell_builtins__set(c->state, name, text) != 0) {
        c->error = "assignment failed";
        return false;
    }
    return true;
}

static long shell_arith__load(shell_arith__cursor_t *c, const char *name) {
    const char *text = shell_builtins__get(c->state, name);
    if (text == NULL || text[0] == '\0') return 0;
    char *stop = NULL;
    long value = strtol(text, &stop, 0);
    return stop == text ? 0 : value; /* a non-numeric variable reads as 0, matching bash */
}

/* Consumes an optional leading '$' and then a NAME, without requiring
 * anything to follow -- used both for a plain variable reference in
 * parse_primary() and for the assignment lookahead in parse_assign(). */
static bool shell_arith__read_name(shell_arith__cursor_t *c, char *name_out, size_t cap) {
    if (c->p < c->end && *c->p == '$') c->p++;
    if (c->p >= c->end || !(isalpha((unsigned char)*c->p) || *c->p == '_')) {
        c->error = "syntax error";
        return false;
    }
    size_t n = 0;
    while (c->p < c->end && (isalnum((unsigned char)*c->p) || *c->p == '_')) {
        if (n + 1 >= cap) {
            c->error = "variable name too long";
            return false;
        }
        name_out[n++] = *c->p++;
    }
    name_out[n] = '\0';
    return true;
}

static bool shell_arith__parse_comma(shell_arith__cursor_t *c, shell_arith__value_t *out);

static bool shell_arith__parse_primary(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    shell_arith__skip_ws(c);
    if (c->p >= c->end) {
        c->error = "unexpected end of expression";
        return false;
    }
    char ch = *c->p;
    if (ch == '(') {
        c->p++;
        if (!shell_arith__parse_comma(c, out)) return false;
        shell_arith__skip_ws(c);
        if (c->p >= c->end || *c->p != ')') {
            c->error = "missing ')'";
            return false;
        }
        c->p++;
        out->is_lvalue = false; /* "(x)++" is unsupported, matching this basic implementation's scope */
        return true;
    }
    if (ch == '$' || isalpha((unsigned char)ch) || ch == '_') {
        char name[SHELL__VARIABLE_NAME_MAX];
        if (!shell_arith__read_name(c, name, sizeof(name))) return false;
        out->value = shell_arith__load(c, name);
        out->is_lvalue = true;
        memcpy(out->name, name, sizeof(out->name));
        return true;
    }
    if (isdigit((unsigned char)ch)) {
        char *stop = NULL;
        long value = strtol(c->p, &stop, 0); /* base 0: plain decimal or a 0x/0-prefixed literal */
        if (stop == c->p) {
            c->error = "invalid number";
            return false;
        }
        c->p = stop;
        out->value = value;
        out->is_lvalue = false;
        return true;
    }
    c->error = "syntax error";
    return false;
}

static bool shell_arith__parse_postfix(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    if (!shell_arith__parse_primary(c, out)) return false;
    shell_arith__skip_ws(c);
    if (out->is_lvalue && c->p + 1 < c->end && c->p[0] == c->p[1] && (c->p[0] == '+' || c->p[0] == '-')) {
        bool increment = c->p[0] == '+';
        c->p += 2;
        long previous = out->value;
        if (!shell_arith__store(c, out->name, increment ? previous + 1 : previous - 1)) return false;
        out->value = previous; /* postfix yields the pre-update value */
        out->is_lvalue = false;
    }
    return true;
}

static bool shell_arith__parse_unary(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    shell_arith__skip_ws(c);
    if (c->p + 1 < c->end && c->p[0] == c->p[1] && (c->p[0] == '+' || c->p[0] == '-')) {
        bool increment = c->p[0] == '+';
        c->p += 2;
        shell_arith__value_t inner;
        if (!shell_arith__parse_unary(c, &inner)) return false;
        if (!inner.is_lvalue) {
            c->error = increment ? "++ requires a variable" : "-- requires a variable";
            return false;
        }
        long updated = increment ? inner.value + 1 : inner.value - 1;
        if (!shell_arith__store(c, inner.name, updated)) return false;
        out->value = updated; /* prefix yields the post-update value */
        out->is_lvalue = false;
        return true;
    }
    if (c->p < c->end && (*c->p == '-' || *c->p == '+' || *c->p == '!' || *c->p == '~')) {
        char op = *c->p;
        c->p++;
        shell_arith__value_t inner;
        if (!shell_arith__parse_unary(c, &inner)) return false;
        out->value = op == '-' ? -inner.value
                     : op == '+' ? inner.value
                     : op == '!' ? (inner.value == 0 ? 1 : 0)
                                  : ~inner.value;
        out->is_lvalue = false;
        return true;
    }
    return shell_arith__parse_postfix(c, out);
}

/* One shared "left op right, left-associative" helper covers every binary
 * precedence level below unary: `next` parses one operand at the next
 * tighter precedence, `ops` is a NUL-terminated array of one-or-two-char
 * operator spellings to accept at this level (checked longest-first so "<="
 * doesn't get cut short as "<"), and `apply` combines two already-evaluated
 * operands. Matches every level's actual shape in the grammar -- the
 * result of "a op b" is never itself an lvalue. */
typedef bool (*shell_arith__level_fn)(shell_arith__cursor_t *c, shell_arith__value_t *out);
typedef bool (*shell_arith__apply_fn)(shell_arith__cursor_t *c, const char *op, long a, long b, long *out);

static bool shell_arith__parse_binary(
    shell_arith__cursor_t *c, shell_arith__value_t *out, shell_arith__level_fn next, const char *const *ops,
    shell_arith__apply_fn apply
) {
    if (!next(c, out)) return false;
    for (;;) {
        shell_arith__skip_ws(c);
        const char *matched = NULL;
        size_t matched_len = 0;
        for (size_t i = 0; ops[i] != NULL; ++i) {
            size_t len = strlen(ops[i]);
            if ((size_t)(c->end - c->p) >= len && memcmp(c->p, ops[i], len) == 0 &&
                (len > matched_len || matched == NULL)) {
                matched = ops[i];
                matched_len = len;
            }
        }
        /* A lone '&' or '|' immediately followed by another of itself is
         * really the start of "&&"/"||" -- those belong to the logical-and
         * /logical-or levels above bitand/bitor, never to a single-char
         * bitwise op here, so this level must stop and let them consume it
         * instead of misreading half of the doubled token as its own
         * operand boundary. */
        if (matched != NULL && matched_len == 1 && (matched[0] == '&' || matched[0] == '|') &&
            c->p + 1 < c->end && c->p[1] == matched[0]) {
            matched = NULL;
        }
        if (matched == NULL) return true;
        c->p += matched_len;
        shell_arith__value_t rhs;
        if (!next(c, &rhs)) return false;
        long result;
        if (!apply(c, matched, out->value, rhs.value, &result)) return false;
        out->value = result;
        out->is_lvalue = false;
    }
}

static bool shell_arith__apply_mult(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    if (op[0] == '*') {
        *out = a * b;
        return true;
    }
    if (b == 0) {
        c->error = op[0] == '/' ? "division by zero" : "modulo by zero";
        return false;
    }
    *out = op[0] == '/' ? a / b : a % b;
    return true;
}

static bool shell_arith__apply_add(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    *out = op[0] == '+' ? a + b : a - b;
    return true;
}

static bool shell_arith__apply_shift(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    *out = op[0] == '<' ? a << b : a >> b;
    return true;
}

static bool shell_arith__apply_relational(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    bool result = strcmp(op, "<=") == 0   ? a <= b
                  : strcmp(op, ">=") == 0 ? a >= b
                  : op[0] == '<'          ? a < b
                                          : a > b;
    *out = result ? 1 : 0;
    return true;
}

static bool shell_arith__apply_equality(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    *out = (op[0] == '=') == (a == b) ? 1 : 0;
    return true;
}

static bool shell_arith__apply_bitand(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    (void)op;
    *out = a & b;
    return true;
}

static bool shell_arith__apply_bitxor(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    (void)op;
    *out = a ^ b;
    return true;
}

static bool shell_arith__apply_bitor(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    (void)op;
    *out = a | b;
    return true;
}

/* && and || are intentionally NOT short-circuited: both operands are always
 * evaluated (including any ++/--/assignment side effects they contain),
 * same simplification -a in shell_condition.c makes for its own logical
 * AND. */
static bool shell_arith__apply_and(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    (void)op;
    *out = (a != 0 && b != 0) ? 1 : 0;
    return true;
}

static bool shell_arith__apply_or(shell_arith__cursor_t *c, const char *op, long a, long b, long *out) {
    (void)c;
    (void)op;
    *out = (a != 0 || b != 0) ? 1 : 0;
    return true;
}

static bool shell_arith__parse_mult(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"*", "/", "%", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_unary, ops, shell_arith__apply_mult);
}
static bool shell_arith__parse_additive(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"+", "-", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_mult, ops, shell_arith__apply_add);
}
static bool shell_arith__parse_shift(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"<<", ">>", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_additive, ops, shell_arith__apply_shift);
}
static bool shell_arith__parse_relational(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"<=", ">=", "<", ">", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_shift, ops, shell_arith__apply_relational);
}
static bool shell_arith__parse_equality(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"==", "!=", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_relational, ops, shell_arith__apply_equality);
}
static bool shell_arith__parse_bitand(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"&", NULL}; /* "&&" is excluded above and matched by parse_logical_and instead */
    return shell_arith__parse_binary(c, out, shell_arith__parse_equality, ops, shell_arith__apply_bitand);
}
static bool shell_arith__parse_bitxor(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"^", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_bitand, ops, shell_arith__apply_bitxor);
}
static bool shell_arith__parse_bitor(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"|", NULL}; /* "||" is excluded above and matched by parse_logical_or instead */
    return shell_arith__parse_binary(c, out, shell_arith__parse_bitxor, ops, shell_arith__apply_bitor);
}
static bool shell_arith__parse_logical_and(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"&&", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_bitor, ops, shell_arith__apply_and);
}
static bool shell_arith__parse_logical_or(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    static const char *const ops[] = {"||", NULL};
    return shell_arith__parse_binary(c, out, shell_arith__parse_logical_and, ops, shell_arith__apply_or);
}

/* Assignment binds tighter than nothing but the comma operator, and is
 * right-associative ("a = b = 1"), so it's tried first via a
 * save-and-backtrack lookahead: read a NAME, and if an assignment operator
 * genuinely follows it, recurse for the right-hand side and store; anything
 * else (no NAME, or a NAME that turns out to just be a value used in a
 * larger expression, e.g. "x + 1") rewinds the cursor and falls through to
 * the logical-or level instead. */
static bool shell_arith__parse_assign(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    const char *saved = c->p;
    shell_arith__skip_ws(c);
    char name[SHELL__VARIABLE_NAME_MAX];
    if (c->p < c->end && (isalpha((unsigned char)*c->p) || *c->p == '_' || *c->p == '$')) {
        if (shell_arith__read_name(c, name, sizeof(name))) {
            shell_arith__skip_ws(c);
            static const char *const compound_ops[] = {"+=", "-=", "*=", "/=", "%=", NULL};
            const char *matched_op = NULL;
            for (size_t i = 0; compound_ops[i] != NULL; ++i) {
                if (c->p + 1 < c->end && memcmp(c->p, compound_ops[i], 2) == 0) {
                    matched_op = compound_ops[i];
                    break;
                }
            }
            bool plain_assign = matched_op == NULL && c->p < c->end && *c->p == '=' &&
                                 !(c->p + 1 < c->end && c->p[1] == '=');
            if (matched_op != NULL || plain_assign) {
                c->p += matched_op != NULL ? 2 : 1;
                shell_arith__value_t rhs;
                if (!shell_arith__parse_assign(c, &rhs)) return false;
                long updated = rhs.value;
                if (matched_op != NULL) {
                    long current = shell_arith__load(c, name);
                    switch (matched_op[0]) {
                        case '+': updated = current + rhs.value; break;
                        case '-': updated = current - rhs.value; break;
                        case '*': updated = current * rhs.value; break;
                        case '/':
                            if (rhs.value == 0) {
                                c->error = "division by zero";
                                return false;
                            }
                            updated = current / rhs.value;
                            break;
                        default: /* '%' */
                            if (rhs.value == 0) {
                                c->error = "modulo by zero";
                                return false;
                            }
                            updated = current % rhs.value;
                            break;
                    }
                }
                if (!shell_arith__store(c, name, updated)) return false;
                out->value = updated;
                out->is_lvalue = true;
                memcpy(out->name, name, sizeof(out->name));
                return true;
            }
        } else {
            c->error = NULL; /* not actually malformed -- just not an assignment; let the fallback re-parse it */
        }
    }
    c->p = saved;
    return shell_arith__parse_logical_or(c, out);
}

static bool shell_arith__parse_comma(shell_arith__cursor_t *c, shell_arith__value_t *out) {
    if (!shell_arith__parse_assign(c, out)) return false;
    for (;;) {
        shell_arith__skip_ws(c);
        if (c->p >= c->end || *c->p != ',') return true;
        c->p++;
        if (!shell_arith__parse_assign(c, out)) return false;
    }
}

bool shell_arith__eval(shell_state_t *state, const char *text, size_t length, long *out_value, const char **error) {
    shell_arith__cursor_t cursor = {.p = text, .end = text + length, .state = state, .error = NULL};
    shell_arith__value_t result;
    if (!shell_arith__parse_comma(&cursor, &result)) {
        *error = cursor.error != NULL ? cursor.error : "syntax error";
        return false;
    }
    shell_arith__skip_ws(&cursor);
    if (cursor.p != cursor.end) {
        *error = "syntax error";
        return false;
    }
    *out_value = result.value;
    return true;
}
