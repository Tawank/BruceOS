#pragma once

/*
 * Registered as the "launcher" built-in.  main.c starts this command at
 * boot; it reads `launcherApp` from bruce.json via the public Config API and
 * starts that command through AppRunner, falling back to "bruce_launcher"
 * when the configured value is empty or fails to start.  See
 * migration_BruceIDF.md, "Bootstrap and launcher selection".
 *
 * This is distinct from "bruce_launcher" (src/modules/bruce_launcher/),
 * which is the actual menu application.
 */
int launcher_app(int argc, char **argv);
