#pragma once

/* "/Network" folder feature: see filemanager_network.c's top comment. Used
 * only by filemanager_app.c (the main dispatch loop) and filemanager_actions.c
 * (Open needs to special-case a "/Network" entry). Not part of the public
 * core_sdk/ API: other modules must not include this header, only
 * filemanager_app.h.
 */

#include <stdbool.h>
#include <stddef.h>

#define FILEMANAGER_NETWORK_DIR "/Network"

/* Rebuilds "/Network" from every configured provider's discovery output.
 * Best-effort throughout: a provider that's missing, fails, or times out
 * just leaves that provider's locations absent rather than blocking entry
 * into the folder. */
void filemanager_network__refresh(void);

/* Resolves the app that should open the "/Network" entry at `path`: true
 * (with `program` filled in) only when `path` is a direct child of
 * FILEMANAGER_NETWORK_DIR named "<provider name> <label>" for a provider
 * still listed in "/config/filemanager.conf". False otherwise (including a
 * stray file dropped into "/Network" by hand, or a provider since removed
 * from the config) -- the caller falls back to its ordinary
 * extension-based Open in that case. */
bool filemanager_network__resolve_program(const char *path, char *program, size_t program_size);
