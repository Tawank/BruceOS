# Bruce Shell

A small, hand-rolled line-oriented shell interpreter, intentionally a basic
subset of bash rather than a full reimplementation. Parsing is split from
execution: `shell_parser.c` tokenizes a line into a flat list of
`shell_command_t` on `;` / `&&` / `||` / `|` / newline and later
word-splits/expands one command; `shell_executor.c` runs that flat list
respecting connector short-circuit semantics; `shell_compound.c` is a
recursive-descent layer on top of the same flat-command-list representation
that understands multi-line constructs (`if`, `for`, `while`, function
definitions) by raw-text keyword matching in command position.

## Implemented

**Commands and connectors**
- `;` sequencing, `&&` / `||` short-circuiting, `|` single-hop pipes.
- Multi-line input: a bare newline is treated as `;`, so a whole multi-line
  construct parses as one flat command list.
- Comments are not supported (`#` is not special).

**Quoting and expansion**
- Single quotes (`'...'`, no expansion), double quotes (`"..."`, `$`
  expansion still happens inside), and backslash-escaping of the next
  character.
- Variable expansion: `$NAME`, `${NAME}`, `$0`..`$9` (single digit only,
  bash-style — `$10` is `$1` followed by a literal `0`), `$#` (positional
  argument count), `$?` (last exit status). An unset variable or unset
  positional parameter expands to empty string.
- **Not implemented:** command substitution (`` `cmd` `` / `$(cmd)`),
  arithmetic expansion `$((...))` as a *word* (only the standalone `((...))`
  statement form exists — see below), brace expansion (`{a,b}`), tilde
  expansion (`~`), pathname globbing (`*`, `?`, `[...]`), `$@`/`$*`, `$$`,
  here-docs/here-strings, and I/O redirection (`<`, `>`, `>>` are rejected
  with a parse error outside of `((...))`).

**Builtins** (`shell_builtins.c`)
- `echo`, `true`, `false`, `cd`, `set`, `unset`, `export`, `clear`, `reset`,
  `help`, `exit [N]`.
- `test` / `[` / `[[`: `-eq -ne -lt -le -gt -ge` (integer comparison),
  `=` / `!=` (string comparison), `-z` / `-n` (empty/non-empty), `-a`
  (logical AND of chained tests). No `-o`, no file-test operators
  (`-e -f -d ...`), no `-lt`-style operators on `[[`'s `<`/`>`.
- `break [N]`: multi-level loop break, matching bash's `break N`.
- `read [NAME...]`: reads one line from stdin; with no names the line is
  stored in `$REPLY`; with names, splits on whitespace and the last name
  gets the remainder of the line (bash field-splitting behavior). No `-p`,
  `-r`, `-a`, `-t`, etc.
- All builtin names are reserved as command words (see
  `s_shell_builtin_names[]` in `shell_builtins.c`).

**Arithmetic — `((...))`** (`shell_arith.c`, `shell_arith.h`)
- A standalone recursive-descent evaluator, usable as its own statement,
  inside `if`/`while` conditions, and as a C-style `for` header. Status is 0
  (true) if the result is non-zero, 1 otherwise, matching bash.
- Assignment `=` and compound assignment `+= -= *= /= %=` to shell
  variables; pre/post `++`/`--`; unary `+ - !` `~`; the usual C-like
  precedence chain for `* / %`, `+ -`, shifts `<< >>`, relational
  `< <= > >=`, equality `== !=`, bitwise `& ^ |`, logical `&& ||`, and
  parenthesized grouping.
- The tokenizer (`shell_parser.c`) tracks `((...))` spans so that `;`,
  `&&`, `||`, `|`, `<`, `>` inside them are left as literal text instead of
  being parsed as shell connectors/redirection — needed for
  `for ((i=0;i<10;i++))` and `(( a < b ))`.
- **Not implemented:** the ternary operator `?:`, comma operator, array
  subscripts, and floating point (everything is `long`).

**Control flow** (`shell_compound.c`)
- `if` / `elif` / `else` / `fi`, with any command list (including `test`,
  `[[`, or `((...))`) as the condition.
- `for NAME in WORD...; do ...; done` (word-list form) and
  `for ((init; cond; incr)); do ...; done` (C-style form, empty segments
  allowed, e.g. `for ((;;))`). `for NAME; do ...; done` with no `in` clause
  iterates the current function's positional parameters (`$1..`).
- `while COND; do ...; done`. `until` is not implemented.
- Loops cooperatively yield each iteration (`runtime__delay(0)`) and check
  `process__current_signal()` so they don't starve the scheduler or ignore
  cancellation/kill.
- `break [N]` unwinds N enclosing loops without corrupting an outer
  `if`/`for`/`while`'s own parse-position bookkeeping, even when it fires
  mid-branch with more structure still ahead (verified by a dedicated
  regression case in the selftests).
- Function definitions: `NAME() { ... }`, `NAME () { ... }`, and
  `function NAME { ... }`. Functions get their own `$0`/positional
  parameters (`$1..$9`, `$#`) for the duration of the call, but ordinary
  variables are **not** function-scoped — there is no `local`; a variable
  set inside a function is visible everywhere after the call returns.
- **Not implemented:** `case`/`esac`, `select`, subshells `(...)`, process
  substitution, `local`, `until`, and command grouping with `{ ...; }` used
  as a value (only as a function body).

## What's left to implement

Roughly in order of how often bash scripts actually use them:
- Command substitution `$(...)`/`` `...` `` and arithmetic expansion as a
  word, `$((...))`.
- I/O redirection (`>`, `>>`, `<`) and here-docs.
- Pathname globbing and brace expansion.
- `case`/`esac`.
- `local` (function-scoped variables).
- `$@`, `$*`, `$$`, `$!`.
- File-test operators for `test`/`[` (`-e -f -d -r -w -x ...`) and `-o`/`[[
  ... || ... ]]`-in-tests.
- `until` loops, C-style `((...))` outside of `for` headers used as a word
  (already works as a statement/condition).
- Comments (`#`).

## Selftests

`src/modules/selftest/shell_test.c` covers the language layer end-to-end
against real firmware output (via `stdio__session_*` + `app_runner__run`):
`selftest__run_shell_language_case`, `..._script_case`,
`..._control_flow_case`, `..._multiline_case`, `..._loops_case` (arithmetic,
both `for` forms, `while`, `break`, `break N`, and the break/catch-up
regression), `..._read_case`, `..._stdio_inheritance_case`, and
`..._tty_size_case`.
