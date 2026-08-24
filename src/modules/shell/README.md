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
- `>` / `>>` output redirection to a file, for an external command only (see
  below).
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
  here-docs/here-strings, and input redirection (`<` is rejected with a
  parse error outside of `((...))`; see the Redirection section below for
  what `>`/`>>` do support).

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

**Redirection — `>` / `>>`** (`shell_parser.c`, `shell_executor.c`)
- A single, trailing `> file` or `>> file` on an **external command** (not a
  builtin, not a shell function): `>` truncates/creates the file, `>>`
  creates/appends. The target undergoes the same quoting/escaping/`$`
  expansion as any other word (`wifi scan >> "$LOG_DIR/wifi.txt"` works), and
  a relative target resolves against `$PWD` the same way `cd`'s argument
  does. A bare `> file` / `>> file` with no command at all just
  creates/truncates the file, same as bash's `: > file` idiom.
- Implementation: the whole external command's stdout is captured into an
  in-memory buffer (the same mechanism `|` piping uses — see below) and
  written to the file once the command exits, rather than streaming to the
  file as the command runs. Consequently, like a pipe's producer side, any
  `NAME=value` assignment prefixed onto the redirected command is dropped
  (not passed to the child) and the command always runs to completion before
  its output is written — there is no live/streaming redirection.
- Only one output redirection per command is recognized, and it must trail
  the command (`cmd arg1 arg2 > file`, not `cmd > file arg2`); a second `>`
  or trailing text after the target is a syntax error rather than being
  silently misparsed.
- Redirecting a builtin, a shell function, or a standalone `((...))`
  statement is rejected with an error (`shell: output redirection currently
  requires an external command`) rather than doing nothing silently.
- **Not implemented:** input redirection (`<`, rejected with a parse error),
  multiple/chained redirections, numbered file descriptors (`2>`), here-docs,
  and streaming (a redirected command's output is only written to the file
  after the whole command has finished, and only if it exited successfully —
  a command that writes partial output before exiting nonzero has that
  partial output discarded, not written).

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
- The text inside `((...))` is evaluated as-is, *not* run through the normal
  `$`-word-expansion pass — variable references there are resolved directly
  by the evaluator, with a leading `$` accepted but optional (`((x = x + 1))`
  and `((x = $x + 1))` both work). Consequently only names shaped like a
  shell identifier (`[A-Za-z_][A-Za-z0-9_]*`) are usable inside `((...))`:
  positional parameters (`$1`, `$2`, ...) do **not** parse there, since they
  don't start with a letter/`_`. Copy one into a named variable first
  (`n=$1`) and use `n` inside the arithmetic.
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
  Recursion works (positional parameters are saved/restored per call), so
  the usual workaround for "no locals" is an accumulator-passing style where
  each level only reads its own arguments *before* recursing and does
  nothing with global state after the recursive call returns — see the
  `factorial` example below.
- There is no `return` builtin. A function's exit status is simply whatever
  its last executed command's status was, same as a plain script; use
  `if`/`elif`/`else` to skip the rest of the body instead of returning
  early. A `break` with no enclosing loop (including one used to try to
  "return" out of a function) is reported as a stray break, not absorbed
  silently.
- **Not implemented:** `case`/`esac`, `select`, subshells `(...)`, process
  substitution, `local`, `until`, and command grouping with `{ ...; }` used
  as a value (only as a function body).

## What's left to implement

Roughly in order of how often bash scripts actually use them:
- Command substitution `$(...)`/`` `...` `` and arithmetic expansion as a
  word, `$((...))`.
- Input redirection (`<`) and here-docs; streaming (rather than
  capture-then-write) `>`/`>>` output redirection; redirecting a builtin or
  shell function's output.
- Pathname globbing and brace expansion.
- `case`/`esac`.
- `local` (function-scoped variables).
- `$@`, `$*`, `$$`, `$!`.
- File-test operators for `test`/`[` (`-e -f -d -r -w -x ...`) and `-o`/`[[
  ... || ... ]]`-in-tests.
- `until` loops, C-style `((...))` outside of `for` headers used as a word
  (already works as a statement/condition).
- Comments (`#`).

## Example

A script exercising most of the above at once: recursive functions (via the
accumulator trick, since there's no `local`/`return`), C-style and word-list
`for`, `while` + `read`, nested loops with `break 2`, and `((...))`
arithmetic including the "copy `$N` into a name first" rule.

```sh
factorial() {
    acc=$1
    n=$2
    if [ $n -le 1 ]; then
        FACT=$acc
    else
        ((next_acc = acc * n))
        ((next_n = n - 1))
        factorial $next_acc $next_n
    fi
}

is_prime() {
    n=$1
    PRIME=1
    if [ $n -lt 2 ]; then
        PRIME=0
    else
        for ((d = 2; d * d <= n; d++)); do
            ((rem = n % d))
            if [ $rem -eq 0 ]; then
                PRIME=0
                break
            fi
        done
    fi
}

echo "== factorials =="
for k in 1 2 3 4 5 6 7; do
    factorial 1 $k
    echo "$k! = $FACT"
done

echo "== primes under 30 =="
for ((m = 2; m < 30; m++)); do
    is_prime $m
    if [ $PRIME -eq 1 ]; then
        echo $m
    fi
done

echo "== multiplication table, stops once a cell exceeds 25 =="
for ((row = 1; row <= 9; row++)); do
    for ((col = 1; col <= 9; col++)); do
        ((cell = row * col))
        if [ $cell -gt 25 ]; then
            break 2
        fi
        echo "$row x $col = $cell"
    done
done

echo "== guess the number (1-100), type 'quit' to give up =="
target=42
attempts=0
while true; do
    read guess
    if [ "$guess" = "quit" ]; then
        echo "the number was $target"
        break
    fi
    ((attempts++))
    if [ $guess -eq $target ]; then
        echo "correct in $attempts tries!"
        break
    elif [ $guess -lt $target ]; then
        echo "too low"
    else
        echo "too high"
    fi
done
```

`factorial` and `is_prime` both start by copying their positional parameter
into a plain named variable (`n=$1`) — required, since `$1` can't appear
inside `((...))` directly. `factorial` passes an accumulator (`acc`) down
through the recursion and only sets `FACT` at the base case; every returning
frame does nothing further, so the shared global `acc`/`n` being overwritten
by deeper calls is harmless. The multiplication table's `break 2` unwinds
both the `col` and `row` loops as soon as one cell exceeds 25, rather than
just skipping the rest of the current row.

## Selftests

`src/modules/selftest/shell_test.c` covers the language layer end-to-end
against real firmware output (via `stdio__session_*` + `app_runner__run`):
`selftest__run_shell_language_case`, `..._script_case`,
`..._control_flow_case`, `..._multiline_case`, `..._loops_case` (arithmetic,
both `for` forms, `while`, `break`, `break N`, and the break/catch-up
regression), `..._read_case`, `..._stdio_inheritance_case`, and
`..._tty_size_case`.
