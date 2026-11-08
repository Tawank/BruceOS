#pragma once

/* Bash-style indexed arrays: "NAME=(a b c)" / "NAME+=(a b c)" literal
 * assignment, "${NAME[idx]}" element lookup, "${#NAME[@]}" element count, and
 * "unset NAME[idx]" (leaving a sparse hole, matching real bash). A separate
 * table from shell_state_t's scalar `variables` (see shell_internal.h) rather
 * than overloading it with mangled names, for the same reason shell_jobs.c
 * and shell_builtins.c's own variable table are already split apart: each
 * has its own distinct shape (a job's pid/number vs. a variable's
 * name/value), and array elements -- a name plus a growable, sparse list of
 * owned strings -- don't fit the plain name/value/exported shape scalars
 * use. Not part of the public core_sdk/ API, same restriction as every other
 * shell_*.h in this directory. */

#include <stddef.h>

#include "shell_internal.h"

/* Finds `name` among state->arrays, or -1 if it doesn't exist yet (never
 * created, or removed by shell_array__unset()). A name can be a scalar
 * variable OR an array, never both at once -- callers that need to tell
 * those apart (e.g. shell_executor__lookup()) check this first. */
int shell_array__find(const shell_state_t *state, const char *name);

/* "${NAME[index]}" -- the element at `index`, or NULL if the array doesn't
 * exist, `index` is out of its current range, or that index is a sparse hole
 * (see shell_array__unset_index() below). Borrowed; valid until the next
 * mutation of this array. */
const char *shell_array__get(const shell_state_t *state, const char *name, size_t index);

/* Element count for "${#NAME[@]}" -- counts only indices that actually hold a
 * value (a hole left by shell_array__unset_index() doesn't count, matching
 * real bash), not state->arrays[i].capacity or the highest index ever
 * assigned. 0 for an array that doesn't exist (same as bash's "${#undef[@]}"). */
size_t shell_array__length(const shell_state_t *state, const char *name);

/* One past the highest index ever assigned to `name` -- NOT
 * shell_array__length()'s "how many elements are actually set" count, which
 * skips holes and so is *smaller* whenever one exists. This is the correct
 * loop bound for "visit every index in order, including any hole, and
 * decide per-index what to do with it" (shell_parser.c's "${NAME[@]}"
 * word-splitting is exactly this: it still needs to skip a hole's index,
 * but must not stop *short* of a later, still-set index just because an
 * earlier one happened to be a hole). 0 for an array that doesn't exist. */
size_t shell_array__index_bound(const shell_state_t *state, const char *name);

/* Sets element `index` of `name` to a copy of `value`, creating the array (or
 * growing its backing storage) as needed -- any newly-exposed index below
 * `index` that wasn't already set becomes a hole (NULL), same as bash
 * extending an array past its current length. Returns 0 on success, or a
 * builtin-style exit code (1 out of memory / resource limit, 2 invalid name)
 * with an error already printed, matching shell_builtins__set()'s own
 * convention. */
int shell_array__set(shell_state_t *state, const char *name, size_t index, const char *value);

/* Appends `value` as a new element one past `name`'s current highest-ever
 * index (not its element *count* -- see shell_array__length()'s own doc
 * comment on the hole distinction) -- "NAME+=(value)"'s per-element
 * primitive. Creates the array if it doesn't exist yet, same as
 * shell_array__set(). */
int shell_array__append(shell_state_t *state, const char *name, const char *value);

/* Replaces the whole array `name` with exactly `count` elements copied from
 * `values` (which may be NULL/0 for an empty array), freeing whatever it
 * held before -- "NAME=(a b c)"'s whole-array primitive, also what
 * "NAME=("${NAME[@]}")" reindex/compaction (see shell_parser.c's "${arr[@]}"
 * word-splitting) relies on to collapse sparse holes back out. Creates the
 * array if it doesn't exist yet. Same return convention as
 * shell_array__set(). */
int shell_array__assign(shell_state_t *state, const char *name, char *const *values, size_t count);

/* "unset NAME[index]" -- frees and clears just that one element, leaving a
 * hole (shell_array__get() then returns NULL for it, shell_array__length()
 * no longer counts it) without touching any other index or the array's own
 * highest-ever-index bookkeeping. A no-op if the array or index doesn't
 * exist. */
void shell_array__unset_index(shell_state_t *state, const char *name, size_t index);

/* "unset NAME" when NAME is an array -- removes the whole array and frees
 * every element it still held. A no-op if `name` isn't a tracked array. */
void shell_array__unset(shell_state_t *state, const char *name);

/* Every element of `name`, in index order, skipping holes, joined by a single
 * space -- "${NAME[@]}"/"${NAME[*]}" in any context *other* than the one
 * special "${NAME[@]}" case that splits into separate words instead (see
 * shell_parser.c's own doc comment on that). NULL for an array that doesn't
 * exist (same as a plain unset scalar's NULL, not an empty string) or on
 * allocation failure; otherwise caller-owned (memory__free()), even for an
 * existing-but-empty array (returns a real, owned "" then). */
char *shell_array__join(const shell_state_t *state, const char *name);

/* Frees every array in state->arrays and the table itself -- called once
 * from shell__state_free(). */
void shell_array__free_all(shell_state_t *state);
