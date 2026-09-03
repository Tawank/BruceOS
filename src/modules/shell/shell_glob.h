#pragma once

#include <stdbool.h>
#include <stddef.h>

/* A small, self-contained fnmatch()-style glob matcher, plus the real
 * pathname-expansion (filesystem-globbing) it backs -- shared by `case`/
 * `esac` pattern matching (shell_compound.c, which never touches the
 * filesystem, only compares one already-expanded string against a pattern)
 * and shell_parser__words()'s own real pathname expansion below (which
 * does). Not part of the public core_sdk/ API. */

/* `*` matches any run of characters (including none), `?` matches exactly
 * one, and `[...]`/`[!...]`/`[^...]` matches a bracket expression (inclusive
 * "lo-hi" ranges, a leading ']' right after '[' or '[!'/'[^' is itself a
 * literal member rather than closing the class early, and an unterminated
 * '[' falls back to matching itself as one ordinary literal character --
 * the usual glob leniency for a stray unclosed bracket); anything else
 * matches itself literally. Standard iterative backtracking on the last `*`
 * seen, so this runs in bounded stack space regardless of pattern/text
 * length. */
bool shell_glob__match(const char *pattern, const char *text);

/* True if `word` contains a glob metacharacter ('*', '?', or '[') that
 * shell_glob__expand_path() below might act on -- a quick check so callers
 * can skip the filesystem walk entirely for an ordinary word. This function
 * itself has no idea whether `word` was quoted; it's shell_parser__words()'s
 * own job (via its `any_quoted` tracking, see its doc comment) to only call
 * shell_glob__expand_path() at all for a word that had no quoting or
 * backslash-escaping anywhere in it, matching bash's real rule that a quoted
 * or escaped glob character is always literal, never a wildcard. `case`'s
 * own pattern matching (shell_compound.c) is unaffected by any of this: it
 * calls shell_glob__match() directly, on a pattern word that's a separate
 * thing from the value being tested, never through this quoting gate. */
bool shell_glob__has_metachars(const char *word);

/* Expands `pattern` -- an absolute or `pwd`-relative path that may contain
 * glob metacharacters in any one (or several) of its '/'-separated
 * components, not just the last one -- e.g. a directory component made of
 * "[abc]" plus a wildcard filename component -- against real files and
 * directories, walking one path component at a time (storage__list() on
 * every directory matched so far), matching each component with
 * shell_glob__match() above; a component with no metacharacter at all is
 * still matched (an exact-name comparison) against real listed entries the
 * same way, so e.g. "sub/exact.txt" only yields a path where "exact.txt"
 * genuinely exists. Like bash, a component's '*'/'?'/bracket-expression
 * never matches a name that itself starts with '.' unless the pattern
 * component itself starts with '.' too (dotfiles are hidden from wildcards
 * by default, though an exact literal component like ".bashrc" still
 * matches one normally); this shell's storage__list() never returns "."/
 * ".." pseudo-entries, so a literal "." or ".." component is instead
 * resolved by direct path manipulation (stay put / drop the last
 * component) rather than by listing, same as this shell's other path
 * resolution (see shell_builtins__resolve_path()). Results are sorted
 * (strcmp) the way bash's own glob results are, and mirror the literal
 * absolute/relative shape `pattern` itself was written with -- a relative
 * pattern (`pwd`-resolved only internally, for the actual storage__list()
 * calls) yields relative results with no $PWD prefix grafted on, exactly
 * like bash's own "echo *.txt" -> "a.txt", not "/cwd/a.txt".
 *
 * On a real match, returns a heap-allocated (memory__malloc()) array of
 * `*out_count` heap-allocated (memory__malloc()) path strings the caller
 * owns (release with shell_glob__free_matches()). Returns NULL with
 * *out_count == 0 when nothing at all matched -- not a failure; the
 * caller's own job is to fall back to `pattern` itself, unchanged, exactly
 * like nullglob-off bash -- or when a fixed safety cap on total matches
 * (a runaway-expansion guard, not a realistic script's needs -- see
 * shell_glob.c) is hit partway through, reported the exact same way rather
 * than as a separate error. `pwd` is the shell's current working directory,
 * used to resolve a relative `pattern` (one not starting with '/'); ignored
 * otherwise. */
char **shell_glob__expand_path(const char *pattern, const char *pwd, size_t *out_count);
void shell_glob__free_matches(char **matches, size_t count);
