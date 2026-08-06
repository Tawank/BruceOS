#pragma once

/* App permissions management UI/CLI. Lives under modules/privileged (not the
 * core_sdk-only modules tree) because it must enumerate every app that has a
 * saved permission decision, and /config/permissions.json is deliberately
 * unreachable through the public core_sdk/storage.h API (see
 * storage__is_protected_path() in core/storage/storage.c) - only Core-private
 * storage__read_file()/storage__write_file_atomic() can see it. See
 * permissions_app.c for the full rationale. */

/* Register with a stack well above the app_runner default (see
 * PERMISSIONS_STACK_BYTES in main.c): the GUI's app/choice lists are
 * fixed-size on-stack arrays (up to 32 apps), and combined with the
 * fopen()-based Core-private storage__read_file() call chain that easily
 * overflows the 4096-byte default into a guard page (StoreProhibited). */
int permissions_app_main(int argc, char **argv);
