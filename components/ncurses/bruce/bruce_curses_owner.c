/* Singleton screen ownership.
 *
 * Real ncurses relies on actual `extern` globals (stdscr, curscr, COLS,
 * LINES, ...) that a caller's own compiled code reads directly -- fine on
 * BruceOS only because a curses screen is a foreground/singleton
 * resource, the same way the display and a routed stdio session already
 * are (see core_sdk/tty.h's own ownership checks for the same pattern).
 * Two processes sharing this one firmware-compiled ncurses concurrently
 * would silently corrupt each other's screen state instead of getting
 * independent ones.
 *
 * bruce_curses__try_acquire() is called from base/lib_newterm.c's
 * NCURSES_SP_NAME(newterm) -- the one real funnel both initscr() and a
 * direct newterm() call go through -- and fails the call (NULL return,
 * exactly like a real ncurses setup failure) if a different, still-alive
 * process already owns the screen.
 *
 * Ownership is self-healing rather than tied only to a voluntary
 * endwin()/delscreen() call: if the recorded owner no longer exists (it
 * exited, crashed, or was killed without ever calling endwin()),
 * process__snapshot() reports it missing and the stale claim is cleared
 * instead of wedging the screen "owned" by a dead process forever. This
 * is a deliberate trade-off over a hard exit hook: it means a *second*
 * acquire attempt is what notices and clears a stale owner, not the
 * first thing that happens after the owner dies -- acceptable here since
 * nothing else needs the screen to become free immediately. */
#include "core_sdk/process.h"

static bruce_process_id_t s_owner = BRUCE_PROCESS_ID_INVALID;

static bool bruce_curses__owner_is_alive(bruce_process_id_t id) {
    bruce_process_snapshot_t snapshot;
    return process__snapshot(id, &snapshot) == BRUCE_OK;
}

int bruce_curses__try_acquire(void) {
    bruce_process_id_t me = process__current_id();

    if (s_owner != BRUCE_PROCESS_ID_INVALID && s_owner != me) {
        if (bruce_curses__owner_is_alive(s_owner)) {
            return -1; /* still genuinely in use by another live process */
        }
        /* Stale claim from a process that's gone -- reclaim it below. */
    }
    s_owner = me;
    return 0;
}

void bruce_curses__release(void) {
    bruce_process_id_t me = process__current_id();
    if (s_owner == me) s_owner = BRUCE_PROCESS_ID_INVALID;
}
