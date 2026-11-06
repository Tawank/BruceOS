/* Replacements for the two upstream files that talk to the real OS tty
 * driver (termios ioctls, nanosleep) via APIs the ELF loader doesn't
 * export and that wouldn't mean anything on BruceOS's session model
 * anyway -- see components/ncurses/README.md's "what needed a real
 * BruceOS-backed rewrite" list. Dropped from the build in favor of this
 * file: tinfo/lib_napms.c. (tinfo/lib_baudrate.c stays upstream, patched
 * in place instead -- see its own comment.)
 *
 * tinfo/lib_ttyflags.c is NOT replaced wholesale: only its two innermost
 * primitives (_nc_get_tty_mode/_nc_set_tty_mode's GET_TTY/SET_TTY calls)
 * are patched in place (see the comment at that file's own call sites) to
 * call bruce_curses__get_tty()/bruce_curses__set_tty() below instead of a
 * real tcgetattr()/tcsetattr(). Everything built on top of those two
 * (raw(), cbreak(), noraw(), nocbreak(), qiflush(), intrflush(), ...in
 * tinfo/lib_raw.c, plus def_prog_mode()/reset_shell_mode()/savetty()/... in
 * lib_ttyflags.c itself) is upstream, unmodified -- it only ever
 * manipulates a TTY struct in memory and calls through those two, so
 * patching just the two bottom primitives keeps the whole rest of that
 * machinery working as-is instead of needing its own BruceOS rewrite.
 */
/* core_sdk headers first: term.h (below) #defines short names like
 * "columns" and "lines" as macros (CUR Numbers[0]/[1], terminfo's own
 * numeric-capability accessors) that would otherwise clobber identically
 * named struct fields/parameters in core_sdk/tty.h if it were included
 * afterward. */
#include "core_sdk/runtime.h"
#include "core_sdk/tty.h"

/* ---- screen size (tinfo/lib_setup.c's _nc_get_screensize()) ----------
 *
 * Defined here, *before* term.h is included below, and deliberately not
 * inside this file's own later functions: term.h #defines bare
 * `columns`/`lines` as CUR Numbers[0]/[1] (terminfo's own numeric-
 * capability accessors) once it's included, which would otherwise
 * mangle this struct's own `columns` field the moment this function's
 * body is parsed -- lib_setup.c hits exactly that same hazard for its
 * *own* reason (its existing "figure out the size of the screen"
 * fallback needs that macro active), which is why it can't just include
 * core_sdk/tty.h directly either, and instead calls this one small
 * wrapper. */
int bruce_curses__get_screen_size(int *out_rows, int *out_columns) {
    if (!tty__isatty()) return -1;
    bruce_tty_size_t size;
    if (tty__get_size(&size) != BRUCE_OK) return -1;
    *out_rows = (int)size.rows;
    *out_columns = (int)size.columns;
    return 0;
}

#include <curses.priv.h>
#include <term.h>

/* ---- napms() --------------------------------------------------------- */

NCURSES_EXPORT(int) NCURSES_SP_NAME(napms)(NCURSES_SP_DCLx int ms) {
    (void)SP_PARM;
    T((T_CALLED("napms(%d)"), ms));
    runtime__sleep(ms > 0 ? (uint32_t)ms : 0);
    returnCode(OK);
}

#if NCURSES_SP_FUNCS
NCURSES_EXPORT(int) napms(int ms) {
    return NCURSES_SP_NAME(napms)(CURRENT_SCREEN, ms);
}
#endif

/* baudrate()/NCURSES_SP_NAME(baudrate) are patched in place in
 * tinfo/lib_baudrate.c instead (that file's _nc_baudrate()/_nc_ospeed()/
 * speeds[] table have no OS dependency and stay upstream unmodified --
 * only the one cfgetospeed() call needed replacing). */

/* ---- _nc_get_tty_mode() / _nc_set_tty_mode() bottom primitives -------- */

/* BruceOS's tty.h is a two-state cooperative flag (RAW/COOKED), not a
 * real termios bit field -- there's no kernel line discipline underneath
 * to configure (see tty.h's own doc comment: BruceOS never echoes or
 * line-edits at the session level, so "raw mode" is already the default
 * behavior). These two synthesize/interpret just enough of a TTY value
 * for lib_raw.c's existing ICANON/ISIG/IEXTEN bit tests to round-trip
 * consistently, without a real tcgetattr()/tcsetattr() underneath. TTY
 * itself is a small BruceOS-owned struct (include/term.h), not
 * `struct termios` -- see that file's own comment for why. */

#define BRUCE_CURSES__RAW_IFLAG ((bruce_tcflag_t)0)
#define BRUCE_CURSES__RAW_LFLAG ((bruce_tcflag_t)0)
#define BRUCE_CURSES__COOKED_IFLAG ((bruce_tcflag_t)(IXON | BRKINT | PARMRK))
#define BRUCE_CURSES__COOKED_LFLAG ((bruce_tcflag_t)(ICANON | ISIG | IEXTEN))

int bruce_curses__get_tty(TTY *buf) {
    if (buf == NULL) return -1;
    memset(buf, 0, sizeof(*buf));
    bool raw_mode = tty__get_mode() == BRUCE_TTY_MODE_RAW;
    buf->c_iflag = raw_mode ? BRUCE_CURSES__RAW_IFLAG : BRUCE_CURSES__COOKED_IFLAG;
    buf->c_lflag = raw_mode ? BRUCE_CURSES__RAW_LFLAG : BRUCE_CURSES__COOKED_LFLAG;
    /* c_oflag/c_cflag: no real output/control-line processing on a drawn
     * terminal window, but tinfo/lib_kernel.c's _use_meta detection
     * (lib_newterm.c) and similar unmodified upstream bit tests still
     * read these -- CS8|CREAD|CLOCAL (an ordinary 8-bit-clean local
     * line, no flow control) is the conventional "nothing unusual here"
     * answer. */
    buf->c_oflag = 0;
    buf->c_cflag = CS8 | CREAD | CLOCAL;
    buf->c_cc[VMIN] = 1;
    buf->c_cc[VTIME] = 0;
    /* erasechar()/killchar() (tinfo/lib_kernel.c, unmodified upstream)
     * read these two straight out of the TTY this function fills in --
     * conventional defaults (backspace, Ctrl-U), not anything BruceOS's
     * own line editing actually enforces (core_sdk/tty.h: BruceOS never
     * line-edits at the session level). */
    buf->c_cc[VERASE] = 0x7f;
    buf->c_cc[VKILL] = 0x15;
    return 0;
}

int bruce_curses__set_tty(const TTY *buf) {
    if (buf == NULL) return -1;
    /* lib_raw.c's raw()/cbreak() both clear ICANON|ISIG|IEXTEN; noraw()/
     * nocbreak() restore them. Either bit clear is enough to call this
     * "raw" -- there's no finer distinction BruceOS's own mode flag can
     * represent. */
    bool wants_raw = (buf->c_lflag & (ICANON | ISIG | IEXTEN)) != BRUCE_CURSES__COOKED_LFLAG;
    tty__set_mode(wants_raw ? BRUCE_TTY_MODE_RAW : BRUCE_TTY_MODE_COOKED);
    return 0;
}
