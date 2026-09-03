#pragma once

#include <stdbool.h>
#include <stddef.h>

/* "{a,b,c}"-style brace (comma-list) expansion -- purely textual, and run by
 * shell_parser__words() before any of the rest of its per-word processing
 * ($NAME/"$(...)"/pathname expansion/quote removal) sees the word at all,
 * the same order bash itself uses. Only a comma-list is supported here, not
 * bash's "{1..5}"/"{a..e}" sequence-range spelling. Not part of the public
 * core_sdk/ API. */

/* True if `text` (`length` bytes, not NUL-terminated) contains at least one
 * unquoted, unescaped "{...}" group with a top-level comma inside it --
 * shell_parser__words() uses this to decide, cheaply, whether a word needs
 * brace expansion at all before paying for shell_brace__expand() below. A
 * "{...}" with no top-level comma (e.g. "{foo}") does not count -- like
 * bash, it's left completely literal, not even considered a group. Quoting
 * (single, double, or backslash) suppresses this the same way it suppresses
 * shell_glob__has_metachars() candidates for pathname expansion (see
 * shell_glob.h) -- a brace group only counts when it's genuinely unquoted. */
bool shell_brace__has_group(const char *text, size_t length);

/* Expands every "{...}" group in `text` (`length` bytes, not NUL-terminated)
 * into its literal combinations: "file{1,2}.txt" -> "file1.txt" and
 * "file2.txt"; multiple groups in one word combine as a cartesian product
 * ("{a,b}{1,2}" -> "a1", "a2", "b1", "b2"); a group can itself contain
 * another group ("{a,{b,c}}" -> "a", "b", "c"), expanded recursively up to a
 * fixed nesting-depth safety cap. `text` is otherwise untouched -- still
 * carrying any '$'/quoting/glob syntax it had -- since each combination goes
 * back through shell_parser__words()'s own normal per-word processing
 * afterward, the same as bash runs brace expansion strictly before (and
 * independently of) the rest of word expansion.
 *
 * On success, returns true and sets *out_variants to a heap-allocated
 * (memory__malloc()) array of *out_count heap-allocated, NUL-terminated
 * strings the caller owns (release with shell_brace__free()) -- always at
 * least 1, even when `text` has no group at all (a single verbatim copy of
 * `text`, letting a caller that already checked shell_brace__has_group()
 * skip calling this in the first place, but not requiring it to). Returns
 * false (with *error set to a static string) only on a real failure: too
 * many combinations (SHELL_BRACE__MAX_ALTS in shell_brace.c -- a
 * runaway-expansion guard, not a realistic script's needs), nesting too
 * deep, or out of memory. */
bool shell_brace__expand(const char *text, size_t length, char ***out_variants, size_t *out_count, const char **error);
void shell_brace__free(char **variants, size_t count);
