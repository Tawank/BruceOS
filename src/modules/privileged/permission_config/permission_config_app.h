#pragma once

/* App permissions management UI/CLI. Lives under modules/privileged (not the
 * core_sdk-only modules tree) because it must enumerate every app that has a
 * saved permission decision, and /config/permissions.json is deliberately
 * unreachable through the public core_sdk/storage.h API (see
 * storage__is_protected_path() in core/storage/storage.c) - only Core-private
 * storage__read_file()/storage__write_file_atomic() can see it. See
 * permission_config_app.c for the full rationale. */

int permission_config_app_main(int argc, char **argv);
