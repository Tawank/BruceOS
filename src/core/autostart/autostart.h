#pragma once

#include <stdbool.h>

/* Launches every app in config__get_startup_apps(), in order, leaving
 * successfully created processes running concurrently. ui_ok gates
 * whether a command that explicitly claims the foreground (a "BG=0"
 * override) is auto-tagged "GUI=1"; when there is no display, apps are
 * left to request GUI themselves. */
void autostart__run(bool ui_ok);
