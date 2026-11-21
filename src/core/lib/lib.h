#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/result.h"

/* Libraries in /lib: ELF shared objects Core loads on demand, for code too big
 * to keep in the firmware (e.g. audio codecs). A library imports only the
 * names in lib_symbols.c and exports one entry point, lib_main(void), whose
 * return value is the library's API; each kind of library defines its own. */
typedef struct lib__handle lib_handle_t;

/* Loads /lib/<name>_<chip>.so or /lib/<name>.so into executable PSRAM, or
 * into swap on builds without it, or shares the one already loaded. A
 * library in swap belongs to the process that loaded it: another process gets
 * BRUCE_ERR_BUSY. The handle is a resource of the calling process, closed if
 * it exits first. *out_api is what lib_main() returned. BRUCE_ERR_NOT_FOUND
 * when there is no such file. */
bruce_result_t lib__open(const char *name, lib_handle_t **out_lib, const void **out_api);
/* Whether lib__open() would find the library's file. */
bool lib__exists(const char *name);
/* Unloads the library when its last user closes it. */
void lib__close(lib_handle_t *lib);

/* For the ELF loader's symbol resolver: true while this task relocates a
 * library, whose imports then come from lib__find_symbol(). */
bool lib__resolving(void);
uintptr_t lib__find_symbol(const char *name);
