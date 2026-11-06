# ncurses

A vendored-and-heavily-adapted copy of upstream ncurses 6.4, compiled once
into firmware and exported to ELF apps symbol-by-symbol (see
`src/modules/loaders/elf/elf_loader_sdk_symbols.c` and `src/core_sdk/curses.h`),
the same pattern `components/args` already uses -- not vendored whole into
each app the way `vi`'s `vi.c` or `nes`'s Nofrendo fork are.

## Why this is possible at all

Real ncurses normally reads a terminfo database from disk to learn a
terminal's capabilities. BruceOS has no terminfo directory tree, and
`storage.h` doesn't shape like the POSIX `opendir`/`stat` calls that
database search needs. ncurses ships an official, supported alternative
for exactly this situation: a **compiled-in fallback table** (its own
`--disable-database --with-fallbacks=...` build mode), normally meant for
small/embedded targets. `bruce/bruce_fallback.c` is that table, generated
for one made-up terminal type (`brucetty`) whose capability strings were
written by hand against what `src/modules/utils/terminal/terminal_ansi.c`
(BruceOS's own terminal emulator) actually implements -- not copied from
any real terminal's entry.

This was verified end-to-end on a host Linux build before any of it was
adapted for BruceOS (see "What's verified" below), including with a fake
terminal name that exists in no real terminfo database anywhere and
`$TERMINFO`/`$HOME` pointed at nonexistent paths, to rule out accidentally
hitting a system fallback instead of this one.

## Layout

- `ncurses/base/`, `ncurses/tty/`, `ncurses/tinfo/`, `ncurses/generated/`,
  `ncurses/trace/varargs.c` -- the trimmed upstream source tree, organized
  the same way upstream's own `ncurses/` directory is. `ncurses/generated/`
  holds files upstream's own build generates via `MKnames.awk` et al.
  (`names.c`, `codes.c`, `comp_captab.c`, `comp_userdefs.c`, `expanded.c`,
  `lib_gen.c`, `lib_keyname.c`, `unctrl.c`) -- pure data tables, vendored
  as plain source here instead of re-running that generation as part of
  this build.
- `ncurses/*.h` (private headers: `curses.priv.h`, `tic.h`, `term_entry.h`,
  `hashsize.h`, `init_keytry.h`, `new_pair.h`, `fifo_defs.h`, ...) and
  `include/*.h` (public/generated headers: `curses.h`, `term.h`,
  `ncurses_cfg.h`, `ncurses_def.h`, `ncurses_dll.h`, `unctrl.h`) --
  generated once by a host configure/build (see "What's verified") and
  committed as plain source, the same reasoning as `ncurses/generated/`.
- `bruce/brucetty.ti` -- the hand-written terminfo source for BruceOS's
  terminal, matched against `terminal_ansi.c`'s real escape-sequence
  coverage.
- `bruce/bruce_fallback.c` -- generated mechanically from `brucetty.ti` by
  upstream's own `ncurses/tinfo/MKfallback.sh` (via `tic`+`infocmp`). Not
  hand-edited.
- `bruce/bruce_stubs.c` -- no-op stand-ins for a few symbols some kept
  files call unconditionally whose real implementation lives in a file
  this build doesn't vendor (soft label keys, signal handling, terminfo
  entry-list cleanup). See the file's own top comment for each one and why.
- `bruce/bruce_tty.c` -- BruceOS-backed replacements for `napms()` (maps to
  `runtime__sleep()`) and the two bottom primitives of
  `_nc_get_tty_mode()`/`_nc_set_tty_mode()` (map to `tty__get_mode()`/
  `tty__set_mode()`).

## What's dropped entirely, and why

- **Mouse** (`lib_mouse.c`): `terminal_ansi.c` implements no mouse-reporting
  protocol.
- **Soft label keys** (`lib_slk*.c`): a hardware feature (dedicated
  function-key label strips on some real terminals) with no BruceOS
  equivalent. Call sites that touch SLK unconditionally go through
  `bruce_stubs.c`'s no-ops instead.
- **Panel library glue** (`nc_panel.c`, `use_window.c`): the separate
  `panel` library, not part of base ncurses functionality.
- **Tracing** (`lib_trace.c`, `visbuf.c`): debug instrumentation, dead
  weight on a device with no attached debugger console for it.
- **Termcap compatibility** (`lib_termcap.c`, `read_termcap.c`,
  `home_terminfo.c`): BSD termcap-format fallback, not needed when every
  lookup already resolves through `bruce_fallback.c`.
- **SIGTSTP job control** (`lib_tstp.c`): no shell job control to suspend
  into (same reasoning `vi.c`'s own `USE_SIGNALS=OFF` already uses
  elsewhere in this repo). Its one externally-called symbol,
  `_nc_signal_handler`, is stubbed in `bruce_stubs.c`.
- **Windows console driver** (`lib_win32con.c`, `lib_win32util.c`): not
  applicable.
- **Terminfo-database/tic-compiler family** (`read_entry.c`,
  `db_iterator.c`, `access.c`, `hashed_db.c`, `entries.c`, `alloc_entry.c`,
  `comp_parse.c`, `comp_scan.c`, `comp_expand.c`, `write_entry.c`,
  `parse_entry.c`, `captoinfo.c`, `make_hash.c`, `make_keys.c`): this is
  the directory-search/terminfo-source-compiler machinery the fallback
  table makes unnecessary. `lib_setup.c`'s own `#if NCURSES_USE_DATABASE
  || NCURSES_USE_TERMCAP` guard (both off here) already skips calling into
  most of it; the two still-unconditionally-referenced symbols
  (`_nc_head`, `_nc_free_entry`) are stubbed in `bruce_stubs.c` as an
  always-empty list, which is exactly correct since nothing ever
  populates it when this path is never taken.

## What needed a real BruceOS-backed rewrite

- **`tinfo/lib_napms.c`** -- replaced by `bruce_tty.c`'s `napms()`, mapped
  to `runtime__sleep()`.
- **`tinfo/lib_ttyflags.c`** -- kept upstream, **not** replaced wholesale.
  Only the two innermost primitives (`_nc_get_tty_mode()`/
  `_nc_set_tty_mode()`'s actual `GET_TTY`/`SET_TTY`, i.e. real
  `tcgetattr()`/`tcsetattr()`, call sites) are patched in place to call
  `bruce_curses__get_tty()`/`bruce_curses__set_tty()` (`bruce_tty.c`)
  instead. Everything built on top -- `lib_raw.c`'s `raw()`/`cbreak()`/
  `noraw()`/`nocbreak()`/`qiflush()`/`intrflush()`, and
  `lib_ttyflags.c`'s own `def_shell_mode()`/`def_prog_mode()`/
  `reset_prog_mode()`/`reset_shell_mode()`/`savetty()`/`resetty()` --
  only ever manipulates a `TTY` struct in memory and calls through those
  two, so it's upstream and unmodified. BruceOS's `tty.h` is a two-state
  cooperative flag (RAW/COOKED), not a real line discipline, so the
  replacement synthesizes/interprets just enough of the `ICANON`/`ISIG`/
  `IEXTEN` bits for that existing bit-testing code to keep working.
- **`tinfo/lib_baudrate.c`** -- kept upstream, **not** replaced wholesale.
  `_nc_baudrate()`/`_nc_ospeed()`/the `speeds[]` table are plain data-table
  lookups with no OS dependency and stay exactly as upstream wrote them
  (other kept files link against them as-is). Only the tail of
  `NCURSES_SP_NAME(baudrate)` -- the one block that called `cfgetospeed()`
  on a real tty -- is patched to return a fixed 115200 instead; a drawn
  terminal window has no real serial line, and this only ever fed
  ncurses' own historical output-padding-delay math.
- **`tty/lib_twait.c`** -- `_nc_timed_wait()` fully replaced. Upstream
  picks `poll()`/`select()`/a BeOS snooze loop to check readiness on a
  real fd; none of those are exported to ELF apps, and BruceOS's stdio
  session isn't a pollable fd anyway. The replacement calls
  `stdio__read()` directly (it already takes the exact timeout semantics
  needed: 0 = poll, N = wait up to N ms, `UINT32_MAX` = block) -- but
  unlike `poll()`/`select()`, that's a *consuming* read, not a peek, so
  the byte it finds is stashed in `bruce_curses__pending_byte` for...
- **`base/lib_getch.c`** -- one line patched (not file-wide): the real
  blocking `read(sp->_ifd, &c2, 1)` call now checks
  `bruce_curses__pending_byte` first and only falls through to a real
  `read()` if `_nc_timed_wait()` didn't already consume a byte. Together
  these two patches reproduce "check readiness within a deadline, then
  read" using BruceOS's single consuming-read primitive, with no byte
  lost or double-read.

## What's verified (on a host Linux build, not BruceOS/ESP32)

All of the following were confirmed concretely, not assumed, before being
adapted here -- see the design-conversation transcript for the exact
commands:

- The trimmed 113-file set (+ `bruce_fallback.c` + `bruce_stubs.c`) links
  clean with no missing symbols, and runs correctly: `initscr`/`move`/
  `addstr`/`refresh`/`endwin`, `start_color`/`init_pair`/`newwin`/`box`/
  `wattron`/`mvwprintw`/`delwin` all produce the escape sequences
  `terminal_ansi.c` actually understands (cross-checked against that
  file's real CSI/SGR/charset-designation support).
- `brucetty.ti` compiles cleanly with upstream's own `tic -x` (no
  warnings) and round-trips through `infocmp`.
- The fallback-only lookup path was confirmed to need zero filesystem
  access: tested with a terminal name (`brucetty`) that matches no real
  terminal anywhere, `$TERMINFO`/`$HOME` pointed at nonexistent paths.
- The `lib_twait.c`/`lib_getch.c` readiness patch was tested both ways:
  `getch()` after `timeout(300)` with no input pending returns `ERR`
  after the deadline; with a byte already available it's returned
  correctly and exactly once (no duplicate/lost byte).

## What's now wired up

- **`src/core_sdk/curses.h`** -- the public app-facing header. It's a
  thin wrapper around the real `ncurses/curses.h` plus a module-level doc
  comment; a native app includes it and calls real, standard ncurses
  functions, no BruceOS-specific API.
- **`elf_loader_sdk_symbols.c`** exports a representative subset (screen
  setup/teardown, windows, output, input/timeouts, tty mode, attributes,
  color, borders -- see that file's own comment for the exact list and
  how to extend it). Declared via narrow local `extern`s there rather
  than `#include "core_sdk/curses.h"` directly: that header's macros
  (`bool`, `TRUE`, `FALSE`, `OK`, `ERR`, and the stdscr-shorthand
  single-word macros like `move()`/`addch()`/`clear()`) would otherwise
  collide with that huge, unrelated file's own identifiers. Every
  exported name was checked with `nm` against the real compiled archive
  before being added -- not just written from memory.
- **Singleton screen ownership** (`bruce/bruce_curses_owner.c`). Real
  ncurses relies on actual `extern` globals (`stdscr`, `curscr`, `COLS`,
  `LINES`) that caller code reads directly -- fine here only because a
  curses screen is a foreground/singleton resource the same way the
  display and a routed stdio session already are. `newterm()` (the one
  real funnel point both `initscr()` and a direct `newterm()` call go
  through) is patched to fail (`NULL`, ncurses' own ordinary failure
  signal) if a different, still-running process already owns the screen;
  `endwin()` releases it. Ownership is self-healing, not just
  voluntary-release-only: a stale claim from a process that exited
  without calling `endwin()` is detected via `process__snapshot()` and
  reclaimed by the next acquire attempt, instead of wedging the screen
  unusable forever. Tested directly (not just via curses) through six
  acquire/release/contention/stale-reclaim scenarios.
- **`tsearch()`/`tfind()`/`tdelete()`** (`bruce/bruce_tsearch.c`) -- a
  small, self-written binary search tree, not copied from any libc.
  `base/new_pair.c` calls these unconditionally whenever
  `NCURSES_EXT_COLORS` is defined, which `include/curses.h` always does
  (part of the public API contract, not something to flip off), and
  `tinfo/lib_tparm.c` calls `tsearch()` too. Neither is in the ELF
  loader's exported symbol table, and whether picolibc provides
  `<search.h>`'s tree functions for this toolchain isn't verifiable in
  this environment -- vendoring a small implementation removes that
  uncertainty entirely. Exercised indirectly by the color-pair test in
  "What's verified" below (color pairs go through this code path).

## Fixes found by an actual ESP-IDF/picolibc build (not host-predictable)

Everything in the sections above was validated on host Linux gcc, which
gives real confidence in logic and symbol-level correctness but can't
catch toolchain-specific header differences. Once this was actually built
against the real ESP-IDF/picolibc toolchain, two real problems surfaced
that host testing couldn't have predicted -- both now fixed and
re-verified on host (forcing the same code paths host gcc wouldn't
otherwise take):

- **No real `poll()`/`select()`.** `ncurses_cfg.h` (copied from a host
  configure run) wrongly advertised `HAVE_WORKING_POLL`/`HAVE_POLL_H`/
  `HAVE_SELECT`/etc as available. Disabled all of them -- zero functional
  loss, since `tty/lib_twait.c`'s own usage was already fully replaced
  (see above) and `tty/tty_update.c`'s one other use is an optional
  "check for pending input mid-redraw" optimization that's safely a
  no-op without it, the same way real ncurses already supports building
  without poll/select at all.
- **No real `struct termios`.** `<termios.h>` exists on picolibc as a
  header but doesn't actually define `struct termios` the way `term.h`'s
  generated `TTY`/`GET_TTY`/`SET_TTY` block (itself frozen at generation
  time from the host's `HAVE_TERMIOS_H`/`HAVE_TCGETATTR`, not something
  `ncurses_cfg.h` edits can fix) expected. Since nothing in this
  vendored+patched tree actually calls a real `tcgetattr()`/`tcsetattr()`
  any more (`bruce/bruce_tty.c`'s two primitives patch that out
  already), `TTY` is now a small, fully BruceOS-owned struct in
  `include/term.h` -- not `struct termios` -- with every field and flag
  constant the kept tree actually references (verified by grepping, not
  guessed a second time): `c_iflag`/`c_oflag`/`c_cflag`/`c_lflag`, the
  full `VINTR`..`VEOL2` control-character indices, `IGNBRK`..`IMAXBEL`/
  `OPOST`..`OFDEL`/`CSIZE`..`CLOCAL`/`ISIG`..`IEXTEN` flag bits, and
  `B0`..`B4000000` speed constants. `tinfo/lib_kernel.c`'s one real OS
  call site (`flushinp()`'s `tcflush()`) is patched to
  `stdio__flush_input()` (now also exported by the ELF loader, it wasn't
  before); `erasechar()`/`killchar()` need no patch of their own --
  they already read straight out of the `TTY` `bruce_curses__get_tty()`
  fills in, so populating `c_cc[VERASE]`/`c_cc[VKILL]` with conventional
  defaults there was enough. Also fixed in the same pass: `curses.priv.h`
  unconditionally `#define`s `INFINITY` as an integer "cost too high"
  sentinel for the cursor-movement optimizer (`lib_mvcur.c`), colliding
  with the real IEEE `INFINITY` from picolibc's `<float.h>` -- renamed to
  `NC_COST_INFINITY` throughout both files.

- **No real `TIOCGWINSZ`/`ioctl()`.** `tinfo/lib_setup.c`'s
  `_nc_get_screensize()` asked the OS for the real terminal size via
  `ioctl(fd, TIOCGWINSZ, &size)`, gated by `HAVE_SIZECHANGE` (also wrong
  in the host-copied `ncurses_cfg.h`, now disabled). Patched to call
  `bruce_curses__get_screen_size()` (`bruce/bruce_tty.c`), a thin wrapper
  around `tty__get_size()` -- the same real primitive `vi.c`'s own
  `WIN_RESIZE` support already uses for the same purpose. Routed through
  a wrapper rather than including `core_sdk/tty.h` directly in
  `lib_setup.c`: that file already relies on `term.h`'s `columns`/`lines`
  macros being active for its own, separate fallback path, which would
  otherwise mangle `core_sdk/tty.h`'s identically-named `columns` struct
  field the moment it's referenced (bit the wrapper function itself once
  too, fixed by defining it before `term.h`'s `#include` in that same
  file -- see `bruce_tty.c`'s own comment). Verified with a new test:
  `LINES`/`COLS` after `initscr()` correctly reflect a stubbed 80x24
  `tty__get_size()` result, not the terminfo fallback default.

- **`newterm()` ignored the one thing that actually mattered: `$TERM`
  itself.** The three fixes above got a real build to compile and link,
  but the very first real run on hardware still failed --
  `initscr()`'s own "Error opening terminal" exit path, not a crash in
  anything this port wrote. Root cause: `initscr()` calls
  `newterm(name, ...)` where `name` comes from `$TERM` (defaulting to
  the literal string `"unknown"` if unset, never `NULL`); nothing on
  BruceOS ever sets `$TERM=brucetty`, and the compiled-in fallback table
  only has an entry for that one exact name -- so the lookup failed
  every time, regardless of what (if anything) `$TERM` happened to be.
  Fixed where ownership is already enforced -- `newterm()`'s one real
  entry point -- by forcing `name` to `"brucetty"` unconditionally:
  there's exactly one real terminal implementation on this device
  (`terminal_ansi.c`) and exactly one fallback entry describing it, so
  `$TERM`-based selection was always going to be pointless here, not
  just unset. Re-verified on host with the exact failing conditions:
  `$TERM` unset, and `$TERM` set to an unrelated value (`xterm`) -- both
  now succeed, where both failed before this fix.

## What's NOT yet done

- **A clean ESP32/picolibc build has not been confirmed end-to-end.**
  The two fixes above came from real build errors against the real
  toolchain; there may be more of the same category (another
  `ncurses_cfg.h` `HAVE_*` flag describing the host instead of the
  target, or another file assuming a libc feature picolibc doesn't have)
  still to find the same way -- one real build at a time, since nothing
  in the environment this was developed in can run that build itself.
- A demo native app (`native_apps/examples/...`) exercising the real
  `core_sdk/curses.h` surface once a build succeeds.
