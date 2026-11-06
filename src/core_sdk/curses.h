#pragma once

/**
 * @brief Text-based UI windows, colors, and keyboard input (ncurses).
 *
 * A vendored, BruceOS-adapted copy of real ncurses 6.4
 * (components/ncurses, compiled once into firmware and exported here
 * function-by-function, the same pattern as args.h) -- not a
 * reimplementation. This is the actual standard ncurses C API:
 * initscr()/endwin(), move()/addch()/addstr()/printw() and their
 * mv/w/mvw variants, newwin()/subwin()/delwin(), getch()/keypad(),
 * start_color()/init_pair()/COLOR_PAIR(), attron()/attroff() and the
 * A_BOLD/A_REVERSE/... attribute flags, box()/border() and the ACS_*
 * line-drawing characters, refresh()/wrefresh(), raw()/cbreak()/
 * noecho()/timeout()/nodelay(), and the rest -- already documented by
 * ncurses' own manual pages (`man ncurses`, `man curs_addch`,
 * `man curs_color`, ...), not repeated here one declaration at a time.
 * A native app that already includes <curses.h> and calls these in the
 * ordinary way should need no BruceOS-specific changes to compile
 * against this header.
 *
 * Differences from a full desktop ncurses worth knowing:
 *  - Exactly one process may hold the screen at a time. initscr()/
 *    newterm() returns NULL (ncurses' own ordinary failure signal, same
 *    as a real setup error) if another still-running process already
 *    owns it -- a curses screen is a foreground/singleton resource here,
 *    the same way the display and a routed stdio session already are.
 *    Ownership is released by endwin() and is also self-healing if the
 *    owning process exits without calling it.
 *  - Mouse reporting and soft label keys (slk_*) are not implemented --
 *    BruceOS's terminal emulator (modules/utils/terminal/terminal_ansi.c)
 *    has no mouse protocol and no hardware label-strip equivalent;
 *    slk_* calls are harmless no-ops rather than missing symbols.
 *  - Only meaningful from a process running inside a terminal session
 *    (see manifest.h's "terminal" manifest property) -- there's no
 *    terminal type to negotiate capabilities against outside of one.
 *
 * See components/ncurses/README.md for exactly what's vendored, dropped,
 * stubbed, or BruceOS-rewritten, and why.
 * Not permission-gated (screen ownership is enforced directly -- see
 * above -- rather than through core_sdk/permission.h).
 */

#include "ncurses/curses.h"
