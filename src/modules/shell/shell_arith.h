#pragma once

/* Evaluates the shell's `((...))` arithmetic-expression syntax: a small,
 * C-like subset of bash's `((...))`/`$((...))` grammar. Not part of the
 * public core_sdk/ API. */

#include <stdbool.h>
#include <stddef.h>

#include "shell_internal.h"

/* Evaluates `text[0..length)` -- the expression between "((" and "))",
 * *not* including those four characters themselves -- as a bash-style
 * arithmetic expression and stores the result in *out_value. Bare
 * identifiers ("x") and "$x" both read/write the shell variable `x` (via
 * shell_builtins__get()/set()); an unset or non-numeric variable reads as 0,
 * matching bash's own arithmetic-context coercion.
 *
 * Supported, left-to-right by precedence (lowest to highest): the comma
 * operator; assignment (=, +=, -=, *=, /=, %=, right-associative); logical
 * || and &&; bitwise |, ^, &; equality == and !=; relational
 * <, <=, >, >=; shifts << and >>; additive + and -; multiplicative *, /, %;
 * unary +, -, !, ~ and prefix/postfix ++/--; and parenthesized grouping.
 * && and || are NOT short-circuited (both sides are always evaluated, same
 * simplification as -a in shell_condition.c); the ternary `? :` operator is
 * unsupported, matching this implementation's basic scope.
 *
 * Returns false with *error set to a human-readable message (syntax error,
 * division/modulo by zero, prefix/postfix ++/-- or assignment applied to
 * something other than a variable, ...) on failure; on success returns true
 * with *out_value holding the expression's final value. */
bool shell_arith__eval(shell_state_t *state, const char *text, size_t length, long *out_value, const char **error);
