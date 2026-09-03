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
- `;` sequencing, `&&` / `||` short-circuiting, `|` pipes (any number of
  chained hops, e.g. `a | b | c`).
- `>` / `>>` output redirection to a file, for an external command only (see
  below).
- Multi-line input: a bare newline is treated as `;`, so a whole multi-line
  construct parses as one flat command list.
- Comments: a `#` at the start of a word begins a comment running to the end
  of that physical line (a `#` glued onto other text, e.g. `foo#bar`, is not
  special, same as bash). Works inside `if`/`for`/`while`/function bodies
  too, not just at top level.

**Quoting and expansion**
- Single quotes (`'...'`, no expansion), double quotes (`"..."`, `$`
  expansion still happens inside), and backslash-escaping of the next
  character.
- Variable expansion: `$NAME`, `${NAME}`, `$0`..`$9` (single digit only,
  bash-style — `$10` is `$1` followed by a literal `0`), `$#` (positional
  argument count), `$?` (last exit status). An unset variable or unset
  positional parameter expands to empty string.
- Command substitution: `$(...)` and `` `...` ``, recognized unquoted and
  inside double quotes (only single quotes suppress it, same as `$NAME`) and
  nestable. Its content runs as a nested `shell -c` child process (see
  `shell_executor__run_substitution()` in `shell_executor.c`) — a real,
  separate process, not bash's copy-on-write subshell: it sees exported
  variables and the filesystem, but never the calling shell's own unexported
  variables or function definitions. Trailing newlines are stripped from the
  captured output, and — like a plain `$NAME` expansion — the result is never
  itself word-split. A nonzero exit discards whatever the substitution
  printed, matching `>`/`>>` and `|`'s existing discard-on-failure rule (see
  below) rather than keeping partial output.
- Arithmetic expansion: `$((...))` as a *word* (e.g. `echo $((1 + 2))`),
  alongside the standalone `((...))` statement form (see below). Recognized
  the same way as `$(...)` command substitution — a `$(` span whose content
  is itself wrapped in one more matched pair of parens is unambiguously the
  doubled-paren arithmetic form, since this shell has no subshell `(...)`
  command grouping to disambiguate against. Unlike `$(...)`, this never
  spawns a nested process: it's evaluated in place against this shell's own
  variables by the same evaluator the statement form uses (see
  `shell_executor__eval_arith_word()` in `shell_executor.c`), so an
  assignment inside it (`$((x = 5))`) is a real side effect on `$x` here, not
  something scoped to the expansion.
- **Not implemented:** brace expansion (`{a,b}`), tilde expansion (`~`),
  pathname globbing (`*`, `?`, `[...]`), `$@`/`$*`, `$$`, and here-strings
  (`<<<`). Here-docs (`<<`) and input redirection (`<`) *are* implemented —
  see the Redirection section below.

**Ctrl+C** -- `terminal_app.c` turns it into a real `SIGINT` (`process__signal`)
on the shell rather than forwarding it as a byte, like a cooked tty's INTR
key. A foreground external command gets it relayed by `shell_executor__wait`,
same as `kill`; with nothing running, the shell aborts the line being typed
and shows a fresh prompt instead of exiting (matching bash), via
`process__clear_signal()`. `stty raw` opts a program (e.g. `ssh`, `tcp`) out
of this so it gets the literal `^C` byte instead.

**Ctrl+D** -- same as bash: on an empty prompt it's end-of-input and exits
the interactive shell (with the last command's exit status); on a non-empty
line it just deletes the character under the cursor, like Delete.

**Builtins** (`shell_builtins.c`)
- `echo`, `true`, `false`, `cd`, `set`, `unset`, `export`, `local`, `clear`,
  `reset`, `help`, `exit [N]`.
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

**Redirection — `>` / `>>` / `<` / here-docs** (`shell_parser.c`,
`shell_executor.c`, `shell_app.c`)
- A single, trailing `> file` or `>> file` on an external command, a
  builtin, or a shell function: `>` truncates/creates the file, `>>`
  creates/appends. A single, trailing `< file` is likewise supported on all
  three, feeding the file's whole content to the command's stdin, and can be
  combined with a `>`/`>>` on the same command (`sort < in.txt > out.txt`).
  Either target undergoes the same quoting/escaping/`$` expansion as any other word
  (`wifi scan >> "$LOG_DIR/wifi.txt"` works), and a relative target resolves
  against `$PWD` the same way `cd`'s argument does. A bare `> file` /
  `>> file` with no command at all just creates/truncates the file, same as
  bash's `: > file` idiom; a bare `< file` with no command just checks the
  file can be opened.
- Implementation: a plain `>`/`>>` redirection with no `<`/here-doc input
  streams an **external command's** stdout straight to the file as it runs
  (`shell_executor__stream_external_to_file()`), rather than buffering it
  first — so, unlike a `NAME=value`-prefixed command, it keeps whatever
  partial output the command wrote before exiting nonzero, matching bash. A
  `<`/here-doc-fed command combined with a `>`/`>>` is the one case that
  still buffers the output in memory first and writes it out once the
  command finishes (`shell_executor__external_with_input()`), the same
  mechanism `|` piping uses — so it shares that path's existing
  `NAME=value`-dropped, discard-partial-output-on-failure limitations. A
  `<`/here-doc-fed command with no `>`/`>>` of its own instead relays its
  output live, exactly like a non-redirected external command. A **builtin
  or shell function** has no separate child process to stream/relay from at
  all — it runs in-line on the shell's own task — so `>`/`>>` on one of
  those instead temporarily reroutes the shell's *own* current session into
  a private capture session (`shell_executor__builtin_redirected()`,
  `stdio__session_capture_self()`/`_release_self()` in `core_sdk/stdio.h`),
  runs it, and writes whatever got captured once it returns; this always
  fully buffers (there's nothing to stream until the call has already
  finished) and nests correctly for a function whose own body redirects
  another builtin/function. A `<`/here-doc feeding a builtin's or function's
  *input* works the same way in reverse (`shell_executor__builtin_with_input()`):
  the whole file/heredoc body is preloaded into the private capture session's
  input side via `stdio__session_write_input()` *before* the call is made,
  since — unlike a real child process's session — nothing services it
  concurrently once the call is blocked inside its own
  `stdio__read()`/`stdio__read_line()` (e.g. the `read` builtin). That caps
  the fed input at whatever the stdio session's input queue can hold in one
  shot — comfortably enough for `read`'s usual "one line" use, but not a
  generic streaming input; anything bigger is reported as a clear error
  (`input too large to redirect into a builtin or function`) instead of
  being silently truncated or left to hang. Once queued,
  `stdio__session_close_input()` (`core_sdk/stdio.h`) marks that as the
  entirety of the input, so a read past the end of it (a function calling
  `read` twice with only one line on hand, say) gets a clean end-of-input the
  moment the queue actually empties, instead of a blocking
  `stdio__read()`/`stdio__read_line()` polling forever for bytes nothing will
  ever add.
- Only one input and one output redirection per command are recognized, and
  they must trail the command (`cmd arg1 arg2 > file`, not
  `cmd > file arg2`); a second `>`/`<`/here-doc marker, or trailing text
  after a target, is a syntax error rather than being silently misparsed. A
  command can carry `<` or a here-doc, never both (they're two ways of
  sourcing the same stdin).
- Here-docs (`<<DELIM`, `<<-DELIM`, and their single-/double-quoted-delimiter
  forms) are **script-files-only** (`shell__run_script()` in `shell_app.c`):
  a script's body-collection reads the following raw lines up to the
  terminator before ordinary parsing resumes, the same way a real shell's
  line reader would, but the interactive prompt and a re-parsed function body
  have no such reader, so a `<<` there reports a clear "not supported here"
  error rather than misbehaving. An unquoted or double-quoted delimiter
  `$`-expands the body the way a double-quoted word would (substitutions run,
  but the result is never word-split or globbed); a single-quoted delimiter
  keeps the body completely literal. `<<-` strips each body line's (and the
  terminator's) leading tabs before comparing, matching bash.
- A standalone `((...))` statement accepts `>`/`>>`/`<`/here-doc too, the
  same "nothing actually consumes it" way a bare `> file`/`< file` with no
  command at all already does: `((...))` reads no stdin and writes no
  stdout of its own, so a `>`/`>>` target is just created/truncated (or left
  unappended-to), and a `<`/here-doc target is only ever validated (or, for
  a here-doc, simply discarded — its body was already read regardless), not
  read from. An arithmetic error (division by zero, a syntax error, ...)
  skips the output target entirely rather than truncating it and then
  failing. A `<`/here-doc on a piped command's stage is still rejected —
  that's the one place a redirected input genuinely has nowhere to go, since
  a pipe stage's stdin is already spoken for by the previous stage's output.
- **Not implemented:** here-docs outside a script file, here-strings
  (`<<<`), multiple/chained redirections, and numbered file descriptors
  (`2>`).

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
  parameters (`$1..$9`, `$#`) for the duration of the call, and ordinary
  variables are dynamically scoped only insofar as `local` (see below) is
  used to shadow them — a variable set inside a function with no `local` is
  visible everywhere after the call returns, same as bash. Recursion works
  (positional parameters are saved/restored per call); the `factorial`
  example below still uses the accumulator-passing style (each level only
  reads its own arguments *before* recursing and does nothing with global
  state after the recursive call returns) since there is no `return`.
- There is no `return` builtin. A function's exit status is simply whatever
  its last executed command's status was, same as a plain script; use
  `if`/`elif`/`else` to skip the rest of the body instead of returning
  early. A `break` with no enclosing loop (including one used to try to
  "return" out of a function) is reported as a stray break, not absorbed
  silently.
- `local NAME[=value]...`: only valid inside a function call (an error
  otherwise, like bash). Shadows the name for the rest of that call --
  including any call it makes in turn, since this is bash's usual *dynamic*
  scoping on the same flat variable table, not lexical -- and reverts to
  whatever it held before (or is removed entirely, if it did not exist
  before) the instant the call returns. A bare `local NAME` with no
  `=value` starts out empty, same as bash.
- **Not implemented:** `case`/`esac`, `select`, subshells `(...)`, process
  substitution, `until`, and command grouping with `{ ...; }` used as a
  value (only as a function body).

## What's left to implement

Roughly in order of how often bash scripts actually use them:
- Pathname globbing and brace expansion.
- `case`/`esac`.
- `$@`, `$*`, `$$`, `$!`.
- File-test operators for `test`/`[` (`-e -f -d -r -w -x ...`) and `-o`/`[[
  ... || ... ]]`-in-tests.
- `until` loops.

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
`..._control_flow_case`, `..._local_case`, `..._command_substitution_case`,
`..._arith_word_case`, `..._output_redirect_case`, `..._builtin_redirect_case`
(`echo`/a shell function redirected to a file, plus `<` alone and combined
with `>` on both a plain builtin and a function, a second `read` past the
input's one line proving `stdio__session_close_input()` ends it cleanly
instead of hanging, and the missing-target error path -- content that traces
back to the redirected input isn't compared byte-for-byte under
`CONFIG_BRUCE_QEMU_TEST_MODE`, same reasoning as `..._pipe_redirect_case`),
`..._input_redirect_case`, `..._arith_redirect_case` (`>`/`>>`/`<` on a
standalone `((...))`, and that an arithmetic error skips the output target),
`..._heredoc_case`, `..._cat_interactive_case` (bare `cat`'s
Ctrl+D-terminated interactive stdin, layered on `stdio__read_line()` --
see `bnu_fs_app.c`'s `bnu__cat_interactive()`), `..._multiline_case` (also
the only case exercising a `#` comment, inside a function body),
`..._loops_case` (arithmetic,
both `for` forms, `while`, `break`, `break N`, and the break/catch-up
regression), `..._read_case`, `..._stdio_inheritance_case`,
`..._tty_size_case`, `..._interrupt_case` (Ctrl+C), and `..._eof_case`
(Ctrl+D on the shell's own prompt, ending the shell -- not to be confused
with `..._cat_interactive_case`'s Ctrl+D, which ends a child's stdin read
instead).
