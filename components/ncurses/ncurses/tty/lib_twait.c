/* BruceOS replacement for upstream lib_twait.c's _nc_timed_wait().
 *
 * Upstream picks one of poll()/select()/a BeOS snooze-and-ioctl loop to
 * check sp->_ifd for readiness within a deadline, then leaves the actual
 * byte read to a plain read(fd,...) call back in base/lib_getch.c. None of
 * those readiness primitives are exported to ELF apps (no poll/select/
 * nanosleep in the loader's symbol table -- see components/ncurses/
 * README.md), and BruceOS's stdio session isn't a real fd select() would
 * recognize as pollable anyway.
 *
 * core_sdk/stdio.h's stdio__read() already takes the exact timeout this
 * function needs (0 = poll, N = wait up to N ms, UINT32_MAX = block), but
 * unlike poll()/select() it's a *consuming* read, not a peek -- there's no
 * way to ask BruceOS "is a byte available?" without actually taking the
 * byte if one is. So this _nc_timed_wait() takes the byte itself when one
 * arrives within the deadline and stashes it in bruce_curses__pending_byte
 * for base/lib_getch.c's own patched read call (see that file's matching
 * comment) to pick up instead of calling read() again -- together the two
 * patched call sites reproduce "check readiness, then read" with a single
 * real read.
 */
#include <curses.priv.h>
#include <term.h>

#include "core_sdk/stdio.h"

MODULE_ID("bruce-tty-twait")

/* Shared with base/lib_getch.c's patched byte-read call site. -1 means
 * empty; 0-255 is a real pending byte already consumed from stdio__read()
 * by the _nc_timed_wait() call below that found it. One byte of lookahead
 * is all a single-threaded, single-SCREEN-at-a-time process ever needs:
 * nothing else reads stdin between this function returning "ready" and
 * lib_getch.c's very next read call. */
int bruce_curses__pending_byte = -1;

NCURSES_EXPORT(int)
_nc_timed_wait(SCREEN *sp GCC_UNUSED,
	       int mode,
	       int milliseconds,
	       int *timeleft
	       EVENTLIST_2nd(_nc_eventlist * evl GCC_UNUSED))
{
    int result = TW_NONE;

    TR(TRACE_IEVENT, ("start twait: %d milliseconds, mode: %d",
		       milliseconds, mode));

    if (timeleft != 0) *timeleft = 0;

    /* No mouse/event fd of our own (TW_MOUSE, TW_EVENT): terminal_ansi.c
     * has no mouse protocol (see components/ncurses/README.md), and this
     * build doesn't define NCURSES_WGETCH_EVENTS. Only TW_INPUT applies. */
    if ((mode & TW_INPUT) && bruce_curses__pending_byte < 0) {
	uint8_t byte;
	size_t got = 0;
	uint32_t timeout_ms = (milliseconds < 0) ? UINT32_MAX : (uint32_t)milliseconds;
	bruce_result_t rc = stdio__read(&byte, 1, timeout_ms, &got);

	if (rc == BRUCE_OK && got == 1) {
	    bruce_curses__pending_byte = (int)byte;
	}
    }

    if (bruce_curses__pending_byte >= 0) {
	result = TW_INPUT;
    }

    TR(TRACE_IEVENT, ("end twait: returned %d", result));
    return result;
}
