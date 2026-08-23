#pragma once

/*
 * Registered as the "launcher" built-in. It reads `launcherApp` from
 * bruce.conf via the public Config API and
 * starts that command through AppRunner, falling back to "bruce_launcher"
 * when the configured value is empty or fails to start.  See
 * migration_plan.md, "Bootstrap and launcher selection".
 *
 * This is distinct from "bruce_launcher" (src/modules/bruce_launcher/),
 * which is the actual menu application.
 * `launcher -s` remains resident and restarts the GUI launcher whenever no
 * foreground application remains.
 * `launcher config` forwards to "<configured launcher app> config" instead
 * of starting the menu, so it opens whichever app's settings screen is
 * actually configured to run (bruce_launcher's by default).
 */
int launcher_app_main(int argc, char **argv);
