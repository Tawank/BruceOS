# cmatrix

A native ELF port of [cmatrix](https://github.com/abishekvashok/cmatrix) --
the classic Matrix digital-rain screensaver. Opens inside the terminal app
(the manifest's `"terminal": true`), fills the screen with falling green
characters immediately, and keeps running until `q`.

Live keys (all from upstream, unchanged): `q` quit, `a` toggle async
scroll, `b`/`B`/`n` bold levels, `r` rainbow, `m` lambda-glyph toggle
(see "What's different" below), `k` toggle character-churn while
scrolling, `p`/`P` pause, `0`-`9` set update speed, and
`!@#$%^&` pick a color (red/green/yellow/blue/magenta/cyan/white).

## Porting approach

Unlike `vi/` (one upstream file vendored verbatim plus a hand-written
shim layer) this app is an **adapted copy**, not a vendored one:
`main/cmatrix.c` carries over upstream's rendering algorithm and main
loop essentially unchanged (same variable names, same control flow --
diffing against upstream's `cmatrix.c` should make the few real changes
obvious), with only the OS-integration layer removed or rewritten. See
this file's own top comment for the exact list; the short version:

- **No command-line parsing.** Upstream's `getopt()`-based flags are all
  gone -- same "no file picker, just sensible defaults" simplification
  `vi/` already uses. Nothing is lost functionally: every flag that
  matters for how it looks while running (`-b`/`-r`/`-C`/`-u`/...) already
  has a live keypress equivalent in upstream's own main loop (listed
  above), so dropping the startup flags only removes the one-time
  "start already red" convenience, not a capability.
- **No signal handling.** Upstream installs real `SIGINT`/`SIGQUIT`/
  `SIGWINCH`/`SIGTSTP` handlers for Ctrl+C, resize, and Ctrl+Z job
  control. Same reasoning `vi/`'s own `USE_SIGNALS=OFF` already uses:
  no SIGWINCH-equivalent by design (`core_sdk/tty.h`'s resize story is a
  generation counter to poll, not a signal) and no shell job control to
  suspend into. `q` was already upstream's own primary quit key
  (checked via `wgetch()`, not a signal), so quitting needs no signal
  either -- only the *redundant* Ctrl+C/Ctrl+\\ quit paths are gone.
- **No live resize.** Screen size is read once at startup (via
  `core_sdk/tty.h`'s `tty__get_size()`, wired through `components/ncurses`
  the same way it's wired into `lib_setup.c` itself -- see that
  component's own README.md) and stays fixed for the session, the same
  one-shot-at-open limitation `vi/` already lives with for the same
  underlying reason.
- **No console-font switching, no alternate-tty redirection, no
  "screensaver" keystroke-injection mode.** Upstream's `-l`/`-x` modes
  shell out to `consolechars`/`setfont` to load a special font with
  remapped glyphs for terminal emulators wired for it; `-t` reopens a
  different tty via `newterm()`; `-s` (`screensaver` mode meant for
  Linux's `X` display manager integration) uses a Linux-only
  `ioctl(..., TIOCSTI, ...)` trick to replay keystrokes back into the
  real console after exiting. None of these mean anything on BruceOS
  (one terminal app, one session, no console font to swap, no X11), so
  all three are gone along with the command-line flags that selected
  them.
- **No wide-character Matrix glyphs.** `components/ncurses` is built
  without wide-char support by design (see that component's own
  README.md -- it was one whole extra category of toolchain risk this
  project deliberately avoided). Upstream's "classic" mode (`-c`) prints
  real half-width Unicode katakana via `addwstr()`/`wchar_t`, which needs
  wide-char ncurses; this port always uses upstream's own *default*
  character set instead (plain ASCII, codepoints 33-122 -- what upstream
  itself uses without `-c`/`-l`/`-x`), rendered with plain `addch()`.
  The `m` lambda-mode toggle still works but prints a plain `$` instead
  of upstream's real "λ" glyph, for the same reason.

## A gap this port's development found in `core_sdk/curses.h`

Porting this surfaced two real missing exports in
`elf_loader_sdk_symbols.c`: `wattr_on`/`wattr_off` (the actual functions
`attron()`/`attroff()` expand to via `wattron()`/`wattroff()` --
unconditional macros in real `curses.h`, not gated behind any build
option, so almost any real curses program needs them) and
`leaveok`/`use_default_colors` (this app calls both directly). All four
are now exported -- see that file's own comment at the export block for
the full reasoning. Worth knowing for whoever ports the next ncurses
app: a plain-looking call like `attron(A_BOLD)` does not necessarily
mean the plainly-named symbol is what needs exporting -- check the real
generated `curses.h` for a macro redefinition first.

## License

cmatrix is GPLv3 (`Copyright (C) 1999-2017 Chris Allegretta,
2017-Present Abishek V Ashok`); `main/cmatrix.c`'s own top comment
carries the full notice. BruceOS itself is AGPLv3, which is compatible
with combining GPLv3 code (same relationship as BusyBox's GPLv2 `vi.c`
already vendored in this repo).
