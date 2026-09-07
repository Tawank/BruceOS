# vi

A native ELF port of [BusyBox](https://busybox.net/)'s `vi` -- a small,
self-contained vi clone (not a wrapper around a shell-out to a real vi/vim
binary, which BruceOS has no shell to exec in the first place; see "No
`:!cmd`" below). Opens with no argument shows a file picker rooted at `/`;
`:w`/`:wq`/`ZZ` save back to the same path.

## Porting approach

Unlike `doom/` and `nes/`, which vendor/fetch a whole multi-file engine,
this app needs exactly one upstream file: BusyBox's `editors/vi.c`. It's
fetched **completely unmodified** (`main/vi_sources.cmake`, pinned to a
commit, integrity-checked via `EXPECTED_HASH`) -- every adaptation lives in
this app's own `main/libbb.h` (a from-scratch, hand-written stand-in for
BusyBox's real Kbuild-generated `libbb.h`, not a fork of it) and
`main/busybox_shim.c` (implementing the ~20 libbb helper functions vi.c
calls). See both files' own top comments for the full design rationale;
the short version:

- **Terminal I/O.** `core_sdk/stdio.h` already behaves like a real pty for
  any ELF app (`stdio__read`/`stdio__write`, no OS-level line-discipline --
  raw mode is already the default) and `core_sdk/tty.h` adds terminal
  geometry (`tty__get_size()`) and a cooperative raw/cooked mode flag
  (`tty__set_mode()`). `busybox_shim.c`'s `safe_read_key()`/`safe_poll()`/
  `set_termios_to_raw()`/`get_terminal_width_height()` route straight
  through these instead of a real termios driver/ioctl -- this app's only
  actual point of contact with BruceOS.
- **File I/O.** Needs nothing extra: `open`/`close`/`read`/`write`/`lseek`/
  `stat`/`fstat`/`getenv` are already real, resolvable POSIX-shaped symbols
  for any Bruce ELF app (`elf_loader_sdk_symbols.c`), and vi.c's own
  `xmalloc_open_read_close()` (implemented in `busybox_shim.c`) is built on
  top of those, not any BruceOS-specific storage API directly.
- **No signals.** `ENABLE_FEATURE_VI_USE_SIGNALS` is off -- vi.c's
  SIGWINCH/SIGTSTP/SIGINT handlers (live resize, Ctrl+Z suspend) have no
  real counterpart here (no SIGWINCH-equivalent by design, no shell job
  control to suspend into). Ctrl+C still reaches vi.c as a plain byte, not
  a signal -- raw mode passes it through.
- **`:!cmd` works.** `ENABLE_FEATURE_ALLOW_EXEC` is on -- unlike doom's
  `system()` stub (`doom/README.md`, a different, genuinely no-shell-
  available case), BruceOS does have a real command processor
  (`modules/shell/`), and `bruce_elf__system()` (`elf_loader_sdk_symbols.c`)
  runs a command through it as a real "shell -c" child process with the
  console inherited live -- the user watches it run and sees its output
  directly, then `Hit_Return()` (vi.c's own code, unmodified) waits for a
  key before redrawing. Whatever `modules/shell/` itself supports (pipes,
  redirection, `$(...)`, control flow, ...) is available on this line, same
  as a real shell's `:!`.
- **No regex search.** `ENABLE_FEATURE_VI_REGEX_SEARCH` is off (this
  toolchain has no `<regex.h>`) -- vi.c's own `#else` path falls back to
  plain substring search.

Everything else (colon commands, `:set`, undo, yank/delete registers,
`.`-repeat, one-shot-at-open terminal-size detection via
`tty__get_size()`) is on -- see `main/libbb.h`'s feature-flag block for
the full list and reasoning per flag.

## Controls

Standard vi: `hjkl`/arrow keys to move, `i`/`a`/`o` to insert, `Esc` back to
command mode, `:w`/`:wq`/`:q`/`ZZ`, `/pattern` to search, `:!cmd` to run a
shell command (see "Porting approach" above). `-R` (via `vi -R`, not
exposed as a launcher option here) opens read-only.
