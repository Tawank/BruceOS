#pragma once

/*
 * Built-in diagnostic app for exercising Core internals directly.
 *
 * Unlike every other built-in module, `selftest` is explicitly exempt from
 * the "built-ins use only core_sdk/ headers" rule (see migration_BruceIDF.md,
 * "Boundaries" / "Public SDK and migration rules").  Its entire purpose is to
 * validate Core's private implementation - task registry, memory tracking,
 * resource cleanup, and so on - so it is allowed to include Core-private
 * headers (core/...) and call FreeRTOS/ESP-IDF APIs directly, the same way
 * Core source itself does.  It must still be registered and launched like any
 * other app (app_runner__run("selftest", ...), from the launcher, or from the
 * terminal) and must not be treated as a source of Core capability for other
 * apps to depend on.
 */
int selftest_app(int argc, char **argv);
