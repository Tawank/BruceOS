#pragma once

#include <stdbool.h>

/* Launches every app in config__get_startup_apps(), in order, leaving
 * successfully created processes running concurrently. display_ok gates
 * whether a command that explicitly claims the foreground (a "BG=0"
 * override) is auto-tagged "GUI=1"; when there is no display, apps are
 * left to request GUI themselves.
 *
 * skip_command, when not NULL, names one configured app to leave unstarted
 * for this boot. It is matched against the entry's command word - what is
 * left after any leading "key=value" environment tokens, and before any
 * arguments - so "launcher" also matches the configured default
 * "launcher -s". app_main() passes it when storage or memory is too broken
 * for that app to do anything but fail the same way in a loop. */
void autostart__run(bool display_ok, const char *skip_command);
