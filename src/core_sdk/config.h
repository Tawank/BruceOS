#pragma once

/*
 * Field-specific Config API (public SDK surface).
 *
 * Stage 4 (A5) replaces whole-config external access with the full set of
 * field-specific getters/setters, `config` permission enforcement, and
 * permanently-protected fields.  This one getter is added early because the
 * "launcher" built-in (Stage 2 / A3) needs to read `launcherApp` from
 * bruce.json through a public API rather than the private core/config/config.h
 * struct.
 */

/* Returns a heap-allocated, NUL-terminated copy of the configured launcher
 * app command name (may be an empty string if unset). Caller frees the
 * result with free(). Returns NULL only on allocation failure. */
char *config__get_launcher_app(void);
