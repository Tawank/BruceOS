#pragma once

/* Low-level GUI helpers shared by every core/dialog/dialog_*.c renderer
 * (message, choice, pick_file, viewer) -- kept in one place so the theme's
 * color mapping and the "does this display get the bigger list font" rule
 * stay a single source of truth rather than one per file. */

#include <stdint.h>

/* pri/sec/bg are the accent, secondary-accent, and full-screen-canvas
 * colors; surface/border are for a raised bordered window on top of that
 * canvas; text is the readable color drawn over pri/sec-filled chrome
 * (title/footer bars) and over bg/surface bodies alike - a light theme can
 * set it dark and every one of those spots stays legible without each
 * caller guessing; success/warning/error are dialog__gui_message()'s
 * BRUCE_DIALOG_SUCCESS / _WARNING / _ERROR title-bar accent. */
void dialog__get_colors(
    uint16_t *pri, uint16_t *sec, uint16_t *bg, uint16_t *surface, uint16_t *text, uint16_t *text_muted,
    uint16_t *border, uint16_t *success, uint16_t *warning, uint16_t *error
);

/* Width threshold above which choice-list rows step up to a larger font,
 * matching bruce_launcher__submenu_font_size() in bruce_launcher_app.c so a
 * menu screen (e.g. Config, App Permissions) reads at the same size whether
 * it's rendered by the launcher's own submenu list or by dialog__choice().
 * Title bars/footers/messages stay at the small size, same as the
 * launcher's small header/status text. */
int dialog__default_list_text_size(void);
