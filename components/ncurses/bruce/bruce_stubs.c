/* Stand-ins for a handful of upstream symbols that this trimmed vendoring
 * intentionally drops the real implementation of, but that a few kept files
 * still call unconditionally (they're not gated behind the feature's own
 * #ifdef at the call site):
 *
 *  - Soft label keys (slk_*, _nc_slk_initialize, _nc_format_slks): a
 *    hardware feature (dedicated function-key labels on some real
 *    terminals) terminal_ansi.c has no equivalent of. resizeterm.c,
 *    lib_newterm.c and lib_set_term.c call these as part of generic
 *    screen setup/teardown regardless of whether the application ever
 *    calls slk_init(), so they need a body, not just an #ifdef'd-out
 *    caller. No-op (return OK / do nothing) is exactly what real ncurses
 *    itself does internally once slk_init() was never called -- these
 *    stubs just skip straight to that empty state.
 *  - _nc_signal_handler: upstream lives in tty/lib_tstp.c, which also pulls
 *    in real SIGTSTP/job-control signal handling this port doesn't want
 *    (see components/ncurses/README.md -- no shell job control to suspend
 *    into, same reasoning vi.c's own USE_SIGNALS=OFF already uses in this
 *    repo). tty_update.c and lib_newterm.c call it unconditionally around
 *    screen switches; a no-op is correct since there's no real signal to
 *    mask/unmask here.
 *  - exit_terminfo: upstream lives in tinfo/entries.c, which this build
 *    doesn't vendor at all (it's the terminfo-*directory*-search family --
 *    see the README's "what's dropped" list). comp_error.c's _nc_err_abort
 *    and progs-only code call it as a cleanup-then-exit; this just does
 *    the "then exit" part since there's no entry-list cleanup to do when
 *    that list (see _nc_head below) is never populated.
 *  - _nc_head / _nc_free_entry: the ENTRY list is populated only by the
 *    terminfo-source/tic-compiler code path (tinfo/alloc_entry.c, also not
 *    vendored), which this build never exercises -- every lookup resolves
 *    through bruce_fallback.c instead. free_ttype.c's cleanup walks this
 *    list unconditionally when freeing a TERMTYPE2, so it needs to exist
 *    and be safe to walk; an always-empty list and a no-op unlink are
 *    exactly correct for a list nothing ever adds to.
 */
#include <curses.priv.h>
#include <term.h>
#include <tic.h>

NCURSES_EXPORT(int) _nc_slk_initialize(WINDOW *win GCC_UNUSED, int cols GCC_UNUSED) {
    return OK;
}

NCURSES_EXPORT(int) _nc_format_slks(SCREEN *sp GCC_UNUSED, int cols GCC_UNUSED) {
    return OK;
}

NCURSES_EXPORT(int) slk_restore_sp(SCREEN *sp GCC_UNUSED) {
    return OK;
}

NCURSES_EXPORT(int) slk_touch_sp(SCREEN *sp GCC_UNUSED) {
    return OK;
}

NCURSES_EXPORT(int) slk_refresh_sp(SCREEN *sp GCC_UNUSED) {
    return OK;
}

NCURSES_EXPORT(int) slk_clear(void) {
    return OK;
}

NCURSES_EXPORT(void) _nc_signal_handler(int enable GCC_UNUSED) { }

NCURSES_EXPORT(GCC_NORETURN void) exit_terminfo(int code) {
    exit(code);
}

NCURSES_EXPORT_VAR(ENTRY *) _nc_head = NULL;

NCURSES_EXPORT(void) _nc_free_entry(ENTRY *head GCC_UNUSED, TERMTYPE2 *tterm GCC_UNUSED) { }
