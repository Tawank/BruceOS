#pragma once

/*
 * Dumps a parsed browser_document_t -- both the raw parsed items and the
 * word-wrapped layout tokens browser_render.c actually draws from -- to the
 * physical console via plain printf(). Debugging aid only; nothing in the app
 * calls this during normal operation.
 */

#include "browser_document.h"

/* `font_scale` should be the view's current browser_view_state_t.font_scale
 * (see browser_render.h) so the printed layout matches what's on screen. */
void browser_debug__dump(const browser_document_t *doc, float font_scale);
